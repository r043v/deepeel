// test_lr_parity.c: fork parity between the explorer's m0boy backend and the
// libretro level-B fork driver (dp_lr_explore) on the generated GB fixtures.
// An in-process native core (the m0boy GB machine behind the trace ABI) is
// driven by dp_lr_explore; the same machine is explored by dp_explore. The
// two sets of opcode starts must agree. Both are tool observations.
#include <stdlib.h>
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/mach/libretro.h"
#include "../src/cpu/sm83/sm83.h"
#include "mach_util.h"
#include "test_util.h"

// ---- the native core: one instruction per dp_lr_step -------------------------------
static DpMach *M;
static DpLrObservers OB;
static int g_force = -1, g_count;

static void on_step(DpMach *m, uint16_t pc, uint8_t op, int entry) {
    (void)entry;
    if (g_count++) dp_mach_cut(m, DP_CUT_STEPS); // second fetch: one instruction ran
    int cond = sm83_cond_of(op);
    if (cond >= 0) {
        if (OB.branch) OB.branch(OB.user, pc);
        if (g_force >= 0) { // make the condition evaluate to the requested outcome
            gbl_cpu_t *c = m->api->gbr;
            int y = cond; // 0 NZ, 1 Z, 2 NC, 3 C
            int want = g_force ? 1 : 0;
            if (y < 2) { // Z flag: zr == 0 <=> Z set
                int taken_if_zr_nonzero = (y == 0);
                c->zr = (want == taken_if_zr_nonzero) ? 1 : 0;
            } else {
                int taken_if_cf = (y == 3);
                c->cf = (want == taken_if_cf) ? 1 : 0;
            }
        }
        g_force = -1;
    }
    if (OB.exec) OB.exec(OB.user, pc);
}
static void on_read(DpMach *m, uint16_t a, uint8_t v) { (void)m; if (OB.read) OB.read(OB.user, a, v); }
static void on_write(DpMach *m, uint16_t a, uint8_t v) { (void)m; if (OB.write) OB.write(OB.user, a, v); }

static int t_step(void) {
    g_count = 0;
    dp_mach_attach(M, NULL, on_step, on_read, on_write, NULL);
    dp_mach_run(M);
    return g_count >= 2;
}
static void t_obs(const DpLrObservers *o) { if (o) OB = *o; else memset(&OB, 0, sizeof OB); }
static uint32_t t_pc(void) { return M->api->gbr->pc; }
static int t_force(int taken) { g_force = taken; return 1; }
static int64_t t_idx(uint32_t a) { return a < 0x8000 ? (int64_t)a : -1; } // ROM-only 32 KiB: window 1 = bank 1
static size_t s_size(void) { return dp_mach_state_bytes(M); }
static bool s_ser(void *d, size_t n) { if (n < s_size()) return false; dp_mach_save(M, d); return true; }
static bool s_unser(const void *d, size_t n) { if (n < s_size()) return false; dp_mach_load(M, d); return true; }

static void parity(const char *dir, const char *name) {
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/fix_%s.gb", dir, name);
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    CHECK_MSG(cart != NULL, path);
    if (!cart) return;
    size_t rn = cart->rom_size;

    // explorer side: reset seed only, no static closure / pointer scan
    DpResults res;
    memset(&res, 0, sizeof res);
    dp_results_init(&res, cart, dp_platform_gb());
    dp_static_scan(&res);
    DpConfig cfg;
    dp_config_defaults(&cfg);
    cfg.no_pointer_scan = cfg.no_closure = cfg.no_irq_fork = cfg.no_key_fork = cfg.no_p1_fork = 1;
    DpMach *m = mach_for(cart);
    CHECK(m != NULL);
    if (!m) return;
    DpExplorer *ex = dp_explorer_new(&cfg, &res, m);
    dp_seed_gb(ex);
    dp_explore(ex);
    dp_explorer_free(ex);
    uint8_t *want = calloc(rn, 1);
    for (size_t i = 0; i < rn; i++) want[i] = (res.exec_flags[i] & DP_XF_START) ? 1 : 0;
    // forced-only entries (vector seeds) are not reachable from the reset flow: keep the
    // reset-lineage subset by exploring the same way below; entries are compared as a subset

    // libretro side: same machine, same reset, the level-B fork driver
    M = m;
    dp_mach_reset(M);
    DpLrTraceApi t = { DP_LR_TRACE_VERSION, t_step, t_obs, t_pc, t_force, t_idx };
    DpLibretro *lr = dp_lr_open_native("m0boy-gb", s_size, s_ser, s_unser, &t);
    CHECK(lr != NULL);
    uint8_t *got = calloc(rn, 1);
    DpLrExploreLimits lim = { 200000, 50000000, 65536, 2 };
    DpLrExploreStats st;
    CHECK(dp_lr_explore(lr, got, rn, &lim, &st));
    size_t miss = 0, extra = 0, nwant = 0, ngot = 0;
    for (size_t i = 0; i < rn; i++) {
        if (i < 0x100) continue; // RST/IRQ vectors: the explorer seeds them cold (not a fork), out of scope here
        if (res.static_flags[i] & DP_SF_PAD) continue; // the explorer does not record executed padding
        nwant += want[i];
        ngot += got[i];
        if (want[i] && !got[i]) { miss++; printf("  only explorer: %04zX\n", i); }
        if (got[i] && !want[i]) { extra++; printf("  only libretro: %04zX\n", i); }
    }
    printf("parity %s: explorer %zu starts, libretro %zu starts, missing %zu, extra %zu, "
           "lr forks %llu, saturation cuts %llu/%llu/%llu\n",
           name, nwant, ngot, miss, extra, (unsigned long long)st.forks,
           (unsigned long long)st.cut_path_steps, (unsigned long long)st.cut_total,
           (unsigned long long)st.dropped_items);
    CHECK(ngot > 0);
    CHECK_MSG(miss == 0 && extra == 0, name);
    dp_lr_close(lr);
    free(want);
    free(got);
    dp_results_free(&res);
    dp_mach_close(m);
    dp_cart_free(cart);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    static const char *const names[] = { "branch", "loop", "vblank", "irq", "copy1", "copyx", "unreach", "jumptable", "nsdata", "pal", "probes" };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) parity(argv[1], names[i]);
    TEST_DONE("test_lr_parity");
}
