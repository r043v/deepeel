// test_audit_parity.c: the engine-parity bench proposed in
// docs/AUDIT-PARITE-MOTEURS.fr.md (plan tranche 3, extension of the audit's
// proof of concept).  Same fixture, same budgets, same ROM index space
// (dp_rom_index): a legacy fork explorer and the generic kernel are compared
// byte for byte — executed sets (natural / forced / IRQ), the natural/forced
// split — and the parity line is printed and written to a JSON bench record,
// never silently assumed.  The pinned contract is the no-loss direction:
// every byte the legacy explorer proves is covered by the kernel; the
// kernel's extra coverage is reported (it exercises data-as-code the legacy
// engine cuts as nonsense — a known policy difference, §5.2 of the audit).
// A legacy forced proof the kernel reached naturally is counted separately
// (forced->natural): a kind difference, not a coverage loss.
//
// Platforms: GB historical (dp_explore vs dp_core_explore on dp_machine_gb)
// and NES 6502 (dp_nes_explore vs dp_core_explore on dp_machine_nes), both on
// fixtures generated at build time.  The JSON record (<dir>/parity.json)
// carries the tool version, the equivalent CLI commands, the budgets, the
// saturations, the byte-set fingerprints and every counted difference with
// its reason; a loss is never folded into an "expected" bucket.
//
// Self-tests of the bench itself (the comparator must not be vacuous):
//   - injected loss: a kernel run with per-path steps cut to 4 proves less
//     than the legacy explorer; the comparator must report missing > 0;
//   - admitted difference: the kernel's extra (natural) bytes are recorded
//     with their reason, never a failure;
//   - truncated coverage: a kernel coverage image that stops mid-fixture
//     hides every legacy proof above the cut; the comparator must report it
//     as missing, never fold it into an admitted difference;
//   - truncated/incompatible result: a kernel result whose index space does
//     not match the fixture fails the comparison explicitly (the detection
//     itself is self-tested against a doctored rom_size).
//
// Scope: the comparison harness on GB/NES fixtures.  This is not a claim
// that every technique is at parity on every platform; the techniques the
// kernel has and the legacy explorers lack (concolic solving, RNG bytes,
// signature learning, index replay) are switched off here so that the two
// runs exercise the same shared core.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/explore/explorer_priv.h"
#include "../src/explore/core.h"
#include "../src/platform/nes/explore.h"
#include "mach_util.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUDGET_TOTAL 2000000   // equal, small budget for both engines (the bench does not need the 20 M default)
#define BUDGET_PATH 1000       // the GB kernel CLI path pins short paths too (DEEPEEL_GB_PATH_STEPS default)
#define FORK_CAP 4             // DP_DEFAULT_BRANCH_FORK_CAP, the legacy default too

// one comparison result, recorded in the JSON bench record
typedef struct {
    const char *platform, *fixture, *cmd_legacy, *cmd_kernel;
    uint64_t budget_total, budget_path, fork_cap;
    size_t leg_nat, leg_for, leg_irq, ker_nat, ker_for;
    size_t missing, for_nat_shift, extra_nat, extra_for;
    uint64_t fp_legacy, fp_kernel;      // FNV-1a over each engine's byte flags
    uint64_t sat_legacy, sat_kernel;    // saturation counters, reported never silent
    int injected_loss, space_ok;
} ParityRow;

static ParityRow rows[16];
static int n_rows;

