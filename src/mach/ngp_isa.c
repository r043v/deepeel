// ngp_isa.c: the TLCS-900H as a DpIsa (spec §7.3).  Wraps the static decoder
// (src/cpu/tlcs900); the constant-transfer function models the immediate
// loads, the register copies, the absolute LDA and the stack arithmetic —
// everything else kills all registers, never guesses (the huc6280 rule).
#include "../core/dpmachine.h"
#include "../cpu/tlcs900/tlcs900.h"
#include <string.h>

// the SP slot (DP_TLCSR_SP) and the register widths by group size (0 b, 1 w, 2 l)
#define TLCS_SP DP_TLCSR_SP
static unsigned width_of(unsigned size) { return size == 2 ? 4u : size == 1 ? 2u : 1u; }

// the slot of a register-code byte (0xE0-0xEF the current bank, 0xF0-0xFF the
// globals, 0x00-0x3F a banked copy in slots 8-19); 255 when not nameable
static unsigned code_slot(uint8_t r, unsigned size) {
    (void)size;
    if (r >= 0xE0 && r < 0xF0) {
        unsigned w = (r >> 1) - 0x70;
        return w & 7;                       // slots 0-3 WA..HL, 4-7 IX..SP
    }
    if (r >= 0xF0) {
        unsigned w = (r >> 1) - 0x78;
        return w & 7;
    }
    if (r < 0x40) {                         // the banked copies, slots 8-19
        unsigned bank = r >> 4, reg = (r >> 2) & 3;
        return DP_TLCSR_BANK0 + bank * 4 + reg;
    }
    return 255;
}

static void kill_reg(DpRegState *s, unsigned slot, unsigned bytes) {
    if (slot >= DP_REG_N || bytes > DP_REG_MAX_BYTES) return;
    s->known[slot] &= (uint8_t)(bytes >= 4 ? 0 : (0x0Fu << bytes) & 0x0F);
}
static void set_reg(DpRegState *s, unsigned slot, unsigned bytes, uint32_t v) {
    if (slot >= DP_REG_N) return;
    if (bytes > DP_REG_MAX_BYTES) bytes = DP_REG_MAX_BYTES;
    s->v[slot] = v;
    s->known[slot] |= (uint8_t)((1u << bytes) - 1);
}
static void step_reg(DpRegState *s, unsigned slot, unsigned bytes, int32_t delta) {
    if (slot >= DP_REG_N || bytes > DP_REG_MAX_BYTES) return;
    uint8_t mask = (uint8_t)((1u << bytes) - 1);
    if ((s->known[slot] & mask) == mask) {
        s->v[slot] = (uint32_t)((int64_t)s->v[slot] + delta);
        s->known[slot] |= mask;
    } else {
        kill_reg(s, slot, bytes);
    }
}
static void copy_reg(DpRegState *out, const DpRegState *in, unsigned dst, unsigned src, unsigned bytes) {
    if (dst >= DP_REG_N || src >= DP_REG_N || bytes > DP_REG_MAX_BYTES) return;
    uint8_t mask = (uint8_t)((1u << bytes) - 1);
    out->v[dst] = (out->v[dst] & ~mask) | (in->v[src] & mask);
    out->known[dst] = (uint8_t)((out->known[dst] & ~mask) | (in->known[src] & mask));
}
static void kill_all(DpRegState *out) {
    memset(out->known, 0, sizeof out->known);
}

// byte register by group-form code (the 0x20-0x27 order: W A B C D E H L —
// W is the HIGH byte of WA, A the low one) -> (slot, byte index)
static void byte_slot(unsigned n, unsigned *slot, unsigned *byte) {
    *slot = n >> 1;
    *byte = (n & 1) ? 0 : 1;   // odd codes are the low bytes (A C E L)
}

static int isa_length(uint8_t op) { return dp_tlcs900_length(op); }

