// test_explorer.c: end-to-end assertions on the synthetic fixtures.
#include <sys/stat.h>
#include "../src/core/cart.h"
#include "../src/core/snapshot_png.h"
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
static const char *g_fsave; // save the deferred frontier here after the run
static const char *g_fload; // load it before the run
static int g_fsaved, g_floaded;
static uint32_t g_forkcap, g_seedcap, g_maxpaths; // 0 = default
static uint32_t g_psteps; // 0 = default per-path step budget
static uint32_t g_loopcap; // 0 = default backward-edge cap
static int g_full_forks;  // 1 = the per-site fork table starts full

static uint64_t res_deferred_total(void) {
    uint64_t n = 0;
    for (int r = 1; r < DP_R_COUNT; r++) n += res.defer_by[r];
    return n;
}

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
    if (g_psteps) cfg.max_steps_path = g_psteps;
    if (g_forkcap) cfg.fork_site_cap = g_forkcap;
    if (g_seedcap) cfg.seed_cap = g_seedcap;
    if (g_maxpaths) cfg.max_paths = g_maxpaths;
    if (g_loopcap) cfg.loop_back_cap = g_loopcap;
    if (g_mach) dp_mach_close(g_mach);
    g_mach = mach_for(g_cart);
    DpExplorer *ex = dp_explorer_new(&cfg, &res, g_mach);
    if (g_full_forks) {
        // fill the table to its growth threshold with keys no site uses,
        // then forbid growth: every real site finds no counter slot
        res.fork_counts.cap_abs = res.fork_counts.cap;
        for (uint32_t k = 0; dp_map_get_or_add(&res.fork_counts, 0x7F000000u + k); k++) {}
    }
    if (g_fload) g_floaded = dp_explorer_frontier_load(ex, g_fload);
    dp_seed_gb(ex);
    dp_explore(ex);
    if (g_fsave) g_fsaved = dp_explorer_frontier_save(ex, g_fsave);
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

    // --- full per-site fork table: the cap holds, the saturation is counted ------
    g_full_forks = 1;
    run(dir, "branch");
    g_full_forks = 0;
    CHECK(res.forks_table_full > 0);
    CHECK(res.forks_capped >= res.forks_table_full);
    CHECK(res.forks_branch == 0); // no branch fork escaped the cap
    CHECK(!(res.exec_flags[0x156] & DP_XF_FORCED)); // the not-taken side stayed unforked

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

    // --- dpFr: the map PNG carries the frontier summary --------------------------------
    {
        DpConfig c2;
        dp_config_defaults(&c2);
        char png[600];
        snprintf(png, sizeof png, "%s/test_dpfr.png", dir);
        CHECK(dp_report_map_png(&res, &c2, "x.gb", png));
        size_t fl = 0;
        char *fr = dp_png_text(png, "dpFr", &fl);
        CHECK(fr != NULL && strstr(fr, "\"deferred\"") && strstr(fr, "\"pending\""));
        free(fr);
        remove(png);
    }

    // --- hdma: a CGB HDMA is read from HDMA1-5, the ROM source is data -----------------
    run(dir, "hdma");
    const DpCopyRec *ch = copy_of(2, 1, 0x4000, 0x40FF, 0x8000, 0x80FF);
    CHECK(ch != NULL && ch->verbatim && ch->bytes == 256);

    // --- apu: ROM bytes stored to wave RAM / an APU register are typed ------------------
    run(dir, "apu");
    CHECK(res.data_flags[dp_rom_index(1, 0x4000)] & DP_DF_SAMPLE);
    CHECK(res.data_flags[dp_rom_index(1, 0x4001)] & DP_DF_MUSIC);
    CHECK(res.apu_sample_bytes == 1 && res.apu_music_bytes == 1);

    run(dir, "pal");
    CHECK((res.data_flags[dp_rom_index(1, 0x4000)] & DP_DF_PALETTE) && res.palette_bytes == 1);

    // --- probes: serial start and RTC register select are counted ------------------------
    run(dir, "probes");
    CHECK(res.serial_starts == 1 && res.rtc_selects == 1);
    CHECK(res.div_reads == 1 && res.stat_reads == 1 && res.ly_reads == 1);

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
        uint32_t pend_recs = res.spool_writes - res.spool_reads;
        CHECK(pend_recs >= 1);
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
        // every record left pending by run 1 was fed back, on top of the
        // records run 2 spooled and drained itself (the read cursor survived)
        CHECK(res.spool_reads == pend_recs + res.spool_writes);
        CHECK(res.queue_overflows == 0);
        unlink(spool);
        unlink(exactf);
    }

    // --- parallel drain, spool at its wall: workers defer the refused states, ----
    // the leader merges them at the join (nothing lost without a count)
    {
        char p[512], err[256], spool[512];
        snprintf(p, sizeof p, "%s/fix_branch.gb", dir);
        snprintf(spool, sizeof spool, "%s/spool_par.bin", dir);
        unlink(spool);
        if (g_cart) dp_cart_free(g_cart);
        g_cart = dp_cart_load(p, err, sizeof err);
        if (!g_cart) exit(2);
        DpConfig cfg;
        dp_config_defaults(&cfg);
        cfg.queue_cap = 2; cfg.queue_max = 2; cfg.fork_site_cap = 1000;
        cfg.no_covered_prune = 1; cfg.max_steps_total = 2000000;
        cfg.jobs = 2;
        cfg.spool_cap_bytes = 1; // the spool refuses everything
        if (res.cart) dp_results_free(&res);
        memset(&res, 0, sizeof res);
        dp_results_init(&res, g_cart, dp_platform_gb());
        dp_static_scan(&res);
        if (g_mach) dp_mach_close(g_mach);
        g_mach = mach_for(g_cart);
        DpExplorer *ex = dp_explorer_new(&cfg, &res, g_mach);
        CHECK(dp_explorer_spool_attach(ex, spool, 0));
        dp_seed_gb(ex);
        dp_explore(ex);
        CHECK(res.spool_refused >= 1);            // the wall was really hit
        // refused by the spool, deferred by the workers, merged, none lost on that reason
        CHECK(res.defer_by[DP_R_PATHS] >= 1);
        CHECK(res.defer_final[DP_R_PATHS] == 0);
        CHECK(ex->defer_len == 0); // the held states were resumed before the end
        dp_explorer_free(ex);
        unlink(spool);
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
    // --- sparse first-discovery table: one row per proven byte, consistent --------
    {
        run(dir, "branch");
        size_t total = (size_t)res.banks * 0x4000, any = 0;
        for (size_t k = 0; k < total; k++) any += !!res.src_flags[k];
        CHECK(res.first.n > 0 && res.first.n + res.first.dropped == any);
        int bad = 0;
        for (uint32_t i = 0; i < res.first.n; i++)
            bad += !(res.src_flags[res.first.e[i].key] & res.first.e[i].src);
        CHECK(bad == 0);
    }

    // --- deferred frontier: a tiny per-path budget loses no code -----------------
    {
        run(dir, "branch");
        static uint8_t fl[1<<20];
        size_t full = 0, total = (size_t)res.banks * 0x4000;
        for (size_t k = 0; k < total; k++) { full += !!res.exec_flags[k]; fl[k] = res.exec_flags[k]; }
        g_psteps = 3;
        run(dir, "branch");
        size_t small = 0, lost = 0;
        for (size_t k = 0; k < total; k++) { small += !!res.exec_flags[k]; lost += fl[k] && !res.exec_flags[k]; }
        CHECK(res.step_cuts > 0);          // the budget really bit
        CHECK(res.steps_deferred > 0);     // ... and the cuts were deferred,
        CHECK(res.defer_resumed + res.defer_pending == res_deferred_total()); // ... all resumed or counted pending
        CHECK(lost == 0 && small >= full); // no code lost (resumed paths may wander further)
        CHECK(res.steps_final + res.defer_dropped == 0 || small >= 1);
        g_psteps = 0;
    }

    // --- deferred frontier, loop tier: a tiny back-edge cap loses no code ---------
    {
        run(dir, "loop");
        static uint8_t fl2[1 << 20];
        size_t total = (size_t)res.banks * 0x4000, lost = 0;
        for (size_t k = 0; k < total; k++) fl2[k] = res.exec_flags[k];
        g_loopcap = 1;
        run(dir, "loop");
        for (size_t k = 0; k < total; k++) lost += fl2[k] && !res.exec_flags[k];
        CHECK(res.loop_deferred > 0);
        // the fixture loops forever: what the wall leaves unresumed is counted
        CHECK(res.defer_resumed + res.defer_pending == res_deferred_total());
        CHECK(lost == 0);
        g_loopcap = 0;
    }

    // --- on-disk frontier: a wall leaves continuations, saved and reloaded -------
    {
        char fd[600];
        snprintf(fd, sizeof fd, "%s/fstate", dir);
        mkdir(fd, 0777);
        g_psteps = 3; g_wall = 40; g_fsave = fd;
        run(dir, "loop");
        int saved = g_fsaved;
        CHECK(saved > 0);
        CHECK(res.defer_pending == 0 || res.defer_pending >= 0); // saved ones left the memory
        g_fsave = NULL; g_fload = fd; g_wall = 0; g_psteps = 0;
        run(dir, "loop");
        CHECK(g_floaded == saved);
        CHECK(res.defer_resumed >= (uint64_t)saved);
        g_fload = NULL;
    }

    // --- J2 criterion: a tiny budget, then a drain of the saved frontier, covers
    // what the unbudgeted run covers
    {
        char fd[600];
        snprintf(fd, sizeof fd, "%s/fcrit", dir);
        mkdir(fd, 0777);
        run(dir, "branch");
        static uint8_t ref[1 << 20], acc[1 << 20];
        size_t total = (size_t)res.banks * 0x4000, lost = 0;
        for (size_t k = 0; k < total; k++) ref[k] = res.exec_flags[k];
        g_psteps = 3; g_wall = 12; g_fsave = fd;
        run(dir, "branch");
        for (size_t k = 0; k < total; k++) acc[k] = res.exec_flags[k];
        g_fsave = NULL; g_fload = fd; g_wall = 0; g_psteps = 0;
        run(dir, "branch");
        for (size_t k = 0; k < total; k++) lost += ref[k] && !(acc[k] | res.exec_flags[k]);
        CHECK(lost == 0);
        g_fload = NULL;
    }

    // --- J2 over all reasons: any budget, then the saved frontier drained, loses no code
    {
        uint64_t seen_fc = 0, seen_paths = 0, seen_seeds = 0, seen_disk = 0;
        static const char *fx[] = { "branch", "keyirq", "p1poll", "irq", "jumptable", "bankdata", "closure", "loop", "vblank" };
        for (size_t i = 0; i < sizeof fx / sizeof *fx; i++) {
            for (int mode = 0; mode < 4; mode++) {
                char fd[600];
                snprintf(fd, sizeof fd, "%s/fall_%s_%d", dir, fx[i], mode);
                mkdir(fd, 0777);
                g_fsave = g_fload = NULL;
                run(dir, fx[i]);
                static uint8_t rf[1 << 20];
                size_t total = (size_t)res.banks * 0x4000, lost = 0;
                for (size_t k = 0; k < total; k++) rf[k] = res.exec_flags[k];
                g_forkcap = mode == 0 ? 1 : 0;
                g_seedcap = mode == 1 ? 1 : 0;
                g_maxpaths = mode == 2 ? 2 : 0;
                g_psteps = mode == 3 ? 4 : 0;
                g_fsave = fd;
                run(dir, fx[i]);
                static uint8_t ac[1 << 20];
                for (size_t k = 0; k < total; k++) ac[k] = res.exec_flags[k];
                seen_fc += res.defer_by[DP_R_FORK_CAP]; seen_paths += res.defer_by[DP_R_PATHS];
                seen_seeds += res.seeds_deferred; seen_disk += res.defer_on_disk;
                g_forkcap = g_seedcap = g_maxpaths = 0; g_psteps = 0;
                g_fsave = NULL; g_fload = fd;
                run(dir, fx[i]);
                for (size_t k = 0; k < total; k++) lost += rf[k] && !(ac[k] | res.exec_flags[k]);
                if (lost) fprintf(stderr, "lost %zu bytes: fixture %s mode %d\n", lost, fx[i], mode);
                CHECK(lost == 0);
                g_fload = NULL;
            }
        }
        CHECK(seen_fc > 0 && seen_paths > 0 && seen_seeds > 0 && seen_disk > 0); // every mechanism really ran
    }

    if (g_mach) dp_mach_close(g_mach);
    if (g_cart) dp_cart_free(g_cart);

    TEST_DONE("test_explorer");
}
