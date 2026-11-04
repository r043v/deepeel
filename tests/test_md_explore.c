// Mega Drive milestone 2: 68000 decoder + forking explorer.
#define _DEFAULT_SOURCE
#include "../src/platform/md/md_registry.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "../src/cpu/m68k/m68k.h"
#include "../src/core/cart_md.h"
#include "../src/platform/md/explore.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *dir;

static int run(const char *name, uint64_t total, DpMdResults *r, DpMdCart *c) {
    char path[512], err[128];
    snprintf(path, sizeof path, "%s/%s.md", dir, name);
    if (dp_md_cart_load(c, path, err, sizeof err)) { fprintf(stderr, "load %s: %s\n", path, err); return -1; }
    DpMdOptions o;
    dp_md_options_default(&o);
    o.max_total = total;
    return dp_md_explore(c, &o, r);
}

static int executed(const DpMdResults *r, uint32_t a) {
    return (r->flags[a] & (DP_MDF_NATURAL | DP_MDF_FORCED)) != 0;
}

static void test_disasm(void) {
    unsigned bad = 0;
    for (unsigned op = 0; op < 0x10000; op++) {
        uint8_t b[12] = {op >> 8, op & 0xFF};
        DpM68kInsn in;
        unsigned n = dp_m68k_decode(b, sizeof b, 0x1000, &in);
        if (n < 2 || (n & 1) || n > 12 || n != in.len || !in.text[0]) bad++;
    }
    CHECK(bad == 0);
    DpM68kInsn in;
    uint8_t bne[] = {0x66, 0x10};
    CHECK(dp_m68k_decode(bne, 2, 0x200, &in) == 2 && in.cls == DP_M68K_BCC && in.has_target && in.target == 0x212);
    uint8_t dbf[] = {0x51, 0xC8, 0xFF, 0xFC};
    CHECK(dp_m68k_decode(dbf, 4, 0x300, &in) == 4 && in.cls == DP_M68K_DBCC && in.target == 0x2FE);
    uint8_t jmp[] = {0x4E, 0xF9, 0x00, 0x00, 0x12, 0x34};
    CHECK(dp_m68k_decode(jmp, 6, 0x200, &in) == 6 && in.cls == DP_M68K_JMP && in.has_target && in.target == 0x1234);
    CHECK(dp_m68k_decode(jmp, 4, 0x200, &in) == 0);   // bytes run out
}

