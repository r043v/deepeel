#include "tlcs900.h"
#include <stdio.h>
#include <string.h>

/* Static TLCS-900H decode.  The byte-for-byte semantics are the interpreter's
 * (m0boy targets/ngp/runtime/ngp_cpu.c): the first byte names the form, the
 * group forms carry the operation in a second byte and their (mem) mode in
 * the first.  All pointers are 24-bit, all immediates little-endian. */

typedef struct {
    const uint8_t *p;
    size_t avail;
    uint32_t pc;      /* address of the next byte (instruction start + pos) */
    unsigned pos;     /* bytes consumed so far */
    int err;
} Ctx;

static unsigned rd8(Ctx *c) {
    if (c->pos >= c->avail) { c->err = 1; c->pos++; return 0; }
    return c->p[c->pos++];
}
static unsigned rd16(Ctx *c) {   /* little-endian */
    unsigned lo = rd8(c), hi = rd8(c);
    return lo | hi << 8;
}
static uint32_t rd24(Ctx *c) {
    uint32_t lo = rd16(c);
    return lo | (uint32_t)rd8(c) << 16;
}
static uint32_t rd32(Ctx *c) {
    uint32_t lo = rd16(c);
    return lo | (uint32_t)rd16(c) << 16;
}

static const char *const CC[16] = {
    "f", "lt", "le", "ule", "ov", "mi", "eq", "ult",
    "t", "ge", "gt", "ugt", "nov", "pl", "ne", "uge"
};
static const char *const RW[8] = { "wa", "bc", "de", "hl", "ix", "iy", "iz", "sp" };
static const char *const RL[8] = { "xwa", "xbc", "xde", "xhl", "xix", "xiy", "xiz", "xsp" };
/* byte registers: the 0x20-0x27 order (W A B C D E H L, high byte first) and
 * the reg group's nibble order (A W B C D E L H) */
static const char *const RB[8] = { "w", "a", "b", "c", "d", "e", "h", "l" };
static const char *const RBN[8] = { "a", "w", "b", "c", "d", "e", "l", "h" };
static const char *const ALU[8] = { "add", "adc", "sub", "sbc", "and", "xor", "or", "cp" };
static const char *const SHF[8] = { "rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl" };
static const char *const BLOCK[8] = { "ldi", "ldir", "ldd", "lddr", "cpi", "cpir", "cpd", "cpdr" };

/* the (mem) operand: consumed bytes, register pair slot, absoluteness, and the
 * text "(xhl)" / "(xbc+8)" / "($0080)" forms */
static void mem_operand(Ctx *c, uint8_t f, uint32_t pc,
                        char *txt, size_t n, unsigned *pair, int *is_abs,
                        uint32_t *abs_addr) {
    unsigned m = f & 7;
    *pair = 255; *is_abs = 0; *abs_addr = 0;
    txt[0] = 0;
    if (f >= 0x80 && f < 0xC0) {            /* (r32) / (r32+d8) */
        *pair = f & 7;
        if (f & 8) { int8_t d = (int8_t)rd8(c); snprintf(txt, n, "%s%+d", RL[f & 7], d); }
        else snprintf(txt, n, "%s", RL[f & 7]);
        return;
    }
    if (f < 0xC0 || f > 0xF5) return;       /* no (mem) operand */
    if ((f >= 0xC8 && f <= 0xCF) || (f >= 0xD8 && f <= 0xDF) || (f >= 0xE8 && f <= 0xEF)) return;
    switch (m) {
        case 0: { unsigned a = rd8(c); *is_abs = 1; *abs_addr = a; snprintf(txt, n, "$%02X", a); break; }
        case 1: { unsigned a = rd16(c); *is_abs = 1; *abs_addr = a; snprintf(txt, n, "$%04X", a); break; }
        case 2: { uint32_t a = rd24(c); *is_abs = 1; *abs_addr = a & 0xFFFFFF; snprintf(txt, n, "$%06X", *abs_addr); break; }
        case 3: {
            uint8_t data = (uint8_t)rd8(c);
            if (data == 0x03) { rd8(c); rd8(c); *pair = 255; snprintf(txt, n, "xrr+ixb"); }
            else if (data == 0x07) { rd8(c); rd8(c); *pair = 255; snprintf(txt, n, "xrr+iyw"); }
            else if (data == 0x13) {   /* undocumented PC-relative */
                int16_t d = (int16_t)rd16(c);
                *is_abs = 1;
                *abs_addr = (pc + c->pos + (uint32_t)(int32_t)d) & 0xFFFFFF;
                snprintf(txt, n, "$%06X", *abs_addr);
            }
            else {
                *pair = (unsigned)(data >> 2) & 3;
                if ((data & 3) == 1) { int16_t d = (int16_t)rd16(c); snprintf(txt, n, "%s%+d", RL[*pair], d); }
                else snprintf(txt, n, "%s", RL[*pair]);
            }
            break;
        }
        case 4: { uint8_t data = (uint8_t)rd8(c); *pair = (unsigned)(data >> 2) & 3; snprintf(txt, n, "-%s", RL[*pair]); break; }
        case 5: { uint8_t data = (uint8_t)rd8(c); *pair = (unsigned)(data >> 2) & 3; snprintf(txt, n, "%s+", RL[*pair]); break; }
        default: break;                     /* mode 7: a register code, no (mem) */
    }
}

/* the register-code operand's name (the current-bank 0xE0-0xEF and global
 * 0xF0-0xFF codes get their names; bank-coded and invalid ones stay numeric) */
