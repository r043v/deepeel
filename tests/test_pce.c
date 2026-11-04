// test_pce.c: the PC Engine bridge (m0boy's HuC6280 machine) and explorer
// contract.  The HuCard is built in-code (no fixture file): a reset flow
// mapping hardware, WRAM and a data page through TAM, a conditional on WRAM
// (one side natural, the other forced), data reads from the mapped page, a
// copy into the VDC data port and an idle loop that must be cut and counted.
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "../src/platform/pce/pce.h"
#include "../src/platform/pce/explore.h"
#include "../src/platform/pce/tas.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { ROM_SIZE = 0x4000 };

static void build(uint8_t *rom) {
    memset(rom, 0, ROM_SIZE);
    static const uint8_t code[] = {
        0x78,                   // 00 SEI
        0xD4,                   // 01 CSH
        0xA9, 0xFF, 0x53, 0x01, // 02 MPR0 = $FF (hardware)
        0xA9, 0xF8, 0x53, 0x02, // 06 MPR1 = $F8 (WRAM)
        0xA9, 0x01, 0x53, 0x04, // 0A MPR2 = page 1
        0xAD, 0x00, 0x20,       // 0E LDA $2000 (WRAM, zero)
        0xD0, 0x05,             // 11 BNE $18 (the forced side)
        0xAD, 0x00, 0x40,       // 13 LDA $4000 (cell $2000)
        0x80, 0x03,             // 16 BRA $1B
        0xAD, 0x08, 0x40,       // 18 LDA $4008 (cell $2008)
        0x03, 0x02,             // 1B ST0 #2 (VRAM write register)
        0xAD, 0x10, 0x40,       // 1D LDA $4010 (cell $2010)
        0x8D, 0x02, 0x00,       // 20 STA $0002 (VDC data)
        0x80, 0xFE,             // 23 BRA $23 (idle)
    };
    memcpy(rom, code, sizeof code);
    for (unsigned i = 0; i < 0x20; i++) rom[0x2000 + i] = (uint8_t)(0x40 + i);
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0; // reset -> $E000 (MPR7 = page 0)
}

static void test_decoder(void) {
    CHECK(dp_huc_length(0x53) == 2, "TAM length");
    CHECK(dp_huc_length(0x03) == 2, "ST0 length");
    CHECK(dp_huc_length(0x73) == 7, "TII length");
    CHECK(dp_huc_length(0xAD) == 3, "LDA abs length");
    CHECK(dp_huc_defined(0xEA), "NOP defined");
}

static void test_bridge(const uint8_t *rom) {
    // a 512-byte copier header is stripped
    uint8_t *h = calloc(1, ROM_SIZE + 512);
    memcpy(h + 512, rom, ROM_SIZE);
    CHECK(dp_pce_init(h, ROM_SIZE + 512), "init with header");
    CHECK(dp_pce_rom_size() == ROM_SIZE, "header stripped: %zu", dp_pce_rom_size());
    free(h);
    CHECK(dp_pce_cpu().pc == 0xE000, "reset vector: %04x", dp_pce_cpu().pc);
    for (int i = 0; i < 40; i++) dp_pce_step(NULL);
    CHECK(dp_pce_mpr(2) == 0x01, "TAM mapped page 1");
    CHECK(dp_pce_rom_index(0x4010) == 0x2010, "rom index through MPR2");
    size_t n = dp_pce_state_size();
    void *s = malloc(n);
    dp_pce_save(s);
    uint64_t a = dp_pce_state_hash();
    for (int i = 0; i < 1000; i++) dp_pce_step(NULL);
    dp_pce_load(s);
    CHECK(dp_pce_state_hash() == a, "snapshot round trip");
    free(s);
    dp_pce_close();
}

