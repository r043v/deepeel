// test_core: the generic kernel (src/explore/core.c) on the Sega backend:
// both sides of a conditional are covered, the forced side is flagged, caps
// are reported.
#include "../src/explore/core.h"
#include "../src/platform/sega/backend.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static void after_step_cb(void *u, DpMachine *m, uint32_t pc) { (void)m; (void)pc; ((uint64_t *)u)[0]++; }
static int veto_all(void *u, DpMachine *m, uint32_t s, uint32_t o) { (void)u; (void)m; (void)s; (void)o; return 0; }
static int stop_at_loop(void *u, DpMachine *m, uint32_t pc) { (void)u; (void)m; return pc != 0x0f; }

static void obs_read(void *u, uint32_t a, uint8_t v) { (void)a; (void)v; ((uint64_t *)u)[0]++; }
static void obs_write(void *u, uint32_t a, uint8_t v) { (void)a; (void)v; ((uint64_t *)u)[1]++; }

#ifdef __linux__
// Fault injection against the anonymous spool already opened by the kernel.
static void truncate_spool(void *u, DpMachine *m, uint32_t pc) {
    (void)m; (void)pc;
    int *done = u;
    if (*done) return;
    for (int fd = 0; fd < 1024; fd++) {
        char path[64], target[512];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, target, sizeof target - 1);
        if (n < 0) continue;
        target[n] = 0;
        if (strstr(target, "/deepeel-spool-") && ftruncate(fd, 0) == 0) { *done = 1; break; }
    }
}
#endif

static const void *pick_gg(unsigned w) { return dp_sega_backend_ggt(); }

