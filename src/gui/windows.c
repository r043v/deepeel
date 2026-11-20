// windows.c: the SDL2 windows of the explorer: the framebuffer watch (--watch) and the
// live ROM map (--mapwin). Both read published/live data only and never drive exploration.
#include "fbwatch.h"
#include "kmapwin.h"
#include "font5x7.h"
#include "../common.h"
#include "../explore/core.h"
#include "../explore/md_ref_feed.h"
#include "../explore/ref_feed.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef DP_HAS_GUI
#include <SDL.h>

static void set_color(SDL_Renderer *ren, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
}

static void draw_text(SDL_Renderer *ren, int x, int y, const char *s, int scale, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    while (*s) {
        char ch = *s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *gl = dp_font5x7[ch - 0x20];
        SDL_Rect rs[35]; int nr = 0;   // one draw call per glyph instead of one per lit pixel
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 7; cy++)
                if (gl[cx] & (1 << cy)) rs[nr++] = (SDL_Rect){ x + cx * scale, y + cy * scale, scale, scale };
        if (nr) SDL_RenderFillRects(ren, rs, nr);
        x += 6 * scale;
        s++;
    }
}

enum { DP_C_TITLE, DP_C_TEXT, DP_C_DIM, DP_C_LABEL, DP_C_WARN };
static SDL_Color gui_color(int id) {
    switch (id) {
    case DP_C_TITLE: return (SDL_Color){ 140, 230, 120 };
    case DP_C_TEXT: return (SDL_Color){ 205, 205, 210 };
    case DP_C_DIM: return (SDL_Color){ 150, 150, 158 };
    case DP_C_LABEL: return (SDL_Color){ 150, 200, 255 };
    default: return (SDL_Color){ 230, 210, 120 };
    }
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

static double watch_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// --- framebuffer watch window (NES/PCE/MD --watch): shows the latest published frame ---
static pthread_t fb_th;
static int fb_running;
static atomic_int fb_quit;
static char fb_title[64];

static void *fb_main(void *arg) {
    (void)arg;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "deepeel: watch: SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_Window *win = SDL_CreateWindow(fb_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;   // vsync: presents land on the display's refresh
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!win || !ren) {
        fprintf(stderr, "deepeel: watch: %s\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return NULL;
    }
    static uint32_t px[DP_FB_MAX_W * DP_FB_MAX_H];
    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    enum { FB_CAP = 30, FB_INFO = 16 };   // caption height under the image (two text lines); the info line at the bottom
    // per-tile speed meters (the GB watch maths): a 1 s window over the frame counter
    static uint32_t rx[3][DP_MDRF_MAX_W * DP_MDRF_MAX_H];
    static double t_main, t_ref[3];
    static uint64_t f_main, f_cur;
    static uint32_t f_ref[3];
    static double s_main, s_ref[3], u_main, w_prev;
    char last_title[260] = "";
    int sized_key = 0;
    while (!atomic_load(&fb_quit)) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) atomic_store(&fb_quit, 1);
        int W = 640, H = 480;
        SDL_GetRendererOutputSize(ren, &W, &H);
        int w, h;
        uint64_t f;
        if (dp_fb_read(px, DP_FB_MAX_W * DP_FB_MAX_H, &w, &h, &f)) {
            f_cur = f;
            if (getenv("DP_WATCH_JITTER")) {   // per window present: how many frames the tile advanced (smooth = steady), once a second
                static uint64_t pf; static double pt; static int mn = 1 << 30, mx, np;
                double jn = watch_now();
                if (pf) { int d = (int)(f - pf); if (d < mn) mn = d; if (d > mx) mx = d; np++; }
                pf = f;
                if (jn - pt >= 1.0) { fprintf(stderr, "jitter: %d presents/s, advance per present min %d max %d\n", np, mn, mx); pt = jn; mn = 1 << 30; mx = 0; np = 0; }
            }
            if (!tex || w != tw || h != th) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                tw = w; th = h;
            }
            if (tex) SDL_UpdateTexture(tex, NULL, px, w * 4);
            double now = watch_now();
            if (now - t_main >= 1.0) {
                double wt = dp_fb_sync_waited();
                if (f >= f_main) {
                    s_main = (double)(f - f_main) / (now - t_main);
                    double busy = (now - t_main) - (wt - w_prev);
                    u_main = busy > 0.02 ? (double)(f - f_main) / busy : s_main;
                }
                w_prev = wt;
                t_main = now; f_main = f;
            }
        }
        int ract[3], rhave[3] = {0}, rw[3] = {0}, rh[3] = {0}, rdone[3] = {0}, ref = 0;
        uint32_t rvf[3] = {0}, rtot[3] = {0};
        for (int k = 0; k < 3; k++) {
            ract[k] = dp_fb_ref_active(k);
            if (!ract[k]) continue;
            ref++;
            static int kw[3], kh[3]; static uint32_t kv[3], kt[3]; static int kd[3];   // last good frame: a torn read keeps showing it instead of blanking the tile
            rhave[k] = dp_fb_ref_read(k, rx[k], &rw[k], &rh[k], &rvf[k], &rtot[k], &rdone[k]);
            if (rhave[k]) { kw[k] = rw[k]; kh[k] = rh[k]; kv[k] = rvf[k]; kt[k] = rtot[k]; kd[k] = rdone[k]; }
            else if (kw[k] > 0) { rw[k] = kw[k]; rh[k] = kh[k]; rvf[k] = kv[k]; rtot[k] = kt[k]; rdone[k] = kd[k]; }
            double now = watch_now();
            if (rhave[k] && now - t_ref[k] >= 1.0) {
                if (rvf[k] >= f_ref[k]) s_ref[k] = (double)(rvf[k] - f_ref[k]) / (now - t_ref[k]);
                t_ref[k] = now; f_ref[k] = rvf[k];
            }
            if (rdone[k]) s_ref[k] = 0;
        }
        SDL_SetRenderDrawColor(ren, 24, 24, 28, 255);   // dark ground: the captions are light
        SDL_RenderClear(ren);
        {   // two tiles side by side, each letterboxed to its image aspect (the
            // GB watch layout), with two caption lines under the image
            int ncell = 1 + ref, cols = ncell > 2 ? 2 : ncell, rows = (ncell + cols - 1) / cols;   // two tiles per row
            if (tex && tw > 0 && th > 0 && sized_key != ncell * 100000 + tw * 10 + th % 10) {   // fit the window to the tiles (2x), once per layout
                sized_key = ncell * 100000 + tw * 10 + th % 10;
                SDL_Rect ub; int mag = 2, winw, winh;   // windowed, never larger than 90% of the usable desktop
                if (SDL_GetDisplayUsableBounds(0, &ub) != 0) { ub.w = 1280; ub.h = 800; }
                for (; mag > 1; mag--) if (cols * tw * mag <= ub.w * 9 / 10 && rows * (th * mag + FB_CAP) + FB_INFO <= ub.h * 9 / 10) break;
                winw = cols * tw * mag; winh = rows * (th * mag + FB_CAP) + FB_INFO;
                SDL_SetWindowFullscreen(win, 0);
                SDL_RestoreWindow(win);
                SDL_SetWindowSize(win, winw, winh);
            }
            int cw = W / cols, rowh = (H - FB_INFO) / rows, ch = rowh - FB_CAP > 0 ? rowh - FB_CAP : 1;
            int slot[4] = { -1, -1, -1, -1 }, ns = 1;   // tile 0: our core, then every active reference
            for (int k = 0; k < 3; k++) if (ract[k]) slot[ns++] = k;
            // one scale for every tile (the smallest that fits them all): equal pixels are drawn equally big, so a
            // tile taller or wider than another shows a real difference of the image, not of the fitting
            double sc_all = 1e9; int sizes_key = 0;
            for (int t = 0; t < ncell; t++) {
                int k = slot[t], iw = t == 0 ? (tex ? tw : 0) : (k >= 0 ? rw[k] : 0), ih = t == 0 ? (tex ? th : 0) : (k >= 0 ? rh[k] : 0);
                if (iw <= 0 || ih <= 0) continue;
                double s1 = (double)cw / iw < (double)ch / ih ? (double)cw / iw : (double)ch / ih;
                if (s1 < sc_all) sc_all = s1;
                sizes_key = sizes_key * 31 + iw * 1000 + ih;
            }
            { static int last_sizes_key; if (sizes_key != last_sizes_key) { last_sizes_key = sizes_key;
                fprintf(stderr, "watch: tile sizes ours %dx%d", tw, th);
                for (int k = 0; k < 3; k++) if (ract[k]) fprintf(stderr, ", ref%d %dx%d", k, rw[k], rh[k]);
                fputc('\n', stderr); } }
            for (int t = 0; t < ncell; t++) {
                int ox = (t % cols) * cw, oy = (t / cols) * rowh;
                int iw = 0, ih = 0, k = slot[t];
                if (t == 0) { if (tex && th > 0) { iw = tw; ih = th; } }
                else if (rh[k] > 0) { iw = rw[k]; ih = rh[k]; }
                if (!iw || !ih) continue;
                double sc = sc_all;
                int dw = (int)(iw * sc), dh = (int)(ih * sc);
                SDL_Rect dst = { ox + (cw - dw) / 2, oy + (ch - dh) / 2, dw, dh };
                {   // one path for every tile, ours included: a texture built from the tile's pixel buffer
                    const uint32_t *src = t == 0 ? px : rx[k];
                    static SDL_Texture *tt[4]; static int ttw[4], tth[4];   // one texture per tile, rebuilt only when its size changes
                    if (tt[t] && (ttw[t] != iw || tth[t] != ih)) { SDL_DestroyTexture(tt[t]); tt[t] = NULL; }
                    if (!tt[t]) { tt[t] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, iw, ih); ttw[t] = iw; tth[t] = ih; }
                    if (tt[t]) {
                        SDL_UpdateTexture(tt[t], NULL, src, iw * 4);
                        SDL_RenderCopy(ren, tt[t], NULL, &dst);
                    }
                }
            }
            // captions: the emulator's name first, then its progress and speed; the movie goes in the window title
            char c1[160], c2[160];
            int done = dp_fb_caption_read(c1, sizeof c1, c2, sizeof c2);
            const char *own = getenv("DEEPEEL_MD_OWN_NAME");
            char wt[300];
            char plat[16] = "md";   // "deepeel <platform> watch" -> the platform
            sscanf(fb_title, "deepeel %15s", plat);
            snprintf(wt, sizeof wt, "deepeel %s watch - %s - %s - %.0f f/s", plat, c1[0] ? c1 : "no movie", c2, s_main);
            if (strcmp(wt, last_title)) { SDL_SetWindowTitle(win, wt); snprintf(last_title, sizeof last_title, "%s", wt); }
            if (own && *own) snprintf(c1, sizeof c1, "%s", own);
            else snprintf(c1, sizeof c1, "deepeel-%s " DEEPEEL_VERSION, plat);
            draw_text(ren, 6, ch + 2, c1, 1, gui_color(DP_C_TITLE));   // tile 0 sits at the origin
            char l2[96]; snprintf(l2, sizeof l2, "frame %llu  %.0f f/s", (unsigned long long)f_cur, s_main);   // the rate actually shown (lockstep: the pace of the slowest core), not the unpaced speed
            draw_text(ren, 6, ch + 16, l2, 1, gui_color(done ? DP_C_DIM : DP_C_TEXT));
            for (int t = 1; t < ncell; t++) {
                int k = slot[t], ox = (t % cols) * cw, oy = (t / cols) * rowh;
                char r2[96];
                static double rr_t[3], rr_v[3], rr_f[3]; double rn = watch_now();   // measured advance of this reference per wall second
                if (rn - rr_t[k] >= 1.0) { if (rr_t[k] > 0 && rvf[k] >= rr_f[k]) rr_v[k] = (rvf[k] - rr_f[k]) / (rn - rr_t[k]); rr_t[k] = rn; rr_f[k] = rvf[k]; }
                snprintf(r2, sizeof r2, "frame %u  %.0f f/s", (unsigned)rvf[k], rr_v[k]);
                draw_text(ren, ox + 6, oy + ch + 2, dp_fb_ref_name(k), 1, gui_color(DP_C_TITLE));
                draw_text(ren, ox + 6, oy + ch + 16, r2, 1, gui_color(rdone[k] ? DP_C_DIM : DP_C_TEXT));
            }
        }
        {   // info line: what the window is, how to leave it
            char inf[200]; snprintf(inf, sizeof inf, "windowed - Esc closes this movie - tiles share one scale - cycle counts are tool observations");
            draw_text(ren, 6, H - FB_INFO + 3, inf, 1, gui_color(DP_C_DIM));
        }
        SDL_RenderPresent(ren);
        SDL_Delay(2);   // the vsynced present paces the loop (a software renderer without vsync: the delay keeps it from spinning)
    }
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return NULL;
}
int dp_fbwatch_available(void) { return 1; }
int dp_fbwatch_start(const char *title) {
    if (fb_running) return 0;
    snprintf(fb_title, sizeof fb_title, "%s", title ? title : "deepeel watch");
    atomic_store(&fb_quit, 0);
    if (pthread_create(&fb_th, NULL, fb_main, NULL) != 0) return 1;
    fb_running = 1;
    return 0;
}
void dp_fbwatch_stop(void) {
    if (!fb_running) return;
    atomic_store(&fb_quit, 1);
    pthread_join(fb_th, NULL);
    fb_running = 0;
}
// ---- kernel ROM map window: reads the live DpCoreResult, no legacy structure involved
static pthread_t kw_th;
static int kw_running;
static atomic_int kw_quit;
static const DpCoreResult *kw_res;
static int kw_scale;
static char kw_title[64];

