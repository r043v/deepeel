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
    set_header(rom, 4, 1, 9);
    char err[256];
    DpNesMach *m = dp_nesmach_open(rom, 16 + 4 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(!m, "mapper 9 refused");
    CHECK(err[0], "a message explains why");
    rom[6] = (19 & 0x0F) << 4; rom[7] = 19 & 0xF0; // mapper 19 (Namco 163)
    m = dp_nesmach_open(rom, 16 + 4 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(!m, "mapper 19 refused");
    free(rom);
}

// GxROM (66): one write picks a 32 KiB PRG bank (bits 4-5) and an 8 KiB CHR bank (bits 0-1)
static void test_gxrom(void) {
    char err[256];
    unsigned sz = 16 + 4 * 0x8000 + 4 * 0x2000;
    uint8_t *rom = calloc(1, sz);
    set_header(rom, 8, 4, 66);
    for (unsigned b = 0; b < 4; b++) rom[16 + b * 0x8000] = (uint8_t)(0x10 + b);
    set_vector(rom, 0, 0xFFFC, 0x8000);
    rom[16 + 3 * 0x8000 + 0x7FFC] = 0x00; rom[16 + 3 * 0x8000 + 0x7FFD] = 0x80;
    DpNesMach *m = dp_nesmach_open(rom, sz, -1, err, sizeof err);
    CHECK(m, "GxROM opens: %s", err);
    if (m) {
        unsigned o, n;
        CHECK(dp_nesmach_mapper_write(m, 0x8000, 0x21, &o, &n) != 0, "a write moves the windows");
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 0, "bank 0 at reset");
        dp_nesmach_close(m);
    }
    free(rom);
    rom = calloc(1, 16 + 0x8000);
    set_header(rom, 2, 0, 66);
    CHECK(!dp_nesmach_open(rom, 16 + 0x8000, -1, err, sizeof err), "GxROM without CHR ROM refused");
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
        CHECK(dp_nesmach_mapper_write(m, 0x8000, 2, &o, &n) == 2 && o == 0 && n == 2, "chr bank 0 -> 2");
        dp_nesmach_run(m, 3);
        CHECK(dp_nesmach_mapper_write(m, 0x8000, 3, &o, &n) == 2 && o == 2, "register latched");
        static uint8_t st[65536];
        dp_nesmach_save(m, st);
        dp_nesmach_reset(m);
        CHECK(dp_nesmach_mapper_write(m, 0x8000, 1, &o, &n) == 2 && o == 0, "reset clears the bank");
        dp_nesmach_load(m, st);
        CHECK(dp_nesmach_mapper_write(m, 0x8000, 1, &o, &n) == 2 && o == 2, "state carries the bank");
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
        CHECK(dp_nesmach_mapper_write(m, 0x8000, 1, &o, &n) == 1 && o == 0 && n == 2, "slot 0 -> 2");
        dp_nesmach_run(m, 1);  // STA $8000
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 2 * 0x4000, "32 KiB bank 1 at $8000");
        CHECK(dp_nesmach_rom_index(m, 0xC000) == 3 * 0x4000, "and its upper half at $C000");
        CHECK(dp_nesmach_read(m, 0x8000) == 0x5A, "bank 1 content");
        dp_nesmach_close(m);
    }
    free(rom);
}

// MMC1 (serial port, 16 KiB switch) and MMC3 (8 KiB windows) composed in the bridge
// DMC DMA: $4012 = $40 (sample at $D000), $4013 = 1 (17 bytes), $4015 bit 4

