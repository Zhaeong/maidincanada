#include <SDL2/SDL.h>
#include <emscripten.h>

#define CANVAS_SIZE 512
#define SQUARE_SIZE 128

static SDL_Renderer *renderer;

static void draw(void) {
    SDL_SetRenderDrawColor(renderer, 255, 165, 0, 255);
    SDL_RenderClear(renderer);

    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
    SDL_Rect square = {
        (CANVAS_SIZE - SQUARE_SIZE) / 2,
        (CANVAS_SIZE - SQUARE_SIZE) / 2,
        SQUARE_SIZE,
        SQUARE_SIZE
    };
    SDL_RenderFillRect(renderer, &square);

    SDL_RenderPresent(renderer);
}

int main(void) {
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window *window;
    SDL_CreateWindowAndRenderer(CANVAS_SIZE, CANVAS_SIZE, 0, &window, &renderer);

    // Render via the browser's animation loop. Drawing only once in main()
    // leaves the canvas blank: without a main loop the WebGL drawing buffer
    // is cleared when the frame is composited.
    emscripten_set_main_loop(draw, 0, 1);

    return 0;
}
