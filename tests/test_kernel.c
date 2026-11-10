// test_core: the generic kernel (src/explore/core.c) on the Sega backend:
// both sides of a conditional are covered, the forced side is flagged, caps
// are reported.
#include "../src/explore/core.h"
#include "../src/platform/sega/backend.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

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
    dp_core_result_free(&r);
    // a tiny budget is reported, never silent
    b->init(rom, rn, NULL, 1);
    cfg.max_total = 3;
    CHECK(dp_core_explore(m, rn, &cfg, &r));
    CHECK(r.sat_total >= 1);
    dp_core_result_free(&r);
    m->close(m);
    return 0;
}
