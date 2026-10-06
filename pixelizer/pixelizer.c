/* pixelizer.c -- load a JPEG, pixelize it, write a 24-bit BMP.
 *
 * Strict C89, compiled with MSVC /Za, and depending on nothing beyond
 * the C standard library.  This includes a self-contained baseline
 * JPEG decoder:
 *   - SOF0/SOF1 (sequential DCT, Huffman) only
 *   - 8-bit precision, 1 or 3 components, sampling factors 1..4
 *   - optional restart intervals (DRI/RSTn)
 * A hand-rolled decoder is used because the Windows SDK headers do not
 * compile in strict ANSI C89 mode (they use anonymous unions, which are
 * a C11 feature), so the system WIC decoder is not usable under /Za.
 *
 * usage: pixelizer <input.jpg> [output.bmp] [-a W:H | ratio] [-p N] [-d]
 *   -a  output aspect ratio, e.g. -a 16:9, -a 1:1, or -a 1.7778;
 *       the canvas is the largest block-aligned rectangle of that
 *       aspect fitting inside 640x480 (default 4:3 = 640x480)
 *   -p  pixel (block) size in output pixels, 1..480; default 8
 *   -d  also dump the decoded source image as decoded.bmp
 */

#define _CRT_SECURE_NO_WARNINGS /* fopen is C89; fopen_s is not */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_W   640             /* canvas width bound               */
#define MAX_H   480             /* canvas height bound (max block) */
#define BPP     3               /* 24bpp BGR                        */

#define JPEG_PI 3.14159265358979323846

static void fatal(const char *what)
{
    fprintf(stderr, "pixelizer: %s\n", what);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL)
        fatal("out of memory");
    return p;
}

static unsigned char clamp255(double v)
{
    long r;

    r = (long)(v + 0.5);
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    return (unsigned char)r;
}

/* ------------------------------------------------------------------ */
/* Baseline JPEG decoder                                              */
/* ------------------------------------------------------------------ */

#define MAX_TQ 4                 /* quantization tables              */
#define MAX_TH 4                 /* Huffman tables per class         */

static int ZZ[64];                /* zigzag index -> raster index     */
static double COS_T[8][8];        /* COS_T[a][f] = cos((2a+1) f pi/16) */
static double CU[8];              /* CU[0] = 1/sqrt(2), others 1      */

static int qtab[MAX_TQ][64];      /* quant values, raster order       */
static int have_qt[MAX_TQ];

typedef struct {
    int ncount[17];              /* codes of each length 1..16       */
    int sym[256];                /* symbols in canonical order        */
    long mincode[17];            /* first canonical code per length   */
    int valptr[17];              /* symbol offset per length          */
} HUFF;

static HUFF huff_dc[MAX_TH], huff_ac[MAX_TH];
static int have_dc[MAX_TH], have_ac[MAX_TH];

typedef struct {
    int id;                      /* component id from SOF             */
    int h, v;                    /* sampling factors                  */
    int tq, td, ta;              /* table selectors                   */
    long dc_pred;                /* DC predictor (reset per restart) */
    int plane_stride, plane_h;   /* padded plane geometry             */
    int real_w, real_h;          /* meaningful sample geometry        */
    unsigned char *plane;        /* decoded samples                   */
} JCOMP;

static JCOMP comps[3];
static int ncomp;
static int img_w, img_h;
static int mcu_w, mcu_h;
static int restart_interval;

static void init_tables(void)
{
    int k, d, r, c, a, f;

    /* zigzag order: anti-diagonals, even up-right, odd down-left */
    k = 0;
    for (d = 0; d <= 14; d++) {
        if (d % 2 == 0) {
            r = (d < 8) ? d : 7;
            c = d - r;
            while (r >= 0 && c <= 7) {
                ZZ[k] = r * 8 + c;
                k++;
                r--;
                c++;
            }
        } else {
            c = (d < 8) ? d : 7;
            r = d - c;
            while (c >= 0 && r <= 7) {
                ZZ[k] = r * 8 + c;
                k++;
                r++;
                c--;
            }
        }
    }

    CU[0] = sqrt(0.5);
    for (f = 1; f < 8; f++)
        CU[f] = 1.0;

    for (a = 0; a < 8; a++)
        for (f = 0; f < 8; f++)
            COS_T[a][f] = cos((2 * a + 1) * f * JPEG_PI / 16.0);
}

