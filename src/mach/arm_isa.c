// arm_isa.c: the ARMv4T (ARM920T) instruction set as a DpIsa.  A code address
// carries the instruction set in DP_ARM_THUMB: ARM and Thumb are one ISA with a
// mode bit, so every path, root and static walk decodes the way the core would.
// ARMv4T interworks through BX only; BL is two halfwords in Thumb and the
// classic `mov lr, pc ; bx rN` pair is one call here (the machine runs both).
#include "../core/dpmachine.h"
#include <string.h>

#define TAG DP_ARM_THUMB
#define COND_AL 14

static uint32_t rd32(const uint8_t *b) { return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t ror32(uint32_t v, unsigned n) { n &= 31u; return n ? v >> n | v << (32u - n) : v; }

// -- flow ---------------------------------------------------------------------
static void set_flow(DpFlowInfo *f, unsigned kind, int cond) {
    f->kind = (uint8_t)kind;
    f->cond = (int8_t)cond;
    if (kind == DP_FLOW_COND) f->fall = 1;
    if (kind == DP_FLOW_CALL) { f->call = 1; f->fall = 1; }
}
// a computed transfer: conditional ones fork on their flags (COND without a target)
static void set_computed(DpFlowInfo *f, unsigned kind, int cond) {
    set_flow(f, cond >= 0 ? DP_FLOW_COND : kind, cond);
}

// the register a computed ARM transfer takes its target from, when it is one register
static int arm_target_reg(uint32_t w) {
    if ((w & 0x0FFFFFF0u) == 0x012FFF10u) return (int)(w & 15u);        // bx rm
    if ((w & 0x0FEFFFF0u) == 0x01A0F000u) return (int)(w & 15u);        // mov pc, rm
    return -1;
}

static void arm_flow(const uint8_t *b, uint32_t at, DpFlowInfo *f) {
    uint32_t w = rd32(b);
    unsigned c = w >> 28;
    int cond = c == COND_AL ? -1 : (int)c;
    f->length = 4;
    if (c == 15u) { set_flow(f, DP_FLOW_ILLEGAL, -1); return; }
    if ((w & 0x0FFFFFFFu) == 0x01A0E00Fu) {   // mov lr, pc ; <computed jump>: a call through a register or a pointer
        uint32_t n = rd32(b + 4);
        int reg = arm_target_reg(n);
        int load = (n & 0x0C10F000u) == 0x0410F000u;
        if (n >> 28 == c && ((reg >= 0 && reg != 14) || load)) {
            f->length = 8;
            f->pair = reg >= 0 && reg != 15 ? (uint8_t)reg : DP_REG_PAIR_NONE;
            set_flow(f, DP_FLOW_CALL, cond);
            return;
        }
    }
    if ((w & 0x0E000000u) == 0x0A000000u) {   // b, bl
        uint32_t off = (uint32_t)((int32_t)(w << 8) >> 6);
        f->target = (int32_t)(at + 8u + off);
        if (w & 0x01000000u) set_flow(f, DP_FLOW_CALL, cond);
        else if (cond >= 0) set_flow(f, DP_FLOW_COND, cond);
        else set_flow(f, DP_FLOW_JUMP, -1);
        return;
    }
    if ((w & 0x0FFFFFF0u) == 0x012FFF10u) {   // bx
        unsigned rm = w & 15u;
        if (rm == 15u) { f->target = (int32_t)(at + 8u); set_computed(f, DP_FLOW_JUMP, cond); return; }
        if (rm == 14u) { set_computed(f, DP_FLOW_RET, cond); return; }
        f->pair = (uint8_t)rm;
        set_computed(f, DP_FLOW_INDIRECT, cond);
        return;
    }
    switch (w >> 26 & 3u) {
    case 0: {
        if ((w & 0x0E000090u) == 0x00000090u) return;                    // multiply, swap, halfword transfer
        if ((w & 0x01900000u) == 0x01000000u) return;                    // status register moves
        unsigned op = w >> 21 & 15u;
        if ((op >= 8u && op <= 11u) || (w >> 12 & 15u) != 15u) return;
        if ((w & 0x0FFFFFF0u) == 0x01A0F000u) {                          // mov pc, rm
            unsigned rm = w & 15u;
            if (rm == 14u) { set_computed(f, DP_FLOW_RET, cond); return; }
            f->pair = (uint8_t)rm;
            set_computed(f, DP_FLOW_INDIRECT, cond);
            return;
        }
        if (w & 0x00100000u) { set_computed(f, DP_FLOW_RET, cond); return; }   // movs/subs pc, lr: exception return
        if ((w & 0x0FFFFFF0u) == 0x008FF100u && cond >= 0) {             // add<c> pc, pc, rN, lsl #2: a switch of branches
            set_flow(f, DP_FLOW_INDIRECT, cond); f->fall = 1; return;
        }
        set_computed(f, DP_FLOW_INDIRECT, cond);
        return;
    }
    case 1: {
        if ((w & 0x02000010u) == 0x02000010u) { set_flow(f, DP_FLOW_ILLEGAL, -1); return; }
        if (!(w & 0x00100000u) || (w >> 12 & 15u) != 15u) return;        // not a load of pc
        if ((w & 0x0FFFFFF0u) == 0x079FF100u && cond >= 0) {             // ldr<c> pc, [pc, rN, lsl #2]: a switch table
            set_flow(f, DP_FLOW_INDIRECT, cond); f->fall = 1; return;
        }
        set_computed(f, (w >> 16 & 15u) == 13u ? DP_FLOW_RET : DP_FLOW_INDIRECT, cond);
        return;
    }
    case 2: {
        if (!(w & 0x00100000u) || !(w & 0x8000u)) return;                // not an ldm with pc
        unsigned rn = w >> 16 & 15u;
        set_computed(f, rn == 13u || rn == 11u ? DP_FLOW_RET : DP_FLOW_INDIRECT, cond);
        return;
    }
    default:
        if ((w & 0x0F000000u) == 0x0F000000u) return;                    // swi
        if ((w & 0x0F000010u) == 0x0E000010u) return;                    // mcr, mrc
        set_flow(f, DP_FLOW_ILLEGAL, -1);                                // no coprocessor data path on this core
        return;
    }
}

static void thumb_flow(const uint8_t *b, uint32_t at, DpFlowInfo *f) {
    uint16_t h = rd16(b);
    f->length = 2;
    if (h == 0x46FEu) {   // mov lr, pc ; bx rN / mov pc, rN
        uint16_t n = rd16(b + 2);
        int reg = (n & 0xFF87u) == 0x4700u || (n & 0xFF87u) == 0x4687u ? (int)(n >> 3 & 15u) : -1;
        if (reg >= 0 && reg < 14) {
            f->length = 4;
            f->pair = (uint8_t)reg;
            set_flow(f, DP_FLOW_CALL, -1);
            return;
        }
    }
    if ((h & 0xF000u) == 0xD000u) {
        unsigned c = h >> 8 & 15u;
        if (c == 14u) { set_flow(f, DP_FLOW_ILLEGAL, -1); return; }
        if (c == 15u) return;                                            // swi
        f->target = (int32_t)((at + 4u + (uint32_t)((int32_t)(int8_t)h * 2)) | TAG);
        set_flow(f, DP_FLOW_COND, (int)c);
        return;
    }
    if ((h & 0xF800u) == 0xE000u) {
        f->target = (int32_t)((at + 4u + (uint32_t)((int32_t)((uint32_t)h << 21) >> 20)) | TAG);
        set_flow(f, DP_FLOW_JUMP, -1);
        return;
    }
    if ((h & 0xF800u) == 0xE800u) { set_flow(f, DP_FLOW_ILLEGAL, -1); return; }
    if ((h & 0xF800u) == 0xF000u) {   // bl prefix
        uint16_t n = rd16(b + 2);
        if ((n & 0xF800u) == 0xF800u) {
            uint32_t hi = (uint32_t)((int32_t)((uint32_t)h << 21) >> 9);
            f->length = 4;
            f->target = (int32_t)((at + 4u + hi + ((n & 0x7FFu) << 1)) | TAG);
            set_flow(f, DP_FLOW_CALL, -1);
        }
        return;                                                          // a lone prefix only writes lr
    }
    if ((h & 0xF800u) == 0xF800u) { set_flow(f, DP_FLOW_CALL, -1); return; }   // a lone suffix: call through lr
    if ((h & 0xFF87u) == 0x4700u) {   // bx
        unsigned rm = h >> 3 & 15u;
        if (rm == 14u) { set_flow(f, DP_FLOW_RET, -1); return; }
        if (rm == 15u) { f->target = (int32_t)((at + 4u) & ~3u); set_flow(f, DP_FLOW_JUMP, -1); return; }   // veneer into ARM
        f->pair = (uint8_t)rm;
        set_flow(f, DP_FLOW_INDIRECT, -1);
        return;
    }
    if ((h & 0xFF80u) == 0x4780u) { set_flow(f, DP_FLOW_ILLEGAL, -1); return; }   // blx: ARMv5
    if (((h & 0xFF00u) == 0x4400u || (h & 0xFF00u) == 0x4600u) && (h & 0x87u) == 0x87u) {   // add pc, rm / mov pc, rm
        unsigned rm = h >> 3 & 15u;
        if ((h & 0xFF00u) == 0x4600u) {
            if (rm == 14u) { set_flow(f, DP_FLOW_RET, -1); return; }
            f->pair = (uint8_t)rm;
        }
        set_flow(f, DP_FLOW_INDIRECT, -1);
        return;
    }
    if ((h & 0xFF00u) == 0xBD00u) { set_flow(f, DP_FLOW_RET, -1); return; }   // pop {..., pc}
    if ((h & 0xF000u) == 0xB000u) {   // the miscellaneous group: add sp, push, pop; the rest is ARMv5+
        unsigned g = h >> 8 & 15u;
        if (g != 0u && g != 4u && g != 5u && g != 12u && g != 13u) set_flow(f, DP_FLOW_ILLEGAL, -1);
    }
}

static int isa_flow(const uint8_t *b, uint32_t pc, DpFlowInfo *f) {
    memset(f, 0, sizeof *f);
    f->kind = DP_FLOW_SEQ;
    f->cond = -1;
    f->target = -1;
    f->pair = DP_REG_PAIR_NONE;
    if (pc & TAG) thumb_flow(b, pc & ~TAG, f);
    else arm_flow(b, pc, f);
    if (f->kind == DP_FLOW_SEQ) f->fall = 1;
    return 1;
}

static int isa_length(uint8_t op) { (void)op; return 4; }   // only a fallback: flow always answers
static int isa_cond_of(uint8_t op) { (void)op; return -1; }  // the condition is not in the first byte
static uint8_t isa_cond_flag(int cond) {   // the CPSR flags (N Z C V as bits 3..0) the condition reads
    static const uint8_t m[16] = { 4, 4, 2, 2, 8, 8, 1, 1, 6, 6, 9, 9, 13, 13, 0, 0 };
    return m[cond & 15];
}

// -- register constants ---------------------------------------------------------
static void kill(DpRegState *s, unsigned r) { if (r < 15u) s->known[r] = 0; }
static void put(DpRegState *s, unsigned r, uint32_t v) { if (r < 15u) { s->v[r] = v; s->known[r] = 0x0F; } }
static int get(const DpRegState *s, unsigned r, uint32_t pcv, uint32_t *v) {
    if (r == 15u) { *v = pcv; return 1; }
    if ((s->known[r] & 0x0F) != 0x0F) return 0;
    *v = s->v[r];
    return 1;
}
static void clobber_call(DpRegState *s) { for (unsigned r = 0; r < 4u; r++) kill(s, r); kill(s, 12); kill(s, 14); }
static int rom_word(const DpProvRom *rom, uint32_t a, uint32_t *v) {
    uint8_t b[4];
    if (!rom || !rom->read) return 0;
    for (unsigned i = 0; i < 4u; i++) if (!rom->read(rom->ctx, a + i, &b[i])) return 0;
    *v = rd32(b);
    return 1;
}
static void load_literal(DpRegState *s, unsigned r, const DpProvRom *rom, uint32_t a) {
    uint32_t v;
    if (rom_word(rom, a, &v)) put(s, r, v); else kill(s, r);
}

static void arm_effect(const uint8_t *b, uint32_t at, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    uint32_t w = rd32(b), pcv = at + 8u;
    int cond = w >> 28 != COND_AL;
    unsigned rn = w >> 16 & 15u, rd = w >> 12 & 15u;
    if ((w & 0x0FFFFFFFu) == 0x01A0E00Fu && arm_target_reg(rd32(b + 4)) >= 0) { clobber_call(out); return; }
    switch (w >> 26 & 3u) {
    case 0:
        if ((w & 0x0FFFFFF0u) == 0x012FFF10u) return;
        if ((w & 0x0E000090u) == 0x00000090u) {
            if ((w & 0x0F0000F0u) == 0x00000090u) { kill(out, rn); kill(out, rd); }        // multiply
            else if ((w & 0x0F000FF0u) == 0x01000090u) kill(out, rd);                       // swap
            else {
                if (w & 0x00100000u) kill(out, rd);
                if (!(w & 0x01000000u) || w & 0x00200000u) kill(out, rn);                   // writeback
            }
            return;
        }
        if ((w & 0x01900000u) == 0x01000000u) { if (!(w & 0x00200000u)) kill(out, rd); return; }
        {
            unsigned op = w >> 21 & 15u;
            if (op >= 8u && op <= 11u) return;
            uint32_t a = 0, c = 0, v = 0;
            int ok = 1;
            if (w & 0x02000000u) c = ror32(w & 255u, (w >> 7) & 30u);
            else if (w & 0x10u) ok = 0;
            else {
                unsigned sh = w >> 5 & 3u, n = w >> 7 & 31u;
                ok = get(in, w & 15u, pcv, &c);
                if (sh == 0u) c <<= n;
                else if (sh == 1u) c = n ? c >> n : 0u;
                else if (sh == 2u) c = (uint32_t)((int32_t)c >> (n ? n : 31u));
                else if (n) c = ror32(c, n);
                else ok = 0;                                                                // rrx reads the carry
            }
            if (ok && op != 13u && op != 15u) ok = get(in, rn, pcv, &a);
            switch (op) {
            case 0: v = a & c; break;
            case 1: v = a ^ c; break;
            case 2: v = a - c; break;
            case 3: v = c - a; break;
            case 4: v = a + c; break;
            case 12: v = a | c; break;
            case 13: v = c; break;
            case 14: v = a & ~c; break;
            case 15: v = ~c; break;
            default: ok = 0; break;
            }
            if (ok && !cond) put(out, rd, v); else kill(out, rd);
            return;
        }
    case 1: {
        int load = (w & 0x00100000u) != 0;
        if (load) {
            if (!cond && (w & 0x0F7F0000u) == 0x051F0000u)                                   // ldr rd, [pc, #imm]
                load_literal(out, rd, rom, w & 0x00800000u ? pcv + (w & 4095u) : pcv - (w & 4095u));
            else kill(out, rd);
        }
        if (!(w & 0x01000000u) || w & 0x00200000u) kill(out, rn);
        return;
    }
    case 2:
        if (w & 0x02000000u) { if (w & 0x01000000u) clobber_call(out); return; }
        if (w & 0x00100000u) for (unsigned r = 0; r < 15u; r++) if (w >> r & 1u) kill(out, r);
        if (w & 0x00200000u) kill(out, rn);
        return;
    default:
        if ((w & 0x0F000000u) == 0x0F000000u) clobber_call(out);                             // swi
        else if ((w & 0x0F100010u) == 0x0E100010u) kill(out, rd);                            // mrc
        return;
    }
}

static void thumb_effect(const uint8_t *b, uint32_t at, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    uint16_t h = rd16(b);
    uint32_t pcv = at + 4u, a, c;
    unsigned rd = h & 7u, rs = h >> 3 & 7u, r8 = h >> 8 & 7u;
    if (h == 0x46FEu) { clobber_call(out); return; }
    switch (h >> 12) {
    case 0: case 1:
        if ((h >> 11) == 3u) {   // add/sub register or imm3
            unsigned rn = h >> 6 & 7u;
            int ok = get(in, rs, pcv, &a);
            if (h & 0x400u) c = rn; else ok = ok && get(in, rn, pcv, &c);
            if (ok) put(out, rd, h & 0x200u ? a - c : a + c); else kill(out, rd);
        } else if (get(in, rs, pcv, &a)) {
            unsigned op = h >> 11 & 3u, n = h >> 6 & 31u;
            if (op == 0u) put(out, rd, a << n);
            else if (op == 1u) put(out, rd, n ? a >> n : 0u);
            else put(out, rd, (uint32_t)((int32_t)a >> (n ? n : 31u)));
        } else kill(out, rd);
        return;
    case 2: case 3: {
        unsigned op = h >> 11 & 3u;
        uint32_t imm = h & 255u;
        if (op == 0u) put(out, r8, imm);
        else if (op != 1u) { if (get(in, r8, pcv, &a)) put(out, r8, op == 2u ? a + imm : a - imm); else kill(out, r8); }
        return;
    }
    case 4:
        if (h & 0x0800u) { load_literal(out, r8, rom, (pcv & ~3u) + (h & 255u) * 4u); return; }
        if (!(h & 0x0400u)) { unsigned op = h >> 6 & 15u; if (op != 8u && op != 10u && op != 11u) kill(out, rd); return; }
        {
            unsigned op = h >> 8 & 3u, d = rd | (h >> 4 & 8u), m = h >> 3 & 15u;
            if (op == 3u || op == 1u) return;
            int ok = get(in, m, pcv, &c);
            if (op == 0u) ok = ok && get(in, d, pcv, &a), c += ok ? a : 0u;
            if (ok) put(out, d, c); else kill(out, d);
        }
        return;
    case 5: if ((h >> 9 & 7u) >= 3u) kill(out, rd); return;
    case 6: case 7: case 8: if (h & 0x0800u) kill(out, rd); return;
    case 9: if (h & 0x0800u) kill(out, r8); return;
    case 10:
        if (!(h & 0x0800u)) put(out, r8, (pcv & ~3u) + (h & 255u) * 4u);
        else if (get(in, 13, pcv, &a)) put(out, r8, a + (h & 255u) * 4u);
        else kill(out, r8);
        return;
    case 11:
        if ((h & 0x0F00u) == 0u) { if (get(in, 13, pcv, &a)) put(out, 13, h & 0x80u ? a - (h & 127u) * 4u : a + (h & 127u) * 4u); }
        else {
            if (h & 0x0800u) for (unsigned r = 0; r < 8u; r++) if (h >> r & 1u) kill(out, r);
            kill(out, 13);
        }
        return;
    case 12:
        if (h & 0x0800u) for (unsigned r = 0; r < 8u; r++) if (h >> r & 1u) kill(out, r);
        kill(out, r8);
        return;
    case 13: if ((h & 0x0F00u) == 0x0F00u) clobber_call(out); return;
    case 15: clobber_call(out); return;
    default: return;
    }
}

static void isa_reg_effect(const uint8_t *b, uint32_t pc, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    *out = *in;
    if (pc & TAG) thumb_effect(b, pc & ~TAG, in, out, rom);
    else arm_effect(b, pc, in, out, rom);
}

// BX selects the instruction set from bit 0 of the register; the other computed transfers keep the current one
static uint32_t isa_reg_target(const uint8_t *b, uint32_t pc, uint32_t v) {
    int bx = 1;
    if (b) {
        if (pc & TAG) { uint16_t h = rd16(b); if (h == 0x46FEu) h = rd16(b + 2); bx = (h & 0xFF87u) == 0x4700u; }
        else { uint32_t w = rd32(b); if ((w & 0x0FFFFFFFu) == 0x01A0E00Fu) w = rd32(b + 4); bx = (w & 0x0FFFFFF0u) == 0x012FFF10u; }
    }
    if (!bx) return pc & TAG ? (v & ~1u & ~TAG) | TAG : v & ~3u & ~TAG;
    return v & 1u ? (v & ~1u) | TAG : v & ~3u & ~TAG;
}

// -- code tables ----------------------------------------------------------------
static uint32_t fetch32(DpMachine *m, uint32_t a) {
    uint8_t b[4];
    for (unsigned i = 0; i < 4u; i++) b[i] = m->fetch(m, a + i);
    return rd32(b);
}
// entries of a switch indexed by `reg`: the bound of the `cmp reg, #n` just above (0: no bound found)
static unsigned arm_bound(DpMachine *m, uint32_t pc, unsigned reg, unsigned cond) {
    for (uint32_t back = 4; back <= 12u; back += 4u) {
        uint32_t w = fetch32(m, pc - back);
        if ((w & 0x0FF0F000u) == 0x03500000u && (w >> 16 & 15u) == reg) {
            uint32_t n = ror32(w & 255u, (w >> 7) & 30u) + (cond == 3u ? 0u : 1u);   // ls: n+1 cases, lo: n
            return n > 256u ? 256u : n;
        }
    }
    return 0;
}
static int isa_code_table(struct DpMachine *m, uint32_t pc, const uint8_t *b, DpCodeTable *out, uint32_t *load_pc) {
    memset(out, 0, sizeof *out);
    out->base_reg = -1;
    out->stride = 4;
    *load_pc = 0;
    if (!(pc & TAG)) {
        uint32_t w = rd32(b);
        if ((w & 0x0FFFFFF0u) == 0x008FF100u || (w & 0x0FFFFFF0u) == 0x079FF100u) {
            unsigned n = arm_bound(m, pc, w & 15u, w >> 28);
            if (!n) return 0;
            out->slots = (w & 0x0FFFFFF0u) == 0x008FF100u;
            out->base = pc + 8u;
            out->count = (uint16_t)n;
            return 1;
        }
        if ((w & 0x0F7FF000u) == 0x051FF000u) {   // ldr pc, [pc, #imm]: a veneer or an exception vector
            out->base = w & 0x00800000u ? pc + 8u + (w & 4095u) : pc + 8u - (w & 4095u);
            out->count = 1;
            return 1;
        }
        if ((w & 0x0FF0FFF0u) == 0x0790F100u && (w >> 16 & 15u) != 15u) {   // ldr pc, [rB, rN, lsl #2]
            out->base_reg = (int8_t)(w >> 16 & 15u);
            out->count = (uint16_t)arm_bound(m, pc, w & 15u, 9u);
            return 1;
        }
        return 0;
    }
    uint16_t h = rd16(b);
    if ((h & 0xFF87u) != 0x4687u) return 0;   // mov pc, rm after `ldr rT, =table ; ldr rm, [rT, rI]`
    uint32_t at = pc & ~TAG, table = 0;
    unsigned bound = 0;
    for (uint32_t back = 2; back <= 16u; back += 2u) {
        uint16_t p = (uint16_t)(m->fetch(m, at - back) | m->fetch(m, at - back + 1u) << 8);
        if (!table && (p & 0xF800u) == 0x4800u) {
            uint32_t v = fetch32(m, ((at - back + 4u) & ~3u) + (p & 255u) * 4u);
            if (m->rom_index(m, v) >= 0 && !(v & 3u)) table = v;
        }
        if (!bound && (p & 0xF800u) == 0x2800u) bound = (p & 255u) + 1u;
    }
    if (!table || !bound) return 0;
    out->base = table;
    out->hi = TAG;
    out->count = (uint16_t)bound;
    return 1;
}

// -- static analysis ----------------------------------------------------------
static int isa_entry_data_sig(const uint8_t *w, unsigned n) {
    if (n < 8u) return 0;
    uint32_t v = rd32(w);
    if (!v || !(w[2] | w[3])) return 1;                                  // a small constant
    if (v >= 0x0C000000u && v < 0x0C800000u) return 1;                   // a RAM pointer (condition EQ as ARM)
    if (v >= 0x14000000u && v < 0x16000000u) return 1;                   // an I/O address
    unsigned text = 0;
    for (unsigned i = 0; i < 8u; i++) text += w[i] >= 0x20u && w[i] < 0x7Fu;
    return text == 8u;
}

static int arm_terminator(uint32_t w) {
    if (w == 0xE12FFF1Eu || w == 0xE1A0F00Eu) return 1;                  // bx lr, mov pc, lr
    if ((w & 0xFF108000u) == 0xE8108000u) return 1;                      // ldm ..., {..., pc}
    if ((w & 0xFF000000u) == 0xEA000000u) return 1;                      // b
    return (w & 0xFFFFFFF0u) == 0xE12FFF10u;                             // bx
}
static int thumb_terminator(uint16_t h) {
    return (h & 0xFF87u) == 0x4700u || (h & 0xFF00u) == 0xBD00u || (h & 0xF800u) == 0xE000u || h == 0x46F7u || h == 0x0000u || h == 0x46C0u;
}
static int thumb_push_lr(uint16_t h) { return (h & 0xFF00u) == 0xB500u; }
static int arm_push_lr(uint32_t w) { return (w & 0xFFFF4000u) == 0xE92D4000u; }

// Routine heads visible in the image: ARM `stmfd sp!, {..., lr}` (with the APCS
// `mov ip, sp` before it), Thumb `push {..., lr}` after a terminator or a
// literal pool, and pointer words that designate code (an odd value: a Thumb
// function pointer; an even one: an ARM routine placed after a terminator).
static size_t isa_static_roots(const uint8_t *rom, size_t n, uint32_t base, uint32_t *out, size_t max) {
    size_t c = 0;
    for (size_t k = 0; k + 4 <= n && c < max; k += 4) {
        uint32_t w = rd32(rom + k);
        if (arm_push_lr(w)) {
            size_t head = k >= 4 && rd32(rom + k - 4) == 0xE1A0C00Du ? k - 4 : k;
            out[c++] = base + (uint32_t)head;
        }
    }
    for (size_t k = 2; k + 4 <= n && c < max; k += 2) {
        if (!thumb_push_lr(rd16(rom + k)) || !rd16(rom + k + 2)) continue;
        uint16_t p = rd16(rom + k - 2);
        uint32_t pool = k >= 4 && !(k & 3u) ? rd32(rom + k - 4) : 0;
        if (thumb_terminator(p) || (pool >= base && pool - base < n)) out[c++] = (base + (uint32_t)k) | TAG;
    }
    for (size_t k = 0; k + 4 <= n && c < max; k += 4) {
        uint32_t v = rd32(rom + k);
        if (v < base || v - base >= n) continue;
        size_t t = (v - base) & ~(size_t)1;
        if (v & 1u) {
            if (t >= 2 && t + 4 <= n && (thumb_push_lr(rd16(rom + t)) || thumb_terminator(rd16(rom + t - 2)))) out[c++] = (base + (uint32_t)t) | TAG;
        } else if (!(v & 2u) && t >= 4 && t + 8 <= n && arm_terminator(rd32(rom + t - 4)) && rd32(rom + t) >> 28 == COND_AL) {
            out[c++] = v;
        }
    }
    return c;
}

const DpIsa *dp_isa_arm920t(void) {
    static const DpIsa isa = {
        .name = "arm920t", .ptr_width = 4, .big_endian = 0, .length = isa_length, .flow = isa_flow, .cond_of = isa_cond_of,
        .cond_flag = isa_cond_flag, .reg_effect = isa_reg_effect, .sp_slot = DP_ARMR_SP, .sp_width = 0,
        .entry_data_sig = isa_entry_data_sig, .code_table = isa_code_table, .mode_mask = DP_ARM_THUMB, .no_ptr16_guard = 1,
        .reg_target = isa_reg_target, .static_roots = isa_static_roots,
    };
    return &isa;
}
