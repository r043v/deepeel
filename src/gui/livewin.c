#include "livewin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef DP_HAS_GUI
#include <SDL.h>
#include <unistd.h>
static char decor_dir[64];
// a private plugin directory holding only libdecor-cairo (LIBDECOR_PLUGIN_DIR); 1 when it is in place
static int cairo_decor_only(void) {
    if (getenv("LIBDECOR_PLUGIN_DIR")) return 1;   // the user's choice
    static const char *const cand[] = { "/usr/lib/libdecor/plugins-1/libdecor-cairo.so", "/usr/lib64/libdecor/plugins-1/libdecor-cairo.so",
        "/usr/lib/x86_64-linux-gnu/libdecor/plugins-1/libdecor-cairo.so", "/usr/lib/aarch64-linux-gnu/libdecor/plugins-1/libdecor-cairo.so" };
    for (size_t i = 0; i < sizeof cand / sizeof *cand; i++) {
        if (access(cand[i], R_OK)) continue;
        snprintf(decor_dir, sizeof decor_dir, "/dev/shm/deepeel-decor-XXXXXX");
        if (!mkdtemp(decor_dir)) return 0;
        char link[128]; snprintf(link, sizeof link, "%s/libdecor-cairo.so", decor_dir);
        if (symlink(cand[i], link)) { rmdir(decor_dir); decor_dir[0] = 0; return 0; }
        setenv("LIBDECOR_PLUGIN_DIR", decor_dir, 1);
        return 1;
    }
    return 0;
}
static SDL_Window *win; static SDL_Renderer *ren; static SDL_Texture *tex; static int tw, th, dead, inited, sized;   // sized: the size is the user's
static int video(void) {   // SDL started on first use; 0: no window to be had
    if (dead) return 0;
    if (inited) return 1;
    inited = 1;
    const char *d = getenv("DISPLAY"), *w = getenv("WAYLAND_DISPLAY");
    if ((!d || !*d) && (!w || !*w)) { dead = 1; return 0; }
    // same minimal decoration as the --watch window: libdecor with its cairo plugin only. Its GTK plugin (picked when SDL runs
    // on the main thread) pulls libglycin, which segfaults. Without that plugin: X11, where the window manager draws the frame.
    if (w && *w && !cairo_decor_only()) { if (d && *d) SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11"); SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0"); }
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // the supervisor owns SIGINT/SIGTERM
    if (SDL_Init(SDL_INIT_VIDEO) != 0 && (SDL_SetHint(SDL_HINT_VIDEODRIVER, ""), SDL_Init(SDL_INIT_VIDEO) != 0)) { fprintf(stderr, "deepeel: live window unavailable: %s\n", SDL_GetError()); dead = 1; return 0; }
    return 1;
}
int dp_livewin_area(int *w, int *h) {
    enum { W = 1900, H = 1000, BARS = 90 };   // without a display; BARS: the desktop's top bar and the window's title bar
    *w = W; *h = H;
    if (!video()) return 0;
    if (win) {
        int cw, ch; SDL_GetWindowSize(win, &cw, &ch);
        if (cw != tw || ch != th || (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)) sized = 1;
        if (sized) { *w = cw; *h = ch; return 1; }
    }
    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(win ? SDL_GetWindowDisplayIndex(win) : 0, &r) && r.w > 320 && r.h > 320) { *w = r.w - 20; *h = r.h - BARS; }
    return 0;
}
int dp_livewin_show(const DpCanvas *c, const char *title) {
    const char *dump = getenv("DEEPEEL_LIVE_DUMP"); static time_t dumped; if (dump && *dump && time(NULL) != dumped) { dumped = time(NULL); char tmp[4096]; snprintf(tmp, sizeof tmp, "%s.tmp", dump); if (dp_canvas_save_png(c, tmp)) rename(tmp, dump); }   /* whole file or the previous one */   // control output: what the window shows, once a second
    if (!video()) return 1;
    if (!win || tw != c->w || th != c->h) {   // a new layout resizes the same window: only the texture is made again
        if (tex) SDL_DestroyTexture(tex);
        if (!win) {
            win = SDL_CreateWindow(title ? title : "deepeel", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, c->w, c->h, SDL_WINDOW_RESIZABLE);
            ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
        } else {
            int cw, ch; SDL_GetWindowSize(win, &cw, &ch);
            if (cw != c->w || ch != c->h) SDL_SetWindowSize(win, c->w, c->h);
        }
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
    if (decor_dir[0]) { char link[128]; snprintf(link, sizeof link, "%s/libdecor-cairo.so", decor_dir); unlink(link); rmdir(decor_dir); decor_dir[0] = 0; unsetenv("LIBDECOR_PLUGIN_DIR"); }
}
#else
int dp_livewin_show(const DpCanvas *c, const char *title) { (void)c; (void)title; return 1; }
int dp_livewin_area(int *w, int *h) { *w = 1900; *h = 1000; return 0; }
void dp_livewin_close(void) {}
#endif
