// test_gb_verdicts.c: witnesses for the GB section verdicts (audit finding
// E3, docs/AUDIT-PARITE-MOTEURS.fr.md).  Before this change the GB explorer
// never attributed DP_V_SPECULATIVE; a section only ever run by speculative
// seeds (pointer scan, static-closure seeds) reported the same verdict as a
// section reached by a fork of the real flow.  The provenance distinction
// rides on the path ancestry: a forced path descending from the reset flow
// (forks keep from_reset) proves FORCED; a forced path descended from a
// synthetic seed (pointer/closure/vector seeds carry from_reset = 0) is
// speculative evidence.  Priorities, per docs/COUVERTURE.fr.md: required
// (boot flow, IRQ handlers, copy sources) outranks played (natural only),
// which outranks forced (a real-flow fork reached it — natural execution and
// flow forks are never requalified downward), which outranks speculative
// (every hit came from a synthetic seed).
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "mach_util.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

static DpCart *g_cart;
static DpResults res;
static DpMach *g_mach;

static void run(const char *dir, const char *name) {
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/fix_%s.gb", dir, name);
    if (g_cart) dp_cart_free(g_cart);
    if (res.cart) dp_results_free(&res);
    memset(&res, 0, sizeof res);
    g_cart = dp_cart_load(path, err, sizeof err);
    if (!g_cart) {
        fprintf(stderr, "cannot load %s: %s\n", path, err);
        exit(2);
    }
    dp_results_init(&res, g_cart, dp_platform_gb());
    dp_static_scan(&res);
    DpConfig cfg;
    dp_config_defaults(&cfg);
    if (g_mach) dp_mach_close(g_mach);
    g_mach = mach_for(g_cart);
    CHECK(g_mach);
    DpExplorer *ex = dp_explorer_new(&cfg, &res, g_mach);
    CHECK(ex);
    dp_seed_gb(ex);
    dp_explore(ex);
    dp_explorer_free(ex);
}

static const DpSection *section_at(int bank, uint16_t addr) {
    for (int i = 0; i < res.section_count; i++) {
        const DpSection *s = &res.sections[i];
        if (s->bank == bank && s->lo <= addr && addr <= s->hi) return s;
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    // natural: the reset flow runs the code -> required (boot evidence)
    run(dir, "verdict_nat");
    const DpSection *s = section_at(0, 0x150);
    CHECK(s && s->verdict == DP_V_REQUIRED && s->boot_hits > 0);

    // forced: the block only the flow's branch fork runs stays forced, with
    // no speculative provenance at all
    run(dir, "verdict_forced");
    s = section_at(0, 0x15D);
    CHECK(s && s->verdict == DP_V_FORCED);
    CHECK(s->forced_hits > 0 && s->spec_hits == 0 && s->natural_hits == 0);

    // speculative: nothing calls the handler; the pointer scan seeds it and
    // its every hit descends from that synthetic seed (forced_hits counts
    // every forced hit, the speculative ones included: equal here)
    run(dir, "verdict_spec");
    s = section_at(0, 0x200);
    CHECK(s && s->verdict == DP_V_SPECULATIVE);
    CHECK(s->spec_hits > 0 && s->natural_hits == 0 && s->forced_hits == s->spec_hits);
    s = section_at(0, 0x150); // the halt the reset flow runs stays required
    CHECK(s && s->verdict == DP_V_REQUIRED);

    // merged provenance: the same handler is seeded AND fork-reachable from
    // the real flow — the flow fork outranks the seed, the verdict is forced
    run(dir, "verdict_mix");
    s = section_at(0, 0x159);
    CHECK(s && s->verdict == DP_V_FORCED);
    CHECK(s->spec_hits > 0 && s->forced_hits > s->spec_hits);

    if (g_mach) dp_mach_close(g_mach);
    if (g_cart) dp_cart_free(g_cart);
    TEST_DONE("gb_verdicts");
}
