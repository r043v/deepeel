// test_nes.c: the NES machine bridge (m0boy's runtime) and explorer
// contract.  ROMs are built in-code (no fixture files): an NROM image
// exercising reset flow, both sides of a conditional, an OAM DMA from ROM
// and an idle loop; an UxROM image exercising bank switching; snapshot
// round trips; a full explorer run over the branching image; a TAS movie
// loading.  Unsupported mappers are refused with a message.
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "../src/mach/nesmach.h"
#include "../src/platform/nes/explore.h"
#include "../src/platform/nes/tas.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// place bytes at PRG address $8000 + bank * 0x4000 + off
static void put(uint8_t *rom, unsigned bank, unsigned off, ...) {
    va_list ap;
    va_start(ap, off);
    unsigned a = 16 + bank * 0x4000 + off;
    for (;;) {
        int b = va_arg(ap, int);
        if (b < 0) break;
        rom[a++] = (uint8_t)b;
    }
    va_end(ap);
}

static void set_header(uint8_t *rom, unsigned prg16, unsigned chr8, unsigned mapper) {
    memcpy(rom, "NES\x1a", 4);
    rom[4] = (uint8_t)prg16;
    rom[5] = (uint8_t)chr8;
    rom[6] = (uint8_t)((mapper & 0x0F) << 4);
    rom[7] = (uint8_t)(mapper & 0xF0);
}

static void set_vector(uint8_t *rom, unsigned bank, unsigned addr, unsigned target) {
    put(rom, bank, 0x3FFA + (addr - 0xFFFA), target & 0xFF, target >> 8, -1);
}

