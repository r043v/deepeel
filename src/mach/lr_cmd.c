// deepeel lr explore CORE ROM [--text FILE] [--map FILE] [--png FILE] [--path-steps N] [--loop-bound N] [--max-total N] [--queue N]
// Forking exploration of a level-B libretro core (see libretro.h). Level-A
// cores cannot fork: the command says so and exits non-zero.
#include "mach/libretro.h"
#include "common.h"
#include "report/report.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void lr_pf(DpBuf *b, const char *fmt, ...) {
    char t[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0) dp_buf_push(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}

// deepeel lr play CORE ROM [--frames N] [--tas FILE] [--ppm FILE]: a session on any (level A) core:
// N frames (inputs from a TAS text file, one hex mask per line, else released), a save/replay/load/replay
// determinism check, the state and RAM hashes, an optional framebuffer dump. No exploration.
static int lr_play(int argc, char **argv) {
    const char *core = argv[3], *rom = argv[4], *tas = NULL, *ppm = NULL;
    unsigned long frames = 120;
    for (int i = 5; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--frames")) frames = strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--tas")) tas = argv[i + 1];
        else if (!strcmp(argv[i], "--ppm")) ppm = argv[i + 1];
        else { fprintf(stderr, "deepeel lr: unknown option %s\n", argv[i]); return 2; }
    }
    if (!frames || frames > 1000000) frames = 120;
    uint16_t *masks = calloc(frames, sizeof *masks);
    uint64_t *h1 = calloc(frames, sizeof *h1), *h2 = calloc(frames, sizeof *h2);
    if (!masks || !h1 || !h2) return 1;
    if (tas) {
        FILE *t = fopen(tas, "r");
        if (!t) { perror(tas); return 1; }
        char ln[64];
        for (unsigned long i = 0; i < frames && fgets(ln, sizeof ln, t); i++) masks[i] = (uint16_t)strtoul(ln, NULL, 16);
        fclose(t);
    }
    char err[256] = "";
    DpLibretro *lr = dp_lr_open_copy(core, err, sizeof err);
    if (!lr || !dp_lr_load_rom(lr, rom, err, sizeof err)) { fprintf(stderr, "deepeel lr: %s\n", err); return 1; }
    size_t n = dp_lr_state_size(lr);
    void *snap = malloc(n ? n : 1);
    if (!snap || !dp_lr_save(lr, snap, n)) { fprintf(stderr, "deepeel lr: core cannot serialize\n"); return 1; }
    dp_lr_replay(lr, masks, (unsigned)frames, h1);
    uint64_t sh = dp_lr_state_hash(lr), rh = dp_lr_ram_hash(lr);
    dp_lr_load(lr, snap, n);
    dp_lr_replay(lr, masks, (unsigned)frames, h2);
    int same = !memcmp(h1, h2, frames * sizeof *h1) && sh == dp_lr_state_hash(lr);
    printf("deepeel libretro session (tool observation)\ncore: %s\n%s\n", dp_lr_core_name(lr), dp_lr_degradations(lr));
    printf("frames %lu, state %zu bytes, state hash %016llx, ram hash %016llx\n", frames, n,
           (unsigned long long)sh, (unsigned long long)rh);
    printf("replay after load: %s\n", same ? "identical" : "DIFFERENT");
    if (ppm) {
        int w, h, pitch, fmt;
        const uint8_t *fb = dp_lr_framebuffer(lr, &w, &h, &pitch, &fmt);
        FILE *o = fb ? fopen(ppm, "wb") : NULL;
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", w, h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint8_t r, g, b;
                    if (fmt == DP_LR_FMT_XRGB8888) { const uint8_t *q = fb + (size_t)y * pitch + x * 4; b = q[0]; g = q[1]; r = q[2]; }
                    else { uint16_t v = *(const uint16_t *)(fb + (size_t)y * pitch + x * 2);
                           if (fmt == DP_LR_FMT_RGB565) { r = (uint8_t)((v >> 11) << 3); g = (uint8_t)(((v >> 5) & 63) << 2); b = (uint8_t)((v & 31) << 3); }
                           else { r = (uint8_t)(((v >> 10) & 31) << 3); g = (uint8_t)(((v >> 5) & 31) << 3); b = (uint8_t)((v & 31) << 3); } }
                    fputc(r, o); fputc(g, o); fputc(b, o);
                }
            fclose(o);
        } else fprintf(stderr, "deepeel lr: no framebuffer or cannot write %s\n", ppm);
    }
    dp_lr_close(lr);
    free(snap); free(masks); free(h1); free(h2);
    return same ? 0 : 3;
}

