// test_p1_probe.c: positive and negative witnesses for the GB P1 poll probe
// (fork_p1, audit finding E1 of docs/AUDIT-PARITE-MOTEURS.fr.md).  SM83
// `ld a,(C)` (op $F2) reads $FF00+C: with C=$00 the effective address is
// $FF00 (P1) and the probe must fork per key exactly like the `ldh a,($00)`
// variant; with C=$FF the effective address is $FFFF (IE) and the probe must
// stay silent.  These fixtures pin the address semantics against the machine
// backend and infirm the audit's original proposal (key the probe on C=$FF):
// that guard would move the probe to the IE register at $FFFF.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "mach_util.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

static DpCart *g_cart;
static DpResults res;
static DpMach *g_mach;

// the same pipeline as the CLI legacy explorer on one fixture;
// `no_ptr` switches the pointer-table scan off (the negative witness runs
// alone: the scan seeds the entry a second time from a cold state, whose
// branch fork reaches the handler by provenance unrelated to the probe)
static void run(const char *dir, const char *name, int no_ptr) {
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
    cfg.no_pointer_scan = no_ptr;
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

    // positive witness: C=$00, the read at $FF00+C is P1.  The probe forks
    // per key (one variant holds Start), the handler runs on that variant
    // only, and the counter is the probe's own (not the branch fork's).
    run(dir, "p1afc", 0);
    CHECK(res.forks_p1 >= 1);                        // the ld a,(C) read forked per key
    CHECK(res.exec_flags[0x175] & DP_XF_FORCED);     // handler reached with Start held
    CHECK(!(res.exec_flags[0x175] & DP_XF_NATURAL)); // never on the released-keys flow
    CHECK(res.vram_write_pages[0] & 1);              // the handler's write happened

    // negative witness: C=$FF, the read at $FF00+C is $FFFF (IE).  The probe
    // stays silent (forks_p1 == 0) and the branch outcome comes from the IE
    // value written beforehand ($01: bit 3 clear -> `and $08` zeroes A) —
    // naturally, with no key variants at all (a P1 read with no keys held
    // would give a low nibble of 1s and would not take the branch).
    run(dir, "p1afcneg", 1);
    CHECK(res.forks_p1 == 0);                        // the IE read is not a P1 poll
    CHECK(res.exec_flags[0x175] & DP_XF_NATURAL);    // the branch took the IE value
    // (the handler may still carry a FORCED mark: unrelated forks — the
    // loop containment on the fall-through self-loop, branch variants from
    // synthetic-state seeds — may reach it; the probe's contract is its own
    // counter and the natural branch outcome, not the absence of forks)

    if (g_mach) dp_mach_close(g_mach);
    if (g_cart) dp_cart_free(g_cart);
    TEST_DONE("p1_probe");
}
