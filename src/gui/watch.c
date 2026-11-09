#include "watch.h"
#include "font5x7.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#define DP_WATCH_HAVE_SDL 1
#endif
#endif

#ifdef DP_WATCH_HAVE_SDL

enum { DP_WATCH_PAD = 8, DP_WATCH_CAP = 20 };  /* gaps; caption band: two text lines */
#define DP_WATCH_TEXT_SC 1
#define DP_WATCH_CHAR_W (6 * DP_WATCH_TEXT_SC)

struct DpWatch {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex[2];
    int ntiles;
    int bw[2], bh[2];            /* native buffer sizes (the platform's screen) */
    int sc;                      /* global display scale: 2 on small screens */
    uint16_t *fb[2];             /* latest frame of each tile (copied) */
    char l1[2][96], l2[2][96];   /* captions, drawn each present */
    Uint32 t_last;
};

static void draw_text(SDL_Renderer *ren, int x, int y, const char *s, int scale) {
    SDL_SetRenderDrawColor(ren, 205, 205, 210, 255);
    while (*s) {
        char ch = *s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *gl = dp_font5x7[ch - 0x20];
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 7; cy++)
                if (gl[cx] & (1 << cy)) {
                    SDL_Rect r = { x + cx * scale, y + cy * scale, scale, scale };
                    SDL_RenderFillRect(ren, &r);
                }
        x += DP_WATCH_CHAR_W;
        s++;
    }
}

/* one caption line clipped to its tile: a too-wide name is elided in the
 * middle (the head and the tail/extension stay visible) */
static void draw_caption(SDL_Renderer *ren, int x, int y, const char *s, int max_px) {
    const int maxc = max_px / DP_WATCH_CHAR_W;
    char buf[96];
    size_t len = strlen(s);
    if ((int)len > maxc && maxc > 4) {
        int keep = maxc - 2;                       /* 2 for the ".." ellipsis */
        int head = keep * 2 / 3, tail = keep - head;
        snprintf(buf, sizeof buf, "%.*s..%s", head, s, s + len - tail);
        s = buf;
    }
    draw_text(ren, x, y, s, DP_WATCH_TEXT_SC);
}

DpWatch *dp_watch_open(const char *title, int ntiles, const int *w, const int *h) {
    if (SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "watch: SDL init failed: %s\n", SDL_GetError());
        return NULL;
    }
    DpWatch *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->ntiles = ntiles > 2 ? 2 : (ntiles < 1 ? 1 : ntiles);
    /* small screens at x2 (the console panels are tiny on a desktop monitor);
     * anything bigger keeps 1:1 */
    k->sc = 1;
    for (int i = 0; i < k->ntiles; i++) {
        k->bw[i] = w[i]; k->bh[i] = h[i];
        if (w[i] * 2 <= 520 && h[i] * 2 <= 520) k->sc = 2;
    }
    int tw = 0, th = 0;
    for (int i = 0; i < k->ntiles; i++) {
        if (k->bw[i] * k->sc > tw) tw = k->bw[i] * k->sc;
        if (k->bh[i] * k->sc > th) th = k->bh[i] * k->sc;
    }
    const int win_w = k->ntiles * tw + (k->ntiles + 1) * DP_WATCH_PAD;
    const int win_h = th + DP_WATCH_CAP + 2 * DP_WATCH_PAD;
    k->win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              win_w, win_h, 0);
    if (!k->win) { fprintf(stderr, "watch: no window: %s\n", SDL_GetError()); free(k); return NULL; }
    k->ren = SDL_CreateRenderer(k->win, -1, 0);
    for (int i = 0; i < k->ntiles; i++) {
        k->tex[i] = SDL_CreateTexture(k->ren, SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING, k->bw[i], k->bh[i]);
        k->fb[i] = malloc((size_t)k->bw[i] * k->bh[i] * sizeof **k->fb);
    }
    k->t_last = SDL_GetTicks();
    fprintf(stderr, "watch: window %dx%d (buffers %dx%d x%d, scale %d)\n",
            win_w, win_h, k->bw[0], k->bh[0], k->ntiles, k->sc);
    return k;
}