static ParityRow *row_open(const char *platform, const char *fixture, const char *cmd_legacy,
                           const char *cmd_kernel, uint64_t btotal, uint64_t bpath) {
    ParityRow *r = &rows[n_rows++];
    memset(r, 0, sizeof *r);
    r->platform = platform;
    r->fixture = fixture;
    r->cmd_legacy = cmd_legacy;
    r->cmd_kernel = cmd_kernel;
    r->budget_total = btotal;
    r->budget_path = bpath;
    r->fork_cap = FORK_CAP;
    r->space_ok = 1;
    return r;
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// the two engines only compare byte for byte inside one index space: a kernel
// result sized differently than the fixture is incompatible (truncated run,
// other format), and the comparison must fail explicitly instead of quietly
// comparing the first N bytes
static int space_matches(const DpCoreResult *kr, size_t fixture_size) {
    return kr->rom_size == fixture_size;
}

// the byte-level comparison shared by every platform row.  Flags carry the
// platform's own NATURAL/FORCED/IRQ bits (numerically the DP_XF_* bits on GB,
// the DP_NES_* ones on NES).  `expect_*` pin the fork counters of the GB
// rows; `want_missing` expects the injected loss (self-test mode).
static void compare_bytes(const char *name, size_t size, const uint8_t *lflags, int lnat, int lfor,
                          int lirq, const uint8_t *kflags, const DpCoreResult *kr, uint64_t leg_sat,
                          int expect_branch, int expect_irq, int want_missing) {
    ParityRow *row = &rows[n_rows - 1];
    const char *dump = getenv("DP_PARITY_DUMP");   // freeze the legacy proof set as a golden reference (see test_parity_golden.c)
    if (dump && !want_missing) {
        char gp[600];
        snprintf(gp, sizeof gp, "%s/parity_%s_%s.golden", dump, row->platform, name);
        FILE *gf = fopen(gp, "wb");
        if (gf) {
            for (size_t i = 0; i < size; i++) {
                uint8_t c = (uint8_t)(((lflags[i] & lnat) ? 1 : 0) | ((lflags[i] & lfor) ? 2 : 0) | ((lirq && (lflags[i] & lirq)) ? 4 : 0));
                fputc(c, gf);
            }
            fclose(gf);
        }
    }
    size_t lg_nat = 0, lg_for = 0, lg_irq = 0, kg_nat = 0, kg_for = 0;
    size_t missing = 0, extra_nat = 0, extra_for = 0, for_nat_shift = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t lx = lflags[i], kf = kflags[i];
        int ln = !!(lx & lnat), lf = !!(lx & lfor);
        int kn = !!(kf & DP_CORE_NATURAL), kfrc = !!(kf & DP_CORE_FORCED);
        if (lirq && (lx & lirq)) {
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
    row->leg_nat = lg_nat; row->leg_for = lg_for; row->leg_irq = lg_irq;
    row->ker_nat = kg_nat; row->ker_for = kg_for;
    row->missing = missing; row->for_nat_shift = for_nat_shift;
    row->extra_nat = extra_nat; row->extra_for = extra_for;
    row->fp_legacy = fnv1a(lflags, size);
    row->fp_kernel = fnv1a(kflags, size);
    row->sat_legacy = leg_sat;
    row->sat_kernel = kr->sat_total;
    row->injected_loss = want_missing;
    printf("audit-parity %s/%s: legacy exec %zu (nat %zu forced %zu irq %zu), "
           "kernel exec %zu (nat %zu forced %zu), missing %zu, "
           "forced->natural %zu, kernel extra (nat %zu forced %zu)\n",
           row->platform, name, lg_nat + lg_for, lg_nat, lg_for, lg_irq, kg_nat + kg_for, kg_nat,
           kg_for, missing, for_nat_shift, extra_nat, extra_for);
    if (want_missing) {
        CHECK_MSG(missing > 0, "injected loss detected"); // the comparator is not vacuous
        return;
    }
    CHECK(missing == 0);   // no-loss direction: the kernel never proves less than the legacy explorer
    if (expect_branch) {
        CHECK(lg_nat > 0 && lg_for > 0 && kg_nat > 0);   // both sides of the conditional, both engines
    }
    (void)expect_irq; // fork-counter pins live in the platform wrappers
}

// one GB fixture: legacy fork explorer vs generic kernel over the same rom
// index space; `expect_branch` requires the conditional fork on both engines,
// `expect_irq` the IRQ fork; `want_loss` runs the kernel with per-path steps
// cut to 4 and expects the comparator to report the injected loss
static void compare_gb(const char *dir, const char *name, int expect_branch, int expect_irq,
                       int want_loss, int trunc_selftest) {
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/fix_%s.gb", dir, name);
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "cannot load %s: %s\n", path, err);
        exit(2);
    }
    row_open("gb", name, "deepeel explore fix.gb --legacy", "deepeel explore fix.gb --core",
             BUDGET_TOTAL, want_loss ? 4 : BUDGET_PATH);

    // legacy fork explorer, the same pipeline as the CLI (reset, seeds,
    // explore); closure rounds / provenance / siblings are the post-passes of
    // the CLI and are out of scope for the byte-level exec comparison, like
    // in test_explorer
    DpResults res;
    memset(&res, 0, sizeof res);
    dp_results_init(&res, cart, dp_platform_gb());
    dp_static_scan(&res);
    DpConfig cfg;
    dp_config_defaults(&cfg);
    cfg.max_steps_total = BUDGET_TOTAL;
    DpMach *mach = mach_for(cart);
    CHECK(mach);
    DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
    CHECK(ex);
    dp_seed_gb(ex);
    dp_explore(ex);
    dp_explorer_free(ex);
    dp_mach_close(mach);

    // generic kernel on the same machine runtime, GB hooks attached, budgets
    // copied from the legacy defaults; the kernel-only techniques are off (see
    // the file header) so both runs exercise the shared core
    DpMach *kmach = mach_for(cart);
    CHECK(kmach);
    dp_mach_reset(kmach);   // power-on state, like the CLI kernel path
    DpMachine *km = dp_machine_gb(kmach);
    CHECK(km && km->pc && km->set_pc);
    dp_machine_gb_source(km, cart->rom, cart->rom_size);
    DpCoreConfig kcfg;
    dp_core_config_default(&kcfg);
    kcfg.max_total = BUDGET_TOTAL;
    kcfg.max_path_steps = want_loss ? 4 : BUDGET_PATH;
    kcfg.fork_site_cap = FORK_CAP;
    kcfg.rom_image = cart->rom;
    kcfg.rom_image_size = cart->rom_size;
    kcfg.hooks = dp_machine_gb_hooks();
    kcfg.no_solve = 1;          // legacy has no concolic side (technique 1)
    kcfg.no_rng_learn = 1;      // technique 4
    kcfg.no_learn_sig = 1;      // technique 3
    kcfg.no_index_replay = 1;   // technique 7
    kcfg.no_decompress = 1;     // technique 6
    kcfg.no_cells = 1;          // technique 8 (legacy dedup differs)
    kcfg.no_novelty_order = 1;  // technique 5 (legacy queue order differs)
    kcfg.no_sessions = 1;       // the legacy pipeline runs no played session here
    kcfg.attract_frames = 0;
    DpCoreResult kr;
    memset(&kr, 0, sizeof kr);
    CHECK(dp_core_explore(km, cart->rom_size, &kcfg, &kr));
    km->close(km);   // owns the DpMach: no dp_mach_close here

    // the kernel result must live in the same index space as the fixture: a
    // truncated or otherwise incompatible result fails explicitly
    ParityRow *row = &rows[n_rows - 1];
    row->space_ok = space_matches(&kr, (size_t)cart->rom_size);
    CHECK_MSG(row->space_ok, "kernel index space matches the fixture");
    {   // self-test: the detection itself must not be vacuous — a result one
        // byte short of the fixture is incompatible and must be reported
        DpCoreResult bad;
        memset(&bad, 0, sizeof bad);
        bad.rom_size = (size_t)cart->rom_size - 1;
        CHECK_MSG(!space_matches(&bad, (size_t)cart->rom_size),
                  "a truncated index space is detected, not accepted");
    }

    compare_bytes(name, cart->rom_size, res.exec_flags, DP_XF_NATURAL, DP_XF_FORCED, DP_XF_IRQ,
                  kr.flags, &kr, res.forks_capped, expect_branch, expect_irq, want_loss);
    if (trunc_selftest) {
        // self-test: a kernel coverage image that stops mid-fixture hides
        // every legacy proof above the cut; the comparator must report the
        // gap as missing bytes, never as an admitted difference.  The cut
        // sits at the middle of the legacy run itself (the fixtures are
        // 32 KiB but their code lives in the first kilobytes), so at least
        // one legacy proof byte is necessarily above it.
        size_t size = (size_t)cart->rom_size;
        size_t hi = 0;
        for (size_t i = 0; i < size; i++)
            if (res.exec_flags[i] & (DP_XF_NATURAL | DP_XF_FORCED | DP_XF_IRQ)) hi = i;
        CHECK_MSG(hi > 1, "self-test fixture has legacy proof bytes");
        uint8_t *trunc = malloc(size);
        CHECK_MSG(trunc, "truncation self-test buffer");
        if (trunc && hi > 1) {
            memcpy(trunc, kr.flags, size);
            memset(trunc + hi / 2, 0, size - hi / 2);
            row_open("gb", name, "deepeel explore fix.gb --legacy", "deepeel explore fix.gb --core",
                     BUDGET_TOTAL, BUDGET_PATH);
            compare_bytes(name, size, res.exec_flags, DP_XF_NATURAL, DP_XF_FORCED, DP_XF_IRQ,
                          trunc, &kr, res.forks_capped, 0, 0, 1);
            free(trunc);
        }
    }
    if (!want_loss) {
        if (expect_branch) {
            CHECK(res.forks_branch >= 1);
            CHECK(kr.forks >= 1);
        }
        if (expect_irq) {
            CHECK(res.forks_irq >= 1);   // the suppressed side of the IRQ dispatch
            CHECK(kr.irq_forks >= 1);
        }
    }
    dp_core_result_free(&kr);
    dp_results_free(&res);
    dp_cart_free(cart);
}