static void test_multitap(const uint8_t *rom) {
    CHECK(dp_pce_init(rom, ROM_SIZE), "init");
    dp_pce_input(0x01);                 // pad 1: I
    dp_pce_pad_n(1, 0x02);              // pad 2: II
    dp_pce_pad_n(2, 0x04);              // pad 3: Select
    uint8_t a = dp_pce_pad_port_read();
    dp_pce_multitap(1);
    dp_pce_pad_port_write(0x03);        // SEL+CLR: scan to pad 1
    dp_pce_pad_port_write(0x00);
    uint8_t p1 = dp_pce_pad_port_read();
    CHECK(dp_pce_tap_index() == 0 && (p1 & 0x0F) == (0x0F ^ 0x01), "pad 1 nibble %02x", p1);
    dp_pce_pad_port_write(0x01);        // SEL rises with CLR low: pad 2
    dp_pce_pad_port_write(0x00);
    CHECK(dp_pce_tap_index() == 1, "pad 2 selected");
    uint8_t p2 = dp_pce_pad_port_read();
    CHECK((p2 & 0x0F) == (0x0F ^ 0x02), "pad 2 nibble %02x", p2);
    dp_pce_pad_port_write(0x01); dp_pce_pad_port_write(0x00);
    CHECK(dp_pce_tap_index() == 2 && (dp_pce_pad_port_read() & 0x0F) == (0x0F ^ 0x04), "pad 3");
    dp_pce_pad_port_write(0x02);        // CLR: back to pad 1
    CHECK(dp_pce_tap_index() == 0, "CLR resets the scan");
    dp_pce_multitap(0);
    CHECK(dp_pce_pad_port_read() == a, "adapter off: the plain pad again");
    dp_pce_close();
}

static void test_explore(const uint8_t *rom) {
    DpPceOptions o = dp_pce_options();
    o.max_total = 2000000;
    o.max_steps = 20000;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(rom, ROM_SIZE, &o, r), "explore");
    if (!r->flags) { free(r); return; }
    CHECK(r->flags[0x00] & DP_PCE_NATURAL, "reset code natural");
    CHECK(r->flags[0x13] & DP_PCE_NATURAL, "taken side natural");
    CHECK(r->flags[0x18] & DP_PCE_FORCED, "other side forced");
    CHECK(r->flags[0x2000] & DP_PCE_DATA, "natural data read");
    CHECK(r->flags[0x2008] & DP_PCE_DATA, "forced data read");
    CHECK(r->flags[0x2010] & DP_PCE_COPIED, "ROM -> VDC copy");
    CHECK(r->copy_count >= 1, "copy record");
    CHECK(r->step_cuts + r->loop_cuts + r->global_cuts > 0, "idle loop cut and counted");
    CHECK(r->exec_outside == 0, "no execution outside ROM");
    dp_pce_results_free(r);
    free(r);
    {   // a one-slot queue and two successive branches: the second fork waits
        // aside, runs later, and the coverage is unchanged (§6)
        uint8_t *rom2 = malloc(ROM_SIZE);
        memcpy(rom2, rom, ROM_SIZE);
        static const uint8_t tail[] = { 0xAD, 0x00, 0x20, 0xD0, 0x02, 0xEA, 0xEA, 0x80, 0xFE };
        memcpy(rom2 + 0x23, tail, sizeof tail);
        DpPceOptions op = o;
        op.queue_cap = 1;
        DpPceResults *rp = calloc(1, sizeof *rp);
        CHECK(dp_pce_explore(rom2, ROM_SIZE, &op, rp), "tiny queue explore");
        if (rp->flags) {
            CHECK(rp->parked >= 1 && rp->parked_resumed == rp->parked && rp->parked_lost == 0, "parked then resumed");
            CHECK(rp->flags[0x13] & DP_PCE_NATURAL, "natural side kept");
            CHECK(rp->flags[0x18] & DP_PCE_FORCED, "forced side reached despite the queue wall");
            CHECK(rp->flags[0x28] & DP_PCE_FORCED, "second fork's side reached");
        }
        dp_pce_results_free(rp);
        free(rp);
        // a wall leaves states: they go to the disk lot, the next run resumes them (§6)
        char lp[] = "/tmp/dp_pce_lotXXXXXX"; int fd = mkstemp(lp); if (fd >= 0) close(fd);
        uint64_t left = 0, found = 0;
        for (uint64_t wall = 1; wall < 600 && !left; wall++) {
            found = wall;
            DpPceOptions ow = o; ow.queue_cap = 1; ow.lot_path = lp; ow.max_total = wall;
            unlink(lp);
            DpPceResults *rw = calloc(1, sizeof *rw);
            CHECK(dp_pce_explore(rom2, ROM_SIZE, &ow, rw), "walled run");
            left = rw->lot_pending;
            if (left) CHECK(rw->pending_dropped == 0, "wall leftovers saved, none dropped");
            dp_pce_results_free(rw); free(rw);
        }
        CHECK(left > 0, "a wall leaves states in the lot");
        {   // drain: only the saved states run (no root), the lot empties
            DpPceOptions od = o; od.queue_cap = 1; od.lot_path = lp; od.lot_drain = 1;
            DpPceResults *rd = calloc(1, sizeof *rd);
            CHECK(dp_pce_explore(rom2, ROM_SIZE, &od, rd), "drain run");
            CHECK(rd->lot_read >= left && rd->lot_pending == 0, "drain empties the lot");
            CHECK(access(lp, F_OK) != 0, "the drained lot file is removed");
            dp_pce_results_free(rd); free(rd);
            DpPceOptions ow = o; ow.queue_cap = 1; ow.lot_path = lp; ow.max_total = found;
            DpPceResults *rw = calloc(1, sizeof *rw);
            CHECK(dp_pce_explore(rom2, ROM_SIZE, &ow, rw), "walled run again");
            dp_pce_results_free(rw); free(rw);
        }
        DpPceOptions orr = o; orr.queue_cap = 1; orr.lot_path = lp;
        DpPceResults *rr = calloc(1, sizeof *rr);
        CHECK(dp_pce_explore(rom2, ROM_SIZE, &orr, rr), "resumed run");
        CHECK(rr->lot_read >= left && rr->lot_pending == 0, "the lot is resumed and emptied");
        CHECK(access(lp, F_OK) != 0, "the emptied lot file is removed");
        dp_pce_results_free(rr); free(rr);
        unlink(lp);
        free(rom2);
    }
}