/* Entropy-coded bit reader: MSB first, 0xFF00 unescaping */

typedef struct {
    const unsigned char *data;
    long size;
    long pos;
    unsigned long bitbuf;        /* one byte, at most                 */
    int bitcnt;                  /* unconsumed bits in bitbuf         */
} BITRD;

static unsigned char next_byte(BITRD *br)
{
    unsigned char c, c2;

    if (br->pos >= br->size)
        fatal("unexpected end of JPEG entropy data");
    c = br->data[br->pos];
    if (c != 0xFF) {
        br->pos++;
        return c;
    }
    if (br->pos + 1 >= br->size)
        fatal("unexpected end of JPEG entropy data");
    c2 = br->data[br->pos + 1];
    if (c2 == 0x00) {            /* byte stuffing */
        br->pos += 2;
        return 0xFF;
    }
    fatal("marker inside entropy data (corrupt or unsupported JPEG)");
}

static int getbit(BITRD *br)
{
    int bit;

    if (br->bitcnt == 0) {
        unsigned char c = next_byte(br);

        br->bitbuf = (unsigned long)c;
        br->bitcnt = 8;
    }
    br->bitcnt--;
    bit = (int)((br->bitbuf >> br->bitcnt) & 1u);
    return bit;
}

static int receive(BITRD *br, int s)
{
    int v = 0;
    int i;

    for (i = 0; i < s; i++)
        v = (v << 1) | getbit(br);
    return v;
}

/* sign extension of an s-bit magnitude value */
static int extend(int v, int s)
{
    if (v < (1 << (s - 1)))
        v -= (1 << s) - 1;
    return v;
}

static void huff_build(HUFF *h)
{
    long code = 0;
    int k = 0;
    int len;

    for (len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += h->ncount[len];
        k += h->ncount[len];
        if (code > (1L << len))
            fatal("invalid Huffman table");
        code <<= 1;
    }
}

static int huff_decode(BITRD *br, const HUFF *h)
{
    long code = 0;
    int len;

    for (len = 1; len <= 16; len++) {
        code = (code << 1) | getbit(br);
        if (h->ncount[len] > 0 &&
            code >= h->mincode[len] &&
            code < h->mincode[len] + h->ncount[len])
            return h->sym[h->valptr[len] + (int)(code - h->mincode[len])];
    }
    fatal("corrupt Huffman data");
}

/* 8x8 float IDCT: F = 1/4 Cu Cv sum ... level-shifted to 0..255 */

static void idct8(const double *in, unsigned char *dst, int stride)
{
    double tmp[64];
    int x, y, f;

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            double s = 0.0;

            for (f = 0; f < 8; f++)
                s += CU[f] * in[y * 8 + f] * COS_T[x][f];
            tmp[y * 8 + x] = s;
        }
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            double s = 0.0;

            for (f = 0; f < 8; f++)
                s += CU[f] * tmp[f * 8 + x] * COS_T[y][f];
            dst[y * stride + x] = clamp255(s * 0.25 + 128.0);
        }
    }
}

static void decode_block(BITRD *br, JCOMP *c, int px, int py)
{
    double coef[64];
    int *qt = qtab[c->tq];
    int t, run, size, k, i;

    for (i = 0; i < 64; i++)
        coef[i] = 0.0;

    t = huff_decode(br, &huff_dc[c->td]);
    if (t > 15)
        fatal("bad DC size");
    if (t != 0)
        c->dc_pred += extend(receive(br, t), t);
    coef[0] = (double)c->dc_pred * (double)qt[0];

    k = 1;
    while (k < 64) {
        int raster;

        t = huff_decode(br, &huff_ac[c->ta]);
        run = t >> 4;
        size = t & 15;
        if (size == 0) {
            if (run == 15) {    /* ZRL: sixteen zeros */
                k += 16;
                continue;
            }
            break;               /* EOB */
        }
        k += run;
        if (k > 63)
            fatal("bad AC data");
        raster = ZZ[k];
        coef[raster] = (double)extend(receive(br, size), size) * (double)qt[raster];
        k++;
    }
    idct8(coef, c->plane + (size_t)py * (size_t)c->plane_stride + px,
          c->plane_stride);
}

