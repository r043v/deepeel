// test_audit_e78.c: unit witnesses for the audit findings E7 and E8
// (docs/AUDIT-PARITE-MOTEURS.fr.md 8), on existing build-generated fixtures.
//
//   E7 — the legacy PCE explorer's IRQ fork: fix_pce_irq.pce unmasks VDC
//        IRQ1, runs CLI, spins; nothing else can deliver the handler at
//        $E040.  The kernel side is already pinned (core_irq_fork_pce CLI
//        test); this pins the historical engine on the same fixture.
//   E8 — the GB same-context eviction (the C2 contract, ported from
//        test_sega_sections.c): fix_vblank.gb re-enters a section start with
//        a context the section cannot distinguish; those admissions are
//        evicted and counted, never silently run.
#include <stdio.h>
#include <string.h>
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/platform/pce/explore.h"
#include "mach_util.h"
#include "test_util.h"

static DpCart *g_cart;
static DpResults res;
static DpMach *g_mach;

static void run_gb(const char *dir, const char *name) {
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

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    // E7: the legacy PCE explorer forks the vblank IRQ the fixture arms
    {
        char path[512];
        snprintf(path, sizeof path, "%s/fix_pce_irq.pce", dir);
        FILE *f = fopen(path, "rb");
        CHECK_MSG(f, "pce irq fixture opens");
        static uint8_t rom[0x4000];
        size_t n = fread(rom, 1, sizeof rom, f);
        fclose(f);
        CHECK_MSG(n == sizeof rom, "pce irq fixture reads");
        DpPceOptions o = dp_pce_options();
        o.max_total = 1000000;
        DpPceResults r;
        memset(&r, 0, sizeof r);
        CHECK_MSG(dp_pce_explore(rom, n, &o, &r), "pce legacy explore runs");
        CHECK_MSG(r.irq_forks >= 1, "the vblank IRQ forked");
        // the handler at $E040 ran inside the dispatch (physical cell $40)
        CHECK_MSG((r.exec_kind[0x40] & DP_PCE_X_IRQ), "handler carries the irq mark");
        dp_pce_results_free(&r);
    }

    // E8: the GB same-context eviction on the re-entry fixture
    run_gb(dir, "vblank");
    CHECK_MSG(res.evicted_ctx >= 1, "same-context entries evicted");
    // and the eviction is bounded, never a loss: the exploration still
    // covered the fixture (its sections exist with natural hits)
    CHECK_MSG(res.section_count >= 1, "sections recorded");

    if (g_mach) dp_mach_close(g_mach);
    if (g_cart) dp_cart_free(g_cart);
    TEST_DONE("audit_e78");
}