// VCE: a ROM byte stored to the colour-data port is palette data
static void test_palette(const uint8_t *rom) {
    uint8_t *r2 = malloc(ROM_SIZE);
    memcpy(r2, rom, ROM_SIZE);
    static const uint8_t tail[] = { 0xAD, 0x14, 0x40, 0x8D, 0x04, 0x04, 0x80, 0xFE };
    memcpy(r2 + 0x23, tail, sizeof tail);
    DpPceOptions o = dp_pce_options();
    o.max_total = 2000000;
    o.max_steps = 20000;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(r2, ROM_SIZE, &o, r), "palette explore");
    if (r->flags) {
        CHECK(r->pal_flags && r->pal_flags[0x2014] == 1, "cell typed as palette");
        CHECK(r->pal_flags && !r->pal_flags[0x2010], "VDC copy source is not palette");
        CHECK(r->palette_bytes == 1 && r->vce_data_writes >= 1, "counted (%llu, %llu)",
              (unsigned long long)r->palette_bytes, (unsigned long long)r->vce_data_writes);
        int pal = 0;
        for (unsigned i = 0; i < r->copy_count; i++) pal |= r->copies[i].channel == 3;
        CHECK(pal, "copy record on the VCE channel");
    }
    dp_pce_results_free(r);
    free(r);
    free(r2);
}

// index inference: abs,X reads at indexes 0,2,3 of $4100 (gap at 1 declared)
// and 1,3 of the biased base $41FF (gap at 2 declared, element before the
// table and index 0 never declared)
static void test_index(const uint8_t *rom) {
    uint8_t *r2 = malloc(ROM_SIZE);
    memcpy(r2, rom, ROM_SIZE);
    static const uint8_t tail[] = { 0xA2, 0x00, 0xBD, 0x00, 0x41, 0xE8, 0xE8, 0xE0, 0x06, 0xD0, 0xF7,
                                    0xA2, 0x01, 0xBD, 0xFF, 0x41, 0xE8, 0xE8, 0xE0, 0x07, 0xD0, 0xF7,
                                    0x80, 0xFE };
    memcpy(r2 + 0x23, tail, sizeof tail);
    DpPceOptions o = dp_pce_options();
    o.max_total = 2000000;
    o.max_steps = 20000;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(r2, ROM_SIZE, &o, r), "index explore");
    if (r->flags) {
        CHECK(r->nidx == 2, "two sites (%u)", r->nidx);
        CHECK(r->idx_flags[0x2101] == 1 && r->idx_flags[0x2103] == 1, "gaps of the first table declared");
        CHECK(!r->idx_flags[0x2100] && !r->idx_flags[0x2102] && !r->idx_flags[0x2105], "observed or out-of-range bytes are not inferred");
        CHECK(r->idx_flags[0x2201] == 1 && r->idx_flags[0x2203] == 1, "gaps of the biased table declared");
        CHECK(!r->idx_flags[0x21FF] && !r->idx_flags[0x2200], "nothing before the biased index range");
        CHECK(r->idx_declared == 4 && r->idx_biased == 1, "counts (%u, %u)", r->idx_declared, r->idx_biased);
    }
    dp_pce_results_free(r);
    free(r);
    free(r2);
}