static uint32_t kw_color(uint8_t f) {
    if (f & DP_CORE_NATURAL) return 0xff30c050u;
    if (f & DP_CORE_FORCED) return 0xffe09030u;
    if (f & DP_CORE_STATIC) return 0xff4070e0u;
    if (f & DP_CORE_DATA) return 0xffd0d040u;
    return 0xff202028u;
}
static void *kw_main(void *arg) {
    (void)arg;
    enum { TILE = 128, GAP = 4, LEG_H = 30, TILE_BYTES = TILE * TILE };
    const DpCoreResult *res = kw_res;
    size_t size = res->rom_size;
    int tiles = (int)((size + TILE_BYTES - 1) / TILE_BYTES);
    if (tiles < 1) return NULL;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "deepeel: mapwin: SDL_Init: %s\n", SDL_GetError()); return NULL; }
    int maxW = 1280 - 40, maxH = 800 - 80;
    SDL_Rect ub;
    if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.w > 0 && ub.h > 0) { maxW = ub.w - 40; maxH = ub.h - 80; }
    int per_row = 8, sc = 1, found = 0, ceil_sc = kw_scale >= 1 ? kw_scale : 4;
    for (int k = ceil_sc; k >= 1 && !found; k--)
        for (int t = 0; t < tiles && !found; t++) {
            int pr = t < 8 ? 8 - t : t + 1;
            if (pr > tiles) continue;
            int rws = (tiles + pr - 1) / pr;
            int w = GAP + pr * (TILE * k + GAP), h = GAP + rws * (TILE * k + GAP) + LEG_H;
            if (w <= maxW && h <= maxH) { found = 1; per_row = pr; sc = k; }
        }
    if (!found) { per_row = tiles < 8 ? tiles : 8; sc = 1; }
    int cols = tiles < per_row ? tiles : per_row, rows = (tiles + per_row - 1) / per_row;
    int W = GAP + cols * (TILE * sc + GAP), H = GAP + rows * (TILE * sc + GAP) + LEG_H;
    SDL_Window *win = SDL_CreateWindow(kw_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H - LEG_H) : NULL;
    uint32_t *px = tex ? malloc((size_t)W * (H - LEG_H) * 4) : NULL;
    if (!px) {
        fprintf(stderr, "deepeel: mapwin: %s\n", SDL_GetError());
        if (tex) SDL_DestroyTexture(tex);
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return NULL;
    }
    for (size_t i = 0; i < (size_t)W * (H - LEG_H); i++) px[i] = 0xff18181c;
    for (int first = 1; first || !atomic_load(&kw_quit); first = 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))) atomic_store(&kw_quit, 1);
        size_t cnt[4] = { 0, 0, 0, 0 };   // natural, forced, static, data
        for (size_t gi = 0; gi < size; gi++) {
            uint8_t f = __atomic_load_n(&res->flags[gi], __ATOMIC_RELAXED);
            cnt[0] += !!(f & DP_CORE_NATURAL); cnt[1] += !!(!(f & DP_CORE_NATURAL) && (f & DP_CORE_FORCED));
            cnt[2] += !!(!(f & (DP_CORE_NATURAL | DP_CORE_FORCED)) && (f & DP_CORE_STATIC));
            cnt[3] += !!(!(f & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC)) && (f & DP_CORE_DATA));
            int t = (int)(gi / TILE_BYTES); size_t g = gi % TILE_BYTES;
            int x = GAP + (t % per_row) * (TILE * sc + GAP) + (int)(g % TILE) * sc, y = GAP + (t / per_row) * (TILE * sc + GAP) + (int)(g / TILE) * sc;
            uint32_t v = kw_color(f);
            for (int dy = 0; dy < sc; dy++)
                for (int dx = 0; dx < sc; dx++) px[(size_t)(y + dy) * W + x + dx] = v;
        }
        const char *dump = getenv("DEEPEEL_MAPWIN_DUMP");   // test hook: the last drawn map as a binary PPM
        if (dump) {
            FILE *f = fopen(dump, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", W, H - LEG_H);
                for (size_t i = 0; i < (size_t)W * (H - LEG_H); i++) { uint8_t t3[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(t3, 1, 3, f); }
                fclose(f);
            }
        }
        SDL_UpdateTexture(tex, NULL, px, W * 4);
        SDL_SetRenderDrawColor(ren, 0x18, 0x18, 0x1c, 255);
        SDL_RenderClear(ren);
        SDL_Rect dst = { 0, 0, W, H - LEG_H };
        SDL_RenderCopy(ren, tex, NULL, &dst);
        char line[160];
        snprintf(line, sizeof line, "natural %.1f%%  forced %.1f%%  static %.1f%%  data %.1f%%", size ? 100.0 * (double)cnt[0] / (double)size : 0.0,
                 size ? 100.0 * (double)cnt[1] / (double)size : 0.0, size ? 100.0 * (double)cnt[2] / (double)size : 0.0, size ? 100.0 * (double)cnt[3] / (double)size : 0.0);
        SDL_Color white = { 0xe0, 0xe0, 0xe0, 255 };
        draw_text(ren, 6, H - LEG_H + 4, line, 1, white);
        SDL_RenderPresent(ren);
        for (int i = 0; i < 12 && !atomic_load(&kw_quit); i++) {   // ~0.5 s between redraws, responsive to events
            SDL_Event e2;
            while (SDL_PollEvent(&e2))
                if (e2.type == SDL_QUIT || (e2.type == SDL_KEYDOWN && (e2.key.keysym.sym == SDLK_ESCAPE || e2.key.keysym.sym == SDLK_q))) atomic_store(&kw_quit, 1);
            SDL_Delay(20);
        }
    }
    free(px);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return NULL;
}
int dp_kmapwin_start(const struct DpCoreResult *live, const char *title, int scale) {
    if (kw_running) return 0;
    kw_res = live; kw_scale = scale;
    snprintf(kw_title, sizeof kw_title, "%s", title ? title : "deepeel ROM map");
    atomic_store(&kw_quit, 0);
    if (pthread_create(&kw_th, NULL, kw_main, NULL) != 0) return 1;
    kw_running = 1;
    return 0;
}
void dp_kmapwin_stop(void) {
    if (!kw_running) return;
    atomic_store(&kw_quit, 1);
    pthread_join(kw_th, NULL);
    kw_running = 0;
}
/* watch sound: one queued SDL device, opened on the first frame; the presenter's 60 fps clock paces it */
static SDL_AudioDeviceID au_dev; static int au_tried;
void dp_audio_play(const int16_t *st, unsigned frames) {
    if (!au_tried) {
        au_tried = 1;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want; SDL_zero(want); want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
            au_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
            if (au_dev) SDL_PauseAudioDevice(au_dev, 0); else fprintf(stderr, "deepeel: watch: no audio device: %s\n", SDL_GetError());
        }
    }
    if (!au_dev || !frames) return;
    if (SDL_GetQueuedAudioSize(au_dev) > 44100u / 6u * 4u) return;   /* more than ~170 ms queued: drop, the latency must not grow */
    SDL_QueueAudio(au_dev, st, frames * 4u);
}
#else   // built without SDL2: the CLI reports it
void dp_audio_play(const int16_t *stereo, unsigned frames) { (void)stereo; (void)frames; }
int dp_fbwatch_available(void) { return 0; }
int dp_fbwatch_start(const char *title) {
    (void)title;
    fprintf(stderr, "deepeel: built without SDL2, the watch window is unavailable (--watch-dump still works)\n");
    return 1;
}
void dp_fbwatch_stop(void) {}
int dp_kmapwin_start(const struct DpCoreResult *live, const char *title, int scale) {
    (void)live; (void)title; (void)scale;
    fprintf(stderr, "deepeel: built without SDL2, --mapwin unavailable\n");
    return 1;
}
void dp_kmapwin_stop(void) {}
#endif
