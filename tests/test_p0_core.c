// test_p0_core: P0 of docs/PLAN-COUVERTURE-MAX.fr.md on the kernel.
//  A1/A2: a 68000 CPU copy of non-zero ROM bytes to work RAM (move.b and move.l) is recognised as a
//         verbatim copy (the read hook carries the byte read; reads are paired with writes in order).
//  A15:   work dropped at the global step wall is counted, never silent.
#include "../src/core/dpmachine.h"
#include "../src/mach/mdmach.h"
#include "../src/explore/core.h"
#include "../src/platform/sega/backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int run_md(const char *dir, const char *name, DpCoreResult *r) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.md", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t rom[0x20000];
    size_t rn = fread(rom, 1, sizeof rom, f);
    fclose(f);
    DpMdMach *mm = dp_mdmach_open(rom, rn);
    if (!mm) return 0;
    dp_mdmach_reset(mm);
    DpMachine *m = dp_machine_md(mm);
    if (!m) return 0;
    dp_machine_md_source(m, rom, rn);
    DpCoreConfig cfg;
    dp_core_config_default(&cfg);
    cfg.hooks = dp_machine_md_hooks();
    cfg.max_total = 20000; cfg.no_sessions = 1;
    int ok = dp_core_explore(m, rn, &cfg, r);
    m->close(m);
    return ok;
}

int main(int argc, char **argv) {
    CHECK(argc > 1);
    DpCoreResult r;
    CHECK(run_md(argv[1], "fix_md_copy_b", &r));

    CHECK(r.copy_bytes >= 16);                       // 16 non-zero bytes copied one by one
    CHECK(r.flags[0x400] & DP_CORE_COPIED);
    dp_core_result_free(&r);
    CHECK(run_md(argv[1], "fix_md_copy_l", &r));
    CHECK(r.copy_bytes >= 16);                       // 4 longs = 16 bytes, 4 reads then 4 writes each
    CHECK((r.flags[0x400] & DP_CORE_COPIED) && (r.flags[0x40F] & DP_CORE_COPIED));
    dp_core_result_free(&r);
    // A3: two dbra loops 64 KiB apart share their low 16 address bits: 40 + 40 iterations must not hit the 64-visit cap
    CHECK(run_md(argv[1], "fix_md_alias", &r));
    CHECK(r.flags[0x10208] & DP_CORE_NATURAL);
    dp_core_result_free(&r);
    {   // A15: a step wall hit with forks still pending drops them, and says so
        char path[512];
        snprintf(path, sizeof path, "%s/fix_z80_branch.sms", argv[1]);
        FILE *f = fopen(path, "rb");
        CHECK(f);
        static uint8_t rom[0x8000];
        size_t rn = fread(rom, 1, sizeof rom, f);
        fclose(f);
        const DpSegaBackend *b = dp_sega_backend_gg();
        CHECK(b && b->init(rom, rn, NULL, 1) == 1);
        DpMachine *m = dp_machine_sega(b);
        CHECK(m);
        DpCoreConfig cfg;
        dp_core_config_default(&cfg);
        cfg.no_sessions = 1; cfg.no_solve = 1; cfg.max_total = 12;
        CHECK(dp_core_explore(m, rn, &cfg, &r));
        CHECK(r.sat_total >= 1);
        fprintf(stderr, "pending_dropped=%llu forks=%llu\n", (unsigned long long)r.pending_dropped, (unsigned long long)r.forks);
        CHECK(r.pending_dropped >= 1);
        dp_core_result_free(&r);
    }
    // P1: A6 origin attribution, B2 256-value fallback
    for (int v = 0; v < 2; v++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", argv[1], v ? "fix_z80_solve256.sms" : "fix_z80_solve.sms");
        FILE *f2 = fopen(path, "rb");
        CHECK(f2);
        static uint8_t rom2[0x8000];
        size_t rn2 = fread(rom2, 1, sizeof rom2, f2);
        fclose(f2);
        const DpSegaBackend *b2 = dp_sega_backend_gg();
        CHECK(b2 && b2->init(rom2, rn2, NULL, 1) == 1);
        DpMachine *m2 = dp_machine_sega(b2);
        DpCoreConfig c2;
        dp_core_config_default(&c2);
        c2.attract_frames = 0; c2.sessions = 0; c2.no_sessions = 1;
        DpCoreResult r2;
        CHECK(dp_core_explore(m2, rn2, &c2, &r2));
        size_t solved = 0, nat_fork = 0;
        for (size_t i = 0; i < r2.rom_size; i++) {
            if (r2.src_flags[i] & DP_SRC_SOLVED) { if (!(r2.src_flags[i] & DP_SRC_FORK_NATURAL)) solved++; CHECK(r2.flags[i] & DP_CORE_NATURAL); }   // reached natural, but only through a fabricated state
            if (r2.src_flags[i] & DP_SRC_FORK_NATURAL) nat_fork++;
        }
        fprintf(stderr, "variant %d: solved bytes %zu, solved forks %llu\n", v, solved, (unsigned long long)r2.solved_forks);
        CHECK(solved > 0 && r2.solved_forks >= 1 && nat_fork > 0);
        dp_core_result_free(&r2);
    }
    printf("p0_core: ok\n");
    return 0;
}