// --jobs 2: the secondary instance plays random sessions while the primary
// drains the power-on forks; the union covers what the serial run covers
static void test_jobs(const uint8_t *rom) {
    DpPceOptions o = dp_pce_options();
    o.max_total = 2000000;
    o.max_steps = 20000;
    o.play_frames = 30;
    o.play_sessions = 2;
    DpPceResults *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    CHECK(dp_pce_explore(rom, ROM_SIZE, &o, a), "serial run");
    CHECK(dp_pce_explore_jobs(rom, ROM_SIZE, &o, b, 2), "jobs 2 run");
    if (a->flags && b->flags) {
        size_t miss = 0;
        for (size_t i = 0; i < a->rom_size; i++)
            if ((a->flags[i] & (DP_PCE_NATURAL | DP_PCE_FORCED)) &&
                !(b->flags[i] & (DP_PCE_NATURAL | DP_PCE_FORCED))) miss++;
        CHECK(miss == 0, "jobs 2 covers every code cell of the serial run (%zu missing)", miss);
        CHECK(b->flags[0x18] & DP_PCE_FORCED, "forced side reached");
        CHECK(b->flags[0x2010] & DP_PCE_COPIED, "ROM -> VDC copy kept");
        CHECK(b->play_sessions_run == a->play_sessions_run, "sessions counted once (%llu vs %llu)",
              (unsigned long long)b->play_sessions_run, (unsigned long long)a->play_sessions_run);
        CHECK(b->section_count > 0, "sections built on the union");
    }
    dp_pce_results_free(a); dp_pce_results_free(b);
    free(a); free(b);
}

static void test_tas_identity(void) {
    extern void dp_sha1(const uint8_t *, size_t, uint8_t[20]);
    uint8_t rom[2048 + 512];
    for (unsigned i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i * 5);
    DpPceTas t;
    memset(&t, 0, sizeof t);
    CHECK(dp_pce_tas_identity(&t, rom, sizeof rom) == 3, "a movie without SHA-1 records no identity");
    uint8_t h[20];
    dp_sha1(rom, sizeof rom, h);
    for (int i = 0; i < 20; i++) snprintf(t.sha1 + 2 * i, 3, "%02X", h[i]);
    CHECK(dp_pce_tas_identity(&t, rom, sizeof rom) == 1, "whole-image SHA-1 matches");
    dp_sha1(rom + 512, sizeof rom - 512, h);
    for (int i = 0; i < 20; i++) snprintf(t.sha1 + 2 * i, 3, "%02x", h[i]);
    CHECK(dp_pce_tas_identity(&t, rom, sizeof rom) == 1, "headerless SHA-1 matches");
    rom[900] ^= 1;
    CHECK(dp_pce_tas_identity(&t, rom, sizeof rom) == 2, "a changed ROM is a different ROM");
}

// S6: a decoder called with a zero-page pointer taken from a table; the stream the ROM
// never calls is read through the replay
static void test_streams(void) {
    uint8_t *rom = calloc(1, ROM_SIZE);
    static const uint8_t code[] = {
        0x78, 0xD4,
        0xA9, 0xFF, 0x53, 0x01,
        0xA9, 0xF8, 0x53, 0x02,
        0xA9, 0x01, 0x53, 0x04,                          // MPR2 = page 1 at $4000
        0xA9, 0x00, 0x85, 0x10, 0xA9, 0x40, 0x85, 0x11, 0x20, 0x00, 0xE1,   // ptr $4000; jsr
        0xA9, 0x10, 0x85, 0x10, 0xA9, 0x40, 0x85, 0x11, 0x20, 0x00, 0xE1,   // ptr $4010; jsr
        0x80, 0xFE,
    };
    memcpy(rom, code, sizeof code);
    rom[0x100] = 0xB2; rom[0x101] = 0x10; rom[0x102] = 0x60;                  // lda ($10); rts
    rom[0x2000] = 5; rom[0x2010] = 6; rom[0x2030] = 7;
    static const uint8_t tab[] = { 0x00, 0x40, 0x10, 0x40, 0x30, 0x40 };
    memcpy(rom + 0x2100, tab, sizeof tab);
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;
    DpPceOptions o = dp_pce_options();
    o.max_total = 200000; o.max_steps = 2000;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(rom, ROM_SIZE, &o, r), "streams explore");
    CHECK(r->s6_decoders >= 1, "decoder detected");
    CHECK(r->s6_tables >= 1 && r->s6_runs >= 1, "table found and replayed");
    CHECK(r->flags && (r->flags[0x2030] & DP_PCE_DATA), "the unplayed stream was read");
    dp_pce_results_free(r);
    free(r); free(rom);
}