static void do_rst(BITRD *br)
{
    int i;

    br->bitcnt = 0;              /* drop padding bits to the byte boundary */
    if (br->pos + 1 >= br->size ||
        br->data[br->pos] != 0xFF ||
        (br->data[br->pos + 1] & 0xF8) != 0xD0)
        fatal("missing restart marker");
    br->pos += 2;
    for (i = 0; i < ncomp; i++)
        comps[i].dc_pred = 0;
}

static void decode_scan(BITRD *br)
{
    long mcus_done = 0;
    int mx, my, ci, bx, by;

    for (my = 0; my < mcu_h; my++) {
        for (mx = 0; mx < mcu_w; mx++) {
            for (ci = 0; ci < ncomp; ci++) {
                JCOMP *c = &comps[ci];

                for (by = 0; by < c->v; by++) {
                    for (bx = 0; bx < c->h; bx++) {
                        decode_block(br, c,
                                     (mx * c->h + bx) * 8,
                                     (my * c->v + by) * 8);
                    }
                }
            }
            mcus_done++;
            if (restart_interval > 0 &&
                mcus_done % restart_interval == 0 &&
                mcus_done < (long)mcu_w * (long)mcu_h)
                do_rst(br);
        }
    }
}

/* Convert decoded planes to a top-down 24bpp BGR buffer.  Chroma is
 * upsampled by nearest neighbour; grayscale needs no conversion. */

static unsigned char *to_bgr(void)
{
    unsigned char *out;
    size_t stride;
    int x, y;

    out = (unsigned char *)xmalloc((size_t)img_w * (size_t)img_h * BPP);
    stride = (size_t)img_w * BPP;

    for (y = 0; y < img_h; y++) {
        const unsigned char *yrow, *cbrow, *crrow;
        unsigned char *orow;

        orow = out + (size_t)y * stride;
        yrow = comps[0].plane +
               (size_t)(y * comps[0].real_h / img_h) * (size_t)comps[0].plane_stride;
        cbrow = NULL;
        crrow = NULL;
        if (ncomp == 3) {
            cbrow = comps[1].plane +
                    (size_t)(y * comps[1].real_h / img_h) * (size_t)comps[1].plane_stride;
            crrow = comps[2].plane +
                    (size_t)(y * comps[2].real_h / img_h) * (size_t)comps[2].plane_stride;
        }
        for (x = 0; x < img_w; x++) {
            int yv = yrow[x * comps[0].real_w / img_w];

            if (ncomp == 3) {
                int cxb = x * comps[1].real_w / img_w;
                int cxr = x * comps[2].real_w / img_w;
                int cb = cbrow[cxb] - 128;
                int cr = crrow[cxr] - 128;
                double r = yv + 1.40200 * cr;
                double g = yv - 0.34414 * cb - 0.71414 * cr;
                double b = yv + 1.77200 * cb;

                orow[0] = clamp255(b);
                orow[1] = clamp255(g);
                orow[2] = clamp255(r);
            } else {
                orow[0] = (unsigned char)yv;
                orow[1] = (unsigned char)yv;
                orow[2] = (unsigned char)yv;
            }
            orow += BPP;
        }
    }
    return out;
}

/* Parse segments, decode the scan, return a top-down 24bpp BGR buffer */