static int isa_flow(const uint8_t *bytes, uint32_t pc, DpFlowInfo *o) {
    DpTlcsInsn r;
    if (!dp_tlcs900_decode(bytes, 10, pc, &r)) return 0;
    memset(o, 0, sizeof *o);
    o->length = (uint8_t)r.len;
    o->cond = (int8_t)(r.cond == TLCS_DJNZ_COND ? TLCS_DJNZ_COND : r.cond);
    o->pair = r.pair == 255 ? (uint8_t)DP_REG_PAIR_NONE : (uint8_t)r.pair;
    switch (r.cls) {
        case DP_TLCS_JP:
            o->kind = DP_FLOW_JUMP; o->fall = 0; o->target = (int32_t)r.target;
            break;
        case DP_TLCS_JCC:
            o->kind = DP_FLOW_COND; o->fall = 1; o->target = (int32_t)r.target;
            break;
        case DP_TLCS_JPMEM:
            if (r.has_target) { o->kind = DP_FLOW_COND; o->fall = 1; o->target = (int32_t)r.target; }
            else { o->kind = DP_FLOW_INDIRECT; o->fall = 0; o->target = -1; }
            break;
        case DP_TLCS_CALL:
            o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->target = (int32_t)r.target;
            break;
        case DP_TLCS_CALLCC:
            if (r.has_target) { o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->target = (int32_t)r.target; }
            else { o->kind = DP_FLOW_INDIRECT; o->call = 1; o->fall = 1; o->target = -1; }
            break;
        case DP_TLCS_RET:
            o->kind = DP_FLOW_RET; o->fall = 0;
            break;
        case DP_TLCS_RETCC:
            o->kind = DP_FLOW_COND; o->fall = 1; o->target = -1;
            break;
        case DP_TLCS_DJNZ:
            o->kind = DP_FLOW_COND; o->fall = 1; o->target = (int32_t)r.target;
            o->cond = TLCS_DJNZ_COND;
            break;
        case DP_TLCS_SWI:
            o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->rst = 1; o->target = -1;
            break;
        case DP_TLCS_HALT:
            o->kind = DP_FLOW_SEQ; o->halt = 1;
            break;
        case DP_TLCS_ILLEGAL:
            o->kind = DP_FLOW_ILLEGAL; o->fall = 1;
            break;
        default:
            // the block ops (ldi/ldir/ldd/lddr/cpi/cpir/cpd/cpdr: the second
            // byte 0x10-0x17 of the (mem) src groups) repeat on V (BC != 0) —
            // the same self-loop conditional the z80 LDIR is, and V inverts
            o->kind = DP_FLOW_SEQ; o->fall = 1;
            if (r.op_byte >= 0x10 && r.op_byte <= 0x17 && (r.op_byte & 1) &&
                ((bytes[0] >= 0x80 && bytes[0] <= 0xAF) ||
                 (bytes[0] >= 0xC0 && bytes[0] <= 0xC5) ||
                 (bytes[0] >= 0xD0 && bytes[0] <= 0xD5) ||
                 (bytes[0] >= 0xE0 && bytes[0] <= 0xE5))) {
                o->kind = DP_FLOW_COND; o->fall = 1; o->target = (int32_t)pc; o->cond = 4;   /* OV: continue while BC != 0 */
            }
            break;
    }
    return (int)r.len;
}

static int isa_cond_of(uint8_t op) {
    return (op >= 0x60 && op < 0x80) ? (int)(op & 0xF) : -1;
}

// the primary flag each condition tests (compound conditions return the
// leading flag; the polarity bit names the taken polarity)
static uint8_t isa_cond_flag(int cond) {
    static const uint8_t flag[16] = {
        0, 0x04, 0x40, 0x01, 0x04, 0x80, 0x40, 0x01,   // F LT LE ULE OV MI EQ ULT
        0, 0x04, 0x40, 0x01, 0x04, 0x80, 0x40, 0x01,   // T GE GT UGT NOV PL NE UGE
    };
    if (cond < 0 || cond > 15) return 0;
    uint8_t f = flag[cond];
    return f ? (uint8_t)(f | ((cond < 8) ? 1 : 0)) : 0;
}

