// deepeel lr explore CORE ROM [--text FILE] [--map FILE] [--path-steps N] [--max-total N] [--queue N]
// Forking exploration of a level-B libretro core (see libretro.h). Level-A
// cores cannot fork: the command says so and exits non-zero.
#include "mach/libretro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int dp_lr_command(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[2], "explore")) {
        fprintf(stderr, "usage: deepeel lr explore CORE ROM [--text FILE] [--map FILE] "
                        "[--path-steps N] [--max-total N] [--queue N]\n");
        return 2;
    }
    const char *core = argv[3], *rom = argv[4], *text = NULL, *map = NULL;
    DpLrExploreLimits lim = { 100000, 20000000, 4096 };
    for (int i = 5; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--text")) text = argv[i + 1];
        else if (!strcmp(argv[i], "--map")) map = argv[i + 1];
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
    fprintf(f, "  path step budget cuts: %llu\n  global step wall cuts: %llu\n  dropped fork items (queue full): %llu\n",
            (unsigned long long)st.cut_path_steps, (unsigned long long)st.cut_total,
            (unsigned long long)st.dropped_items);
    if (f != stdout) fclose(f);
    if (map) { // run-length map: one range per class change, compact ROM indices
        FILE *m = fopen(map, "w");
        if (m) {
            fprintf(m, "# deepeel libretro map v1: code-start bytes reached (1) or not (0)\n");
            size_t a = 0;
            while (a < rn) {
                size_t b = a;
                while (b + 1 < rn && exec[b + 1] == exec[a]) b++;
                fprintf(m, "%04zX %04zX %s\n", a, b, exec[a] ? "code-start" : "unreached");
                a = b + 1;
            }
            fclose(m);
        }
    }
    dp_lr_close(lr);
    free(exec);
    return st.dropped_items || st.cut_total || st.cut_path_steps ? 3 : 0;
}
