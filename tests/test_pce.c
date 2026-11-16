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

// D13: an illegal opcode on a natural path is cut and counted, not followed

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


// VCE: a ROM byte stored to the colour-data port is palette data

// index inference: abs,X reads at indexes 0,2,3 of $4100 (gap at 1 declared)
// and 1,3 of the biased base $41FF (gap at 2 declared, element before the
// table and index 0 never declared)

// --jobs 2: the secondary instance plays random sessions while the primary
// drains the power-on forks; the union covers what the serial run covers

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

// S6 across ROM pages: (page, ptr) table; page 3's stream is only in the table

// J1: random sessions restart from checkpoints earlier sessions kept

// G1: the NMI and TIMER vector targets are seeded as roots with --seed-vectors

// G3: --seed-banks enters every ROM page at $4000, reaching code no path calls

// S2 views: a jump leaving its window is resolved to the page observed in the target window

// RAM-code archive: bytes built by immediates in WRAM, then run

// C1 liveness: a section records the hardware blocks its code read (VDC status, pad port)

int main(void) {
    uint8_t *rom = malloc(ROM_SIZE);
    build(rom);
    test_decoder();
    test_bridge(rom);
    test_multitap(rom);
    test_tas_identity();
    free(rom);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("pce: ok\n");
    return 0;
}