// C3: the constant-transfer function.  Modelled: the immediate loads, the
// register copies, the absolute LDA, the push/pop/call/ret stack arithmetic,
// INC/DEC of a register.  Everything else kills all registers.
static void isa_reg_effect(const uint8_t *bytes, uint32_t pc, const DpRegState *in, DpRegState *out,
                           const DpProvRom *rom) {
    (void)rom;
    DpTlcsInsn r;
    *out = *in;
    if (!dp_tlcs900_decode(bytes, 10, pc, &r)) { kill_all(out); return; }
    const uint8_t f = bytes[0];
    const uint8_t s2 = r.len >= 2 ? bytes[r.len >= 2 && ((f >= 0xC7 && f < 0xF0) && (f & 7) == 7) ? 2 : 1] : 0;

    switch (f) {
        case 0x03: step_reg(out, TLCS_SP, 4, 2); return;            // POP SR
        case 0x02: case 0x09: case 0x0B:                            // PUSH SR/#8/#16
            step_reg(out, TLCS_SP, 4, f == 0x09 ? -1 : -2); return;
        case 0x0F: kill_reg(out, TLCS_SP, 4); return;               // RETD d16
        case 0x14: step_reg(out, TLCS_SP, 4, -1); return;           // PUSH A
        case 0x15: kill_reg(out, DP_TLCSR_WA, 1); step_reg(out, TLCS_SP, 4, 1); return;   // POP A
        case 0x17: {                                                 // LDF #n: the bank view changes
            for (unsigned s = 0; s < 4; s++) kill_reg(out, s, 4);
            return;
        }
        case 0x18: step_reg(out, TLCS_SP, 4, -1); return;           // PUSH F
        case 0x19: step_reg(out, TLCS_SP, 4, 1); return;            // POP F
        case 0x1C: case 0x1D: step_reg(out, TLCS_SP, 4, -4); return; // CALL
        case 0x1E: step_reg(out, TLCS_SP, 4, -4); return;           // CALR
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27: {                // LD r,#8
            unsigned slot, byte;
            byte_slot(f & 7, &slot, &byte);
            out->v[slot] = (out->v[slot] & ~(0xFFu << (byte * 8))) | ((uint32_t)bytes[1] << (byte * 8));
            out->known[slot] |= (uint8_t)(1u << byte);
            return;
        }
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            step_reg(out, TLCS_SP, 4, -2); return;                   // PUSH rr
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37: {                // LD rr,#16
            uint32_t imm = (uint32_t)bytes[1] | (uint32_t)bytes[2] << 8;
            set_reg(out, f & 7, 2, imm);
            return;
        }
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            step_reg(out, TLCS_SP, 4, -4); return;                   // PUSH xrr
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {                // LD xrr,#32
            uint32_t imm = (uint32_t)bytes[1] | (uint32_t)bytes[2] << 8 |
                           (uint32_t)bytes[3] << 16 | (uint32_t)bytes[4] << 24;
            set_reg(out, f & 7, 4, imm);
            return;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:                  // POP rr
            kill_reg(out, f & 7, 2);
            step_reg(out, TLCS_SP, 4, 2);
            return;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:                  // POP xrr
            kill_reg(out, f & 7, 4);
            step_reg(out, TLCS_SP, 4, 4);
            return;
        case 0x0E:                                                   // RET
            step_reg(out, TLCS_SP, 4, 4);
            return;
        case 0x07:                                                   // RETI
            step_reg(out, TLCS_SP, 4, 6);
            return;
        default:
            break;
    }

    /* the group forms */
    if (f >= 0x80 && f <= 0xF5) {
        const unsigned sz = width_of(r.size == 3 ? 0 : r.size);
        if (f >= 0x80 && f < 0xB0) {
            if (s2 >= 0x80 && !(s2 & 8)) { kill_reg(out, s2 & 7, sz); return; }   // ALU R,(mem): R killed
            if (s2 >= 0x20 && s2 < 0x28) { kill_reg(out, s2 & 7, sz); return; }   // LD r,(mem): R killed
            if (s2 >= 0x88 && s2 < 0x90) { kill_reg(out, TLCS_SP, 4); kill_reg(out, s2 & 7, sz); return; }   // conservative
            kill_all(out);
            return;
        }
        if (f >= 0xB0 && f < 0xC0) {
            if (s2 >= 0x20 && s2 < 0x28 && r.mem_is_abs && r.len == 4) {          // LDA rr,($nnnn)
                set_reg(out, s2 & 7, 2, r.mem_addr & 0xFFFF);
                return;
            }
            if (s2 >= 0x30 && s2 < 0x38 && r.mem_is_abs && r.len == 5) {          // LDA xrr,($nnnnnn)
                set_reg(out, s2 & 7, 4, r.mem_addr & 0xFFFFFF);
                return;
            }
            if (s2 == 0x04 || s2 == 0x06) { step_reg(out, TLCS_SP, 4, sz == 4 ? 4 : sz); return; }   // POP (mem)
            if (s2 == 0x00 || s2 == 0x02) return;                                  // LD (mem),#imm: no reg
            kill_all(out);
            return;
        }
        /* the reg group */
        {
            unsigned rc_slot = 255;
            if ((f & 7) == 7) {
                rc_slot = code_slot(bytes[1], r.size == 3 ? 1 : r.size);
            } else {
                unsigned b = f & 7, byte = 0;
                if (r.size == 0) { byte_slot(b, &rc_slot, &byte); }
                else rc_slot = b & 7;
            }
            switch (s2) {
                case 0x03: {                                          // LD r,#imm
                    uint32_t imm = sz == 4 ? (uint32_t)bytes[r.len - 4] | (uint32_t)bytes[r.len - 3] << 8 |
                                             (uint32_t)bytes[r.len - 2] << 16 | (uint32_t)bytes[r.len - 1] << 24
                                  : sz == 2 ? (uint32_t)bytes[r.len - 2] | (uint32_t)bytes[r.len - 1] << 8
                                  : bytes[r.len - 1];
                    if (rc_slot < DP_REG_N && r.size != 0) set_reg(out, rc_slot, sz, imm);
                    else if (rc_slot < DP_REG_N) {
                        unsigned byte = 0;
                        if ((f & 7) != 7) { byte_slot(f & 7, &rc_slot, &byte); }
                        else byte = 0;                                 // mode-7 byte code: the byte index rides the code
                        out->v[rc_slot] = (out->v[rc_slot] & ~(0xFFu << (byte * 8))) | (imm << (byte * 8));
                        out->known[rc_slot] |= (uint8_t)(1u << byte);
                    }
                    return;
                }
                case 0x04: step_reg(out, TLCS_SP, 4, -(int32_t)sz); return;   // PUSH r
                case 0x05: kill_reg(out, rc_slot, sz); step_reg(out, TLCS_SP, 4, (int32_t)sz); return;   // POP r
                case 0x0C: kill_reg(out, rc_slot, sz); kill_reg(out, TLCS_SP, 4); return;   // LINK
                case 0x0D: kill_reg(out, TLCS_SP, 4); return;                 // UNLK
                case 0x1C:                                                    // DJNZ r,d8
                    kill_reg(out, rc_slot, sz == 4 ? 4 : sz == 2 ? 2 : 1);
                    return;
                case 0x60: case 0x61: case 0x62: case 0x63:
                case 0x64: case 0x65: case 0x66: case 0x67:
                case 0x68: case 0x69: case 0x6A: case 0x6B:
                case 0x6C: case 0x6D: case 0x6E: case 0x6F: {                 // INC/DEC #n,r
                    uint8_t n = s2 & 7; if (!n) n = 8;
                    step_reg(out, rc_slot, sz, (s2 & 8) ? -(int32_t)n : (int32_t)n);
                    return;
                }
                case 0x88: case 0x89: case 0x8A: case 0x8B:
                case 0x8C: case 0x8D: case 0x8E: case 0x8F:                   // LD R,r
                    copy_reg(out, in, s2 & 7, rc_slot, sz);
                    return;
                case 0x98: case 0x99: case 0x9A: case 0x9B:
                case 0x9C: case 0x9D: case 0x9E: case 0x9F:                   // LD r,R
                    copy_reg(out, in, rc_slot, s2 & 7, sz);
                    return;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB:
                case 0xAC: case 0xAD: case 0xAE: case 0xAF:                   // LD r,#short
                    set_reg(out, rc_slot, sz, s2 & 7);
                    return;
                case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                case 0xCC: case 0xCD: case 0xCE: case 0xCF:                   // ALU r,#imm: r killed (kind 7 CP writes nothing)
                    if (s2 != 0xCF) kill_reg(out, rc_slot, sz);
                    return;
                case 0xE8: case 0xE9: case 0xEA: case 0xEB:
                case 0xEC: case 0xED: case 0xEE: case 0xEF:                   // shift by count: the register changes
                    kill_reg(out, rc_slot, sz);
                    return;
                default:
                    kill_all(out);
                    return;
            }
        }
    }
    kill_all(out);
}

static int isa_illegal(uint8_t op) {
    return op == 0x1F || op == 0xC6 || op == 0xD6 || op == 0xE6 || op == 0xF6 || (op >= 0x50 && op < 0x58);
}

static int isa_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    DpTlcsInsn r;
    uint32_t at = addr;
    if (at >= rom_size) return 0;
    unsigned len = dp_tlcs900_decode(rom + at, rom_size - at, at, &r);
    if (!len) return 0;
    snprintf(out, outsz, "%s", r.text);
    return (int)len;
}

