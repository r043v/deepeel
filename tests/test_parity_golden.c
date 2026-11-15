// test_parity_golden.c: the kernel against frozen references.  The legacy-vs-kernel bench (test_audit_parity)
// proved, byte for byte, that the kernel covers everything the legacy explorers prove on the shared fixtures.
// The legacy proof sets were frozen under tests/golden/ (DP_PARITY_DUMP=<dir> test_audit_parity <build>;
// one byte per ROM index: 1 natural, 2 forced, 4 IRQ handler), so the guarantee survives the retirement of
// the legacy engines: the kernel alone must still cover every frozen byte (kernel-only techniques off, same
// budgets as the bench).  Not a hardware measurement: tool observations on generated fixtures.
#include "../src/core/cart.h"
#include "../src/explore/core.h"
#include "mach_util.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUDGET_TOTAL 2000000
#define BUDGET_PATH 1000
#define FORK_CAP 4

static uint8_t *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? (size_t)sz : 1);
    if (b) *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

// every frozen byte (natural, forced or IRQ) must be executed by the kernel (natural or forced)
static void check_cover(const char *tag, const uint8_t *gold, size_t gn, const DpCoreResult *kr) {
    size_t frozen = 0, missing = 0;
    CHECK_MSG(kr->rom_size == gn, "kernel index space matches the golden size");
    for (size_t i = 0; i < gn && i < kr->rom_size; i++) {
        if (!gold[i]) continue;
        frozen++;
        if (!(kr->flags[i] & (DP_CORE_NATURAL | DP_CORE_FORCED))) missing++;
    }
    printf("parity-golden %s: frozen %zu, missing %zu\n", tag, frozen, missing);
    CHECK_MSG(frozen > 0, "the golden reference is not empty");
    CHECK_MSG(missing == 0, "the kernel covers every frozen legacy proof");
}

static void gb(const char *dir, const char *gdir, const char *name) {
    char path[600], err[256];
    snprintf(path, sizeof path, "%s/fix_%s.gb", dir, name);
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    CHECK_MSG(cart, "gb fixture loads");
    if (!cart) return;
    DpMach *kmach = mach_for(cart);
    CHECK(kmach);
    dp_mach_reset(kmach);   // power-on state, like the CLI kernel path
    DpMachine *km = dp_machine_gb(kmach);
    CHECK(km && km->pc && km->set_pc);
    dp_machine_gb_source(km, cart->rom, cart->rom_size);
    DpCoreConfig kcfg;
    dp_core_config_default(&kcfg);
    kcfg.max_total = BUDGET_TOTAL;
    kcfg.max_path_steps = BUDGET_PATH;
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
    kcfg.no_sessions = 1;
    kcfg.attract_frames = 0;

    DpCoreResult kr;
    memset(&kr, 0, sizeof kr);
    CHECK(dp_core_explore(km, cart->rom_size, &kcfg, &kr));
    km->close(km);
    size_t gn = 0;
    snprintf(path, sizeof path, "%s/parity_gb_%s.golden", gdir, name);
    uint8_t *gold = load(path, &gn);
    CHECK_MSG(gold, "golden file loads");
    if (gold) check_cover(name, gold, gn, &kr);
    free(gold);
    dp_core_result_free(&kr);
}

static void nes(const char *dir, const char *gdir, const char *name) {
    char path[600];
    snprintf(path, sizeof path, "%s/fix_%s.nes", dir, name);
    size_t size = 0;
    uint8_t *rom = load(path, &size);
    CHECK_MSG(rom && size > 16, "nes fixture reads");
    if (!rom) return;
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
    size_t gn = 0;
    snprintf(path, sizeof path, "%s/parity_nes_%s.golden", gdir, name);
    uint8_t *gold = load(path, &gn);
    CHECK_MSG(gold, "golden file loads");
    if (gold) check_cover(name, gold, gn, &kr);
    free(gold);
    dp_core_result_free(&kr);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *gdir = argc > 2 ? argv[2] : "golden";
    gb(dir, gdir, "branch");
    gb(dir, gdir, "irq");
    nes(dir, gdir, "nes_branch");
    TEST_DONE("parity_golden");
    return 0;
}