// Screen-cell key of the current frame: the framebuffer reduced to a 16x14 grid of 3-bit luma, hashed.
static uint64_t lr_cell(DpLibretro *lr) {
    int w, h, pitch, fmt;
    const uint8_t *fb = dp_lr_framebuffer(lr, &w, &h, &pitch, &fmt);
    uint64_t k = 1469598103934665603ull;
    if (!fb || w < 16 || h < 14) return k;
    for (int cy = 0; cy < 14; cy++)
        for (int cx = 0; cx < 16; cx++) {
            int x = cx * w / 16 + w / 32, y = cy * h / 14 + h / 28, g;
            if (fmt == DP_LR_FMT_XRGB8888) { const uint8_t *q = fb + (size_t)y * pitch + x * 4; g = (q[0] + q[1] * 2 + q[2]) >> 2; }
            else { uint16_t v = *(const uint16_t *)(fb + (size_t)y * pitch + x * 2);
                   g = fmt == DP_LR_FMT_RGB565 ? ((v >> 5) & 63) << 2 : ((v >> 5) & 31) << 3; }
            k = (k ^ (uint64_t)(g >> 5)) * 1099511628211ull;
        }
    return k;
}

typedef struct { void *st; uint64_t cell; } LrCellState;

// deepeel lr novelty CORE ROM [--rounds N] [--seed N] [--hold N]: Go-Explore over screen cells on any
// (level A) core: archive of states keyed by screen cell, random held inputs from a random archived state.
static int lr_novelty(int argc, char **argv) {
    const char *core = argv[3], *rom = argv[4];
    unsigned long rounds = 2000, hold = 8;
    uint64_t rng = 88172645463325252ull;
    for (int i = 5; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--rounds")) rounds = strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--seed")) rng ^= strtoull(argv[i + 1], NULL, 0) * 0x9E3779B97F4A7C15ull;
        else if (!strcmp(argv[i], "--hold")) hold = strtoul(argv[i + 1], NULL, 0);
        else { fprintf(stderr, "deepeel lr: unknown option %s\n", argv[i]); return 2; }
    }
    if (!hold) hold = 8;
    char err[256] = "";
    DpLibretro *lr = dp_lr_open_copy(core, err, sizeof err);
    if (!lr || !dp_lr_load_rom(lr, rom, err, sizeof err)) { fprintf(stderr, "deepeel lr: %s\n", err); return 1; }
    size_t n = dp_lr_state_size(lr);
    size_t cap = 4096, cnt = 0;
    LrCellState *ar = calloc(cap, sizeof *ar);
    void *tmp = malloc(n ? n : 1);
    if (!ar || !tmp || !dp_lr_save(lr, tmp, n)) { fprintf(stderr, "deepeel lr: core cannot serialize\n"); return 1; }
    dp_lr_advance(lr, 1);
    ar[cnt].st = malloc(n); dp_lr_save(lr, ar[cnt].st, n); ar[cnt++].cell = lr_cell(lr);
    unsigned long dropped = 0;
    for (unsigned long r = 0; r < rounds; r++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        LrCellState *from = &ar[rng % cnt];
        dp_lr_load(lr, from->st, n);
        uint16_t mask = (uint16_t)(rng >> 20) & 0x0FFF;
        for (unsigned long f = 0; f < 3; f++) {
            dp_lr_set_input(lr, 0, mask);
            dp_lr_advance(lr, (unsigned)hold);
            uint64_t c = lr_cell(lr);
            size_t i = 0;
            while (i < cnt && ar[i].cell != c) i++;
            if (i == cnt) {
                if (cnt == cap) {
                    if (cap >= 100000) { dropped++; continue; }
                    cap *= 2; ar = realloc(ar, cap * sizeof *ar);
                }
                ar[cnt].st = malloc(n); dp_lr_save(lr, ar[cnt].st, n); ar[cnt++].cell = c;
            }
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            mask = (uint16_t)(rng >> 20) & 0x0FFF;
        }
    }
    printf("deepeel libretro novelty (Go-Explore over screen cells, tool observation)\ncore: %s\n%s\n", dp_lr_core_name(lr), dp_lr_degradations(lr));
    printf("rounds %lu, hold %lu frames, cells reached: %zu, archive states dropped (cap 100000): %lu\n", rounds, hold, cnt, dropped);
    for (size_t i = 0; i < cnt; i++) free(ar[i].st);
    free(ar); free(tmp);
    dp_lr_close(lr);
    return dropped ? 3 : 0;
}

