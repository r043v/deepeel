// test_audit_parity.c: proof of concept for the engine-parity bench proposed
// in docs/AUDIT-PARITE-MOTEURS.fr.md.  Same GB fixture, same budgets, same
// ROM index space (dp_rom_index): the legacy fork explorer (dp_explore) and
// the generic kernel (dp_core_explore) are compared byte for byte — executed
// sets (natural / forced / IRQ), the natural/forced split — and the parity
// line is printed, never silently assumed.  The pinned contract is the
// no-loss direction: every byte the legacy explorer proves is covered by the
// kernel; the kernel's extra coverage is reported (it exercises data-as-code
// the legacy engine cuts as nonsense — a known policy difference, §5.2 of the
// audit).  A legacy forced proof the kernel reached naturally is counted
// separately (forced->natural): a kind difference, not a coverage loss.
//
// Scope: the comparison harness itself on the GB fixtures.  This is not a
// claim that every technique is at parity on every platform; the techniques
// the kernel has and the legacy explorer lacks (concolic solving, RNG bytes,
// signature learning, index replay) are switched off here so that the two
// runs exercise the same shared core.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/explore/explorer_priv.h"
#include "../src/explore/core.h"
#include "mach_util.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

static DpCart *load(const char *dir, const char *name) {
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/fix_%s.gb", dir, name);
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "cannot load %s: %s\n", path, err);
        exit(2);
    }
    return cart;
}

// legacy fork explorer, the same pipeline as the CLI (reset, seeds, explore);
// closure rounds / provenance / siblings are the post-passes of the CLI and
// are out of scope for the byte-level exec comparison, like in test_explorer
static void run_legacy(DpCart *cart, DpResults *res) {
    dp_results_init(res, cart, dp_platform_gb());
    dp_static_scan(res);
    DpConfig cfg;
    dp_config_defaults(&cfg);
    cfg.max_steps_total = 2000000;   // equal, small budget for both engines (the PoC does not need the 20 M default)
    DpMach *mach = mach_for(cart);
    CHECK(mach);
    DpExplorer *ex = dp_explorer_new(&cfg, res, mach);
    CHECK(ex);
    dp_seed_gb(ex);
    dp_explore(ex);
    dp_explorer_free(ex);
    dp_mach_close(mach);
}

// generic kernel on the same machine runtime, GB hooks attached, budgets
// copied from the legacy defaults; the kernel-only techniques are off (see
// the file header) so both runs exercise the shared core
static void run_kernel(DpCart *cart, DpCoreResult *r) {
    DpMach *mach = mach_for(cart);
    CHECK(mach);
    dp_mach_reset(mach);   // power-on state, like the CLI kernel path
    DpMachine *km = dp_machine_gb(mach);
    CHECK(km && km->pc && km->set_pc);
    dp_machine_gb_source(km, cart->rom, cart->rom_size);
    DpCoreConfig cfg;
    dp_core_config_default(&cfg);
    cfg.max_total = 2000000;   // the same budget as the legacy run above
    cfg.max_path_steps = 1000;   // the GB kernel CLI path pins short paths too (DEEPEEL_GB_PATH_STEPS default)
    cfg.fork_site_cap = 4;   // DP_DEFAULT_BRANCH_FORK_CAP, the legacy default too
    cfg.rom_image = cart->rom;
    cfg.rom_image_size = cart->rom_size;
    cfg.hooks = dp_machine_gb_hooks();
    cfg.no_solve = 1;          // legacy has no concolic side (technique 1)
    cfg.no_rng_learn = 1;      // technique 4
    cfg.no_learn_sig = 1;      // technique 3
    cfg.no_index_replay = 1;   // technique 7
    cfg.no_decompress = 1;     // technique 6
    cfg.no_cells = 1;          // technique 8 (legacy dedup differs)
    cfg.no_novelty_order = 1;  // technique 5 (legacy queue order differs)
    cfg.no_sessions = 1;       // the legacy pipeline runs no played session here
    cfg.attract_frames = 0;
    CHECK(dp_core_explore(km, cart->rom_size, &cfg, r));
    km->close(km);
}

// one fixture: compare the two engines over the same rom index space, print
// the parity line; `expect_branch` requires the conditional fork on both
// engines, `expect_irq` the IRQ fork
static void compare(const char *dir, const char *name, int expect_branch, int expect_irq) {
    DpCart *cart = load(dir, name);
    DpResults res;
    memset(&res, 0, sizeof res);
    run_legacy(cart, &res);
    DpCoreResult kr;
    memset(&kr, 0, sizeof kr);
    run_kernel(cart, &kr);
    CHECK(kr.rom_size == (size_t)cart->rom_size);
    size_t lg_nat = 0, lg_for = 0, lg_irq = 0, kg_nat = 0, kg_for = 0;
    size_t missing = 0, extra_nat = 0, extra_for = 0, for_nat_shift = 0;
    for (size_t i = 0; i < kr.rom_size; i++) {
        uint8_t lx = res.exec_flags[i], kf = kr.flags[i];
        int ln = !!(lx & DP_XF_NATURAL), lf = !!(lx & DP_XF_FORCED);
        int kn = !!(kf & DP_CORE_NATURAL), kfrc = !!(kf & DP_CORE_FORCED);
        if (lx & DP_XF_IRQ) {
            lg_irq++;
            // the IRQ handler the legacy explorer reached must be covered by
            // the kernel too (natural: the machine services the IRQ itself)
            if (!kn && !kfrc) missing++;
            continue;   // handler bytes: coverage checked above
        }
        lg_nat += ln;
        lg_for += lf;
        kg_nat += kn;
        kg_for += kfrc;
        if ((ln || lf) && !kn && !kfrc) missing++;
        if (lf && !kfrc) {
            if (kn) for_nat_shift++;   // kernel proved it naturally: no loss, kind differs
            else missing++;            // a forced proof vanished entirely: a real loss
        }
        if (!ln && !lf && kn) extra_nat++;
        if (!ln && !lf && kfrc) extra_for++;
    }
    printf("audit-parity %s: legacy exec %zu (nat %zu forced %zu irq %zu), "
           "kernel exec %zu (nat %zu forced %zu), missing %zu, "
           "forced->natural %zu, kernel extra (nat %zu forced %zu)\n",
           name, lg_nat + lg_for, lg_nat, lg_for, lg_irq, kg_nat + kg_for, kg_nat, kg_for,
           missing, for_nat_shift, extra_nat, extra_for);
    CHECK(missing == 0);   // no-loss direction: the kernel never proves less than the legacy explorer
    if (expect_branch) {
        CHECK(res.forks_branch >= 1);
        CHECK(kr.forks >= 1);
        CHECK(lg_nat > 0 && lg_for > 0 && kg_nat > 0);   // both sides of the conditional, both engines
    }
    if (expect_irq) {
        CHECK(res.forks_irq >= 1);   // the suppressed side of the IRQ dispatch
        CHECK(kr.irq_forks >= 1);
    }
    dp_core_result_free(&kr);
    dp_results_free(&res);
    dp_cart_free(cart);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    // branch: both sides of a conditional plus a verbatim ROM->VRAM copy —
    // the minimal contract both engines must satisfy
    compare(dir, "branch", 1, 0);

    // irq: a vblank handler forked by the legacy explorer and dispatched
    // naturally (plus the suppressed-side fork) by the kernel
    compare(dir, "irq", 0, 1);

    TEST_DONE("audit_parity");
    return 0;
}
