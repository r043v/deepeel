#include "cart.h"
#include "zip.h"

const char *dp_mbc_name(DpMbc mbc) {
    switch (mbc) {
    case DP_MBC_NONE: return "ROM ONLY";
    case DP_MBC1: return "MBC1";
    case DP_MBC2: return "MBC2";
    case DP_MBC3: return "MBC3";
    case DP_MBC5: return "MBC5";
    default: return "UNKNOWN";
    }
}

static int rom_banks_from_code(uint8_t code) {
    switch (code) {
    case 0: return 2;
    case 1: return 4;
    case 2: return 8;
    case 3: return 16;
    case 4: return 32;
    case 5: return 64;
    case 6: return 128;
    case 7: return 256;
    case 8: return 512;
    case 0x52: return 72;
    case 0x53: return 80;
    case 0x54: return 96;
    default: return 0;
    }
}

static uint32_t ram_size_from_code(uint8_t code, DpMbc mbc) {
    if (mbc == DP_MBC2) return 512; // MBC2 has 512 x 4-bit internal RAM
    switch (code) {
    case 0: return 0;
    case 1: return 2048;
    case 2: return 8192;
    case 3: return 32768;
    case 4: return 131072;
    case 5: return 65536;
    default: return 0;
    }
}

static DpCart *finalize(uint8_t *rom, uint32_t size, char *err, size_t errsz) {
    if (size < 0x150) {
        snprintf(err, errsz, "rom too small (%u bytes)", size);
        free(rom);
        return NULL;
    }
    DpCart *c = calloc(1, sizeof *c);
    c->rom = rom;
    c->rom_size = size;
    int banks = rom_banks_from_code(rom[0x148]);
    if (banks == 0 || (uint32_t)banks * 0x4000 > size) banks = (int)(size / 0x4000);
    if (banks < 2) banks = 2;
    c->rom_banks = banks;
    uint8_t code = rom[0x147];
    switch (code) {
    case 0x00: c->mbc = DP_MBC_NONE; break;
    case 0x01: case 0x02: case 0x03: c->mbc = DP_MBC1; break;
    case 0x05: case 0x06: c->mbc = DP_MBC2; break;
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: c->mbc = DP_MBC3; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: c->mbc = DP_MBC5; break;
    default: c->mbc = DP_MBC_UNKNOWN; break;
    }
    c->cgb = (rom[0x143] & 0x80) != 0;
    memcpy(c->title, rom + 0x134, 16);
    c->title[16] = 0;
    for (int i = 0; i < 16; i++)
        if ((unsigned char)c->title[i] < 0x20 || (unsigned char)c->title[i] > 0x7E) c->title[i] = '?';
    c->ram_size = ram_size_from_code(rom[0x149], c->mbc);
    if (c->ram_size) c->ram = calloc(1, c->ram_size);
    return c;
}

DpCart *dp_cart_load(const char *path, char *err, size_t errsz) {
    size_t sz = 0;
    uint8_t *rom = dp_read_rom_file(path, &sz, err, errsz);
    if (!rom) return NULL;
    return finalize(rom, (uint32_t)sz, err, errsz);
}

DpCart *dp_cart_from_memory(uint8_t *rom, uint32_t size, char *err, size_t errsz) {
    if (!rom) {
        size = 0x8000;
        rom = calloc(1, size);
        rom[0x100] = 0x00; // nop
        rom[0x101] = 0xC3;
        rom[0x102] = 0x50;
        rom[0x103] = 0x01; // jp $0150
        rom[0x147] = 0x00; // ROM ONLY
    }
    return finalize(rom, size, err, errsz);
}

void dp_cart_free(DpCart *c) {
    if (!c) return;
    free(c->rom);
    free(c->ram);
    free(c);
}