static void test_machine_nrom(void) {
    uint8_t *rom = calloc(1, 16 + 2 * 0x4000 + 1 * 0x2000);
    set_header(rom, 2, 1, 0);
    // $8000 reset: LDA $9000 / BNE side B / LDA #$11 / JMP join
    put(rom, 0, 0x0000, 0xAD, 0x00, 0x90,       // $8000 LDA $9000
                        0xD0, 0x05,             // $8003 BNE $800A
                        0xA9, 0x11,             // $8005 side A: LDA #$11
                        0x4C, 0x0C, 0x80,       // $8007 JMP $800C
                        0xA9, 0x22, -1);            // $800A side B: LDA #$22
    put(rom, 0, 0x000C, 0x85, 0x10,             // $800C STA $10
                        0xA9, 0x80,             // $800E LDA #$80
                        0x8D, 0x14, 0x40,       // $8010 STA $4014 (OAM DMA page $80)
                        0x4C, 0x13, 0x80, -1);      // $8013 JMP $8013 (idle)
    put(rom, 0, 0x1000, 0x01);                  // $9000: flag byte, taken side
    set_vector(rom, 1, 0xFFFA, 0x8000);
    set_vector(rom, 1, 0xFFFC, 0x8000);
    set_vector(rom, 1, 0xFFFE, 0x8000);

    char err[256];
    DpNesMach *m = dp_nesmach_open(rom, 16 + 2 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(m, "NROM open: %s", err);
    dp_nesmach_reset(m);
    for (unsigned guard = 0; guard < 100 && dp_nesmach_pc(m) != 0x8013; guard++)
        dp_nesmach_run(m, 1); // one instruction per run
    CHECK(dp_nesmach_pc(m) == 0x8013, "reached the idle loop");
    CHECK(dp_nesmach_read(m, 0x0010) == 0x22, "taken side stored 0x%02X",
          dp_nesmach_read(m, 0x0010));
    // OAM filled from ROM page $80 ($8000..): first byte is the LDA opcode
    CHECK(dp_nesmach_oam(m)[0] == 0xAD, "OAM DMA copied ROM page $80");
    int64_t flag_cell = dp_nesmach_rom_index(m, 0x9000);
    CHECK(flag_cell == 0x1000, "flag byte cell %lld", (long long)flag_cell);
    dp_nesmach_close(m);
    free(rom);
}

static void test_machine_uxrom(void) {
    uint8_t *rom = calloc(1, 16 + 4 * 0x4000 + 1 * 0x2000);
    set_header(rom, 4, 0, 2);
    put(rom, 0, 0x0000, 0xA9, 0x02,             // LDA #2
                        0x8D, 0x00, 0x80,       // STA $8000 (bank select)
                        0xAD, 0x00, 0x80, -1);      // LDA $8000 (banked read)
    set_vector(rom, 3, 0xFFFC, 0x8000);
    char err[256];
    DpNesMach *m = dp_nesmach_open(rom, 16 + 4 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(m, "UxROM open: %s", err);
    CHECK(dp_nesmach_rom_index(m, 0xC000) == 3 * 0x4000, "UxROM fixes the last bank");
    CHECK(dp_nesmach_rom_index(m, 0x8000) == 0, "boot bank is 0");
    CHECK(dp_nesmach_bank_count(m) == 4, "four switchable banks");
    dp_nesmach_reset(m);
    for (unsigned guard = 0; guard < 10 && dp_nesmach_pc(m) != 0x8005; guard++)
        dp_nesmach_run(m, 1);
    CHECK(dp_nesmach_rom_index(m, 0x8000) == 2 * 0x4000, "bank 2 shows at $8000");
    CHECK(dp_nesmach_read(m, 0x8000) == 0x00, "banked read returns bank 2 content");
    dp_nesmach_close(m);
    free(rom);
}

static void test_unsupported_mappers(void) {
    uint8_t *rom = calloc(1, 16 + 4 * 0x4000 + 1 * 0x2000);
    set_header(rom, 4, 1, 1);
    char err[256];
    DpNesMach *m = dp_nesmach_open(rom, 16 + 4 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(!m, "MMC1 refused");
    CHECK(err[0], "a message explains why");
    rom[6] = (5 & 0x0F) << 4; // mapper 5 (MMC5)
    m = dp_nesmach_open(rom, 16 + 4 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(!m, "MMC5 refused");
    free(rom);
}

// CNROM: a write to $8000+ picks the 8 KiB CHR bank; AxROM: a 32 KiB PRG bank
static void test_cnrom_axrom(void) {
    char err[256];
    unsigned sz = 16 + 2 * 0x4000 + 4 * 0x2000;
    uint8_t *rom = calloc(1, sz);
    set_header(rom, 2, 4, 3);
    for (unsigned b = 0; b < 4; b++) rom[16 + 2 * 0x4000 + b * 0x2000] = (uint8_t)(0xA0 + b);
    // LDA #2 / STA $8000 / JMP self
    put(rom, 0, 0, 0xA9, 0x02, 0x8D, 0x00, 0x80, 0x4C, 0x05, 0x80, -1);
    set_vector(rom, 1, 0xFFFC, 0x8000);
    DpNesMach *m = dp_nesmach_open(rom, sz, -1, err, sizeof err);
    CHECK(m, "CNROM opens: %s", err);
    if (m) {
        dp_nesmach_reset(m);
        unsigned o, n;
        CHECK(dp_nesmach_mapper_write(m, 2, &o, &n) == 2 && o == 0 && n == 2, "chr bank 0 -> 2");
        dp_nesmach_run(m, 3);
        CHECK(dp_nesmach_mapper_write(m, 3, &o, &n) == 2 && o == 2, "register latched");
        static uint8_t st[65536];
        dp_nesmach_save(m, st);
        dp_nesmach_reset(m);
        CHECK(dp_nesmach_mapper_write(m, 1, &o, &n) == 2 && o == 0, "reset clears the bank");
        dp_nesmach_load(m, st);
        CHECK(dp_nesmach_mapper_write(m, 1, &o, &n) == 2 && o == 2, "state carries the bank");
        dp_nesmach_close(m);
    }
    free(rom);
    // CNROM CHR typing: BG on, switch to CHR bank 2, tile 0 of bank 2 is referenced
    rom = calloc(1, sz);
    set_header(rom, 2, 4, 3);
    put(rom, 0, 0, 0xA9, 0x08, 0x8D, 0x01, 0x20, 0xA9, 0x02, 0x8D, 0x00, 0x80,
                   0x4C, 0x0C, 0x80, -1);
    set_vector(rom, 1, 0xFFFC, 0x8000);
    m = dp_nesmach_open(rom, sz, -1, err, sizeof err);
    CHECK(m, "CNROM (typing) opens");
    if (m) {
        dp_nesmach_reset(m);
        dp_nesmach_run(m, 5);
        static uint64_t t[4096];
        size_t n = dp_nesmach_chr_tiles(m, t, 4096);
        CHECK(dp_nesmach_chr_size(m) == 4 * 8192u, "chr size");
        CHECK(n == 1920 && (t[0] >> 1) == 2 * 8192u && !(t[0] & 1), "bg tiles on bank 2 (%zu, %llu)", n,
              (unsigned long long)(t[0] >> 1));
        dp_nesmach_close(m);
    }
    free(rom);
    // AxROM, 64 KiB PRG = 2 banks of 32 KiB
    sz = 16 + 4 * 0x4000;
    rom = calloc(1, sz);
    set_header(rom, 4, 0, 7);
    put(rom, 0, 0, 0xA9, 0x01, 0x8D, 0x00, 0x80, 0x4C, 0x05, 0x80, -1);
    put(rom, 2, 0, 0x5A, -1);                  // bank 1 starts with $5A
    set_vector(rom, 1, 0xFFFC, 0x8000);
    set_vector(rom, 3, 0xFFFC, 0x8000);
    m = dp_nesmach_open(rom, sz, -1, err, sizeof err);
    CHECK(m, "AxROM opens: %s", err);
    if (m) {
        dp_nesmach_reset(m);
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 0, "bank 0 at $8000");
        dp_nesmach_run(m, 1);  // LDA #1
        unsigned o, n;
        CHECK(dp_nesmach_mapper_write(m, 1, &o, &n) == 1 && o == 0 && n == 2, "slot 0 -> 2");
        dp_nesmach_run(m, 1);  // STA $8000
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 2 * 0x4000, "32 KiB bank 1 at $8000");
        CHECK(dp_nesmach_rom_index(m, 0xC000) == 3 * 0x4000, "and its upper half at $C000");
        CHECK(dp_nesmach_read(m, 0x8000) == 0x5A, "bank 1 content");
        dp_nesmach_close(m);
    }
    free(rom);
}

static void test_snapshot(void) {
    uint8_t *rom = calloc(1, 16 + 2 * 0x4000 + 1 * 0x2000);
    set_header(rom, 2, 1, 0);
    put(rom, 0, 0x0000, 0xA9, 0x42, 0x85, 0x20, 0x4C, 0x05, 0x80);
    set_vector(rom, 1, 0xFFFC, 0x8000);
    char err[256];
    DpNesMach *m = dp_nesmach_open(rom, 16 + 2 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(m, "init");
    dp_nesmach_reset(m);
    dp_nesmach_run(m, 1); // LDA #$42
    static uint8_t state[65536];
    CHECK(dp_nesmach_state_size() <= sizeof(state), "snapshot fits (%zu)",
          dp_nesmach_state_size());
    dp_nesmach_save(m, state);
    uint64_t h1 = dp_nesmach_state_hash(state);
    dp_nesmach_run(m, 1); // STA $20
    CHECK(dp_nesmach_read(m, 0x0020) == 0x42, "store executed");
    dp_nesmach_load(m, state);
    CHECK(dp_nesmach_read(m, 0x0020) == 0, "load rewinds RAM");
    CHECK(dp_nesmach_pc(m) == 0x8002, "load rewinds PC: %04X", dp_nesmach_pc(m));
    uint8_t state2[65536];
    dp_nesmach_save(m, state2);
    CHECK(dp_nesmach_state_hash(state2) == h1, "restored state hashes back");
    dp_nesmach_run(m, 1);
    CHECK(dp_nesmach_read(m, 0x0020) == 0x42, "execution continues after load");
    dp_nesmach_close(m);
    free(rom);
}

static void test_explorer(void) {
    uint8_t *rom = calloc(1, 16 + 2 * 0x4000 + 1 * 0x2000);
    set_header(rom, 2, 1, 0);
    put(rom, 0, 0x0000, 0xAD, 0x00, 0x90,       // $8000 LDA $9000
                        0xD0, 0x05,             // $8003 BNE $800A
                        0xA9, 0x11,             // $8005 side A: LDA #$11
                        0x4C, 0x00, 0x81,       // $8007 JMP $8100 (side A region)
                        0xA9, 0x22, -1);        // $800A side B: LDA #$22
    put(rom, 0, 0x000C, 0x85, 0x10,             // $800C STA $10 (join)
                        0xA9, 0x80,             // $800E LDA #$80
                        0x8D, 0x14, 0x40,       // $8010 STA $4014
                        0x4C, 0x13, 0x80, -1);  // $8013 JMP $8013 idle
    put(rom, 0, 0x0100, 0xA9, 0x33,             // $8100 side A only: LDA #$33
                        0x4C, 0x0C, 0x80, -1);  // $8102 JMP $800C
    put(rom, 0, 0x1000, 0x01, -1);              // $9000 flag: BNE taken
    set_vector(rom, 1, 0xFFFA, 0x8000);
    set_vector(rom, 1, 0xFFFC, 0x8000);
    set_vector(rom, 1, 0xFFFE, 0x8000);

    DpNesResults r;
    DpNesOptions o = dp_nes_options();
    o.max_steps = 2000;
    {   // a queue of one slot: the fork waits aside, runs later, same coverage (§6)
        DpNesResults rp;
        DpNesOptions op = o;
        op.queue_cap = 1;
        CHECK(dp_nes_explore(rom, 16 + 2 * 0x4000 + 0x2000, -1, &op, &rp), "tiny queue runs");
        CHECK(rp.parked >= 1 && rp.parked_resumed == rp.parked && rp.parked_lost == 0, "parked then resumed");
        CHECK(rp.flags[0x0005] && rp.flags[0x000A] && rp.forks >= 1, "both sides reached despite the queue wall");
        dp_nes_results_free(&rp);
    }
    {   // a wall leaves states: they go to the disk lot and the next run resumes them (§6)
        char lp[] = "/tmp/dp_nes_lotXXXXXX"; int fd = mkstemp(lp); if (fd >= 0) close(fd);
        unlink(lp);
        uint64_t left = 0, found = 0;
        for (uint64_t wall = 1; wall < 400 && !left; wall++) {
            DpNesResults rw; DpNesOptions ow = o; found = wall;
            ow.lot_path = lp; ow.max_total = wall;
            unlink(lp);
            CHECK(dp_nes_explore(rom, 16 + 2 * 0x4000 + 0x2000, -1, &ow, &rw), "walled run");
            left = rw.lot_pending;
            if (left) {  CHECK(rw.pending_dropped == 0 && rw.lot_written >= left, "wall leftovers saved, none dropped"); }
            dp_nes_results_free(&rw);
        }
        CHECK(left > 0, "a wall leaves states in the lot");
        {   // drain: only the saved states run (no root), the lot empties
            DpNesResults rd; DpNesOptions od = o; od.lot_path = lp; od.lot_drain = 1;
            CHECK(dp_nes_explore(rom, 16 + 2 * 0x4000 + 0x2000, -1, &od, &rd), "drain run");
            CHECK(rd.lot_read >= left && rd.lot_pending == 0, "drain empties the lot");
            CHECK(access(lp, F_OK) != 0, "the drained lot file is removed");
            dp_nes_results_free(&rd);
            DpNesResults rw; DpNesOptions ow = o; ow.lot_path = lp; ow.max_total = found;
            CHECK(dp_nes_explore(rom, 16 + 2 * 0x4000 + 0x2000, -1, &ow, &rw), "walled run again");
            dp_nes_results_free(&rw);
        }
        DpNesResults rr; DpNesOptions orr = o; orr.lot_path = lp;
        CHECK(dp_nes_explore(rom, 16 + 2 * 0x4000 + 0x2000, -1, &orr, &rr), "resumed run");
        CHECK(rr.lot_read >= left && rr.lot_pending == 0, "the lot is resumed and emptied");
        CHECK(access(lp, F_OK) != 0, "the emptied lot file is removed");
        dp_nes_results_free(&rr);
        unlink(lp);
    }
    CHECK(dp_nes_explore(rom, 16 + 2 * 0x4000 + 0x2000, -1, &o, &r), "explore runs");
    CHECK(r.paths >= 2, "both branch sides explored: %llu paths", (unsigned long long)r.paths);
    CHECK(r.forks >= 1, "at least one fork");
    // both sides' code landed on physical cells
    unsigned off_a = 0x0005, off_b = 0x000A; // bank 0 offsets of side A / B
    CHECK(r.flags[off_a] & DP_NES_NATURAL || r.flags[off_a] & DP_NES_FORCED, "side A executed");
    CHECK(r.flags[off_b] & DP_NES_NATURAL || r.flags[off_b] & DP_NES_FORCED, "side B executed");
    // the flag byte was read as data
    CHECK(r.flags[0x1000] & DP_NES_DATA, "flag byte flagged data");
    // OAM DMA from a ROM page produced a copy record
    CHECK(r.oam_copies >= 1, "OAM DMA counted");
    CHECK(r.copy_count >= 1 && r.copies[0].channel == 1, "copy record channel");
    CHECK(r.copies[0].source == 0, "DMA source cell is page $80 base");
    // a register write landed in the log
    unsigned dma_writes = 0;
    for (unsigned i = 0; i < r.register_count; i++)
        if (r.register_writes[i].addr == 0x4014) dma_writes++;
    CHECK(dma_writes >= 1, "OAMDMA write logged");
    // sections: the join run is required; the side-A-only region at $8100
    // (bank 0 offset $0100) is reached only through a fork -> forced verdict
    int saw_forced = 0, saw_required = 0;
    for (uint32_t i = 0; i < r.section_count; i++) {
        if (r.sections[i].verdict == DP_NES_VERDICT_FORCED) saw_forced = 1;
        if (r.sections[i].verdict == DP_NES_VERDICT_REQUIRED) saw_required = 1;
    }
    CHECK(saw_required, "a required section exists (the join)");
    CHECK(saw_forced, "a forced section exists (side A only)");
    int forced_covers_side = 0;
    for (uint32_t i = 0; i < r.section_count; i++)
        if (r.sections[i].verdict == DP_NES_VERDICT_FORCED &&
            r.sections[i].lo <= 0x0100 && r.sections[i].hi >= 0x0100)
            forced_covers_side = 1;
    CHECK(forced_covers_side, "the forced section is the side-A region");
    CHECK(r.exec_outside == 0, "all execution stayed in PRG");
    dp_nes_results_free(&r);
    free(rom);
}

static void test_loop_bound(void) {
    uint8_t *rom = calloc(1, 16 + 2 * 0x4000 + 1 * 0x2000);
    set_header(rom, 2, 1, 0);
    // tight decrement loop: LDX #10 / DEX / BNE back
    put(rom, 0, 0x0000, 0xA2, 0x64,             // LDX #100
                        0xCA,                   // DEX
                        0xD0, 0xFD,             // BNE $8001 (backwards)
                        0x4C, 0x05, 0x80, -1);      // JMP $8005 idle
    set_vector(rom, 1, 0xFFFC, 0x8000);
    DpNesResults r;
    DpNesOptions o = dp_nes_options();
    o.max_steps = 50000;
    o.loop_cap = 4096;
    CHECK(dp_nes_explore(rom, 16 + 2 * 0x4000 + 0x2000, -1, &o, &r), "explore runs");
    CHECK(r.steps >= 100, "loop body ran: %llu steps", (unsigned long long)r.steps);
    CHECK(r.faults == 0, "no jams");
    dp_nes_results_free(&r);
    free(rom);
}

static void test_tas_load(void) {
    const char *fm2 =
        "version 3\n"
        "romFilename Duck Tales (U)\n"
        "|2|........|||\n"
        "|0|........|||\n"
        "|0|.......A.|||\n"   // A pressed -> bit 0
        "|0|S.......|||\n"    // S (first col slot 4) = Start -> bit 3
        "|0|T.......|||\n"    // T = Select -> bit 2
        "|0|R......B|||\n";   // R = Right bit 7, B = bit 1
    FILE *f = fopen("test_tas.fm2", "wb");
    fwrite(fm2, 1, strlen(fm2), f);
    fclose(f);
    DpNesTas t;
    char err[256];
    CHECK(dp_nes_tas_load("test_tas.fm2", &t, err, sizeof err), "fm2 loads: %s", err);
    CHECK(t.frames == 6, "six frames, got %u", t.frames);
    CHECK(t.cmds[0] & 2, "reset command recorded");
    CHECK(t.rom_name[0] && !strcmp(t.rom_name, "Duck Tales (U)"), "rom name kept");
    CHECK(t.masks[2] == 0x01, "A bit: %02X", t.masks[2]);
    CHECK(t.masks[3] == 0x08, "Start bit: %02X", t.masks[3]);
    CHECK(t.masks[4] == 0x04, "Select bit: %02X", t.masks[4]);
    CHECK(t.masks[5] == 0x82, "Right+B bits: %02X", t.masks[5]);
    dp_nes_tas_free(&t);
    remove("test_tas.fm2");
    // a non-movie is refused with a message
    f = fopen("test_tas.fm2", "wb");
    fwrite("not a movie", 1, 11, f);
    fclose(f);
    CHECK(!dp_nes_tas_load("test_tas.fm2", &t, err, sizeof err), "garbage refused");
    CHECK(err[0], "refusal explains itself");
    remove("test_tas.fm2");
}

int main(void) {
    test_machine_nrom();
    test_machine_uxrom();
    test_unsupported_mappers();
    test_cnrom_axrom();
    test_snapshot();
    test_explorer();
    test_loop_bound();
    test_tas_load();
    if (fails) printf("%d failures\n", fails);
    else printf("test_nes: all green\n");
    return fails != 0;
}