// one NES fixture: the same comparison over the PRG index space; the legacy
// play sessions are off (the kernel run has none either: comparable options)
static void compare_nes(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/fix_%s.nes", dir, name);
    FILE *f = fopen(path, "rb");
    CHECK_MSG(f, "nes fixture opens");
    static uint8_t rom[1 << 17];
    size_t size = fread(rom, 1, sizeof rom, f);
    fclose(f);
    CHECK_MSG(size > 16, "nes fixture reads");
    row_open("nes", name, "deepeel nes explore fix.nes", "deepeel nes explore fix.nes --core",
             BUDGET_TOTAL, BUDGET_PATH);

    DpNesOptions o = dp_nes_options();
    o.max_total = BUDGET_TOTAL;
    o.max_steps = BUDGET_PATH;
    o.play_sessions = 0;   // the kernel run has no sessions: comparable budgets
    DpNesResults r;
    memset(&r, 0, sizeof r);
    CHECK_MSG(dp_nes_explore(rom, size, -1, &o, &r), "nes legacy explore runs");

    char err[256] = "";
    DpMachine *km = dp_machine_nes(rom, size, -1, err, sizeof err);
    if (!km) fprintf(stderr, "nes kernel machine: %s\n", err);
    CHECK_MSG(km, "nes kernel machine opens");
    DpCoreConfig kcfg;
    dp_core_config_default(&kcfg);
    kcfg.max_total = BUDGET_TOTAL;
    kcfg.max_path_steps = BUDGET_PATH;
    kcfg.fork_site_cap = FORK_CAP;
    kcfg.hooks = dp_machine_nes_hooks();
    kcfg.no_solve = 1;
    kcfg.no_rng_learn = 1;
    kcfg.no_learn_sig = 1;
    kcfg.no_index_replay = 1;
    kcfg.no_decompress = 1;
    kcfg.no_cells = 1;
    kcfg.no_novelty_order = 1;
    kcfg.no_sessions = 1;
    kcfg.attract_frames = 0;
    DpCoreResult kr;
    memset(&kr, 0, sizeof kr);
    size_t prg = dp_machine_nes_prg_size(km);
    CHECK_MSG(dp_core_explore(km, prg, &kcfg, &kr), "nes kernel explore runs");
    km->close(km);

    ParityRow *row = &rows[n_rows - 1];
    row->space_ok = kr.rom_size == prg && prg == r.prg_size;
    CHECK_MSG(row->space_ok, "kernel index space matches the fixture");

    compare_bytes(name, prg, r.flags, DP_NES_NATURAL, DP_NES_FORCED, 0, kr.flags, &kr,
                  r.fork_cuts, 1, 0, 0);
    dp_core_result_free(&kr);
    dp_nes_results_free(&r);
}

