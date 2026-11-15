// test_p4_core: P4 of docs/PLAN-COUVERTURE-MAX.fr.md on the kernel.
//  B1: an event-probe site revisited under another semantic cell is re-armed (probe_rearms), bounded by
//      --probe-ctx-cap (saturation counted); cap 1 keeps the historical once-per-site behaviour.
#include "../src/core/dpmachine.h"
#include "../src/mach/nesmach.h"
#include "../src/explore/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int run_nes(const char *dir, const char *name, unsigned cap, DpCoreResult *r) {
    char path[512], err[128] = "";
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t rom[16 + 2 * 0x4000 + 0x2000];
    size_t rn = fread(rom, 1, sizeof rom, f);
    fclose(f);
    DpMachine *m = dp_machine_nes(rom, rn, -1, err, sizeof err);
    if (!m) return 0;
    DpCoreConfig cfg;
    dp_core_config_default(&cfg);
    cfg.hooks = dp_machine_nes_hooks();
    cfg.max_total = 200000; cfg.no_sessions = 1; cfg.probe_ctx_cap = cap;
    int ok = dp_core_explore(m, dp_machine_nes_prg_size(m), &cfg, r);
    m->close(m);
    return ok;
}

int main(int argc, char **argv) {
    CHECK(argc > 1);
    DpCoreResult r;
    CHECK(run_nes(argv[1], "fix_nes_probectx.nes", 1, &r));
    CHECK(r.probe_rearms == 0);
    fprintf(stderr, "cap1: steps %llu paths %llu probes %llu sites %llu\n", (unsigned long long)r.steps, (unsigned long long)r.paths, (unsigned long long)r.event_probes, (unsigned long long)r.event_probe_sites);
    uint64_t once = r.event_probes;
    dp_core_result_free(&r);
    CHECK(run_nes(argv[1], "fix_nes_probectx.nes", 8, &r));
    fprintf(stderr, "B1: probes %llu (once %llu), rearms %llu, ctx saturated %llu\n", (unsigned long long)r.event_probes, (unsigned long long)once, (unsigned long long)r.probe_rearms, (unsigned long long)r.probe_ctx_sat);
    CHECK(r.probe_rearms >= 1 && r.event_probes > once);
    dp_core_result_free(&r);
    CHECK(run_nes(argv[1], "fix_nes_probectx.nes", 2, &r));
    CHECK(r.probe_rearms <= 1);   // cap 2: the first arming plus at most one context
    dp_core_result_free(&r);
    printf("p4_core: ok\n");
    return 0;
}