int main(int argc, char **argv) {
    CHECK(argc > 1);
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
    CHECK(m && m->pc && m->set_pc);
    DpCoreConfig cfg;
    dp_core_config_default(&cfg); cfg.no_solve = 1;   // this test pins the forced side
    cfg.max_total = 20000;
    DpCoreResult r;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    size_t nat = 0, forced = 0;
    for (size_t i = 0; i < r.rom_size; i++) {
        if (r.flags[i] & DP_CORE_NATURAL) nat++;
        if (r.flags[i] & DP_CORE_FORCED) forced++;
        if (r.flags[i] & DP_CORE_START_NATURAL) CHECK(r.flags[i] & DP_CORE_NATURAL);
        if (r.flags[i] & DP_CORE_START_FORCED) CHECK(r.flags[i] & DP_CORE_FORCED);
    }
    CHECK(r.steps > 0 && r.paths >= 2 && r.forks >= 1);
    CHECK(r.flags[0] & DP_CORE_START_NATURAL);
    CHECK(nat > 0 && forced > 0);
    printf("core: steps %llu paths %llu forks %llu natural %zu forced %zu\n",
           (unsigned long long)r.steps, (unsigned long long)r.paths,
           (unsigned long long)r.forks, nat, forced);
    size_t sn_nat = 0, sn_frc = 0;
    for (size_t i = 0; i < r.rom_size; i++) {
        if (r.src_flags[i] & DP_SRC_FORK_NATURAL) sn_nat++;
        if (r.src_flags[i] & DP_SRC_FORK_FORCED) sn_frc++;
        // executed bytes only: STATIC (closure) and DATA (read) bytes carry no first-seen step or fork source
        int ex = !!(r.flags[i] & (DP_CORE_NATURAL | DP_CORE_FORCED));
        CHECK(ex == !!r.first_seen[i] && r.first_seen[i] <= r.steps + 1);
        CHECK(ex == !!(r.src_flags[i] & (DP_SRC_FORK_NATURAL | DP_SRC_FORK_FORCED | DP_SRC_SESSION | DP_SRC_DYN_FABRICATED)));
    }
    CHECK(sn_nat > 0 && sn_frc > 0);
    DpCoreSection secs[16];
    size_t ns = dp_core_sections(&r, secs, 16);
    CHECK(ns >= 1 && ns <= 16);
    size_t sn = 0, sf = 0;
    for (size_t i = 0; i < ns; i++) { CHECK(secs[i].required == (secs[i].natural > 0)); sn += secs[i].natural; sf += secs[i].forced; }
    CHECK(sn > 0 && sf > 0);   // natural code plus the side only a fork reached
    dp_core_result_free(&r);
    // a tiny budget is reported, never silent
    b->init(rom, rn, NULL, 1);
    cfg.max_total = 3;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    CHECK(r.sat_total >= 1);
    dp_core_result_free(&r);
    // A resumed frontier must obey the RAM queue limit as it is loaded.
    // Several synthetic entry points remain pending when the first run stops.
    char state_dir[] = "/tmp/deepeel-kernel-resume-XXXXXX";
    CHECK(mkdtemp(state_dir));
    uint32_t entries[12];
    for (unsigned i = 0; i < 12; i++) entries[i] = 0x100 + i;
    b->init(rom, rn, NULL, 1);
    dp_core_config_default(&cfg);
    cfg.state_dir = state_dir; cfg.max_total = 1; cfg.queue_max = 1;
    cfg.no_sessions = 1; cfg.attract_frames = 0; cfg.no_novelty_order = 1;
    cfg.seeds = entries; cfg.n_seeds = 12;
    int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin >= 0) close(STDIN_FILENO);  // the spool may legitimately receive fd 0
    int first_ok = dp_core_explore(m, rn, &cfg, &r);
    if (saved_stdin >= 0) { CHECK(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO); close(saved_stdin); }
    CHECK(first_ok);
    CHECK(r.saved >= 12 && r.spooled >= 11);
    char first_frontier[512]; snprintf(first_frontier, sizeof first_frontier, "%s/frontier.bin", state_dir);
    f = fopen(first_frontier, "rb");
    uint32_t header[3], first_record[4];
    CHECK(f && fread(header, sizeof *header, 3, f) == 3 && fread(first_record, sizeof *first_record, 4, f) == 4);
    CHECK(first_record[1] == 3);  // forced entry, retaining its seed identity
    CHECK(fseek(f, (long)header[1], SEEK_CUR) == 0);
    int active_saved = 0;
    for (uint32_t i = 1; i < header[2]; i++) {
        uint32_t item[4];
        CHECK(fread(item, sizeof *item, 4, f) == 4);
        if (item[0] == 3 && item[1] == 0) active_saved = 1;
        CHECK(fseek(f, (long)header[1], SEEK_CUR) == 0);
    }
    CHECK(active_saved);  // natural path stopped after ld a,($C000)
    fclose(f);
    uint32_t *sources = malloc(r.rom_size * sizeof *sources);
    CHECK(sources);
    memcpy(sources, r.src_flags, r.rom_size * sizeof *sources);
    CHECK(sources[0] & DP_SRC_FORK_NATURAL);
    dp_core_result_free(&r);
    b->init(rom, rn, NULL, 1);
    m->set_pc(m, 0x200);  // the only new instruction cannot recreate source 0
    cfg.resume = 1; cfg.seeds = NULL; cfg.n_seeds = 0;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    CHECK(r.resumed >= 12 && r.spooled >= r.resumed - 1);
    CHECK(r.saved > 0);
    for (size_t i = 0; i < r.rom_size; i++) CHECK((r.src_flags[i] & sources[i]) == sources[i]);
    free(sources);
    dp_core_result_free(&r);
    // An older file with no source extension still resumes its byte map.
    char coverage_path[512]; snprintf(coverage_path, sizeof coverage_path, "%s/coverage.bin", state_dir);
    CHECK(truncate(coverage_path, (off_t)(8 + rn)) == 0);
    b->init(rom, rn, NULL, 1);
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    CHECK(r.flags[0] & DP_CORE_NATURAL);
    dp_core_result_free(&r);
    // A truncated frontier is rejected without replacing the damaged file.
    char frontier_path[512]; snprintf(frontier_path, sizeof frontier_path, "%s/frontier.bin", state_dir);
    CHECK(truncate(frontier_path, 12) == 0);
    struct stat st;
    b->init(rom, rn, NULL, 1);
    CHECK(!dp_core_explore(m, rn, &cfg, &r));
    CHECK(r.resumed == 0 && r.saved == 0);
    CHECK(stat(frontier_path, &st) == 0 && st.st_size == 12);
    dp_core_result_free(&r);
    // A short write must not replace the existing frontier or count it saved.
    uint8_t old_header[12], new_header[12];
    f = fopen(frontier_path, "rb"); CHECK(f && fread(old_header, 1, sizeof old_header, f) == sizeof old_header); fclose(f);
    struct rlimit old_limit, short_limit;
    CHECK(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    short_limit = old_limit; short_limit.rlim_cur = 12;
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    CHECK(setrlimit(RLIMIT_FSIZE, &short_limit) == 0);
    b->init(rom, rn, NULL, 1);
    cfg.resume = 0; cfg.seeds = entries; cfg.n_seeds = 12;
    int failed_write = !dp_core_explore(m, rn, &cfg, &r);
    CHECK(setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    signal(SIGXFSZ, old_sig);
    CHECK(failed_write && r.saved == 0);
    CHECK(stat(frontier_path, &st) == 0 && st.st_size == 12);
    f = fopen(frontier_path, "rb"); CHECK(f && fread(new_header, 1, sizeof new_header, f) == sizeof new_header); fclose(f);
    CHECK(memcmp(old_header, new_header, sizeof old_header) == 0);
    dp_core_result_free(&r);
    // A destination that cannot be replaced must not report a successful save.
    CHECK(unlink(frontier_path) == 0 && mkdir(frontier_path, 0700) == 0);
    b->init(rom, rn, NULL, 1);
    cfg.resume = 0; cfg.seeds = entries; cfg.n_seeds = 12;
    CHECK(!dp_core_explore(m, rn, &cfg, &r));
    CHECK(r.saved == 0 && stat(frontier_path, &st) == 0 && S_ISDIR(st.st_mode));
    dp_core_result_free(&r);
    CHECK(rmdir(frontier_path) == 0);
#ifdef __linux__
    // A lost spool record is an error, not a successful exhausted search.
    int spool_truncated = 0;
    DpPlatformHooks fault_hooks = {0};
    fault_hooks.user = &spool_truncated; fault_hooks.after_step = truncate_spool; fault_hooks.veto_fork = veto_all;
    for (unsigned i = 0; i < 12; i++) entries[i] = 0;
    b->init(rom, rn, NULL, 1);
    cfg.resume = 0; cfg.max_total = 20000; cfg.hooks = &fault_hooks; cfg.no_solve = 1;
    CHECK(!dp_core_explore(m, rn, &cfg, &r));
    CHECK(spool_truncated && r.spool_left > 0 && r.saved == 0);
    dp_core_result_free(&r);
#endif
    const char *state_files[] = { "frontier.bin", "coverage.bin", "disc.bin", "cells.bin" };
    for (unsigned i = 0; i < sizeof state_files / sizeof state_files[0]; i++) {
        char file[512]; snprintf(file, sizeof file, "%s/%s", state_dir, state_files[i]);
        unlink(file);
    }
    CHECK(rmdir(state_dir) == 0);
    // In unbounded mode an already covered entry is still explored from its
    // own state, rather than discarded solely on its address.
    uint32_t covered_entry = 0;
    DpPlatformHooks seed_hooks = {0}; seed_hooks.veto_fork = veto_all; seed_hooks.before_step = stop_at_loop;
    b->init(rom, rn, NULL, 1);
    dp_core_config_default(&cfg);
    cfg.no_sessions = 1; cfg.attract_frames = 0; cfg.no_solve = 1;
    cfg.visits_per_pc = 2; cfg.max_total = 20000; cfg.hooks = &seed_hooks;
    cfg.seeds = &covered_entry; cfg.n_seeds = 1;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    uint64_t bounded_paths = r.paths;
    dp_core_result_free(&r);
    b->init(rom, rn, NULL, 1);
    cfg.unbounded = 1;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    CHECK(r.paths > bounded_paths);
    dp_core_result_free(&r);
    // hooks: after_step sees every instruction, veto_fork refuses the fork
    typedef struct { uint64_t steps; } H;
    H h = {0};
    DpPlatformHooks hooks = {0};
    hooks.user = &h;
    hooks.after_step = after_step_cb;
    hooks.veto_fork = veto_all;
    b->init(rom, rn, NULL, 1);
    dp_core_config_default(&cfg);
    cfg.max_total = 20000; cfg.no_sessions = 1; cfg.no_solve = 1; cfg.attract_frames = 0;   // the contract below is about the one natural path
    cfg.hooks = &hooks;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    CHECK(h.steps == r.steps && r.steps > 0);
    CHECK(r.forks == 0 && r.hook_vetoed >= 1 && r.paths == 1);
    dp_core_result_free(&r);
    // observe: the fixture does ld a,(RAM): at least one data read is reported
    b->init(rom, rn, NULL, 1);
    uint64_t acc[2] = {0, 0};
    DpObservers ob = { NULL, obs_read, obs_write };
    m->observe(m, &ob, acc);
    for (int i = 0; i < 20 && m->run(m); i++) {}
    CHECK(acc[0] >= 1);
    // poke: a byte written through the map is read back by the machine and is not observed
    CHECK(m->poke);
    {
        b->init(rom, rn, NULL, 1);
        uint64_t acc2[2] = {0, 0};
        DpObservers ob2 = { NULL, obs_read, obs_write };
        m->observe(m, &ob2, acc2);
        m->poke(m, 0xC123, 0x5A);
        CHECK(m->fetch(m, 0xC123) == 0x5A);
        CHECK(acc2[1] == 0);
    }
    // clone: an independent worker instance starting from the same program
    dp_machine_sega_source(m, rom, rn, pick_gg);
    DpMachine *c = m->clone ? m->clone(m, 1) : NULL;
    CHECK(c != NULL);
    if (c) {
        CHECK(!c->attach || c->attach(c));   // the worker thread builds its runtime
        uint32_t pm = m->pc(m), pc0 = c->pc(c);
        CHECK(pc0 == 0);
        for (int i = 0; i < 5; i++) CHECK(c->run(c));
        CHECK(c->pc(c) != pc0);       // the clone advanced...
        CHECK(m->pc(m) == pm);
        c->close(c);
    }
    m->close(m);
    TEST_DONE("kernel");
    return 0;
}