static void json_write(const char *dir) {
    char jpath[512];
    snprintf(jpath, sizeof jpath, "%s/parity.json", dir);
    FILE *f = fopen(jpath, "w");
    CHECK(f);
    fprintf(f,
        "{\"tool\":\"test_audit_parity\",\"deepeel_version\":\"%s\","
        "\"contract\":\"no-loss: every byte the legacy explorer proves is covered by the kernel; "
        "every counted difference keeps its reason, none is assumed away\","
        "\"bench\":%s,"
        "\"rows\":[\n",
        DEEPEEL_VERSION,
        "\"GB historical vs kernel (dp_machine_gb), NES 6502 vs kernel (dp_machine_nes); "
        "same fixtures, same budgets, same rom index space; kernel-only techniques off "
        "(no_solve, no_rng_learn, no_learn_sig, no_index_replay, no_decompress, no_cells, "
        "no_novelty_order, no_sessions)\"");
    for (int i = 0; i < n_rows; i++) {
        ParityRow *r = &rows[i];
        fprintf(f,
            "{\"platform\":\"%s\",\"fixture\":\"%s\","
            "\"cmd_legacy\":\"%s\",\"cmd_kernel\":\"%s\","
            "\"budget\":{\"total\":%llu,\"path_steps\":%llu,\"fork_site_cap\":%llu},"
            "\"legacy\":{\"exec\":%zu,\"natural\":%zu,\"forced\":%zu,\"irq\":%zu,"
            "\"fingerprint\":\"%016llx\",\"saturations\":{\"capped\":%llu}},"
            "\"kernel\":{\"exec\":%zu,\"natural\":%zu,\"forced\":%zu,"
            "\"fingerprint\":\"%016llx\",\"saturations\":{\"total\":%llu}},"
            "\"missing\":%zu,\"forced_to_natural\":%zu,"
            "\"kernel_extra\":{\"natural\":%zu,\"forced\":%zu,"
            "\"reason\":\"kernel executes data-as-code the legacy engine cuts as nonsense "
            "(AUDIT-PARITE-MOTEURS.fr.md 5.2); reported, never a failure\"},"
            "\"injected_loss\":%s,\"index_space_ok\":%s}%s\n",
            r->platform, r->fixture, r->cmd_legacy, r->cmd_kernel,
            (unsigned long long)r->budget_total, (unsigned long long)r->budget_path,
            (unsigned long long)r->fork_cap,
            r->leg_nat + r->leg_for, r->leg_nat, r->leg_for, r->leg_irq,
            (unsigned long long)r->fp_legacy, (unsigned long long)r->sat_legacy,
            r->ker_nat + r->ker_for, r->ker_nat, r->ker_for,
            (unsigned long long)r->fp_kernel, (unsigned long long)r->sat_kernel,
            r->missing, r->for_nat_shift, r->extra_nat, r->extra_for,
            r->injected_loss ? "true" : "false", r->space_ok ? "true" : "false",
            i + 1 < n_rows ? "," : "");
    }
    fprintf(f, "]}\n");
    fclose(f);
    printf("audit-parity: bench record written to %s\n", jpath);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    // branch: both sides of a conditional plus a verbatim ROM->VRAM copy —
    // the minimal contract both engines must satisfy
    compare_gb(dir, "branch", 1, 0, 0, 0);

    // irq: a vblank handler forked by the legacy explorer and dispatched
    // naturally (plus the suppressed-side fork) by the kernel
    compare_gb(dir, "irq", 0, 1, 0, 0);

    // self-test: a kernel run with per-path steps cut to 4 proves less; the
    // comparator must report the injected loss, never swallow it
    compare_gb(dir, "branch", 0, 0, 1, 0);

    // self-test: a kernel coverage image truncated mid-fixture must be
    // reported as missing bytes, never folded into an admitted difference
    compare_gb(dir, "branch", 0, 0, 0, 1);

    // second ISA: the NES 6502 pair on the both-sides fixture
    compare_nes(dir, "nes_branch");

    json_write(dir);
    TEST_DONE("audit_parity");
    return 0;
}
