/* frontend_preview.c — standalone SDL2+GL preview of the frontend.
 * Usage: frontend_preview [width height]
 *   default 640×480, try 1280×720 for widescreen check.
 *
 * Keys: ↑↓ navigate, Enter confirm, Esc back/quit. */
#ifdef __APPLE__
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <SDL.h>
#include "frontend.h"
#include "frontend_draw.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int w = 640, h = 480;
    if (argc >= 3) { w = atoi(argv[1]); h = atoi(argv[2]); }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

    SDL_Window *win = SDL_CreateWindow("OpenUG2 Frontend Preview",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "Window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl)  { fprintf(stderr, "GL ctx: %s\n", SDL_GetError()); return 1; }

    Fe fe;
    fe_init(&fe);
    FeMenuEntry entries[] = {
        {"FREE ROAM",    FE_ACTION_FREE_ROAM,   1},
        {"RACE SELECT",  FE_ACTION_RACE_SELECT,  1},
        {"QUIT",         FE_ACTION_QUIT,          1},
    };
    fe_set_entries(&fe, entries, 3);

    FeDraw *draw = fed_init();
    if (!draw) { fprintf(stderr, "fed_init failed\n"); return 1; }

    int running = 1;
    Uint32 last = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                switch (ev.key.keysym.sym) {
                case SDLK_UP:     fe_input(&fe, FE_INPUT_UP);      break;
                case SDLK_DOWN:   fe_input(&fe, FE_INPUT_DOWN);    break;
                case SDLK_RETURN: fe_input(&fe, FE_INPUT_CONFIRM); break;
                case SDLK_ESCAPE: fe_input(&fe, FE_INPUT_BACK);    break;
                default: break;
                }
            }
        }

        Uint32 now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        last = now;

        /* held-key repeat from keyboard state */
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        int dir = 0;
        if (ks[SDL_SCANCODE_DOWN])      dir = +1;
        else if (ks[SDL_SCANCODE_UP])   dir = -1;
        fe_held(&fe, dir, dt);
        fe_update(&fe, dt);

        FeAction act = fe_poll_action(&fe);
        if (act == FE_ACTION_QUIT)        { running = 0; printf("→ QUIT\n"); }
        else if (act == FE_ACTION_FREE_ROAM)   printf("→ FREE ROAM\n");
        else if (act == FE_ACTION_RACE_SELECT) printf("→ RACE SELECT\n");

        SDL_GetWindowSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        fed_draw(draw, &fe, w, h);
        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }

    fed_free(draw);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