static void code_name(char *o, size_t n, uint8_t r, unsigned size) {
    if (r >= 0xE0 && r < 0xF0) {
        unsigned w = (r >> 1) - 0x70;
        snprintf(o, n, "%s", size == 0 ? RBN[w & 7] : RW[w >> 1]);
    } else if (r >= 0xF0) {
        unsigned w = r - 0xF0;
        snprintf(o, n, "%s", size == 0 ? (w == 0 ? "f" : "g.b") : RW[w >> 1]);
    } else if (r < 0x40) {
        snprintf(o, n, "%%r%u.%u.%u", r >> 4, (r >> 2) & 3, r & 3);
    } else {
        snprintf(o, n, "$%02X", r);
    }
}

/* the register name a reg-group nibble stands at (bytes A W B C D E L H,
 * words/longs WA..HL IX..SP) */
static const char *reg_nibble(unsigned n, unsigned size) {
    if (size == 0) return RBN[n & 7];
    if (size == 1) return RW[n & 7];
    return RL[n & 7];
}

unsigned dp_tlcs900_decode(const uint8_t *code, size_t avail, uint32_t pc, DpTlcsInsn *r) {
    memset(r, 0, sizeof *r);
    r->cond = -1;
    r->pair = 255;
    r->size = 3;
    if (avail < 1) return 0;
    Ctx c = { code, avail, pc, 0, 0 };
    uint8_t f = (uint8_t)rd8(&c);
    char *t = r->text;
    const size_t T = sizeof r->text;
    DpTlcsClass cls = DP_TLCS_OTHER;
    char mem[32];
    unsigned pair = 255; int mem_abs = 0; uint32_t mem_addr = 0;

    /* the (mem) operand the group forms name: its addressing bytes come
     * straight after the first byte, before the operation byte */
    if ((f >= 0x80 && f < 0xC0) || (f >= 0xC0 && f < 0xC6) || (f >= 0xD0 && f < 0xD6) ||
        (f >= 0xE0 && f < 0xE6) || (f >= 0xF0 && f < 0xF6)) {
        mem_operand(&c, f, pc, mem, sizeof mem, &pair, &mem_abs, &mem_addr);
        r->pair = pair;                 /* slots 0-3 WA..HL, 4-7 IX..SP; 255 none */
        r->mem_is_abs = mem_abs;
        r->mem_addr = mem_addr;
    }
    unsigned size = 3;
    if ((f >= 0x80 && f < 0x90) || (f >= 0xC0 && f <= 0xC7)) size = 0;
    else if ((f >= 0x90 && f < 0xA0) || (f >= 0xD0 && f <= 0xD7)) size = 1;
    else if ((f >= 0xA0 && f < 0xB0) || (f >= 0xE0 && f <= 0xE7)) size = 2;
    r->size = size;
    const char *SZC = size == 2 ? "l" : size == 1 ? "w" : "b";

    uint8_t s2 = 0, rcod = 0;
    /* decode_addr's mode 7 (first bytes C7/CF/D7/DF/E7/EF): the register code
     * comes before the operation byte */
    if (f >= 0xC7 && f < 0xF0 && (f & 7) == 7) rcod = (uint8_t)rd8(&c);
    int two_byte = (f >= 0x80 && f <= 0xF5 && f != 0xC6 && f != 0xD6 && f != 0xE6 && f != 0xF6) || f == 0xF7;
    if (two_byte && c.pos < c.avail) {
        r->op_pos = c.pos;
        s2 = (uint8_t)rd8(&c);
        r->op_byte = s2;
        r->reg_code = rcod;
    }

    switch (f) {
        /* ---- single-byte forms ---- */
        case 0x00: snprintf(t, T, "nop"); break;
        case 0x01: snprintf(t, T, "normal"); break;
        case 0x02: snprintf(t, T, "push sr"); break;
        case 0x03: snprintf(t, T, "pop sr"); break;
        case 0x04: snprintf(t, T, "max"); break;
        case 0x05: snprintf(t, T, "halt"); cls = DP_TLCS_HALT; break;
        case 0x06: { uint8_t n = (uint8_t)rd8(&c); snprintf(t, T, n == 7 ? "di" : "ei %u", n & 7); break; }
        case 0x07: snprintf(t, T, "reti"); cls = DP_TLCS_RET; break;
        case 0x08: { uint8_t d = (uint8_t)rd8(&c), v = (uint8_t)rd8(&c); snprintf(t, T, "ld ($%02X),$%02X", d, v); r->mem_is_abs = 1; r->mem_addr = d; r->mem_access = 2; break; }
        case 0x09: { uint8_t v = (uint8_t)rd8(&c); snprintf(t, T, "push #$%02X", v); break; }
        case 0x0A: { uint8_t d = (uint8_t)rd8(&c); uint16_t v = (uint16_t)rd16(&c); snprintf(t, T, "ld ($%02X),$%04X", d, v); r->mem_is_abs = 1; r->mem_addr = d; r->mem_access = 2; break; }
        case 0x0B: { uint16_t v = (uint16_t)rd16(&c); snprintf(t, T, "push #$%04X", v); break; }
        case 0x0C: snprintf(t, T, "incf"); break;
        case 0x0D: snprintf(t, T, "decf"); break;
        case 0x0E: snprintf(t, T, "ret"); cls = DP_TLCS_RET; break;
        case 0x0F: { int16_t d = (int16_t)rd16(&c); snprintf(t, T, "retd %d", d); cls = DP_TLCS_RET; break; }
        case 0x10: snprintf(t, T, "rcf"); break;
        case 0x11: snprintf(t, T, "scf"); break;
        case 0x12: snprintf(t, T, "ccf"); break;
        case 0x13: snprintf(t, T, "zcf"); break;
        case 0x14: snprintf(t, T, "push a"); break;
        case 0x15: snprintf(t, T, "pop a"); break;
        case 0x16: snprintf(t, T, "ex f,f'"); break;
        case 0x17: { uint8_t n = (uint8_t)rd8(&c); snprintf(t, T, "ldf %u", n & 3); break; }
        case 0x18: snprintf(t, T, "push f"); break;
        case 0x19: snprintf(t, T, "pop f"); break;
        case 0x1A: { r->has_target = 1; r->target = rd16(&c) & 0xFFFFFF; cls = DP_TLCS_JP; snprintf(t, T, "jp $%06X", r->target); break; }
        case 0x1B: { r->has_target = 1; r->target = rd24(&c) & 0xFFFFFF; cls = DP_TLCS_JP; snprintf(t, T, "jp $%06X", r->target); break; }
        case 0x1C: { r->has_target = 1; r->target = rd16(&c) & 0xFFFFFF; cls = DP_TLCS_CALL; snprintf(t, T, "call $%06X", r->target); break; }
        case 0x1D: { r->has_target = 1; r->target = rd24(&c) & 0xFFFFFF; cls = DP_TLCS_CALL; snprintf(t, T, "call $%06X", r->target); break; }
        case 0x1E: { int16_t d = (int16_t)rd16(&c); r->has_target = 1; r->target = (pc + c.pos + (uint32_t)(int32_t)d) & 0xFFFFFF; cls = DP_TLCS_CALL; snprintf(t, T, "calr $%06X", r->target); break; }
        case 0x1F: snprintf(t, T, "dc $1F"); cls = DP_TLCS_ILLEGAL; c.err = 1; break;
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27: {
            uint8_t v = (uint8_t)rd8(&c);
            snprintf(t, T, "ld %s,$%02X", RB[f & 7], v);
            r->size = 0;
            break;
        }
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            snprintf(t, T, "push %s", RW[f & 7]); r->size = 1;
            break;
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37: {
            uint16_t v = (uint16_t)rd16(&c);
            snprintf(t, T, "ld %s,$%04X", RW[f & 7], v); r->size = 1;
            break;
        }
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            snprintf(t, T, "push %s", RL[f & 7]); r->size = 2;
            break;
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {
            uint32_t v = rd32(&c);
            snprintf(t, T, "ld %s,$%08X", RL[f & 7], v); r->size = 2;
            break;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            snprintf(t, T, "pop %s", RW[f & 7]); r->size = 1;
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            snprintf(t, T, "dc.w $%02X", f); cls = DP_TLCS_ILLEGAL; c.err = 1;
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            snprintf(t, T, "pop %s", RL[f & 7]); r->size = 2;
            break;
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
            int8_t d = (int8_t)rd8(&c);
            r->has_target = 1; r->target = (pc + c.pos + (uint32_t)(int32_t)d) & 0xFFFFFF;
            r->cond = f & 0xF; cls = DP_TLCS_JCC;
            snprintf(t, T, "jr %s,$%06X", CC[r->cond], r->target);
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int16_t d = (int16_t)rd16(&c);
            r->has_target = 1; r->target = (pc + c.pos + (uint32_t)(int32_t)d) & 0xFFFFFF;
            r->cond = f & 0xF; cls = DP_TLCS_JCC;
            snprintf(t, T, "jrl %s,$%06X", CC[r->cond], r->target);
            break;
        }
        case 0xC6: case 0xD6: case 0xE6: case 0xF6:
            snprintf(t, T, "dc.w $%02X", f); cls = DP_TLCS_ILLEGAL; c.err = 1;
            break;
        case 0xF7: {
            /* LDX (mem8),(mem8): three mode bytes frame the two addresses */
            (void)rd8(&c); uint8_t d = (uint8_t)rd8(&c);
            (void)rd8(&c); uint8_t s = (uint8_t)rd8(&c);
            (void)rd8(&c);
            snprintf(t, T, "ldx ($%02X),($%02X)", d, s);
            r->size = 0;
            r->mem_is_abs = 1; r->mem_addr = d; r->mem_access = 2;
            break;
        }
        case 0xF8: case 0xF9: case 0xFA: case 0xFB:
        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            snprintf(t, T, "swi %u", f & 7);
            cls = DP_TLCS_SWI;
            break;

        /* ---- the group forms ---- */
        default: {
            unsigned szs = size == 3 ? 0 : size;
            switch (f >> 4) {
                /* src group, (reg)/(reg+d8) modes (80-AF) */
                case 0x8: case 0x9: case 0xA: {
                    if (s2 >= 0x80) {   /* ALU with the register operand: the decades add/adc/sub/–/and/xor/or/cp */
                        static const char *const SRC_ALU[8] = { "add", "adc", "sub", 0, "and", "xor", "or", "cp" };
                        unsigned d = (s2 >> 4) - 8;
                        if (!SRC_ALU[d]) {
                            snprintf(t, T, "dc $%02X,$%02X", f, s2);
                            cls = DP_TLCS_ILLEGAL; c.err = 1;
                            break;
                        }
                        int to_mem = s2 & 8;
                        if (d == 7) to_mem = !to_mem;   /* CP names its operands the other way round */
                        const char *rn = size == 0 ? RB[s2 & 7] : size == 1 ? RW[s2 & 7] : RL[s2 & 7];
                        if (to_mem) snprintf(t, T, "%s%.s (%s),%s", SRC_ALU[d], SZC, mem, rn);
                        else snprintf(t, T, "%s%.s %s,(%s)", SRC_ALU[d], SZC, rn, mem);
                        r->mem_access = to_mem ? 3 : 1;
                        break;
                    }
                    switch (s2) {
                        case 0x04: snprintf(t, T, "push (%s)", mem); r->mem_access = 1; break;
                        case 0x06: snprintf(t, T, "rld a,(%s)", mem); r->mem_access = 3; break;
                        case 0x07: snprintf(t, T, "rrd a,(%s)", mem); r->mem_access = 3; break;
                        case 0x10: case 0x11: case 0x12: case 0x13:
                        case 0x14: case 0x15: case 0x16: case 0x17:
                            snprintf(t, T, "%s%.s", BLOCK[s2 & 7], SZC);
                            r->mem_access = 3;
                            break;
                        case 0x19: { uint32_t a = rd16(&c); snprintf(t, T, "ld ($%04X),(%s)", a, mem); r->mem_access = 3; break; }
                        case 0x20: case 0x21: case 0x22: case 0x23:
                        case 0x24: case 0x25: case 0x26: case 0x27:
                            snprintf(t, T, "ld %s,(%s)", size == 0 ? RB[s2 & 7] : size == 1 ? RW[s2 & 7] : RL[s2 & 7], mem);
                            r->mem_access = 1;
                            break;
                        case 0x30: case 0x31: case 0x32: case 0x33:
                        case 0x34: case 0x35: case 0x36: case 0x37:
                            snprintf(t, T, "ex (%s),%s", mem, size == 0 ? RB[s2 & 7] : RW[s2 & 7]);
                            r->mem_access = 3;
                            break;
                        case 0x38: case 0x39: case 0x3A: case 0x3B:
                        case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
                            uint32_t imm = size == 0 ? rd8(&c) : rd16(&c);
                            snprintf(t, T, "%s%.s (%s),$%X", ALU[s2 & 7], SZC, mem, imm);
                            r->mem_access = 3;
                            break;
                        }
                        case 0x40: case 0x41: case 0x42: case 0x43:
                        case 0x44: case 0x45: case 0x46: case 0x47:
                        case 0x48: case 0x49: case 0x4A: case 0x4B:
                        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
                            snprintf(t, T, "mul%s (%s)", (s2 & 8) ? "s" : "", mem);
                            r->mem_access = 1;
                            break;
                        case 0x50: case 0x51: case 0x52: case 0x53:
                        case 0x54: case 0x55: case 0x56: case 0x57:
                        case 0x58: case 0x59: case 0x5A: case 0x5B:
                        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                            snprintf(t, T, "div%s (%s)", (s2 & 8) ? "s" : "", mem);
                            r->mem_access = 1;
                            break;
                        case 0x60: case 0x61: case 0x62: case 0x63:
                        case 0x64: case 0x65: case 0x66: case 0x67:
                        case 0x68: case 0x69: case 0x6A: case 0x6B:
                        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
                            uint8_t n = s2 & 7; if (!n) n = 8;
                            snprintf(t, T, "%s%.s #%u,(%s)", (s2 & 8) ? "dec" : "inc", SZC, n, mem);
                            r->mem_access = 3;
                            break;
                        }
                        case 0x78: case 0x79: case 0x7A: case 0x7B:
                        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                            snprintf(t, T, "%s%.s (%s)", SHF[s2 & 7], SZC, mem);
                            r->mem_access = 3;
                            break;
                        default:
                            snprintf(t, T, "dc $%02X,$%02X", f, s2);
                            cls = DP_TLCS_ILLEGAL; c.err = 1;
                            break;
                    }
                    break;
                }
                /* dst group, (reg)/(reg+d8) modes (B0-BF) */
                case 0xB: {
                    switch (s2) {
                        case 0x00: { uint8_t v = (uint8_t)rd8(&c); snprintf(t, T, "ld (%s),$%02X", mem, v); r->mem_access = 2; break; }
                        case 0x02: { uint16_t v = (uint16_t)rd16(&c); snprintf(t, T, "ld (%s),$%04X", mem, v); r->mem_access = 2; break; }
                        case 0x04: snprintf(t, T, "pop (%s)", mem); r->mem_access = 2; break;
                        case 0x06: snprintf(t, T, "pop2 (%s)", mem); r->mem_access = 2; break;
                        case 0x14: { uint32_t a = rd16(&c); snprintf(t, T, "ld (%s),($%04X)", mem, a); r->mem_access = 3; break; }
                        case 0x16: { uint32_t a = rd16(&c); snprintf(t, T, "ld.w (%s),($%04X)", mem, a); r->mem_access = 3; break; }
                        case 0x20: case 0x21: case 0x22: case 0x23:
                        case 0x24: case 0x25: case 0x26: case 0x27:
                            snprintf(t, T, "lda %s,(%s)", RW[s2 & 7], mem);
                            break;
                        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: {
                            static const char *const BF[5] = { "andcf", "orcf", "xorcf", "ldcf", "stcf" };
                            snprintf(t, T, "%s a,(%s)", BF[s2 - 0x28], mem);
                            r->mem_access = (s2 - 0x28) == 4 ? 3 : 1;
                            break;
                        }
                        case 0x30: case 0x31: case 0x32: case 0x33:
                        case 0x34: case 0x35: case 0x36: case 0x37:
                            snprintf(t, T, "lda %s,(%s)", RL[s2 & 7], mem);
                            break;
                        case 0x40: case 0x41: case 0x42: case 0x43:
                        case 0x44: case 0x45: case 0x46: case 0x47:
                            snprintf(t, T, "ld (%s),%s", mem, RB[s2 & 7]);
                            r->mem_access = 2;
                            break;
                        case 0x50: case 0x51: case 0x52: case 0x53:
                        case 0x54: case 0x55: case 0x56: case 0x57:
                            snprintf(t, T, "ld (%s),%s", mem, RW[s2 & 7]);
                            r->mem_access = 2;
                            break;
                        case 0x60: case 0x61: case 0x62: case 0x63:
                        case 0x64: case 0x65: case 0x66: case 0x67:
                            snprintf(t, T, "ld (%s),%s", mem, RL[s2 & 7]);
                            r->mem_access = 2;
                            break;
                        case 0x80: case 0x88: case 0x90: case 0x98: case 0xA0: {
                            static const char *const BF[5] = { "andcf", "orcf", "xorcf", "ldcf", "stcf" };
                            snprintf(t, T, "%s #%u,(%s)", BF[(s2 - 0x80) >> 3], s2 & 7, mem);
                            r->mem_access = ((s2 - 0x80) >> 3) == 4 ? 3 : 1;
                            break;
                        }
                        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
                        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
                            snprintf(t, T, "tset #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
                        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                            snprintf(t, T, "res #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
                        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
                            snprintf(t, T, "set #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
                        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
                            snprintf(t, T, "chg #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                            snprintf(t, T, "bit #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 1;
                            break;
                        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
                        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
                        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
                        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
                            r->cond = s2 & 0xF; cls = DP_TLCS_JPMEM;
                            if (mem_abs) { r->has_target = 1; r->target = mem_addr & 0xFFFFFF; }
                            snprintf(t, T, "jp %s,(%s)", CC[r->cond], mem);
                            break;
                        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
                        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
                        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
                        case 0xEC: case 0xED: case 0xEE: case 0xEF:
                            r->cond = s2 & 0xF; cls = DP_TLCS_CALLCC;
                            if (mem_abs) { r->has_target = 1; r->target = mem_addr & 0xFFFFFF; }
                            snprintf(t, T, "call %s,(%s)", CC[r->cond], mem);
                            break;
                        case 0xF0: case 0xF1: case 0xF2: case 0xF3:
                        case 0xF4: case 0xF5: case 0xF6: case 0xF7:
                        case 0xF8: case 0xF9: case 0xFA: case 0xFB:
                        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
                            r->cond = s2 & 0xF; cls = DP_TLCS_RETCC;
                            snprintf(t, T, "ret %s", CC[r->cond]);
                            break;
                        default:
                            snprintf(t, T, "dc $%02X,$%02X", f, s2);
                            cls = DP_TLCS_ILLEGAL; c.err = 1;
                            break;
                    }
                    break;
                }
                case 0xC: case 0xD: case 0xE: {   /* src explicit (C0-C5/D0-D5/E0-E5) and the reg group (C7-CF/D7-DF/E7-EF) */
                    if ((f & 0x0F) < 6) {
                        /* src group with explicit modes: the same operations
                         * as the 80-AF forms */
                        if (s2 >= 0x80) {
                            static const char *const SRC_ALU[8] = { "add", "adc", "sub", 0, "and", "xor", "or", "cp" };
                            unsigned d = (s2 >> 4) - 8;
                            if (!SRC_ALU[d]) {
                                snprintf(t, T, "dc $%02X,$%02X", f, s2);
                                cls = DP_TLCS_ILLEGAL; c.err = 1;
                                break;
                            }
                            int to_mem = s2 & 8;
                            if (d == 7) to_mem = !to_mem;   /* CP names its operands the other way round */
                            const char *rn = size == 0 ? RB[s2 & 7] : size == 1 ? RW[s2 & 7] : RL[s2 & 7];
                            if (to_mem) snprintf(t, T, "%s%.s (%s),%s", SRC_ALU[d], SZC, mem, rn);
                            else snprintf(t, T, "%s%.s %s,(%s)", SRC_ALU[d], SZC, rn, mem);
                            r->mem_access = to_mem ? 3 : 1;
                            break;
                        }
                        switch (s2) {
                            case 0x04: snprintf(t, T, "push (%s)", mem); r->mem_access = 1; break;
                            case 0x06: snprintf(t, T, "rld a,(%s)", mem); r->mem_access = 3; break;
                            case 0x07: snprintf(t, T, "rrd a,(%s)", mem); r->mem_access = 3; break;
                            case 0x10: case 0x11: case 0x12: case 0x13:
                            case 0x14: case 0x15: case 0x16: case 0x17:
                                snprintf(t, T, "%s%.s", BLOCK[s2 & 7], SZC);
                                r->mem_access = 3;
                                break;
                            case 0x19: { uint32_t a = rd16(&c); snprintf(t, T, "ld ($%04X),(%s)", a, mem); r->mem_access = 3; break; }
                            case 0x20: case 0x21: case 0x22: case 0x23:
                            case 0x24: case 0x25: case 0x26: case 0x27:
                                snprintf(t, T, "ld %s,(%s)", size == 0 ? RB[s2 & 7] : size == 1 ? RW[s2 & 7] : RL[s2 & 7], mem);
                                r->mem_access = 1;
                                break;
                            case 0x30: case 0x31: case 0x32: case 0x33:
                            case 0x34: case 0x35: case 0x36: case 0x37:
                                snprintf(t, T, "ex (%s),%s", mem, size == 0 ? RB[s2 & 7] : RW[s2 & 7]);
                                r->mem_access = 3;
                                break;
                            case 0x38: case 0x39: case 0x3A: case 0x3B:
                            case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
                                uint32_t imm = size == 0 ? rd8(&c) : rd16(&c);
                                snprintf(t, T, "%s%.s (%s),$%X", ALU[s2 & 7], SZC, mem, imm);
                                r->mem_access = 3;
                                break;
                            }
                            case 0x40: case 0x41: case 0x42: case 0x43:
                            case 0x44: case 0x45: case 0x46: case 0x47:
                            case 0x48: case 0x49: case 0x4A: case 0x4B:
                            case 0x4C: case 0x4D: case 0x4E: case 0x4F:
                                snprintf(t, T, "mul%s (%s)", (s2 & 8) ? "s" : "", mem);
                                r->mem_access = 1;
                                break;
                            case 0x50: case 0x51: case 0x52: case 0x53:
                            case 0x54: case 0x55: case 0x56: case 0x57:
                            case 0x58: case 0x59: case 0x5A: case 0x5B:
                            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                                snprintf(t, T, "div%s (%s)", (s2 & 8) ? "s" : "", mem);
                                r->mem_access = 1;
                                break;
                            case 0x60: case 0x61: case 0x62: case 0x63:
                            case 0x64: case 0x65: case 0x66: case 0x67:
                            case 0x68: case 0x69: case 0x6A: case 0x6B:
                            case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
                                uint8_t n = s2 & 7; if (!n) n = 8;
                                snprintf(t, T, "%s%.s #%u,(%s)", (s2 & 8) ? "dec" : "inc", SZC, n, mem);
                                r->mem_access = 3;
                                break;
                            }
                            case 0x78: case 0x79: case 0x7A: case 0x7B:
                            case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                                snprintf(t, T, "%s%.s (%s)", SHF[s2 & 7], SZC, mem);
                                r->mem_access = 3;
                                break;
                            default:
                                snprintf(t, T, "dc $%02X,$%02X", f, s2);
                                cls = DP_TLCS_ILLEGAL; c.err = 1;
                                break;
                        }
                        break;
                    }
                    /* the reg group (C7-CF, D7-DF, E7-EF): the mode-7 forms
                     * (C7/CF/D7/DF/E7/EF) fetch the register code, the others
                     * name it in the first byte's nibble */
                    if (f >= 0xC7) {
                        char rc[16];
                        if ((f & 7) == 7) {
                            code_name(rc, sizeof rc, rcod, szs);
                        } else {
                            snprintf(rc, sizeof rc, "%s", reg_nibble(f & 7, szs));
                        }
                        switch (s2) {
                            case 0x03:
                                if (szs == 0) { uint8_t v = (uint8_t)rd8(&c); snprintf(t, T, "ld %s,$%02X", rc, v); }
                                else if (szs == 1) { uint16_t v = (uint16_t)rd16(&c); snprintf(t, T, "ld %s,$%04X", rc, v); }
                                else { uint32_t v = rd32(&c); snprintf(t, T, "ld %s,$%08X", rc, v); }
                                r->size = szs;
                                break;
                            case 0x04: snprintf(t, T, "push %s", rc); break;
                            case 0x05: snprintf(t, T, "pop %s", rc); break;
                            case 0x06: snprintf(t, T, "cpl %s", rc); break;
                            case 0x07: snprintf(t, T, "neg %s", rc); break;
                            case 0x08: case 0x09: {
                                uint32_t imm = szs == 0 ? rd8(&c) : rd16(&c);
                                snprintf(t, T, "mul%s %s,$%X", (s2 & 1) ? "s" : "", rc, imm);
                                break;
                            }
                            case 0x0A: case 0x0B: {
                                uint32_t imm = szs == 0 ? rd8(&c) : rd16(&c);
                                snprintf(t, T, "div%s %s,$%X", (s2 & 1) ? "s" : "", rc, imm);
                                break;
                            }
                            case 0x0C: { int16_t d = (int16_t)rd16(&c); snprintf(t, T, "link %s,%d", rc, d); break; }
                            case 0x0D: snprintf(t, T, "unlk %s", rc); break;
                            case 0x0E: snprintf(t, T, "bs1f a,%s", rc); break;
                            case 0x0F: snprintf(t, T, "bs1b a,%s", rc); break;
                            case 0x10: snprintf(t, T, "daa %s", rc); break;
                            case 0x12: snprintf(t, T, "extz %s", rc); break;
                            case 0x13: snprintf(t, T, "exts %s", rc); break;
                            case 0x14: snprintf(t, T, "paa %s", rc); break;
                            case 0x16: snprintf(t, T, "mirr %s", rc); break;
                            case 0x19: snprintf(t, T, "mula %s", rc); break;
                            case 0x1C: {
                                int8_t d = (int8_t)rd8(&c);
                                r->has_target = 1; r->target = (pc + c.pos + (uint32_t)(int32_t)d) & 0xFFFFFF;
                                r->cond = TLCS_DJNZ_COND; cls = DP_TLCS_DJNZ;
                                snprintf(t, T, "djnz %s,$%06X", rc, r->target);
                                break;
                            }
                            case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: {
                                static const char *const BF[5] = { "andcf", "orcf", "xorcf", "ldcf", "stcf" };
                                uint8_t bit = (uint8_t)rd8(&c);
                                snprintf(t, T, "%s #%u,%s", BF[s2 - 0x20], bit & 0xF, rc);
                                break;
                            }
                            case 0x2E: { uint8_t cr = (uint8_t)rd8(&c); snprintf(t, T, "ldc $%02X,%s", cr, rc); break; }
                            case 0x2F: { uint8_t cr = (uint8_t)rd8(&c); snprintf(t, T, "ldc %s,$%02X", rc, cr); break; }
                            case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: {
                                static const char *const BF[5] = { "res", "set", "chg", "bit", "tset" };
                                uint8_t bit = (uint8_t)rd8(&c);
                                snprintf(t, T, "%s #%u,%s", BF[s2 - 0x30], bit & 0xF, rc);
                                break;
                            }
                            case 0x38: case 0x39: case 0x3A: {
                                uint16_t n = (uint16_t)rd16(&c);
                                snprintf(t, T, "minc%u %s,#%u", 1u << (s2 - 0x38), rc, n);
                                break;
                            }
                            case 0x3C: case 0x3D: case 0x3E: {
                                uint16_t n = (uint16_t)rd16(&c);
                                snprintf(t, T, "mdec%u %s,#%u", 1u << (s2 - 0x3C), rc, n);
                                break;
                            }
                            case 0x60: case 0x61: case 0x62: case 0x63:
                            case 0x64: case 0x65: case 0x66: case 0x67:
                            case 0x68: case 0x69: case 0x6A: case 0x6B:
                            case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
                                uint8_t n = s2 & 7; if (!n) n = 8;
                                snprintf(t, T, "%s%.s #%u,%s", (s2 & 8) ? "dec" : "inc", SZC, n, rc);
                                break;
                            }
                            case 0x70: case 0x71: case 0x72: case 0x73:
                            case 0x74: case 0x75: case 0x76: case 0x77:
                            case 0x78: case 0x79: case 0x7A: case 0x7B:
                            case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                                snprintf(t, T, "setcc %s,%s", CC[s2 & 0xF], rc);
                                break;
                            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                            case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
                                uint32_t imm = szs == 0 ? rd8(&c) : szs == 1 ? rd16(&c) : rd32(&c);
                                snprintf(t, T, "%s%.s %s,$%X", ALU[s2 & 7], SZC, rc, imm);
                                r->size = szs;
                                break;
                            }
                            case 0xD8: case 0xD9: case 0xDA: case 0xDB:
                            case 0xDC: case 0xDD: case 0xDE: case 0xDF:
                                snprintf(t, T, "cp %s,#%u", rc, s2 & 7);
                                break;
                            case 0xE8: case 0xE9: case 0xEA: case 0xEB:
                            case 0xEC: case 0xED: case 0xEE: case 0xEF: {
                                uint8_t n = (uint8_t)rd8(&c);
                                snprintf(t, T, "%s%.s %s,#%u", SHF[s2 & 7], SZC, rc, n & 0xF);
                                break;
                            }
                            case 0xF8: case 0xF9: case 0xFA: case 0xFB:
                            case 0xFC: case 0xFD: case 0xFE: case 0xFF:
                                snprintf(t, T, "%s%.s %s,a", SHF[s2 & 7], SZC, rc);
                                break;
                            default: {
                                /* register-to-register ALU and LD: 8 add,
                                 * 9 ld (both directions), A0 sub / A8 ld #short,
                                 * B0 sbc / B8 ex, C and, D0 xor / D8 cp #short,
                                 * E or, F0 cp */
                                unsigned dec = s2 >> 3;
                                unsigned rn = s2 & 7;
                                const char *rn_name = reg_nibble(rn, szs);
                                int to_rc = (s2 & 8) != 0;
                                if (dec >= 8 && !(dec == 0xA || dec == 0xD || dec == 0xF)) {
                                    static const char *const D2[8] = { "add", "adc", "sub", "sbc", "and", "xor", "or", "cp" };
                                    unsigned al = dec & 3;
                                    if (dec == 0x9) al = 8;   /* ld */
                                    else if (dec == 0xB) al = 9;   /* sbc (3) / ex */
                                    if (dec == 0x9 && to_rc) snprintf(t, T, "ld%.s %s,%s", SZC, rc, rn_name);
                                    else if (dec == 0xA && to_rc) snprintf(t, T, "ld%.s %s,#%u", SZC, rc, rn);
                                    else if (dec == 0xD && to_rc) snprintf(t, T, "cp %s,#%u", rc, rn);
                                    else if (dec == 0xB && to_rc) snprintf(t, T, "ex%.s %s,%s", SZC, rc, rn_name);
                                    else snprintf(t, T, "%s%.s %s,%s", D2[al], SZC, to_rc ? rc : rn_name, to_rc ? rn_name : rc);
                                } else {
                                    snprintf(t, T, "dc $%02X,$%02X", f, s2);
                                    cls = DP_TLCS_ILLEGAL; c.err = 1;
                                }
                                break;
                            }
                        }
                        break;
                    }
                    break;   /* C6/D6/E6 never reach the group decode */
                }
                /* dst explicit modes (F0-F5); the (reg) modes (B0-BF)
                 * carry the same table one case earlier */
                case 0xF: {
                    switch (s2) {
                        case 0x00: { uint8_t v = (uint8_t)rd8(&c); snprintf(t, T, "ld (%s),$%02X", mem, v); r->mem_access = 2; break; }
                        case 0x02: { uint16_t v = (uint16_t)rd16(&c); snprintf(t, T, "ld (%s),$%04X", mem, v); r->mem_access = 2; break; }
                        case 0x04: snprintf(t, T, "pop (%s)", mem); r->mem_access = 2; break;
                        case 0x06: snprintf(t, T, "pop2 (%s)", mem); r->mem_access = 2; break;
                        case 0x14: { uint32_t a = rd16(&c); snprintf(t, T, "ld (%s),($%04X)", mem, a); r->mem_access = 3; break; }
                        case 0x16: { uint32_t a = rd16(&c); snprintf(t, T, "ld.w (%s),($%04X)", mem, a); r->mem_access = 3; break; }
                        case 0x20: case 0x21: case 0x22: case 0x23:
                        case 0x24: case 0x25: case 0x26: case 0x27:
                            snprintf(t, T, "lda %s,(%s)", RW[s2 & 7], mem);
                            break;
                        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: {
                            static const char *const BF[5] = { "andcf", "orcf", "xorcf", "ldcf", "stcf" };
                            snprintf(t, T, "%s a,(%s)", BF[s2 - 0x28], mem);
                            r->mem_access = (s2 - 0x28) == 4 ? 3 : 1;
                            break;
                        }
                        case 0x30: case 0x31: case 0x32: case 0x33:
                        case 0x34: case 0x35: case 0x36: case 0x37:
                            snprintf(t, T, "lda %s,(%s)", RL[s2 & 7], mem);
                            break;
                        case 0x40: case 0x41: case 0x42: case 0x43:
                        case 0x44: case 0x45: case 0x46: case 0x47:
                            snprintf(t, T, "ld (%s),%s", mem, RB[s2 & 7]);
                            r->mem_access = 2;
                            break;
                        case 0x50: case 0x51: case 0x52: case 0x53:
                        case 0x54: case 0x55: case 0x56: case 0x57:
                            snprintf(t, T, "ld (%s),%s", mem, RW[s2 & 7]);
                            r->mem_access = 2;
                            break;
                        case 0x60: case 0x61: case 0x62: case 0x63:
                        case 0x64: case 0x65: case 0x66: case 0x67:
                            snprintf(t, T, "ld (%s),%s", mem, RL[s2 & 7]);
                            r->mem_access = 2;
                            break;
                        case 0x80: case 0x88: case 0x90: case 0x98: case 0xA0: {
                            static const char *const BF[5] = { "andcf", "orcf", "xorcf", "ldcf", "stcf" };
                            snprintf(t, T, "%s #%u,(%s)", BF[(s2 - 0x80) >> 3], s2 & 7, mem);
                            r->mem_access = ((s2 - 0x80) >> 3) == 4 ? 3 : 1;
                            break;
                        }
                        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
                        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
                            snprintf(t, T, "tset #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
                        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                            snprintf(t, T, "res #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
                        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
                            snprintf(t, T, "set #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
                        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
                            snprintf(t, T, "chg #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 3;
                            break;
                        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                            snprintf(t, T, "bit #%u,(%s)", s2 & 7, mem);
                            r->mem_access = 1;
                            break;
                        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
                        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
                        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
                        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
                            r->cond = s2 & 0xF; cls = DP_TLCS_JPMEM;
                            if (mem_abs) { r->has_target = 1; r->target = mem_addr & 0xFFFFFF; }
                            snprintf(t, T, "jp %s,(%s)", CC[r->cond], mem);
                            break;
                        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
                        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
                        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
                        case 0xEC: case 0xED: case 0xEE: case 0xEF:
                            r->cond = s2 & 0xF; cls = DP_TLCS_CALLCC;
                            if (mem_abs) { r->has_target = 1; r->target = mem_addr & 0xFFFFFF; }
                            snprintf(t, T, "call %s,(%s)", CC[r->cond], mem);
                            break;
                        case 0xF0: case 0xF1: case 0xF2: case 0xF3:
                        case 0xF4: case 0xF5: case 0xF6: case 0xF7:
                        case 0xF8: case 0xF9: case 0xFA: case 0xFB:
                        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
                            r->cond = s2 & 0xF; cls = DP_TLCS_RETCC;
                            snprintf(t, T, "ret %s", CC[r->cond]);
                            break;
                        default:
                            snprintf(t, T, "dc $%02X,$%02X", f, s2);
                            cls = DP_TLCS_ILLEGAL; c.err = 1;
                            break;
                    }
                    break;
                }
                default:
                    snprintf(t, T, "dc.w $%02X", f);
                    cls = DP_TLCS_ILLEGAL; c.err = 1;
                    break;
            }
            break;
        }
    }

    r->len = c.pos;
    r->cls = cls;
    if (c.err) r->has_target = 0;
    return c.pos;
}

/* length bound from the first byte only (flow()'s decode is the exact source) */
int dp_tlcs900_length(uint8_t op) {
    switch (op) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x07:
        case 0x0C: case 0x0D: case 0x0E: case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x18: case 0x19:
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            return 1;
        case 0x06: case 0x08: case 0x09: case 0x0A: case 0x17:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x1A: case 0x1C: case 0x1E:
            return 3;
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
            return 2;   /* jr cc,d8 */
        case 0x0B: case 0x0F: case 0x1B: case 0x1D:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return 4;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            return 5;
        case 0xF7:
            return 6;
        default:
            return 8;   /* a bound: the group forms with the widest (mem) mode and immediate */
    }
}