void dp_watch_tile(DpWatch *k, int which, const uint16_t *fb565, int stride) {
    if (!k || which < 0 || which >= k->ntiles || !fb565) return;
    for (int y = 0; y < k->bh[which]; y++)
        memcpy(k->fb[which] + (size_t)y * k->bw[which],
               fb565 + (size_t)y * stride, (size_t)k->bw[which] * sizeof *fb565);
}

void dp_watch_caption(DpWatch *k, int which, const char *line1, const char *line2) {
    if (!k || which < 0 || which >= k->ntiles) return;
    snprintf(k->l1[which], sizeof k->l1[which], "%s", line1 ? line1 : "");
    snprintf(k->l2[which], sizeof k->l2[which], "%s", line2 ? line2 : "");
}

void dp_watch_frame(DpWatch *k, int *stop) {
    if (!k) return;
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT ||
            (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
            fprintf(stderr, "watch: window closed, stopping\n");
            *stop = 1;
        }
    SDL_SetRenderDrawColor(k->ren, 32, 32, 32, 255);
    SDL_RenderClear(k->ren);
    int tw = 0, th = 0;
    for (int i = 0; i < k->ntiles; i++) {
        if (k->bw[i] * k->sc > tw) tw = k->bw[i] * k->sc;
        if (k->bh[i] * k->sc > th) th = k->bh[i] * k->sc;
    }
    for (int i = 0; i < k->ntiles; i++) {
        /* aspect-fit the native buffer in its cell: the ratio is always the
         * platform's own, never a stretch */
        SDL_Rect dst = { DP_WATCH_PAD + i * (tw + DP_WATCH_PAD), DP_WATCH_PAD,
                         k->bw[i] * k->sc, k->bh[i] * k->sc };
        SDL_UpdateTexture(k->tex[i], NULL, k->fb[i], k->bw[i] * sizeof(uint16_t));
        SDL_RenderCopy(k->ren, k->tex[i], NULL, &dst);
        draw_caption(k->ren, dst.x, th + DP_WATCH_PAD + 2, k->l1[i], tw);
        draw_caption(k->ren, dst.x, th + DP_WATCH_PAD + 2 + 10, k->l2[i], tw);
    }
    SDL_RenderPresent(k->ren);
    Uint32 spent = SDL_GetTicks() - k->t_last;   /* ~60 f/s pacing */
    k->t_last = SDL_GetTicks();
    if (spent < 16) SDL_Delay(16 - spent);
}

void dp_watch_close(DpWatch *k) {
    if (!k) return;
    for (int i = 0; i < k->ntiles; i++) {
        if (k->tex[i]) SDL_DestroyTexture(k->tex[i]);
        free(k->fb[i]);
    }
    if (k->ren) SDL_DestroyRenderer(k->ren);
    if (k->win) SDL_DestroyWindow(k->win);
    SDL_Quit();
    free(k);
}

#else  /* headless bench: the same entry points, all no-ops */

struct DpWatch { int unused; };

DpWatch *dp_watch_open(const char *title, int ntiles, const int *w, const int *h) {
    (void)title; (void)ntiles; (void)w; (void)h;
    return NULL;
}
void dp_watch_tile(DpWatch *k, int which, const uint16_t *fb565, int stride) {
    (void)k; (void)which; (void)fb565; (void)stride;
}
void dp_watch_caption(DpWatch *k, int which, const char *line1, const char *line2) {
    (void)k; (void)which; (void)line1; (void)line2;
}
void dp_watch_frame(DpWatch *k, int *stop) { (void)k; (void)stop; }
void dp_watch_close(DpWatch *k) { (void)k; }

#endif
