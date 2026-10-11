/* simple rect -- SDL2, strict C89.
 *
 * Two builds, one source:
 *   - WebAssembly (emcc): uses <SDL2/SDL.h> and the browser animation loop
 *   - Native (MSVC cl):   uses the flat <SDL.h> from the SDL2 VC package
 *     (app\third_party\SDL2-2.32.10) and a classic polling event loop
 */

#ifdef __EMSCRIPTEN__
#include <SDL2/SDL.h>
#include <emscripten.h>
#else
#define SDL_MAIN_HANDLED   /* keep our own main(); no SDL2main glue needed */
#include <SDL.h>
#endif

#define CANVAS_SIZE 512
#define SQUARE_SIZE 128

static SDL_Renderer *renderer;

static void draw(void) {
    SDL_Rect square = {
        (CANVAS_SIZE - SQUARE_SIZE) / 2,
        (CANVAS_SIZE - SQUARE_SIZE) / 2,
        SQUARE_SIZE,
        SQUARE_SIZE
    };

    SDL_SetRenderDrawColor(renderer, 255, 165, 0, 255);
    SDL_RenderClear(renderer);

    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
    SDL_RenderFillRect(renderer, &square);

    SDL_RenderPresent(renderer);
}

int main(void) {
    SDL_Window *window;

    SDL_Init(SDL_INIT_VIDEO);

    SDL_CreateWindowAndRenderer(CANVAS_SIZE, CANVAS_SIZE, 0, &window, &renderer);

#ifdef __EMSCRIPTEN__
    /* Render via the browser's animation loop. Drawing only once in main()
     * leaves the canvas blank: without a main loop the WebGL drawing buffer
     * is cleared when the frame is composited.
     */
    emscripten_set_main_loop(draw, 0, 1);
#else
    /* Native: keep drawing until the window is closed. */
    {
        int running = 1;

        while (running) {
            SDL_Event event;

            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT)
                    running = 0;
            }
            draw();
            SDL_Delay(16);
        }
    }
#endif

    SDL_Quit();
    return 0;
}
