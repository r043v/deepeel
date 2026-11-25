#include "livewin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef DP_HAS_GUI
#include <SDL.h>
static SDL_Window *win; static SDL_Renderer *ren; static SDL_Texture *tex; static int tw, th, dead, inited;
int dp_livewin_show(const DpCanvas *c, const char *title) {
    const char *dump = getenv("DEEPEEL_LIVE_DUMP"); if (dump && *dump) dp_canvas_save_png(c, dump);   // control output: what the window shows
    if (dead) return 1;
    if (!inited) {
        inited = 1;
        const char *d = getenv("DISPLAY"), *w = getenv("WAYLAND_DISPLAY");
        if ((!d || !*d) && (!w || !*w)) { dead = 1; return 1; }
        // X11 (XWayland) first: the compositor then draws a normal title bar and border (move, resize), with no libdecor plugin
        if (d && *d) SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
        SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");   // libdecor's GTK plugin pulls libglycin, which segfaults in its worker thread
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // the supervisor owns SIGINT/SIGTERM
        if (SDL_Init(SDL_INIT_VIDEO) != 0 && (SDL_SetHint(SDL_HINT_VIDEODRIVER, ""), SDL_Init(SDL_INIT_VIDEO) != 0)) { fprintf(stderr, "deepeel: live window unavailable: %s\n", SDL_GetError()); dead = 1; return 1; }
    }
    if (!win || tw != c->w || th != c->h) {
        if (tex) SDL_DestroyTexture(tex); if (ren) SDL_DestroyRenderer(ren); if (win) SDL_DestroyWindow(win);
        win = SDL_CreateWindow(title ? title : "deepeel", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, c->w, c->h, SDL_WINDOW_RESIZABLE);
        ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
        tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, c->w, c->h) : NULL;
        if (!tex) { fprintf(stderr, "deepeel: live window unavailable: %s\n", SDL_GetError()); dead = 1; return 1; }
        SDL_RenderSetLogicalSize(ren, c->w, c->h);   // resizing keeps the aspect ratio (letterbox)
        tw = c->w; th = c->h;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))) { dp_livewin_close(); dead = 1; return 1; }
    SDL_UpdateTexture(tex, NULL, c->px, c->w * 4);
    SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, NULL, NULL); SDL_RenderPresent(ren);
    return 0;
}
void dp_livewin_close(void) {
    if (tex) SDL_DestroyTexture(tex); if (ren) SDL_DestroyRenderer(ren); if (win) SDL_DestroyWindow(win);
    tex = NULL; ren = NULL; win = NULL;
}
#else
int dp_livewin_show(const DpCanvas *c, const char *title) { (void)c; (void)title; return 1; }
void dp_livewin_close(void) {}
#endif
