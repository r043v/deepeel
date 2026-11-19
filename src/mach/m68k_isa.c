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
    (void)rom;
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
    if ((op & 0xF1FF) == 0x41FA) {   // lea d16(pc),An: a proven constant (the ROM table a shared dispatch helper reads)
        int16_t d = (int16_t)(b[2] << 8 | b[3]);
        m68_set32(out, DP_M68KR_A0 + dst_reg, (pc + 2u + (uint32_t)(int32_t)d) & 0xFFFFFFu);
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
    if ((op & 0xFFC0) == 0x4EC0) keep = 1;     // jmp: writes no register (lea T(pc),a5 ; jmp helper keeps the table base for the helper)
    unsigned pea = (op & 0xFFF8) == 0x4870 || op == 0x4878 || op == 0x4879;
    if (pea) keep = 1;                         // pea (An)/(xxx).w/(xxx).l: writes no D/A
    if (keep) {
        if ((op & 0xF000) == 0x6000 && (op & 0x0100) == 0x0100) m68_kill(out, DP_M68KR_A7);   // bsr pushes
        if ((op & 0xFFC0) == 0x4E80) m68_kill(out, DP_M68KR_A7);                              // jsr pushes
        if (pea) m68_kill(out, DP_M68KR_A7);                                                  // pea pushes
        if (op == 0x4E75 || op == 0x4E73 || op == 0x4E77) m68_kill(out, DP_M68KR_A7);         // rts/rte/rtr pop
        return;
    }
    {   // writers whose only register effect is one Dn (plus an autoincremented/decremented source An): the other constants survive
        unsigned grp = op >> 12, mode = (op >> 3) & 7, rg0 = op & 7, rx = (op >> 9) & 7, opm = (op >> 6) & 7;
        int dn = -1, an = -1, ok = 0;
        if ((grp == 0x8 || grp == 0xC || grp == 0x9 || grp == 0xD) && !(op & 0x0100) && opm != 3 && opm != 7) { dn = (int)rx; ok = 1; }   // or/and/sub/add <ea>,Dn
        else if ((grp == 0x8 || grp == 0xC) && (opm == 3 || opm == 7) && !(grp == 0xC && opm == 7 && mode <= 1)) { dn = (int)rx; ok = 1; }   // mulu/muls/divu/divs <ea>,Dn
        else if ((grp == 0x9 || grp == 0xD) && (opm == 3 || opm == 7)) { an = (int)rx; ok = 1; }                                         // suba/adda <ea>,An
        else if (grp == 0x0 && mode == 0 && (op & 0x0100) == 0 && ((op & 0x0E00) == 0x0000 || (op & 0x0E00) == 0x0200 || (op & 0x0E00) == 0x0400 || (op & 0x0E00) == 0x0600 || (op & 0x0E00) == 0x0A00)) { dn = (int)rg0; ok = 1; mode = 8; }   // ori/andi/subi/addi/eori #imm,Dn
        else if (grp == 0xE && ((op >> 6) & 3) != 3) { dn = (int)rg0; ok = 1; mode = 8; }                                                  // register shift/rotate
        else if ((op & 0xFFF8) == 0x4880 || (op & 0xFFF8) == 0x48C0 || (op & 0xFFF8) == 0x49C0 || (op & 0xFF38) == 0x4400 || (op & 0xFF38) == 0x4000 || (op & 0xFF38) == 0x4600) { if ((op & 0xFF38) == 0x4400 || (op & 0xFF38) == 0x4000 || (op & 0xFF38) == 0x4600) { if (mode == 0 && ((op >> 6) & 3) != 3) { dn = (int)rg0; ok = 1; mode = 8; } } else { dn = (int)rg0; ok = 1; mode = 8; } }   // neg/negx/not/ext/extb Dn
        if (ok) {
            if (dn >= 0) m68_kill(out, DP_M68KR_D0 + (unsigned)dn);
            if (an >= 0) m68_kill(out, DP_M68KR_A0 + (unsigned)an);
            if (mode == 3 || mode == 4) m68_kill(out, DP_M68KR_A0 + rg0);
            return;
        }
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

// B10: `move.l #imm,<dst>` where dst is a memory slot (abs.w/abs.l RAM, (An), (An)+, -(An), d16(An)): the
// immediate is a routine address parked in RAM (function-pointer slot, object routine field).
static int isa_code_ptr_store(const uint8_t *b, uint32_t *imm) {
    unsigned w = (b[0] << 8) | b[1];
    if ((w & 0xF03Fu) != 0x203Cu) return 0;   // move.l, source = immediate
    unsigned mode = (w >> 6) & 7, reg = (w >> 9) & 7;
    if (mode == 0 || mode == 1) return 0;     // register destinations are not a store
    if (mode == 7 && reg > 1) return 0;       // only abs.w / abs.l
    if (mode == 7) {
        uint32_t da = reg == 0 ? ((uint32_t)(int32_t)(int16_t)((b[6] << 8) | b[7]) & 0xFFFFFFu)
                               : (((uint32_t)b[6] << 24 | (uint32_t)b[7] << 16 | (uint32_t)b[8] << 8 | b[9]) & 0xFFFFFFu);
        if (da < 0xFF0000u && !(da >= 0xA00000u && da < 0xA10000u)) return 0;   // stores into RAM only
    }
    *imm = (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5];
    return 1;
}

// C3: `rts` pulls the 32-bit return address at (A7); `rte`/`rtr` pull more state — not modeled.
static int isa_sp_ret(const uint8_t *b, unsigned *w, unsigned *bias, unsigned *off) {
    if ((b[0] << 8 | b[1]) != 0x4E75) return 0;
    *w = 4; *bias = 0; *off = 0; return 1;
}
// M1: immediate of `width` bytes at b: returns its length in bytes, value in *v
static int m68k_imm(const uint8_t *b, int width, uint32_t *v) {
    if (width == 4) { *v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; return 4; }
    if (width == 2) { *v = (uint32_t)b[0] << 8 | b[1]; return 2; }
    *v = b[1]; return 2;
}
// M1: direct RAM load into a data register, `move.{b,w,l} abs.w|abs.l,Dn`: 00 ss DDD 000 111 00x (techniques 3, 4, 7)
static int isa_ram_load(const uint8_t *b, uint32_t *addr, int *idx_reg, int *width, int *dreg) {
    (void)idx_reg;
    unsigned w = (b[0] << 8) | b[1], sz = (w >> 12) & 3;
    if (!sz || ((w >> 6) & 7) != 0 || ((w >> 3) & 7) != 7 || (w & 7) > 1) return 0;
    *width = sz == 1 ? 1 : sz == 3 ? 2 : 4;
    *dreg = (int)((w >> 9) & 7);
    if ((w & 7) == 0) { *addr = (uint32_t)(int32_t)(int16_t)((b[2] << 8) | b[3]) & 0xFFFFFF; return 4; }
    *addr = ((uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]) & 0xFFFFFF;
    return 6;
}
// M1: signature pairing for the wide ISA — a standalone `cmpi.x #imm,abs` (00 xx 111 xxx), or the
// `cmpi.x`/`cmp.x` of the register just loaded.  Fills address, value and width; returns 1 on a signature.
static int isa_ram_signature_wide(DpMachine *m, uint32_t pc, const uint8_t *b, const uint8_t *f,
                                  int dl_len, int dl_width, int dl_dreg, uint32_t dl_addr,
                                  uint32_t *addr, uint32_t *val, int *width) {
    unsigned w0 = (b[0] << 8) | b[1];
    if (w0 == 0x0C38 || w0 == 0x0C39 || w0 == 0x0C78 || w0 == 0x0C79 || w0 == 0x0CB8 || w0 == 0x0CB9) {   // cmpi.x #imm,abs
        int sz = (w0 >> 6) & 3, wd = sz == 0 ? 1 : sz == 1 ? 2 : 4;
        uint8_t g[12];
        for (unsigned i = 0; i < sizeof g; i++) g[i] = m->fetch(m, pc + 2 + i);
        uint32_t imm; int il = m68k_imm(g, wd, &imm);
        uint32_t a;
        if (w0 & 1) a = ((uint32_t)g[il] << 24 | (uint32_t)g[il + 1] << 16 | (uint32_t)g[il + 2] << 8 | g[il + 3]) & 0xFFFFFF;
        else a = (uint32_t)(int32_t)(int16_t)((g[il] << 8) | g[il + 1]) & 0xFFFFFF;
        if (m->rom_index(m, a) >= 0) return 0;
        *addr = a; *val = imm; *width = wd;
        return 1;
    }
    if (!dl_len || dl_dreg < 0) return 0;
    unsigned w = (f[0] << 8) | f[1];
    int sz = dl_width == 1 ? 0 : dl_width == 2 ? 1 : 2;
    if (w == (0x0C00u | (unsigned)sz << 6 | (unsigned)dl_dreg) ||                       // cmpi.x #imm,Dn
        w == (0xB03Cu | (unsigned)dl_dreg << 9 | (unsigned)sz << 6)) {                   // cmp.x #imm,Dn
        uint32_t imm; m68k_imm(f + 2, dl_width, &imm);
        *addr = dl_addr; *val = imm; *width = dl_width;
        return 1;
    }
    return 0;
}
// M1: proven length (in entries) of the long table whose base `lea abs.l,An` / `lea d16(pc),An` (a ROM base)
// follows the load; an entry is proven while its long designates an even ROM address at or above code_min
static unsigned isa_table_proven(const DpIsa *isa, DpMachine *m, uint32_t pc, int len) {
    for (unsigned i = 0; i + 5 < 16; i += 2) {
        uint32_t w = (uint32_t)m->fetch(m, pc + len + i) << 8 | m->fetch(m, pc + len + i + 1);
        uint32_t base;
        if ((w & 0xF1FF) == 0x41F9)
            base = ((uint32_t)m->fetch(m, pc + len + i + 2) << 24 | (uint32_t)m->fetch(m, pc + len + i + 3) << 16 |
                    (uint32_t)m->fetch(m, pc + len + i + 4) << 8 | m->fetch(m, pc + len + i + 5)) & 0xFFFFFF;
        else if ((w & 0xF1FF) == 0x41FA)
            base = (pc + (uint32_t)len + i + 2 + (uint32_t)(int32_t)(int16_t)((m->fetch(m, pc + len + i + 2) << 8) | m->fetch(m, pc + len + i + 3))) & 0xFFFFFF;
        else continue;
        if (m->rom_index(m, base) < 0) continue;
        unsigned n = 0;
        while (n < 128) {
            uint32_t a4 = base + 4u * n;
            uint32_t t = ((uint32_t)m->fetch(m, a4) << 24 | (uint32_t)m->fetch(m, a4 + 1) << 16 | (uint32_t)m->fetch(m, a4 + 2) << 8 | m->fetch(m, a4 + 3)) & 0xFFFFFF;
            if ((t & 1u) || t < isa->code_min || m->rom_index(m, t) < 0) break;
            n++;
        }
        return n >= 2 ? n : 0;
    }
    return 0;
}
// M1: a ROM table access indexed by the loaded value follows within 16 bytes:
// ROM base into An (lea abs.l / lea d16(pc) / movea.l #imm), then an (d8,An,Xn) operand of a move/ALU word
static int isa_rom_table_follows(const DpIsa *isa, DpMachine *m, uint32_t pc, int len) {
    (void)isa;
    uint8_t win[16];
    for (unsigned i = 0; i < sizeof win; i++) win[i] = m->fetch(m, pc + len + i);
    int tab = 0;
    for (unsigned i = 0; i + 1 < sizeof win; i += 2) {
        unsigned w = (win[i] << 8) | win[i + 1];
        if (i + 5 < sizeof win && ((w & 0xF1FF) == 0x41F9 || w == (0x207Cu | (w & 0x0E00)))) {
            uint32_t a = ((uint32_t)win[i + 2] << 24 | (uint32_t)win[i + 3] << 16 | (uint32_t)win[i + 4] << 8 | win[i + 5]) & 0xFFFFFF;
            if (m->rom_index(m, a) >= 0) tab = 1;
            i += 4;   // skip the address extension
            continue;
        } else if (i + 3 < sizeof win && (w & 0xF1FF) == 0x41FA) {
            uint32_t a = (pc + (uint32_t)len + i + 2 + (uint32_t)(int32_t)(int16_t)((win[i + 2] << 8) | win[i + 3])) & 0xFFFFFF;
            if (m->rom_index(m, a) >= 0) tab = 1;
            i += 2;
            continue;
        }
        if (tab && ((w >> 3) & 7) == 6 && (w & 0xF000) != 0x4000 && (w & 0xF000) != 0x6000) return 1;
    }
    return 0;
}
// Register/addressing helpers for the table-read idioms.
static unsigned rd16(struct DpMachine *m, uint32_t at) { return (unsigned)(m->fetch(m, at) << 8 | m->fetch(m, at + 1)); }
static unsigned insn_len(struct DpMachine *m, uint32_t at) {
    uint8_t b[10]; for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, at + i);
    DpFlowInfo f; memset(&f, 0, sizeof f);
    return isa_flow(b, at, &f) && f.length ? (unsigned)f.length : 0;
}
// What consumes the value read from a table: up to 4 instructions of glue (movea.l Dn,Am / ext.l Dn / adda.l Dn,Am /
// adda.w Dn,Am) then jmp/jsr (Am) or jmp/jsr (d8,PC|Am,Xn).  Returns 1 when the value reaches an indirect jump/call.
// *via_idx = 1 when the final jump re-indexes the table itself (the value is an offset from the base).
static int reaches_jump(struct DpMachine *m, uint32_t at, unsigned dn, int *via_idx, int *d8j, int *abs_via_a) {
    const uint32_t start = at; unsigned cur = dn; int isa_ = 0; *via_idx = 0; *abs_via_a = -1; *d8j = 0;
    for (int n = 0; n < 4; n++) {
        unsigned op = rd16(m, at); unsigned len = insn_len(m, at);
        if (!len) return 0;
        if ((op & 0xFFF8u) == 0x4EF0u || (op & 0xFFF8u) == 0x4EB0u || op == 0x4EFB || op == 0x4EBB) {   // a PC-relative jump is compared to the read by effective base: the reads sit at start-4, the jump at `at`
            *via_idx = 1; *d8j = (int8_t)m->fetch(m, at + 3);
            if (op == 0x4EFB || op == 0x4EBB) *d8j += (int)(at - start) + 4;
            return 1;
        }
        if ((op & 0xFFF8u) == 0x4ED0u || (op & 0xFFF8u) == 0x4E90u) { *abs_via_a = (int)(op & 7); return 1; }
        if ((op & 0xF1F8u) == 0x2040u && (op & 7) == cur) isa_ = 1;                    // movea.l Dn,Am
        else if ((op & 0xFFF8u) == 0x48C0u && (op & 7) == cur) isa_ = 1;               // ext.l Dn
        else if ((op & 0xF1F8u) == 0xD1C0u && (op & 7) == cur) isa_ = 1;               // adda.l Dn,Am
        else if ((op & 0xF1F8u) == 0xD0C0u && (op & 7) == cur) isa_ = 1;               // adda.w Dn,Am
        else if ((op & 0xF1F8u) == 0x41F0u) isa_ = 1;                                  // lea (An,Xn),Am (rebase)
        else return 0;
        at += len;
    }
    return 0;
}
// Indirect dispatch through a table the code reads just before: the kernel calls this at every sequential or indirect
// instruction; it returns the table once the read is followed (within 4 instructions) by a jump or call through it.
//   jmp/jsr (d8,PC|An,Xn)                         word offsets from the base               (rel16)
//   move.w T(pc|An,Xn),Dn ; ... ; jmp T(pc|An,Xn) word offsets from the base               (rel16)
//   adda.w (An,Xn),An ; jmp (An)                  word offsets from An                     (rel16, base_reg)
//   move.w T,Dn ; ext.l ; adda.l Dn,An ; jmp (An) word offsets from An                     (rel16, base_reg)
//   move.w T,Dn ; movea.l Dn,Am ; jmp/jsr (Am)    16-bit absolute addresses                (abs16)
//   move.l/movea.l T,Dn|Am ; ... ; jsr (Am)       32-bit absolute addresses                (abs32)
// The base is PC-relative or a register slot (+ displacement) read from the concrete CPU / a walk's proven constants.
static int isa_code_table(struct DpMachine *m, uint32_t pc, const uint8_t *b, DpCodeTable *out, uint32_t *load_pc) {
    unsigned op = (unsigned)(b[0] << 8 | b[1]);
    *load_pc = 0; out->base_reg = -1; out->disp = 0; out->slots = 0; out->hi = 0; out->tdelta = 0;
    int jump = op == 0x4EFB || op == 0x4EBB || (op & 0xFFF8u) == 0x4EF0u || (op & 0xFFF8u) == 0x4EB0u;
    int pcrel, size = 0, adda = 0; unsigned dn = 8;
    if (jump) { pcrel = (op & 0xFFF0u) != 0x4EB0u && (op & 0xFFF0u) != 0x4EF0u; pcrel = op == 0x4EFB || op == 0x4EBB; size = 2; }
    else if ((op & 0xF1FFu) == 0x303Bu || (op & 0xF1F8u) == 0x3030u) { pcrel = (op & 0xF1FFu) == 0x303Bu; size = 2; dn = (op >> 9) & 7; }   // move.w (d8,PC|An,Xn),Dn
    else if ((op & 0xF1FFu) == 0x203Bu || (op & 0xF1F8u) == 0x2030u) { pcrel = (op & 0xF1FFu) == 0x203Bu; size = 4; dn = (op >> 9) & 7; }   // move.l ...,Dn
    else if ((op & 0xF1FFu) == 0x207Bu || (op & 0xF1F8u) == 0x2070u) { pcrel = (op & 0xF1FFu) == 0x207Bu; size = 4; dn = 8; }              // movea.l ...,Am
    else if ((op & 0xF1F8u) == 0xD0F0u) { pcrel = 0; size = 2; adda = 1; }                                                                  // adda.w (d8,An,Xn),Am
    else return 0;
    if (b[2] & 0x07) return 0;                     // brief extension word: bits 10..8 are zero
    unsigned an = pcrel ? 8 : (op & 7);
    int d8 = (int8_t)b[3];
    if (jump) {   // the jump itself indexes the table: offsets relative to its own base
        out->stride = 2; out->rel = 1;
        if (pcrel) {
            out->base = (pc + 2u + (uint32_t)(int32_t)d8) & 0xFFFFFFu;
            unsigned w0 = rd16(m, out->base);   // jmp d8(pc,Xn) into a run of branch instructions: the slots are the code (bra.w / bra.s / jmp)
            unsigned sl = w0 == 0x6000u ? 4 : (w0 & 0xFF00u) == 0x6000u && (w0 & 0xFFu) != 0xFFu ? 2 : w0 == 0x4EFAu || w0 == 0x4EF8u ? 4 : w0 == 0x4EF9u ? 6 : 0;
            if (sl) { out->slots = 1; out->stride = (uint8_t)sl; out->rel = 0; }
            return 1;
        }
        if (0) out->base = 0;
        else { out->base_reg = (int8_t)(DP_M68KR_A0 + an); out->disp = d8; out->base = 0; }
        return 1;
    }
    uint32_t after = pc + 4u; int via_idx, d8j, via_a;
    unsigned cur = dn;
    if (adda) {
        // the sum lands in Am; Am must then be jumped through
        cur = 8;
        unsigned am = (op >> 9) & 7;
        unsigned nop = rd16(m, after);
        if (!((nop & 0xFFF8u) == 0x4ED0u || (nop & 0xFFF8u) == 0x4E90u) || (nop & 7) != am || am != (op & 7) || d8 != 0) return 0;
        out->stride = 2; out->rel = 1; out->base_reg = (int8_t)(DP_M68KR_A0 + am); out->disp = 0; out->base = 0;
        return 1;
    }
    if (cur == 8) {   // movea.l T,Am: the jump must go through Am
        unsigned am = (op >> 9) & 7, nop = rd16(m, after);
        if (!((nop & 0xFFF8u) == 0x4ED0u || (nop & 0xFFF8u) == 0x4E90u) || (nop & 7) != am) return 0;
        via_idx = 0;
    } else if (!reaches_jump(m, after, cur, &via_idx, &d8j, &via_a)) return 0;
    if (size == 4) {
        if (via_idx) return 0;                      // a long read re-indexed: not a table of code
        out->stride = 4; out->rel = 0;              // absolute pointers (entry 0 may precede the table: ids start at 1)
    } else if (via_idx) {                           // move.w T,Dn ... jmp T(pc|An,Xn): offsets from the same base (displacement must match)
        int dl = d8j - d8;   // the jump's base may sit a few bytes after the read's (move.w 4(pc,d0.w),d0 ; jmp 2(pc,d0.w)): targets measured from it
        if (dl != 0 && !(pcrel && dl >= -8 && dl <= 8 && !(dl & 1))) return 0;
        out->stride = 2; out->rel = 1; out->tdelta = (int8_t)dl;
    } else {
        // the word is added to a register (ext.l ; adda.l) or loaded as an absolute address: told apart by the glue
        int rebase = 0;
        for (uint32_t at = after, n = 0; n < 4; n++) {
            unsigned o = rd16(m, at), l = insn_len(m, at);
            if (!l) break;
            if ((o & 0xF1F8u) == 0xD1C0u || (o & 0xF1F8u) == 0xD0C0u) { rebase = (int)(((o >> 9) & 7) + 1); break; }   // adda: offsets from Am
            if ((o & 0xFFF8u) == 0x4ED0u || (o & 0xFFF8u) == 0x4E90u) break;
            at += l;
        }
        if (rebase) {   // offsets from the register read as base: only the same register as the read's An is a table-relative base
            if (pcrel || (unsigned)(rebase - 1) != an || d8 != 0) return 0;
            out->stride = 2; out->rel = 1; out->base_reg = (int8_t)(DP_M68KR_A0 + an); out->disp = 0; out->base = 0;
            return 1;
        }
        out->stride = 2; out->rel = 0;              // 16-bit absolute addresses (high word clear unless the dispatcher seeded it)
        if (dn < 8)   // moveq #k,Dn ; swap Dn shortly before the read: the loaded word completes an address in bank k
            for (uint32_t back = 4; back <= 16; back += 2) {
                uint32_t at = pc - back;
                if (rd16(m, at + 2) == (0x4840u | dn) && (rd16(m, at) & 0xF100u) == 0x7000u && ((rd16(m, at) >> 9) & 7) == dn) { out->hi = (uint32_t)(rd16(m, at) & 0x7Fu) << 16; break; }
            }
    }
    if (pcrel) out->base = (pc + 2u + (uint32_t)(int32_t)d8) & 0xFFFFFFu;
    else { out->base_reg = (int8_t)(DP_M68KR_A0 + an); out->disp = d8; out->base = 0; }
    (void)via_a;
    return 1;
}
static const DpIsa isa = { "68000", 3, 1, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_reg_effect, isa_illegal, isa_disasm, isa_mem_operand, 0x200u, 0, 0, isa_imm_ptr,
                           .sp_slot = DP_M68KR_A7, .sp_width = 4, .sp_addr_mask = 0xFFFFFFFFu, .sp_base = 0, .entry_data_sig = NULL, .sp_ret = isa_sp_ret,
                           .ram_load = isa_ram_load, .ram_signature_wide = isa_ram_signature_wide,
                           .table_proven = isa_table_proven, .rom_table_follows = isa_rom_table_follows,
                           .table_entry_width = 4, .code_table = isa_code_table, .code_ptr_store = isa_code_ptr_store };   // B4: the lea tables are longs
const DpIsa *dp_isa_68000(void) { return &isa; }
