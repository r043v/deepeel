// test_p3_core: P3 of docs/PLAN-COUVERTURE-MAX.fr.md on the kernel.
//  B6: a path stopped by a bounded cap (sat_loop here) is reported, not lost: its state joins the frontier
//      (deferred > 0, saved > 0) and a --resume --unbounded run drains it, reaching the code past the loop.
#include "../src/core/dpmachine.h"
#include "../src/mach/mdmach.h"
#include "../src/explore/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int run(const char *dir, DpCoreConfig *cfg, DpCoreResult *r) {
    char path[512];
    snprintf(path, sizeof path, "%s/fix_md_alias.md", dir);
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
    if (!cfg->hooks) cfg->hooks = dp_machine_md_hooks();
    int ok = dp_core_explore(m, rn, cfg, r);
    m->close(m);
    return ok;
}

static int stop_at_exit(void *user, DpMachine *m, uint32_t pc) {
    (void)user; (void)m;
    return pc != 0x10208;
}

static int no_forks(void *user, DpMachine *m, uint32_t site, uint32_t other) {
    (void)user; (void)m; (void)site; (void)other;
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc > 1);
    char sd[512];
    snprintf(sd, sizeof sd, "%s/p3_state", argv[1]);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", sd);
    CHECK(system(cmd) == 0);
    mkdir(sd, 0777);
    DpCoreConfig cfg;
    DpCoreResult r;
    // A finite path must reach its exit even when the scheduling quantum is
    // much shorter than the loop; a quantum is not a destructive step cap.
    dp_core_config_default(&cfg);
    const DpPlatformHooks hooks = { .before_step = stop_at_exit, .veto_fork = no_forks };
    cfg.unbounded = 1; cfg.max_path_steps = 8; cfg.no_sessions = 1;
    cfg.attract_frames = 0; cfg.no_exact = 1; cfg.hooks = &hooks;
    CHECK(run(argv[1], &cfg, &r));
    fprintf(stderr, "unbounded: steps %llu repeats %llu yields %llu natural@10208 %d\n", (unsigned long long)r.steps, (unsigned long long)r.state_repeats, (unsigned long long)r.cooperative_yields, !!(r.flags[0x10208] & DP_CORE_NATURAL));
    CHECK(r.flags[0x10206] & DP_CORE_NATURAL);
    CHECK(r.sat_path == 0);
    CHECK(r.cell_pruned == 0 && r.proj_pruned == 0);
    dp_core_result_free(&r);
    dp_core_config_default(&cfg);
    cfg.max_total = 200000; cfg.no_sessions = 1; cfg.visits_per_pc = 8; cfg.state_dir = sd;
    CHECK(run(argv[1], &cfg, &r));
    fprintf(stderr, "phase 1: sat_loop %llu deferred %llu saved %llu natural@10208 %d\n", (unsigned long long)r.sat_loop, (unsigned long long)r.deferred, (unsigned long long)r.saved, !!(r.flags[0x10208] & DP_CORE_NATURAL));
    CHECK(r.sat_loop >= 1);
    CHECK(r.deferred >= 1 && r.saved >= r.deferred);
    int before = !!(r.flags[0x10208] & DP_CORE_NATURAL);
    dp_core_result_free(&r);
    dp_core_config_default(&cfg);
    cfg.no_sessions = 1; cfg.state_dir = sd; cfg.resume = 1; cfg.unbounded = 1; cfg.max_total = 0;
    const DpPlatformHooks resume_hooks = { .veto_fork = no_forks };
    cfg.hooks = &resume_hooks;
    CHECK(run(argv[1], &cfg, &r));
    fprintf(stderr, "phase 2: resumed %llu natural@10208 %d\n", (unsigned long long)r.resumed, !!(r.flags[0x10208] & DP_CORE_NATURAL));
    CHECK(r.resumed >= 1);
    CHECK(r.cell_pruned == 0 && r.proj_pruned == 0);
    CHECK(r.flags[0x10208] & DP_CORE_NATURAL);
    (void)before;
    dp_core_result_free(&r);
    printf("p3_core: ok\n");
    return 0;
}