// S6 across ROM pages: (page, ptr) table; page 3's stream is only in the table
static void test_streams_banked(void) {
    uint8_t *rom = calloc(1, 0x8000);
    static const uint8_t code[] = {
        0x78, 0xD4,
        0xA9, 0xFF, 0x53, 0x01,
        0xA9, 0xF8, 0x53, 0x02,
        0xA9, 0x01, 0x53, 0x04,                          // MPR2 = page 1
        0xA9, 0x00, 0x85, 0x10, 0xA9, 0x40, 0x85, 0x11, 0x20, 0x00, 0xE1,
        0xA9, 0x02, 0x53, 0x04,                          // MPR2 = page 2
        0xA9, 0x10, 0x85, 0x10, 0xA9, 0x40, 0x85, 0x11, 0x20, 0x00, 0xE1,
        0x80, 0xFE,
    };
    memcpy(rom, code, sizeof code);
    rom[0x100] = 0xB2; rom[0x101] = 0x10; rom[0x102] = 0x60;
    rom[0x2000] = 5; rom[0x4010] = 6; rom[0x6020] = 7;
    static const uint8_t tab[] = { 1, 0x00, 0x40, 2, 0x10, 0x40, 3, 0x20, 0x40 };
    memcpy(rom + 0x200, tab, sizeof tab);
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;   // reset vector in page 0
    DpPceOptions o = dp_pce_options();
    o.max_total = 200000; o.max_steps = 2000;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(rom, 0x8000, &o, r), "banked streams explore");
    CHECK(r->s6_tables >= 1 && r->s6_runs >= 1, "banked table found and replayed");
    CHECK(r->flags && (r->flags[0x6020] & DP_PCE_DATA), "page 3's unplayed stream was read");
    dp_pce_results_free(r);
    free(r); free(rom);
}

// J1: random sessions restart from checkpoints earlier sessions kept
static void test_play_pool(void) {
    uint8_t *rom = calloc(1, ROM_SIZE);
    static const uint8_t code[] = { 0x78, 0x80, 0xFE };   // sei; bra *
    memcpy(rom, code, sizeof code);
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;
    DpPceOptions o = dp_pce_options();
    o.max_total = 50000000; o.max_steps = 2000; o.play_frames = 600; o.play_sessions = 4; o.play_pool = 8;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(rom, ROM_SIZE, &o, r), "pool explore");
    CHECK(r->pool_saved >= 2, "checkpoints kept");
    CHECK(r->pool_starts >= 1, "a session started from the pool");
    dp_pce_results_free(r);
    free(r); free(rom);
}

// RAM-code archive: bytes built by immediates in WRAM, then run
static void test_ramcode(void) {
    uint8_t *rom = calloc(1, ROM_SIZE);
    static const uint8_t code[] = {
        0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xA9, 0xF8, 0x53, 0x02,
        0xA9, 0xA9, 0x8D, 0x00, 0x22, 0xA9, 0x07, 0x8D, 0x01, 0x22, 0xA9, 0x60, 0x8D, 0x02, 0x22,
        0x20, 0x00, 0x22, 0xA9, 0xEA, 0x8D, 0x00, 0x22, 0x80, 0xFE,
    };
    memcpy(rom, code, sizeof code);
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;
    DpPceOptions o = dp_pce_options();
    o.max_total = 20000; o.max_steps = 500;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(rom, ROM_SIZE, &o, r), "ramcode explore");
    CHECK(r->ra.n >= 2, "RAM instructions archived (%u)", r->ra.n);
    int found = 0;
    for (uint32_t i = 0; i < r->ra.cap; i++)
        if (r->ra.e[i].key && r->ra.e[i].len == 2 && r->ra.e[i].pc == 0x2200 && r->ra.e[i].b[1] == 0x07) found = 1;
    CHECK(found, "LDA #$07 at $2200 kept");
    CHECK(r->smc_writes >= 1, "SMC store counted (%llu)", (unsigned long long)r->smc_writes);
    dp_pce_results_free(r);
    free(r); free(rom);
}

int main(void) {
    uint8_t *rom = malloc(ROM_SIZE);
    build(rom);
    test_decoder();
    test_bridge(rom);
    test_multitap(rom);
    test_explore(rom);
    test_palette(rom);
    test_index(rom);
    test_jobs(rom);
    test_streams();
    test_streams_banked();
    test_play_pool();
    test_ramcode();
    test_tas_identity();
    free(rom);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("pce: ok\n");
    return 0;
}
