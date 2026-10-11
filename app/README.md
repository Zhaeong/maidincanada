# Basic Emscripten App

A minimal C program compiled to WebAssembly with Emscripten. It uses SDL2
(bundled with Emscripten via `-sUSE_SDL=2`) to draw a blue square centered
on a 512×512 canvas.

## Layout

- `src/main.c` — the C source. Creates a 512×512 SDL window (mapped to the
  page's `<canvas>`), clears it to white, and fills a centered blue square.
- `build.bat` — sets up the emsdk environment (from `../emsdk`) and compiles
  to `out/index.html` + `.js` + `.wasm`.

## Build

First need to install emsdk:

```
python first needs to be installed (e.g. use miniforge)
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
.\emsdk install latest
.\emsdk activate latest
The can activate for later use:
emsdk_env.bat
```
Afterwards the build.bat script should handle all that:
```
build.bat

Which does:
emcc src\main.c -O2 -o out\index.html -sUSE_SDL=2

```

## Run

The generated `.wasm` must be served over HTTP (not opened via `file://`):

```
python -m http.server --directory out
```

Then open http://localhost:8000 — the canvas on the shell page shows the
blue square. (Rendering needs a browser, so `node out/index.js` is not
applicable for this version.)