#ifdef DP_HAS_GUI
#include <SDL.h>
// deepeel lr watch CORE ROM [--scale N] [--max-frames N]: human mode on any core. Arrows = d-pad, Z = B,
// X = A, A = Y, S = X, Enter = start, right shift = select, Q/Escape quits. Real time at ~60 fps.
static int lr_watch(int argc, char **argv) {
    const char *core = argv[3], *rom = argv[4];
    int scale = 2;
    unsigned long maxf = 0;
    for (int i = 5; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--scale")) scale = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--max-frames")) maxf = strtoul(argv[i + 1], NULL, 0);
        else { fprintf(stderr, "deepeel lr: unknown option %s\n", argv[i]); return 2; }
    }
    if (scale < 1 || scale > 8) scale = 2;
    char err[256] = "";
    DpLibretro *lr = dp_lr_open_copy(core, err, sizeof err);
    if (!lr || !dp_lr_load_rom(lr, rom, err, sizeof err)) { fprintf(stderr, "deepeel lr: %s\n", err); return 1; }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "deepeel lr: SDL: %s\n", SDL_GetError()); return 1; }
    dp_lr_advance(lr, 1);
    int w, h, pitch, fmt;
    if (!dp_lr_framebuffer(lr, &w, &h, &pitch, &fmt)) { fprintf(stderr, "deepeel lr: core produced no video\n"); return 1; }
    SDL_Window *win = SDL_CreateWindow("deepeel lr watch", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * scale, h * scale, 0);
    SDL_Renderer *rd = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
    if (!rd) { fprintf(stderr, "deepeel lr: SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Texture *tx = NULL;
    int tw = 0, th = 0;
    int quit = 0;
    unsigned long fr = 0;
    while (!quit && (!maxf || fr < maxf)) {
        Uint32 t0 = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_q || e.key.keysym.sym == SDLK_ESCAPE))) quit = 1;
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        uint16_t m = 0;
        if (k[SDL_SCANCODE_Z]) m |= 1u << 0;
        if (k[SDL_SCANCODE_A]) m |= 1u << 1;
        if (k[SDL_SCANCODE_RSHIFT]) m |= 1u << 2;
        if (k[SDL_SCANCODE_RETURN]) m |= 1u << 3;
        if (k[SDL_SCANCODE_UP]) m |= 1u << 4;
        if (k[SDL_SCANCODE_DOWN]) m |= 1u << 5;
        if (k[SDL_SCANCODE_LEFT]) m |= 1u << 6;
        if (k[SDL_SCANCODE_RIGHT]) m |= 1u << 7;
        if (k[SDL_SCANCODE_X]) m |= 1u << 8;
        if (k[SDL_SCANCODE_S]) m |= 1u << 9;
        dp_lr_set_input(lr, 0, m);
        dp_lr_advance(lr, 1);
        fr++;
        const uint8_t *fb = dp_lr_framebuffer(lr, &w, &h, &pitch, &fmt);
        if (fb) {
            if (!tx || tw != w || th != h) {
                if (tx) SDL_DestroyTexture(tx);
                tx = SDL_CreateTexture(rd, fmt == DP_LR_FMT_XRGB8888 ? SDL_PIXELFORMAT_ARGB8888 : fmt == DP_LR_FMT_RGB565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB1555,
                                       SDL_TEXTUREACCESS_STREAMING, w, h);
                tw = w; th = h;
            }
            if (tx) { SDL_UpdateTexture(tx, NULL, fb, pitch); SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); SDL_RenderPresent(rd); }
        }
        Uint32 dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay(16 - dt);
    }
    printf("deepeel lr watch: %lu frames (tool observation, %s)\n", fr, dp_lr_core_name(lr));
    if (tx) SDL_DestroyTexture(tx);
    SDL_DestroyRenderer(rd); SDL_DestroyWindow(win); SDL_Quit();
    dp_lr_close(lr);
    return 0;
}
#endif