static unsigned char *jpeg_decode(const unsigned char *data, long size,
                                   int *out_w, int *out_h)
{
    long pos;
    int m = 0, len = 0, sof_seen = 0;
    unsigned char *result;

    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        fatal("not a JPEG (missing SOI)");
    pos = 2;

    for (;;) {
        if (pos + 2 > size)
            fatal("truncated JPEG (marker expected)");
        if (data[pos] != 0xFF)
            fatal("desynchronized JPEG markers");
        m = data[pos + 1];
        pos += 2;
        if (m == 0x01)            /* TEM, no payload */
            continue;
        if (m == 0xD9)
            fatal("EOI before image data");
        if (m >= 0xD0 && m <= 0xD7)
            fatal("unexpected restart marker");

        if (pos + 2 > size)
            fatal("truncated JPEG marker");
        len = ((int)data[pos] << 8) | (int)data[pos + 1];
        if (len < 2 || pos + len > size)
            fatal("bad JPEG marker length");

        if (m == 0xC0 || m == 0xC1) {
            int p = pos + 2;
            int prec, n, i, hmax, vmax;

            if (sof_seen)
                fatal("multiple SOF markers");
            prec = data[p];
            img_h = ((int)data[p + 1] << 8) | (int)data[p + 2];
            img_w = ((int)data[p + 3] << 8) | (int)data[p + 4];
            n = data[p + 5];
            if (prec != 8)
                fatal("only 8-bit JPEG precision supported");
            if (n != 1 && n != 3)
                fatal("only grayscale or 3-component JPEG supported");
            if (img_w <= 0 || img_h <= 0)
                fatal("bad image dimensions");
            if (len < 6 + 3 * n)
                fatal("bad SOF segment");
            ncomp = n;
            p += 6;
            hmax = 1;
            vmax = 1;
            for (i = 0; i < ncomp; i++) {
                int hv = data[p + 1];

                comps[i].id = data[p];
                comps[i].h = hv >> 4;
                comps[i].v = hv & 15;
                comps[i].tq = data[p + 2];
                if (comps[i].h < 1 || comps[i].h > 4 ||
                    comps[i].v < 1 || comps[i].v > 4)
                    fatal("bad sampling factors");
                if (comps[i].tq >= MAX_TQ)
                    fatal("bad quantization table id");
                if (comps[i].h > hmax)
                    hmax = comps[i].h;
                if (comps[i].v > vmax)
                    vmax = comps[i].v;
                p += 3;
            }
            mcu_w = (img_w + hmax * 8 - 1) / (hmax * 8);
            mcu_h = (img_h + vmax * 8 - 1) / (vmax * 8);
            for (i = 0; i < ncomp; i++) {
                int bw = mcu_w * comps[i].h;
                int bh = mcu_h * comps[i].v;

                comps[i].plane_stride = bw * 8;
                comps[i].plane_h = bh * 8;
                comps[i].real_w = (img_w * comps[i].h + hmax - 1) / hmax;
                comps[i].real_h = (img_h * comps[i].v + vmax - 1) / vmax;
                comps[i].dc_pred = 0;
                comps[i].plane = (unsigned char *)xmalloc(
                    (size_t)comps[i].plane_stride * (size_t)comps[i].plane_h);
            }
            sof_seen = 1;
        } else if (m == 0xDB) {
            int p = pos + 2;
            int end = pos + len;

            while (p < end) {
                int pq, tq, k;

                if (p + 65 > end)
                    fatal("bad DQT segment");
                pq = data[p] >> 4;
                tq = data[p] & 15;
                if (pq != 0)
                    fatal("16-bit quant tables not supported");
                if (tq >= MAX_TQ)
                    fatal("bad quantization table id");
                for (k = 0; k < 64; k++)
                    qtab[tq][ZZ[k]] = data[p + 1 + k];
                have_qt[tq] = 1;
                p += 65;
            }
        } else if (m == 0xC4) {
            int p = pos + 2;
            int end = pos + len;

            while (p < end) {
                int tc, th, total, j;
                HUFF *h;

                if (p + 17 > end)
                    fatal("bad DHT segment");
                tc = data[p] >> 4;
                th = data[p] & 15;
                if ((tc != 0 && tc != 1) || th >= MAX_TH)
                    fatal("bad Huffman table id");
                h = (tc == 0) ? &huff_dc[th] : &huff_ac[th];
                total = 0;
                for (j = 1; j <= 16; j++) {
                    h->ncount[j] = data[p + j];
                    total += h->ncount[j];
                }
                if (total > 256 || p + 17 + total > end)
                    fatal("bad DHT segment");
                for (j = 0; j < total; j++)
                    h->sym[j] = data[p + 17 + j];
                huff_build(h);
                if (tc == 0)
                    have_dc[th] = 1;
                else
                    have_ac[th] = 1;
                p += 17 + total;
            }
        } else if (m == 0xDD) {
            if (len < 4)
                fatal("bad DRI segment");
            restart_interval = ((int)data[pos + 2] << 8) | (int)data[pos + 3];
        } else if (m == 0xDA) {
            int p = pos + 2;
            int ns, i, ss, se, ah, al;
            BITRD br;

            if (!sof_seen)
                fatal("SOS before SOF");
            if (len < 6)
                fatal("bad SOS segment");
            ns = data[p];
            if (ns != ncomp)
                fatal("only single-scan JPEG supported");
            p++;
            for (i = 0; i < ns; i++) {
                int cid = data[p];
                int sel = data[p + 1];
                int j, found = 0;

                for (j = 0; j < ncomp; j++) {
                    if (comps[j].id == cid) {
                        comps[j].td = sel >> 4;
                        comps[j].ta = sel & 15;
                        if (comps[j].td >= MAX_TH || comps[j].ta >= MAX_TH)
                            fatal("bad Huffman table selector");
                        if (!have_dc[comps[j].td] || !have_ac[comps[j].ta])
                            fatal("Huffman table not defined");
                        if (!have_qt[comps[j].tq])
                            fatal("quantization table not defined");
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    fatal("SOS references unknown component");
                p += 2;
            }
            ss = data[p];
            se = data[p + 1];
            ah = data[p + 2] >> 4;
            al = data[p + 2] & 15;
            if (ss != 0 || se != 63 || ah != 0 || al != 0)
                fatal("progressive JPEG not supported");

            br.data = data;
            br.size = size;
            br.pos = pos + len;
            br.bitbuf = 0;
            br.bitcnt = 0;
            decode_scan(&br);
            result = to_bgr();
            *out_w = img_w;
            *out_h = img_h;
            for (i = 0; i < ncomp; i++) {
                free(comps[i].plane);
                comps[i].plane = NULL;
            }
            return result;
        } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xC7) ||
                   m == 0xC8 || m == 0xC9 || m == 0xCA || m == 0xCB ||
                   (m >= 0xCD && m <= 0xCF) || m == 0xCC) {
            fatal("unsupported JPEG coding (only baseline sequential Huffman)");
        }
        /* APPn, COM, and anything else: skip */

        pos += len;
    }
}