int main(int argc, char **argv) {
    dir = argc > 1 ? argv[1] : ".";
    test_disasm();
    DpMdResults r;
    DpMdCart c;

    {
        char path[512], err[128];
        DpMdMovie movie;
        snprintf(path, sizeof path, "%s/fix_md_movie.gmv", dir);
        CHECK(dp_md_movie_load(path, &movie, err, sizeof err));
        CHECK(movie.frames == 2 && movie.pads[0] == 0x80 && movie.pads[1] == 0x08);
        snprintf(path, sizeof path, "%s/fix_md_loop.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o; dp_md_options_default(&o);
        o.movie = &movie; o.max_total = 100000; o.path_steps = 1000;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.movie_frames == 2 && r.sat_path == 0 && r.sat_visit == 0);
        dp_md_results_free(&r); dp_md_cart_free(&c); dp_md_movie_free(&movie);
        snprintf(path, sizeof path, "%s/fix_md_movie_state.gmv", dir);
        CHECK(!dp_md_movie_load(path, &movie, err, sizeof err));
    }

    // both outcomes of a conditional branch
    CHECK(run("fix_md_fork", 100000, &r, &c) == 0);
    CHECK(executed(&r, 0x204));        // natural side (fall-through)
    CHECK(executed(&r, 0x208));        // forced side
    CHECK(r.flags[0x208] & DP_MDF_FORCED);
    CHECK(r.forks >= 1);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_reader", 100000, &r, &c) == 0);
    CHECK(r.data_only_bytes == 2 && r.code_or_vram_bytes == r.exec_bytes);
    CHECK(r.observed_bytes == r.exec_bytes + 2);
    {
        unsigned i;
        for (i = 0; i < r.nreaders && r.readers[i].pc != 0x202; i++) {}
        CHECK(i < r.nreaders);
        if (i < r.nreaders) {
            CHECK(r.readers[i].lo == 0x400 && r.readers[i].hi == 0x401);
            CHECK(r.readers[i].natural_accesses == 2);
        }
    }
    dp_md_results_free(&r); dp_md_cart_free(&c);

    {
        char path[512], err[128];
        snprintf(path, sizeof path, "%s/fix_md_reader.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o; dp_md_options_default(&o); o.exclude_read_pc = 0x202;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.dread_bytes == 2 && r.observed_bytes == r.exec_bytes + 2);
        CHECK(r.filtered_observed_bytes == r.exec_bytes && !r.nonexcluded_reads[0x400]);
        dp_md_results_free(&r); dp_md_cart_free(&c);
        snprintf(path, sizeof path, "%s/fix_md_reader_overlap.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.filtered_observed_bytes == r.exec_bytes + 2 && r.nonexcluded_reads[0x400]);
        dp_md_results_free(&r); dp_md_cart_free(&c);
    }

    CHECK(run("fix_md_revisit", 100000, &r, &c) == 0);
    CHECK(r.forks >= 2); // includes the alternative to the already executed $202
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_pad", 100000, &r, &c) == 0);
    CHECK(r.pad_forks == 14 && r.paths == 15);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_pad6", 100000, &r, &c) == 0);
    CHECK((r.flags[0x251] & DP_MDF_DATA) && (r.flags[0x24E] & DP_MDF_DATA));   // D10: bits 0-3 read zero only on a six-button pad's fourth TH phase
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_fault", 100000, &r, &c) == 0);
    CHECK(r.faults == 1 && r.nfault_sites == 1 && r.fault_sites_overflow == 0);
    if (r.nfault_sites) {
        CHECK(r.fault_sites[0].code == 1 && r.fault_sites[0].pc == 0x200);
        CHECK(r.fault_sites[0].natural == 1 && r.fault_sites[0].count == 1);
        CHECK(r.fault_sites[0].trace_len == 1 && r.fault_sites[0].trace[0] == 0x200);
    }
    dp_md_results_free(&r); dp_md_cart_free(&c);

    {
        char path[512], err[128];
        snprintf(path, sizeof path, "%s/fix_md_pad.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o;
        dp_md_options_default(&o);
        o.queue_cap = 1; o.queue_max = 16; o.max_total = 100000;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.queue_growths == 4 && r.sat_queue == 0 && r.pad_forks == 14);
        dp_md_results_free(&r); dp_md_cart_free(&c);
    }

    {   // saturated queue: forks wait aside and run later, same coverage (§6)
        char path[512], err[128];
        snprintf(path, sizeof path, "%s/fix_md_pad.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o;
        dp_md_options_default(&o);
        o.queue_cap = 1; o.queue_max = 1; o.max_total = 100000;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.sat_queue > 0 && r.parked > 0 && r.parked_resumed == r.parked && r.parked_lost == 0 && r.pad_forks == 14);
        dp_md_results_free(&r); dp_md_cart_free(&c);
    }

    {   // a wall leaves states: they go to the disk lot, the next run resumes them (§6)
        char path[512], err[128], lp[] = "/tmp/dp_md_lotXXXXXX";
        snprintf(path, sizeof path, "%s/fix_md_pad.md", dir);
        int fd = mkstemp(lp); if (fd >= 0) close(fd);
        uint64_t left = 0;
        for (uint64_t wall = 1; wall < 2000 && !left; wall++) {
            CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
            DpMdOptions o; dp_md_options_default(&o);
            o.lot_path = lp; o.max_total = wall; unlink(lp);
            CHECK(dp_md_explore(&c, &o, &r) == 0);
            left = r.lot_pending;
            if (left) CHECK(r.dropped_pending == 0);
            dp_md_results_free(&r); dp_md_cart_free(&c);
        }
        CHECK(left > 0);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o; dp_md_options_default(&o);
        o.lot_path = lp; o.max_total = 100000;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.lot_read >= left && r.lot_pending == 0 && access(lp, F_OK) != 0);
        dp_md_results_free(&r); dp_md_cart_free(&c);

        // drain: the saved states only, the reset path is not run again
        for (uint64_t wall = 1; wall < 2000; wall++) {
            CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
            DpMdOptions ow; dp_md_options_default(&ow); ow.lot_path = lp; ow.max_total = wall; unlink(lp);
            CHECK(dp_md_explore(&c, &ow, &r) == 0);
            uint64_t l2 = r.lot_pending; dp_md_results_free(&r); dp_md_cart_free(&c);
            if (l2) break;
        }
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions od; dp_md_options_default(&od); od.lot_path = lp; od.lot_drain = 1; od.max_total = 100000;
        CHECK(dp_md_explore(&c, &od, &r) == 0);
        CHECK(r.lot_read >= 1 && r.lot_pending == 0);
        dp_md_results_free(&r); dp_md_cart_free(&c);
        unlink(lp);

        // F2 resume: the drain reloads the tree the walled run saved
        {
            char tp[] = "/tmp/dp_md_treeXXXXXX"; int tfd = mkstemp(tp); if (tfd >= 0) close(tfd);
            uint64_t saved = 0;
            for (uint64_t wall = 1; wall < 2000; wall++) {
                CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
                DpMdOptions ow; dp_md_options_default(&ow); ow.lot_path = lp; ow.tree_path = tp; ow.max_total = wall; unlink(lp);
                CHECK(dp_md_explore(&c, &ow, &r) == 0);
                uint64_t l2 = r.lot_pending; saved = r.tree_nodes; dp_md_results_free(&r); dp_md_cart_free(&c);
                if (l2) break;
            }
            CHECK(saved > 1);
            CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
            DpMdOptions od2; dp_md_options_default(&od2); od2.lot_path = lp; od2.tree_path = tp; od2.lot_drain = 1; od2.max_total = 100000;
            CHECK(dp_md_explore(&c, &od2, &r) == 0);
            CHECK(r.tree_loaded == saved && r.tree_nodes >= saved);
            dp_md_results_free(&r); dp_md_cart_free(&c);
            unlink(tp);
        }
        unlink(lp);
    }

    {
        char path[512], err[128];
        snprintf(path, sizeof path, "%s/fix_md_pad_effect.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o;
        dp_md_options_default(&o);
        o.fork_cap = 0; // only a real input change can take the exit
        o.max_total = 100000;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.pad_forks == 14 && executed(&r, 0x20c));
        CHECK(!(r.flags[0x20c] & DP_MDF_NATURAL));
        dp_md_results_free(&r); dp_md_cart_free(&c);
    }

    CHECK(run("fix_md_fork_fault", 100000, &r, &c) == 0);
    CHECK(r.faults >= 1 && r.nfault_sites >= 1);
    if (r.nfault_sites) CHECK(r.fault_sites[0].pc == 0x208 && r.fault_sites[0].origin == 0x202);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_fault_reg", 100000, &r, &c) == 0);
    CHECK(r.nfault_sites == 1);
    if (r.nfault_sites) CHECK(r.fault_sites[0].an[3] == 0x1234 && r.fault_sites[0].an_writer[3] == 0x200);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    {
        char path[512], err[128];
        snprintf(path, sizeof path, "%s/fix_md_watch.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o; dp_md_options_default(&o); o.watch_write = 0xff1001;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.nfault_sites == 1);
        if (r.nfault_sites) {
            CHECK(r.fault_sites[0].watch_pc == 0x200 && r.fault_sites[0].watch_addr == 0xff1000);
            CHECK(r.fault_sites[0].watch_size == 2 && r.fault_sites[0].watch_value == 0x1234);
        }
        dp_md_results_free(&r); dp_md_cart_free(&c);
    }

    // dbcc: loop exit and loop body both reached
    CHECK(run("fix_md_dbcc", 100000, &r, &c) == 0);
    CHECK(executed(&r, 0x206));
    CHECK(executed(&r, 0x204) && executed(&r, 0x205)); // DBcc displacement, including when branch is taken
    dp_md_results_free(&r); dp_md_cart_free(&c);

    // a self loop is bounded and its saturation is reported
    CHECK(run("fix_md_loop", 100000, &r, &c) == 0);
    CHECK(r.sat_visit + r.sat_path + r.sat_total >= 1);
    CHECK(r.steps < 100000);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    // Visit limits above 255 must not wrap and turn a loop into a path cut.
    {
        char path[512], err[128];
        snprintf(path, sizeof path, "%s/fix_md_loop.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o;
        dp_md_options_default(&o);
        o.visit_cap = 300;
        o.max_total = 100000;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.sat_visit == 1 && r.sat_total == 0 && r.steps < 1000);
        dp_md_results_free(&r); dp_md_cart_free(&c);
    }

    // A loop spanning several frames is bounded by the path budget, not by
    // accumulated visits from earlier frames.
    {
        char path[512], err[128];
        snprintf(path, sizeof path, "%s/fix_md_loop.md", dir);
        CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
        DpMdOptions o;
        dp_md_options_default(&o);
        o.visit_cap = 15000;
        o.path_steps = 50000;
        o.max_total = 100000;
        CHECK(dp_md_explore(&c, &o, &r) == 0);
        CHECK(r.sat_path == 1 && r.sat_visit == 0 && r.steps == 50000);
        dp_md_results_free(&r); dp_md_cart_free(&c);
    }

    // V-int enabled: an interrupt fork reaches the handler
    CHECK(run("fix_md_vint", 100000, &r, &c) == 0);
    CHECK(r.irq_forks >= 1);
    CHECK(executed(&r, 0x300));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_vint_masked", 100000, &r, &c) == 0);
    CHECK(r.irq_forks == 0 && !executed(&r, 0x300));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    // ROM -> VDP DMA is recorded
    CHECK(run("fix_md_dma", 100000, &r, &c) == 0);
    CHECK(r.ncopy >= 1);
    if (r.ncopy >= 1) { CHECK(r.copies[0].src == 0x400); CHECK(r.copies[0].len == 0x10); }
    CHECK((r.flags[0x400] & DP_MDF_DMA_SOURCE) && (r.flags[0x41f] & DP_MDF_DMA_SOURCE));
    CHECK((r.flags[0x400] & DP_MDF_DMA_READ) && (r.flags[0x41f] & DP_MDF_DMA_READ));
    CHECK(r.dma_read_bytes == 0x20);
    CHECK(!(r.flags[0x420] & DP_MDF_DMA_SOURCE));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_dma_fault", 100000, &r, &c) == 0);
    CHECK(r.faults == 1 && r.dma_read_bytes == 0x10);
    CHECK((r.flags[0xff0] & DP_MDF_DMA_READ) && (r.flags[0xfff] & DP_MDF_DMA_READ));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_cpu_vram", 100000, &r, &c) == 0);
    CHECK(r.cpu_vram_words == 4 && r.cpu_vram_bytes == 8);
    {   // D3: the explorer counts bus accesses per hardware register
        int k = dp_md_reg_find(0xC00000);
        CHECK(k >= 0 && k < 64 && r.reg_w[k] >= 4);   // VDC data port writes counted per register
    }
    CHECK(r.code_or_vram_bytes == r.exec_bytes + 8 && r.data_only_bytes == 0);
    CHECK((r.flags[0x400] & DP_MDF_CPU_VRAM) && (r.flags[0x407] & DP_MDF_CPU_VRAM));
    CHECK(!(r.flags[0x408] & DP_MDF_CPU_VRAM));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_z80", 300000, &r, &c) == 0);
    CHECK(r.z80_steps > 0 && r.z80_exec_ram > 0);
    CHECK(r.z80_copied_bytes == 13 && r.z80_code_bytes == 13 && r.z80_ram_copied == 13);
    CHECK((r.z80_flags[0x400] & (DP_MDZ_COPIED | DP_MDZ_CODE_RAM)) == (DP_MDZ_COPIED | DP_MDZ_CODE_RAM));
    CHECK(!(r.z80_flags[0x40D] & DP_MDZ_COPIED));
    CHECK(r.z80_data_bytes == 1 && (r.z80_flags[0x320] & (DP_MDZ_DATA | DP_MDZ_SAMPLE)) == (DP_MDZ_DATA | DP_MDZ_SAMPLE));
    CHECK(r.z80_sample_bytes == 1 && r.z80_music_bytes == 0 && r.z80_ym_writes == 1 && r.z80_banks_read == 1);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_cpu_vram_reg", 100000, &r, &c) == 0);
    CHECK(r.cpu_vram_words == 4 && r.cpu_vram_bytes == 8);
    CHECK((r.flags[0x400] & DP_MDF_CPU_VRAM) && (r.flags[0x407] & DP_MDF_CPU_VRAM));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_cpu_vram_gap", 100000, &r, &c) == 0);
    CHECK(r.cpu_vram_words == 0 && r.cpu_vram_bytes == 0);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_sram_overlay", 1000, &r, &c) == 0);
    CHECK(r.sram_reads >= 1 && r.sram_exec >= 1 && r.exec_outside >= r.sram_exec);
    CHECK(!(r.flags[0x200000] & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DATA)));
    CHECK(!(r.flags[0x200002] & DP_MDF_DATA));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    CHECK(run("fix_md_smc", 100000, &r, &c) == 0);
    CHECK(r.ram_exec >= 1 && r.ra.n >= 1 && r.smc_writes >= 1);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    {   // G1: the V-int vector target is a root with --seed-vectors only
        uint8_t rom[0x1000];
        memset(rom, 0, sizeof rom);
        memcpy(rom + 0x100, "SEGA MEGA DRIVE ", 16);
        rom[1] = 0xFF;                                          // SSP $FF0000
        rom[6] = 0x02;                                          // reset -> $200
        rom[0x7A] = 0x03;                                       // level 6 (V-int) -> $300
        rom[0x200] = 0x60; rom[0x201] = 0xFE;                   // bra *
        { static const uint8_t h[] = { 0x4E,0x71, 0x70,0x00, 0x66,0x04, 0x60,0xFE, 0x4E,0x71, 0x60,0xFE }; memcpy(rom + 0x300, h, sizeof h); }   // nop; moveq #0,d0; bne.s +4: the taken side ($30A) is the fork
        char err[128];
        for (int s = 0; s < 2; s++) {
            CHECK(dp_md_cart_from_mem(&c, rom, sizeof rom, err, sizeof err) == 0);
            DpMdOptions o; dp_md_options_default(&o);
            o.max_total = 100000; o.seed_vectors = s;
            CHECK(dp_md_explore(&c, &o, &r) == 0);
            if (s) { CHECK(r.vectors_seeded == 1 && executed(&r, 0x300)); CHECK((r.flags[0x300] & DP_MDF_FORCED) && !(r.flags[0x300] & DP_MDF_NATURAL)); CHECK((r.exec_kind[0x300] & DP_MDX_ENTRY) && (r.exec_kind[0x300] & DP_MDX_IRQ)); CHECK((r.exec_kind[0x200] & DP_MDX_ENTRY) && !(r.exec_kind[0x200] & DP_MDX_IRQ)); CHECK((r.flags[0x30A] & DP_MDF_FORCED) && (r.exec_kind[0x30A] & DP_MDX_IRQ)); }
            else CHECK(r.vectors_seeded == 0 && !executed(&r, 0x300));
            dp_md_results_free(&r); dp_md_cart_free(&c);
        }
    }

    puts(fails ? "md_explore: FAILED" : "md_explore: ok");
    return fails != 0;
}