// the group size a first byte runs at (0 b, 1 w, 2 l, 3 none) — shared with the signature pairing
static unsigned first_byte_size(uint8_t f) {
    if ((f >= 0x80 && f < 0x90) || (f >= 0xC0 && f <= 0xC7)) return 0;
    if ((f >= 0x90 && f < 0xA0) || (f >= 0xD0 && f <= 0xD7)) return 1;
    if ((f >= 0xA0 && f < 0xB0) || (f >= 0xE0 && f <= 0xE7)) return 2;
    return 3;
}

static int isa_mem_operand(const uint8_t *bytes, DpMemOp *out) {
    DpTlcsInsn r;
    memset(out, 0, sizeof *out);
    if (!dp_tlcs900_decode(bytes, 10, 0, &r) || (!r.mem_is_abs && r.pair == 255)) return 0;
    out->access = r.mem_access;   // the decoder's 1/2/3 are the DP_MA_READ/WRITE/RW values
    out->width = (uint8_t)width_of(r.size == 3 ? 0 : r.size);
    if (r.mem_is_abs) { out->mode = DP_AM_ABS; out->addr = r.mem_addr; }
    else { out->mode = DP_AM_PAIR; out->pair = (uint8_t)r.pair; }
    return 1;
}

// C3 concrete-stack returns: RET (4 bytes), RETI (6, the PC sits 2 above the SP)
static int isa_sp_ret(const uint8_t *bytes, unsigned *width, unsigned *bias, unsigned *off) {
    if (bytes[0] == 0x0E) { *width = 4; *bias = 0; *off = 0; return 1; }
    if (bytes[0] == 0x07) { *width = 6; *bias = 0; *off = 2; return 1; }
    if (bytes[0] >= 0xB0 && bytes[0] < 0xC0 && bytes[1] >= 0xF0) { *width = 4; *bias = 0; *off = 0; return 1; }
    if (bytes[0] >= 0xF0 && bytes[0] < 0xF6 && bytes[1] >= 0xF0) { *width = 4; *bias = 0; *off = 0; return 1; }
    return 0;
}

