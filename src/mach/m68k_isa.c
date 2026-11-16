// m68k_isa.c: the 68000 as a DpIsa (spec §7.3).  Wraps the existing static
// decoder (src/cpu/m68k/m68k_disasm.c), which already produces the text.
#include "../core/dpmachine.h"
#include "../cpu/m68k/m68k.h"
#include <stdio.h>

static int isa_length(uint8_t op) {
    DpM68kInsn in;
    uint8_t bytes[10] = { op, 0 };
    return dp_m68k_decode(bytes, sizeof bytes, 0, &in) ? (int)in.len : 2;
}

static int isa_flow(const uint8_t *bytes, uint32_t pc, DpFlowInfo *o) {
    DpM68kInsn in;
    o->kind = DP_FLOW_SEQ; o->fall = 1; o->call = 0; o->rst = 0; o->halt = 0;
    o->target = -1; o->cond = -1; o->length = 2; o->pair = DP_REG_PAIR_NONE;
    unsigned len = dp_m68k_decode(bytes, 10, pc, &in);
    if (!len) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return 2; }
    o->length = (uint8_t)len;
    o->cond = in.cond;
    uint16_t op = (uint16_t)(bytes[0] << 8 | bytes[1]);
    if ((op & 0xFFF8) == 0x4ED0) o->pair = DP_M68KR_A0 + (op & 7);   // jmp (An)
    if ((op & 0xFFF8) == 0x4E90) o->pair = DP_M68KR_A0 + (op & 7);   // jsr (An)
    switch (in.cls) {
    case DP_M68K_OTHER:
        break;
    case DP_M68K_BRA:
        o->kind = DP_FLOW_JUMP; o->fall = 0; o->target = in.target;
        break;
    case DP_M68K_BSR:
    case DP_M68K_TRAP:
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1;
        if (in.has_target) o->target = (int32_t)in.target;
        break;
    case DP_M68K_BCC:
    case DP_M68K_DBCC:
        o->kind = DP_FLOW_COND; o->fall = 1; o->target = in.target;
        break;
    case DP_M68K_JMP:
        if (in.has_target) { o->kind = DP_FLOW_JUMP; o->fall = 0; o->target = (int32_t)in.target; }
        else o->kind = DP_FLOW_INDIRECT;   // (An) / (d16,An) / (d8,PC,Xn): computed
        break;
    case DP_M68K_JSR:
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1;
        if (in.has_target) o->target = (int32_t)in.target;
        else o->kind = DP_FLOW_INDIRECT;   // jsr (An)...: computed, still a call
        break;
    case DP_M68K_RTS:
    case DP_M68K_RTE:
    case DP_M68K_RTR:
        o->kind = DP_FLOW_RET; o->fall = 0;
        break;
    case DP_M68K_STOP:
        o->halt = 1; o->fall = 0;
        break;
    case DP_M68K_ILLEGAL:
        o->kind = DP_FLOW_ILLEGAL; o->fall = 1;   // runs the illegal-exception vector
        break;
    }
    return o->length;
}

static int isa_cond_of(uint8_t op) {
    // Bcc (0x6n) and DBcc (0x50C8|cc) carry the condition in the low nibble;
    // the generic first-byte question can only answer Bcc
    if ((op & 0xF0) == 0x60) return op & 15;
    return -1;
}

// the tested flags are the SR condition codes; the explorer reads them live
static uint8_t isa_cond_flag(int cond) {
    (void)cond;
    return 0;   // no single-flag mapping (68000 conditions are composed)
}

static int isa_illegal(uint8_t op) {
    // line A (0xAxxx) and line F (0xFxxx) are the illegal groups; a single
    // byte cannot separate 0xA0xx legal (movec et al. are 68010) — the decode
    // is the authority
    DpM68kInsn in;
    uint8_t bytes[10] = { op, 0 };
    if (!dp_m68k_decode(bytes, sizeof bytes, 0, &in)) return 1;
    return in.cls == DP_M68K_ILLEGAL;
}