static unsigned char *read_file(const char *path, long *out_size)
{
    FILE *f;
    unsigned char *buf;
    long sz;

    f = fopen(path, "rb");
    if (f == NULL)
        fatal("cannot open input file");
    if (fseek(f, 0, SEEK_END) != 0)
        fatal("cannot seek input file");
    sz = ftell(f);
    if (sz < 0)
        fatal("cannot size input file");
    if (fseek(f, 0, SEEK_SET) != 0)
        fatal("cannot seek input file");
    if (sz == 0)
        fatal("input file is empty");
    buf = (unsigned char *)xmalloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        fatal("cannot read input file");
    fclose(f);
    *out_size = sz;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Pixelization                                                       */
/* ------------------------------------------------------------------ */

/* Parse an aspect ratio: "W:H" (e.g. 16:9) or a plain number (e.g. 1.7778). */
static double parse_ratio(const char *s)
{
    const char *colon;
    char buf[64];
    char *end;
    double num, den;
    size_t n;

    colon = strchr(s, ':');
    if (colon == NULL) {
        num = strtod(s, &end);
        if (end == s || *end != '\0' || num <= 0.0)
            fatal("bad aspect ratio (use W:H or a number, e.g. 16:9)");
        return num;
    }
    n = (size_t)(colon - s);
    if (n == 0 || n >= sizeof(buf))
        fatal("bad aspect ratio (use W:H, e.g. 16:9)");
    memcpy(buf, s, n);
    buf[n] = '\0';
    num = strtod(buf, &end);
    if (end == buf || *end != '\0' || num <= 0.0)
        fatal("bad aspect ratio (use W:H, e.g. 16:9)");
    den = strtod(colon + 1, &end);
    if (end == colon + 1 || *end != '\0' || den <= 0.0)
        fatal("bad aspect ratio (use W:H, e.g. 16:9)");
    return num / den;
}

/* Snap a positive length to the nearest multiple of block (min block). */
static int snap_block(double v, int block)
{
    long n;

    n = (long)(v / (double)block + 0.5);
    if (n < 1)
        n = 1;
    return (int)(n * block);
}

/* Largest block-aligned canvas of the given aspect ratio that fits
 * inside MAX_W x MAX_H.  With block 8 and ratio 4:3 this is 640x480. */
static void canvas_for_ratio(double ratio, int block, int *cw, int *ch)
{
    int wcap, hcap;
    double w, h;

    wcap = (MAX_W / block) * block;
    hcap = (MAX_H / block) * block;
    if (ratio >= (double)wcap / (double)hcap) {
        w = (double)wcap;
        h = w / ratio;
        if (h > (double)hcap)
            h = (double)hcap;
    } else {
        h = (double)hcap;
        w = h * ratio;
        if (w > (double)wcap)
            w = (double)wcap;
    }
    *cw = snap_block(w, block);
    *ch = snap_block(h, block);
    if (*cw > wcap)
        *cw = wcap;
    if (*ch > hcap)
        *ch = hcap;
}

/* Parse the pixel (block) size: an integer from 1 to MAX_H.  The block
 * must not exceed MAX_H so a whole block always fits the canvas. */
static int parse_block(const char *s)
{
    char *end;
    long v;

    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > MAX_H)
        fatal("pixel size must be an integer from 1 to 480");
    return (int)v;
}