static void test_mmc1_mmc3(void) {
    char err[256];
    unsigned sz = 16 + 4 * 0x4000 + 0x2000;
    uint8_t *rom = calloc(1, sz);
    set_header(rom, 4, 1, 1);
    // $C000: LDA #1 / STA $E000 / LSR A / STA $E000 x4 : PRG bank 1 at $8000
    put(rom, 3, 0, 0xA9, 0x01, 0x8D, 0x00, 0xE0, 0x4A, 0x8D, 0x00, 0xE0, 0x8D, 0x00, 0xE0,
                   0x8D, 0x00, 0xE0, 0x8D, 0x00, 0xE0, 0x4C, 0x14, 0xC0, -1);
    put(rom, 1, 0, 0x77, -1);
    set_vector(rom, 3, 0xFFFC, 0xC000);
    DpNesMach *m = dp_nesmach_open(rom, sz, -1, err, sizeof err);
    CHECK(m, "MMC1 opens: %s", err);
    if (m) {
        dp_nesmach_reset(m);
        CHECK(dp_nesmach_rom_index(m, 0xC000) == 3 * 0x4000, "last bank fixed at $C000");
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 0, "bank 0 at $8000 after reset");
        unsigned o, n;
        dp_nesmach_run(m, 6);
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 0, "four bits in: unchanged");
        CHECK(dp_nesmach_mapper_write(m, 0xE000, 0, &o, &n) == 1 && o == 0 && n == 1, "fifth write: 0 -> 1");
        dp_nesmach_run(m, 1);
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 0x4000, "MMC1 bank 1 at $8000");
        CHECK(dp_nesmach_read(m, 0x8000) == 0x77, "its content");
        CHECK(dp_nesmach_rom_index(m, 0xC000) == 3 * 0x4000, "fixed bank unchanged");
        dp_nesmach_close(m);
    }
    free(rom);
    sz = 16 + 4 * 0x4000 + 0x2000;
    rom = calloc(1, sz);
    set_header(rom, 4, 1, 4);
    // $E000: LDA #6 / STA $8000 / LDA #3 / STA $8001 : R6 = 8 KiB bank 3
    put(rom, 3, 0x2000, 0xA9, 0x06, 0x8D, 0x00, 0x80, 0xA9, 0x03, 0x8D, 0x01, 0x80,
                        0x4C, 0x0A, 0xE0, -1);
    put(rom, 1, 0, 0x99, -1);                // 8 KiB slot 2 starts $99; slot 3 = +0x2000
    rom[16 + 3 * 0x2000] = 0x66;
    set_vector(rom, 3, 0xFFFC, 0xE000);
    m = dp_nesmach_open(rom, sz, -1, err, sizeof err);
    CHECK(m, "MMC3 opens: %s", err);
    if (m) {
        dp_nesmach_reset(m);
        CHECK(dp_nesmach_rom_index(m, 0xE000) == 7 * 0x2000, "last 8 KiB fixed at $E000");
        CHECK(dp_nesmach_rom_index(m, 0xC000) == 6 * 0x2000, "second to last at $C000");
        dp_nesmach_run(m, 4);
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 3 * 0x2000, "R6 = 3 at $8000");
        CHECK(dp_nesmach_read(m, 0x8000) == 0x66, "its content");
        dp_nesmach_close(m);
    }
    free(rom);
}

// MMC5 (bridged subset): PRG modes and the $5114-$5117 registers
static void test_mmc5(void) {
    char err[256];
    unsigned sz = 16 + 4 * 0x4000 + 0x2000;
    uint8_t *rom = calloc(1, sz);
    set_header(rom, 4, 1, 5);
    // $E000: LDA #3 / STA $5114 / LDA #0 / STA $5100 / LDA #$84 / STA $5117 / JMP *
    put(rom, 3, 0x2000, 0xA9, 0x03, 0x8D, 0x14, 0x51, 0xA9, 0x00, 0x8D, 0x00, 0x51,
                        0xA9, 0x84, 0x8D, 0x17, 0x51, 0x4C, 0x11, 0xE0, -1);
    set_vector(rom, 3, 0xFFFC, 0xE000);
    rom[16 + 3 * 0x2000] = 0x66;
    DpNesMach *m = dp_nesmach_open(rom, sz, -1, err, sizeof err);
    CHECK(m, "MMC5 opens: %s", err);
    if (m) {
        unsigned o, n;
        dp_nesmach_reset(m);
        CHECK(dp_nesmach_rom_index(m, 0xE000) == 7 * 0x2000, "MMC5: last 8 KiB under $E000 at power-on");
        CHECK(dp_nesmach_mapper_write(m, 0x5114, 3, &o, &n) == 1 && o == 0 && n == 3, "MMC5: $5114 write seen as a PRG change");
        dp_nesmach_run(m, 2);
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 3 * 0x2000, "MMC5 mode 3: $5114 = 3 at $8000");
        CHECK(dp_nesmach_read(m, 0x8000) == 0x66, "its content");
        dp_nesmach_run(m, 4);   // mode 0 then $5117 = $84: 32 KiB from slot 4
        CHECK(dp_nesmach_rom_index(m, 0x8000) == 4 * 0x2000 && dp_nesmach_rom_index(m, 0xE000) == 7 * 0x2000,
              "MMC5 mode 0: 32 KiB from $5117 (%llu)", (unsigned long long)dp_nesmach_rom_index(m, 0x8000));
        dp_nesmach_close(m);
    }
    free(rom);
}

// D13: a KIL (jam) on a natural path ends it; the bytes behind it are not followed

// G1: --seed-vectors roots the NMI and IRQ targets as forced code

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