// B9: a full pointer immediate (LD rr,#16 / LD xrr,#32)
static int isa_imm_ptr(const uint8_t *bytes, uint32_t *imm) {
    if (bytes[0] >= 0x30 && bytes[0] < 0x38) {
        *imm = (uint32_t)bytes[1] | (uint32_t)bytes[2] << 8;
        return *imm >= 0x100;
    }
    if (bytes[0] >= 0x40 && bytes[0] < 0x48) {
        *imm = ((uint32_t)bytes[1] | (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3] << 16) & 0xFFFFFF;
        return *imm >= 0x100;
    }
    return 0;
}

// B9: an 8-bit immediate load (LD r,#8, the 0x20-0x27 forms)
static int isa_imm_byte(const uint8_t *bytes, uint8_t *v) {
    if (bytes[0] >= 0x20 && bytes[0] < 0x28) { *v = bytes[1]; return 1; }
    return 0;
}

// C3: LD A,#8 (the one accumulator load)
static int isa_acc_imm(const uint8_t *bytes, uint8_t *v) {
    if (bytes[0] == 0x21) { *v = bytes[1]; return 1; }
    return 0;
}

// C3: LD (abs),A — the store forms whose register byte names A (0x41)
static int isa_st_acc_abs(const uint8_t *bytes, uint32_t *addr) {
    DpTlcsInsn r;
    if (!dp_tlcs900_decode(bytes, 10, 0, &r)) return 0;
    if (!r.mem_is_abs || r.op_byte != 0x41) return 0;
    if (!((bytes[0] >= 0xB0 && bytes[0] < 0xC0) || (bytes[0] >= 0xF0 && bytes[0] < 0xF6))) return 0;
    *addr = r.mem_addr;
    return 1;
}