// --- C3: constant transfer over the DpRegState lattice ----------------------
// Sound subset: moveq, lea/movea absolute, immediate loads, register-to-
// register moves by size, addq/subq and clr on a register, exg/swap.  A move
// from memory kills its target; anything not modelled kills everything (never
// guessed).  Pointers are 24-bit: the walker masks the value with ptr_width.
static void m68_set32(DpRegState *s, unsigned slot, uint32_t v) { s->v[slot] = v; s->known[slot] = 0x0F; }
static void m68_setn(DpRegState *s, unsigned slot, uint32_t v, unsigned bytes) {
    s->v[slot] = v & ~(bytes == 4 ? 0u : 0xffffffffu << (8 * bytes));
    s->known[slot] = (uint8_t)((1u << bytes) - 1);
}
static void m68_kill(DpRegState *s, unsigned slot) { s->known[slot] = 0; s->v[slot] = 0; }
static void isa_reg_effect(const uint8_t *b, uint32_t pc, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    (void)pc; (void)rom;
    *out = *in;
    uint16_t op = (uint16_t)(b[0] << 8 | b[1]);
    unsigned src_mode = (op >> 3) & 7, src_reg = op & 7;
    unsigned dst_reg = (op >> 9) & 7, dst_mode = (op >> 6) & 7;
    unsigned size = op >> 12 == 1 ? 1 : op >> 12 == 2 ? 4 : op >> 12 == 3 ? 2 : 0;   // move.b/w/l byte counts
    if ((op & 0xF100) == 0x7000) { m68_set32(out, DP_M68KR_D0 + dst_reg, (uint32_t)(int32_t)(int8_t)op); return; }   // moveq
    if ((op & 0xF1FF) == 0x41F9) {   // lea (xxx).l,An
        m68_set32(out, DP_M68KR_A0 + dst_reg, (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]);
        return;
    }
    if ((op & 0xF1FF) == 0x207C || (op & 0xF1FF) == 0x307C) {   // movea.l/.w #imm,An
        uint32_t v = (op & 0xF1FF) == 0x207C
            ? (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]
            : (uint32_t)(int32_t)(int16_t)(b[2] << 8 | b[3]);
        m68_set32(out, DP_M68KR_A0 + dst_reg, v);
        return;
    }
    if ((op & 0xF1FF) == 0x203C || (op & 0xF1FF) == 0x103C || (op & 0xF1FF) == 0x303C) {   // move.l/b/w #imm,Dn
        unsigned n = (op & 0xF1FF) == 0x203C ? 4 : (op & 0xF1FF) == 0x103C ? 1 : 2;
        uint32_t v = n == 4 ? (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]
                            : n == 1 ? b[2] : (uint32_t)(uint16_t)(b[2] << 8 | b[3]);
        m68_setn(out, DP_M68KR_D0 + dst_reg, v, n);
        return;
    }
    if ((op & 0xF100) == 0x5000 && dst_mode <= 1 && ((op >> 6) & 3)) {   // addq/subq #q,<Dn|An>
        unsigned sz = (op >> 6) & 3, bytes = sz == 1 ? 1 : sz == 2 ? 2 : 4;
        unsigned sl = (dst_mode ? DP_M68KR_A0 : DP_M68KR_D0) + dst_reg;
        unsigned need = dst_mode ? 0x0F : (uint8_t)((1u << bytes) - 1);   // word/long ops on An run on the whole register
        if ((out->known[sl] & need) == need) {
            uint32_t q = dst_reg, d2 = (op & 0x0100) ? -(int32_t)q : (int32_t)q;   // bits 11-9 hold the quick data
            uint32_t v = out->v[sl];
            uint32_t mask = dst_mode || bytes == 4 ? 0xffffffffu : (1u << (8 * bytes)) - 1;
            m68_setn(out, sl, (v & ~mask) | ((uint32_t)((v + d2) & mask) & mask), dst_mode ? 4 : bytes);
        } else m68_kill(out, sl);
        return;
    }
    if ((op & 0xFF00) == 0x4200 && dst_mode <= 1 && (op >> 6 & 3)) {   // clr <Dn|An>: the zero is proven
        m68_setn(out, (dst_mode ? DP_M68KR_A0 : DP_M68KR_D0) + dst_reg, 0, (op >> 6 & 3) == 1 ? 1 : (op >> 6 & 3) == 2 ? 2 : 4);
        return;
    }
    if ((op & 0xF1F8) == 0xC140 || (op & 0xF1F8) == 0xC148 || (op & 0xF1F8) == 0xC188) {   // exg: the knowledge swaps too
        unsigned s1 = (op & 0xF1F8) == 0xC148 ? DP_M68KR_A0 + dst_reg : DP_M68KR_D0 + dst_reg;
        unsigned s2 = (op & 0xF1F8) == 0xC140 ? DP_M68KR_D0 + src_reg : DP_M68KR_A0 + src_reg;
        uint32_t tv = out->v[s1]; uint8_t tk = out->known[s1];
        out->v[s1] = out->v[s2]; out->known[s1] = out->known[s2];
        out->v[s2] = tv; out->known[s2] = tk;
        return;
    }
    if ((op & 0xFFF8) == 0x4840) {   // swap dn: the halves exchange their knowledge
        unsigned sl = DP_M68KR_D0 + src_reg;
        uint32_t v = out->v[sl];
        out->v[sl] = (v >> 16) | (v << 16);
        out->known[sl] = (uint8_t)(((out->known[sl] & 3) << 2) | (out->known[sl] >> 2));
        return;
    }
    if ((op & 0xF100) == 0x1000 || (op & 0xF100) == 0x2000 || (op & 0xF100) == 0x3000) {   // move <ea>,<Dn|An>
        if (dst_mode <= 1) {
            unsigned d2 = (dst_mode ? DP_M68KR_A0 : DP_M68KR_D0) + dst_reg;
            if (src_mode <= 1) {   // register source: copy the known bytes, size-limited
                unsigned ss = (src_mode ? DP_M68KR_A0 : DP_M68KR_D0) + src_reg;
                uint8_t k = (uint8_t)(out->known[ss] & ((1u << size) - 1));
                uint32_t v = out->v[ss] & (size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1);
                out->v[d2] = v; out->known[d2] = k;
                if (dst_mode && size == 2 && (k & 2)) {   // movea.w: bits 15 sign-extend bytes 2..3
                    out->v[d2] |= (out->v[d2] & 0x8000u) ? 0xffff0000u : 0;
                    out->known[d2] = (uint8_t)(k | 0x0C);
                }
            } else m68_kill(out, d2);   // memory/immediate source (the immediate forms above return first)
            return;
        }
        return;   // a memory destination writes no register (the stack push is the A7 rule below)
    }
    // the modelled no-write space keeps every register
    static const uint16_t keeps[] = {
        0x4E71,                  // nop
        0x4E70, 0x4E72,          // stop trapv (the instruction itself writes no D/A)
    };
    unsigned keep = (op & 0xF000) == 0x6000;   // bra/bsr/bcc/dbcc
    for (size_t i = 0; i < sizeof keeps / sizeof *keeps; i++) if (keeps[i] == op) keep = 1;
    if ((op & 0xF1C0) == 0xB000 || (op & 0xF1C0) == 0xB0C0 || (op & 0xFF00) == 0x0C00 ||
        (op & 0xFF00) == 0x0C80 || (op & 0xFF00) == 0x0D00 || (op & 0xFF00) == 0x0D80) keep = 1;   // cmp/cmpa/cmpi family
    if ((op & 0xF9FF) == 0x4AC0) keep = 1;     // tst (reads only)
    unsigned pea = (op & 0xFFF8) == 0x4870 || op == 0x4878 || op == 0x4879;
    if (pea) keep = 1;                         // pea (An)/(xxx).w/(xxx).l: writes no D/A
    if (keep) {
        if ((op & 0xF000) == 0x6000 && (op & 0x0100) == 0x0100) m68_kill(out, DP_M68KR_A7);   // bsr pushes
        if ((op & 0xFFC0) == 0x4E80) m68_kill(out, DP_M68KR_A7);                              // jsr pushes
        if (pea) m68_kill(out, DP_M68KR_A7);                                                  // pea pushes
        if (op == 0x4E75 || op == 0x4E73 || op == 0x4E77) m68_kill(out, DP_M68KR_A7);         // rts/rte/rtr pop
        return;
    }
    for (unsigned s2 = 0; s2 < 16; s2++) m68_kill(out, s2);   // unmodelled: never guessed
}

