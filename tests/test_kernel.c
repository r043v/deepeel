// test_core: the generic kernel (src/explore/core.c) on the Sega backend:
// both sides of a conditional are covered, the forced side is flagged, caps
// are reported.
#include "../src/explore/core.h"
#include "../src/platform/sega/backend.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

static void after_step_cb(void *u, DpMachine *m, uint32_t pc) { (void)m; (void)pc; ((uint64_t *)u)[0]++; }
static int veto_all(void *u, DpMachine *m, uint32_t s, uint32_t o) { (void)u; (void)m; (void)s; (void)o; return 0; }

static void obs_read(void *u, uint16_t a, uint8_t v) { (void)a; (void)v; ((uint64_t *)u)[0]++; }
static void obs_write(void *u, uint16_t a, uint8_t v) { (void)a; (void)v; ((uint64_t *)u)[1]++; }

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
    dp_core_config_default(&cfg);
    cfg.max_total = 20000;
    DpCoreResult r;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    size_t nat = 0, forced = 0;
    for (size_t i = 0; i < r.rom_size; i++) {
        if (r.flags[i] & DP_CORE_NATURAL) nat++;
        if (r.flags[i] & DP_CORE_FORCED) forced++;
    }
    CHECK(r.steps > 0 && r.paths >= 2 && r.forks >= 1);
    CHECK(nat > 0 && forced > 0);
    printf("core: steps %llu paths %llu forks %llu natural %zu forced %zu\n",
           (unsigned long long)r.steps, (unsigned long long)r.paths,
           (unsigned long long)r.forks, nat, forced);
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
    // hooks: after_step sees every instruction, veto_fork refuses the fork
    typedef struct { uint64_t steps; } H;
    H h = {0};
    DpPlatformHooks hooks = {0};
    hooks.user = &h;
    hooks.after_step = after_step_cb;
    hooks.veto_fork = veto_all;
    b->init(rom, rn, NULL, 1);
    dp_core_config_default(&cfg);
    cfg.max_total = 20000;
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
    m->close(m);
    TEST_DONE("kernel");
    return 0;
}