static void test_tas_load(void) {
    const char *fm2 =
        "version 3\n"
        "romFilename Duck Tales (U)\n"
        "|2|........|||\n"
        "|0|........|||\n"
        "|0|.......A.|||\n"   // A pressed -> bit 0
        "|0|....T...|||\n"    // RLDUTSBA: T = Start -> bit 3
        "|0|.....S..|||\n"    // S = Select -> bit 2
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

static void test_tas_identity(void) {
    uint8_t rom[64];
    for (unsigned i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i * 7);
    DpNesTas t;
    memset(&t, 0, sizeof t);
    CHECK(dp_nes_tas_identity(&t, rom, sizeof rom) == 3, "a movie without SHA-1 records no identity");
    // SHA-1 of the whole file, then of the file without the 16-byte header
    extern void dp_sha1(const uint8_t *, size_t, uint8_t[20]);
    uint8_t h[20];
    dp_sha1(rom, sizeof rom, h);
    for (int i = 0; i < 20; i++) snprintf(t.sha1 + 2 * i, 3, "%02X", h[i]);   // upper case is accepted
    CHECK(dp_nes_tas_identity(&t, rom, sizeof rom) == 1, "whole-file SHA-1 matches");
    dp_sha1(rom + 16, sizeof rom - 16, h);
    for (int i = 0; i < 20; i++) snprintf(t.sha1 + 2 * i, 3, "%02x", h[i]);
    CHECK(dp_nes_tas_identity(&t, rom, sizeof rom) == 1, "headerless SHA-1 matches");
    rom[40] ^= 1;
    CHECK(dp_nes_tas_identity(&t, rom, sizeof rom) == 2, "a changed ROM is a different ROM");
}

// S6: a decoder called with a zero-page pointer taken from a table; the stream the ROM
// never calls is read through the replay

// S6 across UxROM banks: (bank, ptr) table in the fixed bank; bank 0's stream is only in the table

// S6 across MMC1 banks: same table, the bank set through the serial port

// S6 across MMC3 banks: 8 KiB R6 window, bank set by $8000/$8001 writes

// S6 across MMC3 banks in PRG mode 1: $C000 is the switchable window, code sits in the fixed top banks

// S6 across Camerica (71) / UN1ROM (94) banks: UxROM-like, bank written at $C000+ (71) or in bits 2-4 (94)

// S6 across mapper-180 banks: $8000 is fixed, the switchable bank sits at $C000

// J1: random sessions restart from checkpoints earlier sessions kept

// RAM-code archive: bytes built by immediates, then run from $0300


// C1 liveness: a section records the work-RAM pages and register classes its code read

// C2: the context hash projects on the RAM pages a section reads
static void test_ctx_projection(void) {
    uint8_t *rom = calloc(1, 16 + 2 * 0x4000 + 0x2000);
    set_header(rom, 2, 1, 0);
    put(rom, 0, 0x0000, 0x4C, 0x00, 0x80, -1);
    set_vector(rom, 1, 0xFFFA, 0x8000);
    set_vector(rom, 1, 0xFFFC, 0x8000);
    set_vector(rom, 1, 0xFFFE, 0x8000);
    char err[256];
    DpNesMach *m = dp_nesmach_open(rom, 16 + 2 * 0x4000 + 0x2000, -1, err, sizeof err);
    CHECK(m, "ctx open: %s", err);
    if (m) {
        dp_nesmach_reset(m);
        uint8_t *a = malloc(dp_nesmach_state_size()), *b = malloc(dp_nesmach_state_size());
        dp_nesmach_save(m, a); dp_nesmach_save(m, b);
        dp_nesmach_state_ram(b)[5 * 256 + 7] ^= 0x5A;   // differs in RAM page 5 only
        CHECK(dp_nesmach_state_ctx_hash_ram(a, 0x00) == dp_nesmach_state_ctx_hash_ram(b, 0x00), "no page read: same context");
        CHECK(dp_nesmach_state_ctx_hash_ram(a, 0xDF) == dp_nesmach_state_ctx_hash_ram(b, 0xDF), "page 5 unread: same context");
        CHECK(dp_nesmach_state_ctx_hash_ram(a, 0x20) != dp_nesmach_state_ctx_hash_ram(b, 0x20), "page 5 read: different context");
        free(a); free(b);
        dp_nesmach_close(m);
    }
    free(rom);
}

int main(void) {
    test_machine_nrom();
    test_machine_uxrom();
    test_unsupported_mappers();
    test_cnrom_axrom();
    test_gxrom();
    test_mmc1_mmc3();
    test_mmc5();
    test_snapshot();
    test_tas_load();
    test_tas_identity();
    test_ctx_projection();
    if (fails) printf("%d failures\n", fails);
    else printf("test_nes: all green\n");
    return fails != 0;
}