/* Average the source onto a gw x gh grid of block-sized cells covering
 * the cw x ch canvas (aspect-preserving, letterboxed with black),
 * then replicate each cell to block x block output pixels.  Returns
 * a top-down 24bpp BGR buffer, cw x ch. */

static unsigned char *pixelize(const unsigned char *src, int sw, int sh,
                                int cw, int ch, int block)
{
    unsigned char *dst;
    long grid_w, grid_h, gw, gh, gox, goy;
    long cx, cy, c, i, j;

    dst = (unsigned char *)xmalloc((size_t)cw * (size_t)ch * (size_t)BPP);

    grid_w = cw / block;
    grid_h = ch / block;

    /* fitted image size in cells, snapped to whole cells */
    if ((double)sw / (double)sh > (double)cw / (double)ch) {
        gw = grid_w;
        gh = (long)((double)cw * (double)sh / (double)sw / (double)block + 0.5);
    } else {
        gh = grid_h;
        gw = (long)((double)ch * (double)sw / (double)sh / (double)block + 0.5);
    }
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    if (gw > grid_w) gw = grid_w;
    if (gh > grid_h) gh = grid_h;
    gox = (grid_w - gw) / 2;     /* letterbox offsets in grid cells */
    goy = (grid_h - gh) / 2;

    for (cy = 0; cy < grid_h; cy++) {
        for (cx = 0; cx < grid_w; cx++) {
            unsigned char color[BPP];

            if (cx < gox || cx >= gox + gw || cy < goy || cy >= goy + gh) {
                for (c = 0; c < BPP; c++)
                    color[c] = 0;              /* letterbox: black */
            } else {
                unsigned long sum[BPP];
                unsigned long n;
                long lx, ly, sx0, sx1, sy0, sy1;
                const unsigned char *row;

                lx = cx - gox;
                ly = cy - goy;
                sx0 = lx * (long)sw / gw;      /* source rect for cell */
                sx1 = (lx + 1) * (long)sw / gw;
                sy0 = ly * (long)sh / gh;
                sy1 = (ly + 1) * (long)sh / gh;
                if (sx1 <= sx0) sx1 = sx0 + 1; /* never empty */
                if (sy1 <= sy0) sy1 = sy0 + 1;
                if (sx1 > (long)sw) sx1 = (long)sw;
                if (sy1 > (long)sh) sy1 = (long)sh;

                n = (unsigned long)(sx1 - sx0) * (unsigned long)(sy1 - sy0);
                for (c = 0; c < BPP; c++)
                    sum[c] = 0;
                for (j = sy0; j < sy1; j++) {
                    row = src + ((size_t)j * (size_t)sw + (size_t)sx0) * (size_t)BPP;
                    for (i = sx0; i < sx1; i++) {
                        for (c = 0; c < BPP; c++)
                            sum[c] += row[c];
                        row += BPP;
                    }
                }
                for (c = 0; c < BPP; c++)
                    color[c] = (unsigned char)(sum[c] / n);
            }

            /* stamp the block x block cell */
            for (j = 0; j < block; j++) {
                unsigned char *out;

                out = dst + (((size_t)(cy * (long)block + j) * (size_t)cw) +
                             (size_t)(cx * (long)block)) * (size_t)BPP;
                for (i = 0; i < block; i++) {
                    for (c = 0; c < BPP; c++)
                        out[c] = color[c];
                    out += BPP;
                }
            }
        }
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/* BMP output (24bpp, bottom-up rows, little-endian, no structs so    */
/* there is no dependence on compiler packing)                       */
/* ------------------------------------------------------------------ */

static void put16(FILE *f, unsigned int v)
{
    fputc((int)(v & 0xFFu), f);
    fputc((int)((v >> 8) & 0xFFu), f);
}

static void put32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xFFu), f);
    fputc((int)((v >> 8) & 0xFFu), f);
    fputc((int)((v >> 16) & 0xFFu), f);
    fputc((int)((v >> 24) & 0xFFu), f);
}