// "zero data marked code" point 3: a window of no-op software interrupts
// (SWI n with n even or 7: the machine does nothing but burn cycles) is a
// padding line, never the head of real code
static int isa_entry_data_sig(const uint8_t *w, unsigned n) {
    if (n < 4) return 0;
    for (unsigned i = 0; i < n; i++)
        if (!((w[i] & 0xF8) == 0xF8 && ((w[i] & 7) == 0 || (w[i] & 7) == 2 || (w[i] & 7) == 4 || (w[i] & 7) == 6 || (w[i] & 7) == 7)))
            return 0;
    return 1;
}

// M1: a direct RAM load (LD r,($nnnn) / LD xrr,($nnnnnn), the explicit-mode
// forms whose operation byte names a register destination)
static int isa_ram_load(const uint8_t *bytes, uint32_t *addr, int *idx_reg, int *width, int *dreg) {
    DpTlcsInsn r;
    if (!dp_tlcs900_decode(bytes, 10, 0, &r)) return 0;
    if (!r.mem_is_abs || r.op_pos == 0) return 0;
    unsigned op = r.op_byte;
    /* the loads: src second bytes 0x20-0x27 (byte/word) and 0x30-0x37 (long,
     * the explicit long forms only) */
    if (bytes[0] >= 0xC0 && bytes[0] < 0xC6) {           // byte form
        if (r.op_pos != 3 || op < 0x20 || op >= 0x28) return 0;
        *addr = r.mem_addr; *idx_reg = 0; *width = 1; *dreg = -1;
        return (int)r.len;
    }
    if (bytes[0] >= 0xD0 && bytes[0] < 0xD6) {           // word form
        if (r.op_pos != 3 || op < 0x20 || op >= 0x28) return 0;
        *addr = r.mem_addr; *idx_reg = 0; *width = 2; *dreg = (int)(op & 7);
        return (int)r.len;
    }
    if (bytes[0] >= 0xE0 && bytes[0] < 0xE6) {           // long form
        if (r.op_pos != 4 || op < 0x30 || op >= 0x38) return 0;
        *addr = r.mem_addr; *idx_reg = 0; *width = 4; *dreg = (int)(op & 7);
        return (int)r.len;
    }
    return 0;
}

// M1 wide pairing: CP r,#imm of the just-loaded register, or a standalone
// CP (abs),#imm (the (mem),#imm forms)
static int isa_ram_signature_wide(struct DpMachine *m, uint32_t pc, const uint8_t *b, const uint8_t *f,
                                  int dl_len, int dl_width, int dl_dreg, uint32_t dl_addr,
                                  uint32_t *addr, uint32_t *val, int *width) {
    (void)m; (void)pc; (void)b; (void)dl_len; (void)dl_dreg;
    if (dl_width == 1) {   // a byte load: the next CP w/a,#imm8 (reg group mode-7: C7 + code A + CP #)
        for (int i = 0; i < 10; i++) {
            if (f[i] >= 0xC7 && f[i] < 0xF0 && f[i + 1] == 0xE1 && f[i + 2] == 0xCF) {
                *addr = dl_addr; *val = f[i + 3]; *width = 1; return 1;
            }
        }
    }
    if (dl_width == 2 && dl_dreg >= 0) {   // a word load into a named register: CP reg,#imm16
        for (int i = 0; i < 10; i++) {
            DpTlcsInsn q;
            if (dp_tlcs900_decode(f + i, 10 - i, 0, &q) && q.cls == DP_TLCS_OTHER &&
                q.op_byte == 0xCF && (first_byte_size(f[i]) == 1) && (f[i] & 7) == (unsigned)dl_dreg) {
                *addr = dl_addr; *val = (uint32_t)f[i + q.op_pos + 1] | (uint32_t)f[i + q.op_pos + 2] << 8;
                *width = 2; return 1;
            }
        }
    }
    (void)f;
    return 0;
}