static int isa_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    DpM68kInsn in;
    unsigned avail = rom_size > addr ? (unsigned)(rom_size - addr) : 0;
    if (!avail) return 0;
    unsigned len = dp_m68k_decode(rom + addr, avail, addr, &in);
    if (!len) return 0;
    snprintf(out, outsz, "%s", in.text);
    return (int)len;
}

static int isa_mem_operand(const uint8_t *b, DpMemOp *o) {
    (void)b;
    o->access = DP_MA_NONE; o->mode = DP_AM_NONE; o->pair = 0; o->post = 0; o->width = 0; o->addr = 0;
    return 0;   // the 68000 operand taxonomy (§5) stays with the MD explorer for now
}

static int isa_imm_ptr(const uint8_t *b, uint32_t *imm) {   // B9: lea (xxx).l,An / movea.l #imm,An / pea (xxx).l
    int lea = (b[0] & 0xF1) == 0x41 && b[1] == 0xF9;
    int movea = (b[0] & 0xF1) == 0x20 && b[1] == 0x7C;
    int pea = b[0] == 0x48 && b[1] == 0x79;
    if (!lea && !movea && !pea) return 0;
    *imm = (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5];
    return 1;
}

static const DpIsa isa = { "68000", 3, 1, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_reg_effect, isa_illegal, isa_disasm, isa_mem_operand, 0x200u, 0, 0, isa_imm_ptr };
const DpIsa *dp_isa_68000(void) { return &isa; }
