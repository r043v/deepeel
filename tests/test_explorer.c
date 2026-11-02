// test_explorer.c: end-to-end assertions on the synthetic fixtures.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/report/report.h"
#include "mach_util.h"
#include <unistd.h>
#include "test_util.h"

static DpCart *g_cart;
static DpResults res;
static DpMach *g_mach;

// runs the full pipeline on one fixture; the results stay in `res` until cleanup
static uint64_t g_wall; // 0 = default budget

static void run(const char *dir, const char *name) {
    char path[512];
    char err[256];
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
    if (g_wall) cfg.max_steps_total = g_wall;
    if (g_mach) dp_mach_close(g_mach);
    g_mach = mach_for(g_cart);
    DpExplorer *ex = dp_explorer_new(&cfg, &res, g_mach);
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

static const DpCopyRec *copy_of(uint8_t channel, uint32_t sbank, uint16_t slo, uint16_t shi, uint16_t dlo,
                                uint16_t dhi) {
    for (int i = 0; i < res.copy_count; i++) {
        const DpCopyRec *r = &res.copies[i];
        if (r->channel == channel && r->src_bank == sbank && r->src_lo <= slo && slo <= r->src_hi &&
            r->src_lo <= shi && shi <= r->src_hi && r->dst_lo <= dlo && dlo <= r->dst_hi && r->dst_lo <= dhi &&
            dhi <= r->dst_hi)
            return r;
    }
    return NULL;
}

static int bank_events_of(uint32_t old_bank, uint32_t new_bank) {
    int n = 0;
    for (int i = 0; i < res.bank_count; i++)
        if (res.bank_events[i].old_bank == old_bank && res.bank_events[i].new_bank == new_bank) n++;
    return n;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    // --- branch: both sides, verdicts, copy record ------------------------------
    run(dir, "branch");
    const DpSection *s154 = section_at(0, 0x154);
    CHECK(s154 && s154->cond_sites >= 1 && s154->cond_both >= 1); // both outcomes ran
    // the not-taken side bytes: only ever reached through the fork
    CHECK((res.exec_flags[0x156] & DP_XF_FORCED) && !(res.exec_flags[0x156] & DP_XF_NATURAL));
    const DpSection *s180 = section_at(0, 0x180);
    CHECK(s180 && s180->verdict == DP_V_REQUIRED && s180->copies >= 1);
    CHECK(copy_of(0, 0, 0x300, 0x30F, 0x8000, 0x800F) != NULL);
    CHECK(res.data_flags[0x300] & DP_DF_COPIED);
    CHECK(res.exec_flags[0x150] & DP_XF_ENTRY); // jump target flagged as entry
    CHECK(res.exec_hits[0x156] > 0);
    // real usage counts played sessions only: the fork explorer leaves it at 0
    {
        uint64_t played = 0;
        for (size_t k = 0; k < (size_t)res.banks * 0x4000; k++)
            played += res.play_exec_hits[k] + res.play_data_hits[k];
        CHECK(played == 0);
    }
    // the rom data never runs on the natural flow (forked wandering may touch it)
    CHECK(!(res.exec_flags[0x300] & DP_XF_NATURAL));

    // --- mbc5: bank timeline + cross-bank code -----------------------------------
    run(dir, "mbc5");
    CHECK(bank_events_of(0, 2) >= 1 && bank_events_of(2, 3) >= 1); // MBC5 boots on bank 0
    CHECK(res.exec_flags[dp_rom_index(2, 0x4000)] & DP_XF_NATURAL);  // bank 2 code ran
    CHECK(res.exec_flags[dp_rom_index(3, 0x8000)] & DP_XF_NATURAL);  // bank 3 code ran (offset 0)
    CHECK(res.exec_hits[dp_rom_index(2, 0x4002)] > 0);
    CHECK(section_at(2, 0x4000) && section_at(2, 0x4000)->verdict == DP_V_REQUIRED);
    // mapper registers in the io stats
    int romb0 = -1;
    for (int i = 0; i < res.io_count; i++)
        if (!strcmp(res.plat->io_regs[i].name, "ROMB0")) romb0 = i;
    CHECK(romb0 >= 0 && res.io[romb0].wr >= 2 && res.io[romb0].bank_events >= 2);

    // --- dma: OAM DMA record -------------------------------------------------------
    run(dir, "dma");
    CHECK(copy_of(1, 0, 0x3000, 0x309F, 0xFE00, 0xFE9F) != NULL);
    CHECK(res.data_flags[0x3000] & DP_DF_COPIED);
    int dmareg = -1;
    for (int i = 0; i < res.io_count; i++)
        if (!strcmp(res.plat->io_regs[i].name, "DMA")) dmareg = i;
    CHECK(dmareg >= 0 && res.io[dmareg].copies >= 1);

    // --- irq: fork dispatch + handler classification --------------------------------
    run(dir, "irq");
    CHECK(res.forks_irq >= 1);
    const DpSection *s40 = section_at(0, 0x40);
    CHECK(s40 && (s40->kind_bits & DP_XF_IRQ) && s40->verdict == DP_V_REQUIRED);
    // the handler's VRAM write happened in the forked dispatch
    CHECK(res.vram_write_pages[0] & 1);

    // --- keyirq: the joypad-IRQ key simulation ---------------------------------
    run(dir, "keyirq");
    CHECK(res.forks_key >= 1); // keys simulated, IF bit 4 armed with nothing pending

    // --- p1poll: keys read straight from $FF00, no IRQ -------------------------
    run(dir, "p1poll");
    CHECK(res.forks_p1 >= 1);                            // the read forked per key
    CHECK(res.exec_flags[0x170] & DP_XF_FORCED);         // handler reached with Start held
    CHECK(!(res.exec_flags[0x170] & DP_XF_NATURAL));     // never on the released-keys flow
    CHECK(res.vram_write_pages[0] & 1);                  // the handler's write happened

    // --- jumptable: JP HL reaches both handlers -------------------------------------
    run(dir, "jumptable");
    CHECK(section_at(0, 0x220) != NULL && section_at(0, 0x210) != NULL);
    CHECK(res.exec_hits[0x215] > 0 && res.exec_hits[0x225] > 0); // both handlers halted
    // provenance: HL proven at each jp hl, and the pointer table proven data
    dp_provenance(&res);
    CHECK(res.prov_jp_hl >= 3);                  // $0210, $0220 (imm) and $0240 (walk)
    CHECK(res.data_flags[0x230] & DP_DF_STATIC); // table bytes proven data
    CHECK(res.data_flags[0x231] & DP_DF_STATIC);
    CHECK(res.exec_hits[dp_rom_index(0, 0x240)] > 0); // the walk really landed there

    // --- loop: infinite loop contained ----------------------------------------------
    run(dir, "loop");
    CHECK(res.cycle_cuts >= 1);
    CHECK(res.steps_total <= 10 * 100000ull); // bounded, no runaway

    // --- unreach: data blob stays data, unreachable code stays unexplored -----------
    run(dir, "unreach");
    for (int i = 0x200; i < 0x300; i++)
        if (res.exec_flags[i] & DP_XF_NATURAL) {
            CHECK_MSG(0, "data blob on the natural flow");
            break;
        }
    for (int i = 0x200; i < 0x300; i++)
        if (res.static_flags[i] & DP_SF_PAD) {
            CHECK_MSG(0, "data blob misread as padding");
            break;
        }
    CHECK(!(res.exec_flags[0x400] & DP_XF_NATURAL)); // never on the natural flow

    // --- closure: a tiny wall leaves code unvisited; direct flow finds it ------------
    g_wall = 9;
    run(dir, "closure");
    g_wall = 0;
    {
        uint32_t k3 = dp_rom_index(2, 0x4030), k20 = dp_rom_index(2, 0x4020), k6 = dp_rom_index(2, 0x4006);
        CHECK(!(res.exec_flags[k3] & (DP_XF_NATURAL | DP_XF_FORCED))); // the wall really cut the run
        uint32_t marked = dp_static_closure(&res);
        CHECK(marked > 0 && res.closure_starts > 0);
        CHECK((res.exec_flags[k3] & DP_XF_STATIC) && (res.exec_flags[k3] & DP_XF_START));
        CHECK(res.exec_flags[k20] & DP_XF_STATIC);
        CHECK(res.exec_flags[k6] & DP_XF_STATIC);
        CHECK(res.clo_entry_n >= 1);
        // idempotent: a second pass finds nothing new
        CHECK(dp_static_closure(&res) == 0);
        DpBuf o;
        dp_buf_init(&o);
        dp_report_cov(&res, &o);
        int spec = 0;
        char *q = (char *)o.p, *end = (char *)o.p + o.len;
        while (q < end) {
            char *nl = memchr(q, '\n', (size_t)(end - q));
            if (!nl) break;
            unsigned b, a, f;
            if (*q != '#' && sscanf(q, "%x %x %x", &b, &a, &f) == 3 && b == 2 && a == 0x4030 && f == 6) spec++;
            q = nl + 1;
        }
        CHECK(spec == 1); // static entry: speculative (4) + entry (2), never executed (1)
        dp_buf_free(&o);
    }
    // direct flow never enters proven data
    run(dir, "unreach");
    dp_static_closure(&res);
    CHECK(!(res.exec_flags[0x200] & DP_XF_STATIC));

    // --- bankdata: reads through the banked window -----------------------------------
    run(dir, "bankdata");
    CHECK(res.data_flags[dp_rom_index(2, 0x4000)] & DP_DF_READ);
    CHECK(res.data_flags[dp_rom_index(2, 0x4007)] & DP_DF_READ);
    CHECK(copy_of(0, 2, 0x4000, 0x4007, 0x8000, 0x8007) != NULL);
    CHECK(bank_events_of(0, 2) >= 1); // MBC5 boots on bank 0

    // --- copy1: a pure 1:1 ROM->VRAM copy is proven verbatim --------------------------
    run(dir, "copy1");
    const DpCopyRec *cv = copy_of(0, 1, 0x4000, 0x40FF, 0x8000, 0x80FF);
    CHECK(cv != NULL && cv->verbatim);
    {
        int unmarked = 0;
        for (int i = 0x4000; i < 0x4100; i++)
            if (!(res.data_flags[i] & DP_DF_VERBATIM)) unmarked++;
        CHECK(unmarked == 0);
        // the cov export carries the proof as flag 8, never on an executed line
        DpBuf o;
        dp_buf_init(&o);
        dp_report_cov(&res, &o);
        int data_lines = 0, exec_data = 0;
        char *q = (char *)o.p, *end = (char *)o.p + o.len;
        while (q < end) {
            char *nl = memchr(q, '\n', (size_t)(end - q));
            if (!nl) break;
            if (*q != '#') {
                unsigned b, a, f;
                if (sscanf(q, "%x %x %x", &b, &a, &f) == 3 && (f & 8)) {
                    data_lines++;
                    if (f & 1) exec_data++;
                }
            }
            q = nl + 1;
        }
        CHECK(data_lines >= 0x100);
        CHECK(exec_data == 0);
        dp_buf_free(&o);
    }

    // --- copyx: a transformed copy is data, but not proven verbatim --------------------
    run(dir, "copyx");
    const DpCopyRec *cx = copy_of(0, 1, 0x4000, 0x40FF, 0x8000, 0x80FF);
    CHECK(cx != NULL && !cx->verbatim);
    CHECK(!(res.data_flags[0x4000] & DP_DF_VERBATIM));

    // --- nonsense: echo/OAM execution is evicted --------------------------------------
    run(dir, "nsecho");
    CHECK(res.nonsense_echo >= 1);
    CHECK(res.paths_run <= 3); // the nonsense path was cut at once

    // --- nonsense: execution inside verbatim-proven data is evicted -------------------
    // the proof only exists once the copying path has ended, so the jump into
    // the data lives on the not-taken side of a conditional the fork explores
    run(dir, "nsdata");
    CHECK(res.nonsense_data >= 1);
    CHECK(res.data_flags[0x200] & DP_DF_VERBATIM);

    // --- nonsense: RET with the stack outside WRAM/HRAM is evicted --------------------
    run(dir, "nsstack");
    CHECK(res.nonsense_stack >= 1);

    // --- static scan: a detected padding run is marked whole, head included ------
    {
        size_t n = (size_t)res.banks * 0x4000;
        uint32_t run = 0;
        size_t start = 0, checked = 0, heads = 0;
        for (size_t i = 0; i < n; i++) {
            uint8_t b = i < res.cart->rom_size ? res.cart->rom[i]
                                               : res.cart->rom[res.cart->rom_size - 1];
            if (b == 0x00 || b == 0xFF) {
                if (!run) start = i;
                run++;
                if (run == 12) { // well past the 8-byte detection threshold
                    for (size_t j = start; j < start + 8; j++)
                        if (!(res.static_flags[j] & DP_SF_PAD)) heads++;
                    checked++;
                }
            } else {
                run = 0;
            }
        }
        CHECK(checked >= 1);      // the zero-filled fixtures have long 00 runs
        CHECK(heads == 0);        // the 7 bytes before the detection point are marked too
    }

    // --- adaptive worklist: a full queue grows up to queue_max instead of ---------
    // dropping whole paths; the growth is counted and the kept paths really run
    {
        char p[512], err[256];
        snprintf(p, sizeof p, "%s/fix_branch.gb", dir);
        if (g_cart) dp_cart_free(g_cart);
        g_cart = dp_cart_load(p, err, sizeof err);
        if (!g_cart) exit(2);
        if (res.cart) dp_results_free(&res);
        memset(&res, 0, sizeof res);
        dp_results_init(&res, g_cart, dp_platform_gb());
        dp_static_scan(&res);
        DpConfig cfg;
        dp_config_defaults(&cfg);
        cfg.queue_cap = 2;
        cfg.queue_max = 2; // ceiling == cap: no growth possible
        cfg.fork_site_cap = 1000;
        cfg.no_covered_prune = 1;
        cfg.max_steps_total = 2000000;
        if (g_mach) dp_mach_close(g_mach);
        g_mach = mach_for(g_cart);
        DpExplorer *ex = dp_explorer_new(&cfg, &res, g_mach);
        dp_seed_gb(ex);
        dp_explore(ex);
        dp_explorer_free(ex);
        uint64_t paths_capped = res.paths_run;
        CHECK(res.queue_overflows >= 1); // the tiny queue really dropped paths

        // same run with growth allowed: the dropped paths run, the growth shows
        dp_results_free(&res);
        memset(&res, 0, sizeof res);
        dp_results_init(&res, g_cart, dp_platform_gb());
        dp_static_scan(&res);
        if (g_mach) dp_mach_close(g_mach);
        g_mach = mach_for(g_cart);
        cfg.queue_max = 4096; // far above this fixture's occupancy: no loss under it
        DpExplorer *ex2 = dp_explorer_new(&cfg, &res, g_mach);
        dp_seed_gb(ex2);
        dp_explore(ex2);
        dp_explorer_free(ex2);
        CHECK(res.queue_growths >= 1);        // the growth happened, counted
        CHECK(res.paths_run > paths_capped);  // the kept paths really ran
        CHECK(res.queue_overflows == 0);      // under the ceiling nothing is lost
    }

    // --- disk spool: the states a tiny worklist cannot hold wait on disk, ---------
    // a stopped run resumes exactly where it stopped (worklist + exact dedup)
    {
        char p[512], err[256], spool[512], exactf[512];
        snprintf(p, sizeof p, "%s/fix_branch.gb", dir);
        snprintf(spool, sizeof spool, "%s/spool_test.bin", dir);
        snprintf(exactf, sizeof exactf, "%s/exact_test.bin", dir);
        unlink(spool);
        unlink(exactf);
        if (g_cart) dp_cart_free(g_cart);
        g_cart = dp_cart_load(p, err, sizeof err);
        if (!g_cart) exit(2);
        DpConfig cfg;
        dp_config_defaults(&cfg);
        cfg.queue_cap = 2;
        cfg.queue_max = 2; // no RAM growth: everything overflows to the spool
        cfg.fork_site_cap = 1000;
        cfg.no_covered_prune = 1;
        cfg.max_steps_total = 2000000;

        // run 1: a hard stop after a few paths, the worklist is flushed out
        if (res.cart) dp_results_free(&res);
        memset(&res, 0, sizeof res);
        dp_results_init(&res, g_cart, dp_platform_gb());
        dp_static_scan(&res);
        if (g_mach) dp_mach_close(g_mach);
        g_mach = mach_for(g_cart);
        cfg.max_paths = 5;
        DpExplorer *ex = dp_explorer_new(&cfg, &res, g_mach);
        CHECK(dp_explorer_spool_attach(ex, spool, 0));
        dp_seed_gb(ex);
        dp_explore(ex); // the path wall stops it, the worklist is still full
        uint64_t paths_run1 = res.paths_run;
        CHECK(res.spool_writes >= 1); // the queue really overflowed to disk
        CHECK(res.queue_overflows == 0); // nothing was dropped
        uint64_t pend = dp_explorer_spool_flush(ex);
        CHECK(pend >= 1); // the stop left pending states for the next run
        CHECK(dp_explorer_exact_save(ex, exactf));
        dp_explorer_free(ex);

        // run 2: --resume — the spool and the dedup come back, the exploration
        // continues and runs strictly more paths than the stopped run
        dp_results_init(&res, g_cart, dp_platform_gb());
        dp_static_scan(&res);
        if (g_mach) dp_mach_close(g_mach);
        g_mach = mach_for(g_cart);
        cfg.max_paths = 0;
        DpExplorer *ex2 = dp_explorer_new(&cfg, &res, g_mach);
        CHECK(dp_explorer_spool_attach(ex2, spool, 1));
        CHECK(dp_explorer_exact_load(ex2, exactf));
        ex2->tree = NULL;
        dp_seed_gb(ex2);
        dp_explore(ex2);
        uint64_t pend2 = dp_explorer_spool_flush(ex2);
        dp_explorer_free(ex2);
        CHECK(res.spool_reads >= 1); // the spooled states really came back
        CHECK(res.paths_run > paths_run1); // the resumed run went further
        CHECK(pend2 == 0); // the resumed run drained the spool completely
        CHECK(res.queue_overflows == 0);
        unlink(spool);
        unlink(exactf);
    }

    // --- reports produce output -------------------------------------------------------
    {
        DpConfig cfg;
        dp_config_defaults(&cfg);
        DpBuf out;
        dp_buf_init(&out);
        dp_report_text(&res, &cfg, &out);
        CHECK(out.len > 100);
        dp_buf_free(&out);
        dp_buf_init(&out);
        dp_report_json(&res, &cfg, &out);
        CHECK(out.len > 100);
        CHECK(out.p[0] == '{');
        dp_buf_free(&out);
        dp_buf_init(&out);
        dp_report_cov(&res, &out);
        CHECK(out.len > 20);
        dp_buf_free(&out);
    }

    if (res.cart) dp_results_free(&res);
    if (g_mach) dp_mach_close(g_mach);
    if (g_cart) dp_cart_free(g_cart);
    TEST_DONE("test_explorer");
}