int dp_lr_command(int argc, char **argv) {
#ifdef DP_HAS_GUI
    if (argc >= 5 && !strcmp(argv[2], "watch")) return lr_watch(argc, argv);
#endif
    if (argc >= 5 && !strcmp(argv[2], "novelty")) return lr_novelty(argc, argv);
    if (argc >= 5 && !strcmp(argv[2], "play")) return lr_play(argc, argv);
    if (argc < 5 || strcmp(argv[2], "explore")) {
        fprintf(stderr, "usage: deepeel lr play CORE ROM [--frames N] [--tas FILE] [--ppm FILE]\n       deepeel lr watch CORE ROM [--scale N] [--max-frames N]\n       deepeel lr novelty CORE ROM [--rounds N] [--seed N] [--hold N]\n       deepeel lr explore CORE ROM [--text FILE] [--map FILE] "
                        "[--png FILE] [--path-steps N] [--max-total N] [--queue N]\n");
        return 2;
    }
    const char *core = argv[3], *rom = argv[4], *text = NULL, *map = NULL, *png = NULL;
    DpLrExploreLimits lim = { 100000, 20000000, 4096, 2, 3 };
    for (int i = 5; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--text")) text = argv[i + 1];
        else if (!strcmp(argv[i], "--map")) map = argv[i + 1];
        else if (!strcmp(argv[i], "--png")) png = argv[i + 1];
        else if (!strcmp(argv[i], "--path-steps")) lim.path_steps = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--max-total")) lim.total_steps = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--loop-bound")) lim.loop_bound = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--queue")) lim.max_items = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        else { fprintf(stderr, "deepeel lr: unknown option %s\n", argv[i]); return 2; }
    }
    char err[256] = "";
    DpLibretro *lr = dp_lr_open_copy(core, err, sizeof err);
    if (!lr || !dp_lr_load_rom(lr, rom, err, sizeof err)) {
        fprintf(stderr, "deepeel lr: %s\n", err);
        return 1;
    }
    struct stat sb;
    size_t rn = stat(rom, &sb) == 0 ? (size_t)sb.st_size : 0;
    uint8_t *exec = calloc(rn ? rn : 1, 1);
    DpLrExploreStats st;
    if (!dp_lr_explore(lr, exec, rn, &lim, &st)) {
        fprintf(stderr, "deepeel lr: %s\n", dp_lr_degradations(lr));
        fprintf(stderr, "deepeel lr: core has no trace extension (level A cannot fork)\n");
        dp_lr_close(lr);
        free(exec);
        return 1;
    }
    FILE *f = text ? fopen(text, "w") : stdout;
    if (!f) f = stdout;
    fprintf(f, "deepeel libretro (forking exploration, tool observation)\n");
    fprintf(f, "core: %s\n%s\n", dp_lr_core_name(lr), dp_lr_degradations(lr));
    fprintf(f, "rom: %zu bytes\n", rn);
    fprintf(f, "steps: %llu in %llu paths, forks %llu (deduped %llu), branch sites %u\n",
            (unsigned long long)st.steps, (unsigned long long)st.paths,
            (unsigned long long)st.forks, (unsigned long long)st.deduped, st.branch_sites);
    fprintf(f, "code starts reached: %u of %zu bytes\n", st.covered, rn);
    fprintf(f, "saturation (a non-zero line means the exploration is incomplete):\n");
    fprintf(f, "  fork site cap hits: %llu\n  path step budget cuts: %llu\n  global step wall cuts: %llu\n  dropped fork items (queue full): %llu\n  loop bound cuts: %llu\n",
            (unsigned long long)st.capped_forks, (unsigned long long)st.cut_path_steps,
            (unsigned long long)st.cut_total, (unsigned long long)st.dropped_items,
            (unsigned long long)st.cut_loop);
    if (f != stdout) fclose(f);
    if (map || png) {
        // GB-bank layout ("bank start end class", CPU-view addresses) so the
        // existing map text/PNG tools read it: only opcode starts the fork
        // exploration reached are "code-natural"; everything else is unknown
        size_t banks = (rn + 0x3FFF) / 0x4000;
        if (!banks) banks = 1;
        DpBuf mt;
        dp_buf_init(&mt);
        lr_pf(&mt, "# deepeel libretro map v1: opcode starts reached by the forking exploration\n");
        for (size_t b = 0; b < banks; b++) {
            unsigned win = b ? 0x4000u : 0u;
            size_t a = 0;
            while (a < 0x4000) {
                size_t base = b * 0x4000;
                int v = base + a < rn ? exec[base + a] : 0;
                size_t e = a;
                while (e + 1 < 0x4000 && (base + e + 1 < rn ? exec[base + e + 1] : 0) == v) e++;
                lr_pf(&mt, "%zu %04zX %04zX %s\n", b, win + a, win + e, v ? "code-natural" : "unknown");
                a = e + 1;
            }
        }
        if (map) {
            FILE *m = fopen(map, "w");
            if (m) { fwrite(mt.p, 1, mt.len, m); fclose(m); }
        }
        if (png && !dp_map_png_from_maptext((const char *)mt.p, mt.len, (int)banks, "libretro", "{}", png,
                                            "libretro", rom, (unsigned)rn))
            fprintf(stderr, "deepeel lr: cannot write %s\n", png);
        dp_buf_free(&mt);
    }
    dp_lr_close(lr);
    free(exec);
    return st.dropped_items || st.capped_forks || st.cut_total || st.cut_path_steps ? 3 : 0;
}