// M1 technique 7: a LD xrr,#ROM table base within 16 bytes of the load
static const uint8_t *table_base_after(const uint8_t *b, uint32_t pc, int len, uint32_t *base) {
    for (int i = len; i < 16; i++) {
        if (b[i] >= 0x40 && b[i] < 0x48 && i + 4 < 64) {
            uint32_t imm = (uint32_t)b[i + 1] | (uint32_t)b[i + 2] << 8 | (uint32_t)b[i + 3] << 16;
            (void)pc;
            *base = imm & 0xFFFFFF;
            return b + i;
        }
    }
    return NULL;
}

static unsigned isa_table_proven(const struct DpIsa *isa, struct DpMachine *m, uint32_t pc, int len) {
    (void)isa;
    uint8_t w[80];
    if (m->fetch_n) m->fetch_n(m, pc, w, sizeof w); else for (unsigned i = 0; i < sizeof w; i++) w[i] = m->fetch(m, pc + i);
    uint32_t base;
    if (!table_base_after(w, pc, len, &base)) return 0;
    unsigned n = 0;
    for (; n < 64; n++) {   // the entries are long little-endian 24-bit pointers
        uint32_t e = (uint32_t)m->fetch(m, base + n * 4) | (uint32_t)m->fetch(m, base + n * 4 + 1) << 8 |
                     (uint32_t)m->fetch(m, base + n * 4 + 2) << 16;
        if ((e & 1) || m->rom_index(m, e) < 0) break;
    }
    return n;
}

static int isa_rom_table_follows(const struct DpIsa *isa, struct DpMachine *m, uint32_t pc, int len) {
    (void)isa;
    uint8_t w[80];
    if (m->fetch_n) m->fetch_n(m, pc, w, sizeof w); else for (unsigned i = 0; i < sizeof w; i++) w[i] = m->fetch(m, pc + i);
    uint32_t base;
    if (!table_base_after(w, pc, len, &base)) return 0;
    return m->rom_index(m, base) >= 0;
}

// B4: the tables are arrays of long 24-bit pointers
static int isa_table_dispatch(const uint8_t *bytes, uint32_t *parameter) {
    DpTlcsInsn r;
    if (!dp_tlcs900_decode(bytes, 10, 0, &r)) return 0;
    if ((r.cls == DP_TLCS_JPMEM || r.cls == DP_TLCS_CALLCC) && !r.has_target && r.pair != 255) {
        *parameter = r.pair;
        return 1;
    }
    return 0;
}

static const DpIsa isa = {
    "tlcs900h", 3, 0,
    isa_length, isa_flow, isa_cond_of, isa_cond_flag, isa_reg_effect, isa_illegal, isa_disasm,
    isa_mem_operand,
    0x4000,        // code_min: the work RAM floor (the cartridge windows live above $200000)
    0, 0,          // hl_jp_table, abs_ind_table
    isa_imm_ptr, isa_imm_byte, isa_acc_imm, isa_st_acc_abs,
    DP_TLCSR_SP, 4, 0xFFFFFF, 0,   // the SP slot, 4-byte slots, 24-bit flat stack
    isa_entry_data_sig, isa_sp_ret,
    NULL,          // out_port: no IN/OUT, the I/O is memory-mapped
    isa_ram_load, 0x00, isa_ram_signature_wide,
    isa_table_proven, isa_rom_table_follows,
    4,             // table_entry_width: the long pointer tables
    isa_table_dispatch,
    NULL, NULL,    // code_table, code_ptr_store
};
const DpIsa *dp_isa_tlcs900(void) { return &isa; }
