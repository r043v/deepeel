// deepeel lr explore CORE ROM [--text FILE] [--map FILE] [--png FILE] [--path-steps N] [--max-total N] [--queue N]
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

int dp_lr_command(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[2], "play")) return lr_play(argc, argv);
    if (argc < 5 || strcmp(argv[2], "explore")) {
        fprintf(stderr, "usage: deepeel lr play CORE ROM [--frames N] [--tas FILE] [--ppm FILE]\n       deepeel lr explore CORE ROM [--text FILE] [--map FILE] "
                        "[--png FILE] [--path-steps N] [--max-total N] [--queue N]\n");
        return 2;
    }
    const char *core = argv[3], *rom = argv[4], *text = NULL, *map = NULL, *png = NULL;
    DpLrExploreLimits lim = { 100000, 20000000, 4096, 2 };
    for (int i = 5; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--text")) text = argv[i + 1];
        else if (!strcmp(argv[i], "--map")) map = argv[i + 1];
        else if (!strcmp(argv[i], "--png")) png = argv[i + 1];
        else if (!strcmp(argv[i], "--path-steps")) lim.path_steps = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--max-total")) lim.total_steps = strtoull(argv[i + 1], NULL, 0);
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
    fprintf(f, "  fork site cap hits: %llu\n  path step budget cuts: %llu\n  global step wall cuts: %llu\n  dropped fork items (queue full): %llu\n",
            (unsigned long long)st.capped_forks, (unsigned long long)st.cut_path_steps,
            (unsigned long long)st.cut_total, (unsigned long long)st.dropped_items);
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
