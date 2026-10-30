// sm83.h: SM83 executor interface + register pair helpers.
#ifndef MS_SM83_H
#define MS_SM83_H

#include "../../core/machine.h"

static inline uint16_t r_bc(const MsMachine *m) { return (uint16_t)((m->b << 8) | m->c); }
static inline uint16_t r_de(const MsMachine *m) { return (uint16_t)((m->d << 8) | m->e); }
static inline uint16_t r_hl(const MsMachine *m) { return (uint16_t)((m->h << 8) | m->l); }

// Executes one instruction at m->pc.  Returns 0 = ok, 1 = halted/stopped,
// 2 = undefined opcode.  Reports through m->trace.
int sm83_step(MsMachine *m);

// Disassembles the instruction at rom[addr] (linear view of one bank slice,
// rom must have at least addr+3 readable bytes).  Returns length (1-3), or
// 0 when the opcode is undefined on the SM83.
int sm83_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz);

// condition index of a conditional opcode (JP/JR/CALL/RET cc), or -1
static inline int sm83_cond_of(uint8_t op) {
    if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 || op == 0xC0 || op == 0xC8 ||
        op == 0xC2 || op == 0xCA || op == 0xC4 || op == 0xCC || op == 0xD0 || op == 0xD8 ||
        op == 0xD2 || op == 0xDA || op == 0xD4 || op == 0xDC)
        return (op >> 3) & 3;
    return -1;
}

// state component of the tested flag for condition y (0 NZ, 1 Z, 2 NC, 3 C):
// bit 1 of y selects the flag (Z or C), bit 0 is the polarity
static inline uint8_t sm83_cond_flag(int y) { return (y & 2) ? 0x10 : 0x80; }

// instruction length from the opcode byte (CB prefix included)
static inline int sm83_len(uint8_t op) {
    if (op == 0xCB) return 2;
    switch (op) {
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x08:
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xC3:
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
    case 0xEA: case 0xFA:
        return 3;
    case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xE0: case 0xF0: case 0xE8: case 0xF8:
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        return 2;
    default:
        return 1;
    }
}

#endif
