// sm83_disasm.c: text disassembly for the map view, the report and the GUI.
#include "sm83.h"
#include <stdio.h>

static const char *r8n[8] = { "b", "c", "d", "e", "h", "l", "(hl)", "a" };
static const char *r16n[4] = { "bc", "de", "hl", "sp" };
static const char *condn[4] = { "nz", "z", "nc", "c" };
static const char *alun[8] = { "add a,", "adc a,", "sub ", "sbc a,", "and ", "xor ", "or ", "cp " };
static const char *cbx0[8] = { "rlc", "rrc", "rl", "rr", "sla", "sra", "swap", "srl" };

static uint8_t rd(const uint8_t *rom, uint32_t size, uint32_t addr) {
    return addr < size ? rom[addr] : 0xFF;
}

int sm83_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    uint32_t a = addr;
    uint8_t op = rd(rom, rom_size, a++);
    switch (op) {
    case 0x00: snprintf(out, outsz, "nop"); return 1;
    case 0x01: case 0x11: case 0x21: case 0x31:
        snprintf(out, outsz, "ld %s,$%02x%02x", r16n[(op >> 4) & 3], rd(rom, rom_size, a + 1), rd(rom, rom_size, a));
        return 3;
    case 0x02: snprintf(out, outsz, "ld (bc),a"); return 1;
    case 0x12: snprintf(out, outsz, "ld (de),a"); return 1;
    case 0x22: snprintf(out, outsz, "ld (hl+),a"); return 1;
    case 0x32: snprintf(out, outsz, "ld (hl-),a"); return 1;
    case 0x03: case 0x13: case 0x23: case 0x33: snprintf(out, outsz, "inc %s", r16n[(op >> 4) & 3]); return 1;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: snprintf(out, outsz, "dec %s", r16n[(op >> 4) & 3]); return 1;
    case 0x07: snprintf(out, outsz, "rlca"); return 1;
    case 0x0F: snprintf(out, outsz, "rrca"); return 1;
    case 0x17: snprintf(out, outsz, "rla"); return 1;
    case 0x1F: snprintf(out, outsz, "rra"); return 1;
    case 0x08: snprintf(out, outsz, "ld ($%02x%02x),sp", rd(rom, rom_size, a + 1), rd(rom, rom_size, a)); return 3;
    case 0x09: case 0x19: case 0x29: case 0x39: snprintf(out, outsz, "add hl,%s", r16n[(op >> 4) & 3]); return 1;
    case 0x0A: snprintf(out, outsz, "ld a,(bc)"); return 1;
    case 0x1A: snprintf(out, outsz, "ld a,(de)"); return 1;
    case 0x2A: snprintf(out, outsz, "ld a,(hl+)"); return 1;
    case 0x3A: snprintf(out, outsz, "ld a,(hl-)"); return 1;
    case 0x10: snprintf(out, outsz, "stop"); return 2;
    case 0x18: snprintf(out, outsz, "jr $%04x", (uint16_t)(addr + 2 + (int8_t)rd(rom, rom_size, a))); return 2;
    case 0x20: case 0x28: case 0x30: case 0x38:
        snprintf(out, outsz, "jr %s,$%04x", condn[(op >> 3) & 3], (uint16_t)(addr + 2 + (int8_t)rd(rom, rom_size, a)));
        return 2;
    case 0x27: snprintf(out, outsz, "daa"); return 1;
    case 0x2F: snprintf(out, outsz, "cpl"); return 1;
    case 0x37: snprintf(out, outsz, "scf"); return 1;
    case 0x3F: snprintf(out, outsz, "ccf"); return 1;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        snprintf(out, outsz, "jp %s,$%02x%02x", condn[(op >> 3) & 3], rd(rom, rom_size, a + 1), rd(rom, rom_size, a));
        return 3;
    case 0xC3: snprintf(out, outsz, "jp $%02x%02x", rd(rom, rom_size, a + 1), rd(rom, rom_size, a)); return 3;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        snprintf(out, outsz, "call %s,$%02x%02x", condn[(op >> 3) & 3], rd(rom, rom_size, a + 1), rd(rom, rom_size, a));
        return 3;
    case 0xCD: snprintf(out, outsz, "call $%02x%02x", rd(rom, rom_size, a + 1), rd(rom, rom_size, a)); return 3;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: snprintf(out, outsz, "ret %s", condn[(op >> 3) & 3]); return 1;
    case 0xC9: snprintf(out, outsz, "ret"); return 1;
    case 0xD9: snprintf(out, outsz, "reti"); return 1;
    case 0xC1: case 0xD1: case 0xE1: case 0xF1: snprintf(out, outsz, "pop %s", (op == 0xF1) ? "af" : r16n[(op >> 4) & 3]); return 1;
    case 0xC5: case 0xD5: case 0xE5: case 0xF5: snprintf(out, outsz, "push %s", (op == 0xF5) ? "af" : r16n[(op >> 4) & 3]); return 1;
    case 0xE9: snprintf(out, outsz, "jp hl"); return 1;
    case 0xF9: snprintf(out, outsz, "ld sp,hl"); return 1;
    case 0xFB: snprintf(out, outsz, "ei"); return 1;
    case 0xF3: snprintf(out, outsz, "di"); return 1;
    case 0xE0: snprintf(out, outsz, "ldh ($ff%02x),a", rd(rom, rom_size, a)); return 2;
    case 0xF0: snprintf(out, outsz, "ldh a,($ff%02x)", rd(rom, rom_size, a)); return 2;
    case 0xE2: snprintf(out, outsz, "ld ($ff00+c),a"); return 1;
    case 0xF2: snprintf(out, outsz, "ld a,($ff00+c)"); return 1;
    case 0xEA: snprintf(out, outsz, "ld ($%02x%02x),a", rd(rom, rom_size, a + 1), rd(rom, rom_size, a)); return 3;
    case 0xFA: snprintf(out, outsz, "ld a,($%02x%02x)", rd(rom, rom_size, a + 1), rd(rom, rom_size, a)); return 3;
    case 0xE8: snprintf(out, outsz, "add sp,%d", (int8_t)rd(rom, rom_size, a)); return 2;
    case 0xF8: snprintf(out, outsz, "ld hl,sp%+d", (int8_t)rd(rom, rom_size, a)); return 2;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        snprintf(out, outsz, "rst $%02x", ((op >> 3) & 7) << 3);
        return 1;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        snprintf(out, outsz, "%s$%02x", alun[(op >> 3) & 7], rd(rom, rom_size, a));
        return 2;
    case 0xCB: {
        uint8_t cb = rd(rom, rom_size, a);
        int x = cb >> 6, y = (cb >> 3) & 7, z = cb & 7;
        switch (x) {
        case 0: snprintf(out, outsz, "%s %s", cbx0[y], r8n[z]); break;
        case 1: snprintf(out, outsz, "bit %d,%s", y, r8n[z]); break;
        case 2: snprintf(out, outsz, "res %d,%s", y, r8n[z]); break;
        default: snprintf(out, outsz, "set %d,%s", y, r8n[z]); break;
        }
        return 2;
    }
    default:
        break;
    }
    if ((op & 0xC7) == 0x04) { snprintf(out, outsz, "inc %s", r8n[(op >> 3) & 7]); return 1; }
    if ((op & 0xC7) == 0x05) { snprintf(out, outsz, "dec %s", r8n[(op >> 3) & 7]); return 1; }
    if ((op & 0xC7) == 0x06) { snprintf(out, outsz, "ld %s,$%02x", r8n[(op >> 3) & 7], rd(rom, rom_size, a)); return 2; }
    if ((op & 0xC0) == 0x40) { snprintf(out, outsz, "ld %s,%s", r8n[(op >> 3) & 7], r8n[op & 7]); return 1; }
    if ((op & 0xC0) == 0x80) { snprintf(out, outsz, "%s%s", alun[(op >> 3) & 7], r8n[op & 7]); return 1; }
    snprintf(out, outsz, ".db $%02x", op);
    return 0; // undefined opcode: length 0 signals invalid
}