static void write_bmp(const char *path, const unsigned char *px, int w, int h)
{
    FILE *f;
    unsigned long header_size = 54;
    unsigned long stride = (unsigned long)w * (unsigned long)BPP;
    unsigned long image_size = stride * (unsigned long)h;
    unsigned int y;

    f = fopen(path, "wb");
    if (f == NULL)
        fatal("cannot open output file");
    put16(f, 0x4D42);            /* "BM" */
    put32(f, header_size + image_size);
    put32(f, 0);                 /* reserved */
    put32(f, header_size);       /* offset to pixel data */
    put32(f, 40);                /* BITMAPINFOHEADER */
    put32(f, (unsigned long)w);
    put32(f, (unsigned long)h);
    put16(f, 1);                 /* planes */
    put16(f, 24);                /* bits per pixel */
    put32(f, 0);                 /* BI_RGB */
    put32(f, image_size);
    put32(f, 0);                 /* x pixels per metre */
    put32(f, 0);                 /* y pixels per metre */
    put32(f, 0);                 /* colors used */
    put32(f, 0);                 /* colors important */
    for (y = 0; y < (unsigned int)h; y++) {
        const unsigned char *row;

        row = px + (size_t)((long)h - 1 - (long)y) * (size_t)(w * BPP);
        if (fwrite(row, BPP, w, f) != (size_t)w)
            fatal("cannot write BMP pixels");
    }
    if (fclose(f) != 0)
        fatal("cannot close output file");
}

int main(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *ratio_s = NULL;
    const char *block_s = NULL;
    unsigned char *jpg;
    unsigned char *src;
    unsigned char *out;
    long jpg_size;
    int sw, sh;
    int cw, ch;
    int debug = 0;
    int i;
    int block = 8;               /* default pixel size */
    double ratio = (double)MAX_W / (double)MAX_H;   /* default 4:3 */

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            if (i + 1 >= argc)
                fatal("option -a needs an aspect ratio, e.g. -a 16:9");
            ratio_s = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc)
                fatal("option -p needs a pixel size, e.g. -p 16");
            block_s = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0) {
            debug = 1;
        } else if (in_path == NULL) {
            in_path = argv[i];
        } else if (out_path == NULL) {
            out_path = argv[i];
        } else {
            fatal("too many arguments");
        }
    }
    if (in_path == NULL) {
        fprintf(stderr,
                "usage: pixelizer <input.jpg> [output.bmp]"
                " [-a W:H | ratio] [-p N] [-d]\n"
                "  -a  output aspect ratio (e.g. 16:9, 4:3, 1:1, 1.7778);\n"
                "      default 4:3; canvas fits inside 640x480\n"
                "  -p  pixel (block) size in output pixels, 1..480; default 8\n"
                "  -d  also dump the decoded source image as decoded.bmp\n");
        return EXIT_FAILURE;
    }
    if (out_path == NULL)
        out_path = "pixelized.bmp";
    if (ratio_s != NULL)
        ratio = parse_ratio(ratio_s);
    if (block_s != NULL)
        block = parse_block(block_s);
    canvas_for_ratio(ratio, block, &cw, &ch);

    init_tables();
    jpg = read_file(in_path, &jpg_size);
    src = jpeg_decode(jpg, jpg_size, &sw, &sh);
    free(jpg);

    out = pixelize(src, sw, sh, cw, ch, block);

    if (debug)
        write_bmp("decoded.bmp", src, sw, sh);
    free(src);

    write_bmp(out_path, out, cw, ch);
    free(out);

    printf("%s: %dx%d -> %dx%d grid (block %dx%d) -> %s (%dx%d)\n",
           in_path, sw, sh, cw / block, ch / block, block, block,
           out_path, cw, ch);
    return 0;
}
