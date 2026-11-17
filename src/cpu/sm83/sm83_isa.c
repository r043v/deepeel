// sm83_isa.c: the SM83 as a DpIsa (spec §7.3).  Wraps the existing helpers;
// the classification mirrors what the static closure used to open-code.
#include "../../core/dpmachine.h"
#include "sm83.h"

static int is_illegal(uint8_t op) {
    switch (op) {
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4:
    case 0xEB: case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
        return 1;
    default:
        return 0;
    }
}

static int isa_cond_of(uint8_t op) { return sm83_cond_of(op); }
static uint8_t isa_cond_flag(int c) { return sm83_cond_flag(c); }
static int isa_illegal(uint8_t op) { return is_illegal(op); }

// --- C3: constant transfer over the DpRegState lattice ----------------------
// Sound and minimal: immediates, register copies, proven (hl)/(nn) reads and
// the deterministic inc/dec of known bytes and pairs keep or set constants;
// anything written without a provable value loses them.  Untracked: F (the
// flags byte of AF never rides a code pointer).
// slot of register code y/z (0 B .. 5 L, 7 A): pairs, low byte first
static unsigned sm_byte_slot(unsigned r, unsigned *byte) {
    *byte = (r & 1) ^ 1;                       // B,D,H high; C,E,L low
    return r < 6 ? r >> 1 : DP_SM83R_A;         // BC DE HL, then A
}
static void sm_set_byte(DpRegState *s, unsigned slot, unsigned byte, uint8_t v) {
    unsigned bit = 1u << byte;
    s->v[slot] = (s->v[slot] & ~(0xffu << (8 * byte))) | (uint32_t)v << (8 * byte);
    s->known[slot] |= (uint8_t)bit;
}
static void sm_kill_byte(DpRegState *s, unsigned slot, unsigned byte) {
    unsigned bit = 1u << byte;
    s->v[slot] &= ~(0xffu << (8 * byte));
    s->known[slot] &= (uint8_t)~bit;
}
static void sm_step_byte(DpRegState *s, unsigned slot, unsigned byte, int delta) {
    if (s->known[slot] & (1u << byte)) sm_set_byte(s, slot, byte, (uint8_t)((s->v[slot] >> (8 * byte)) + delta));
    else sm_kill_byte(s, slot, byte);
}
static void sm_step_pair(DpRegState *s, unsigned slot, int delta) {
    if ((s->known[slot] & 3) == 3) s->v[slot] = (uint32_t)((uint16_t)(s->v[slot] + delta));
    else { sm_step_byte(s, slot, 0, delta); sm_kill_byte(s, slot, 1); }
}
static void sm_kill_pair(DpRegState *s, unsigned slot) { s->known[slot] &= (uint8_t)~3; s->v[slot] &= 0xffff0000u; }
static void isa_reg_effect(const uint8_t *b, uint32_t pc, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    (void)pc;
    *out = *in;
    uint8_t op = b[0];
    unsigned by, dslot, sslot;
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E:   // ld r,n
        dslot = sm_byte_slot((op >> 3) & 7, &by);
        sm_set_byte(out, dslot, by, b[1]);
        break;
    case 0x01: case 0x11: case 0x21: case 0x31:   // ld rr,nn
        { unsigned s2 = op == 0x31 ? DP_SM83R_SP : op >> 4;
          out->v[s2] = (uint32_t)(b[1] | (b[2] << 8)); out->known[s2] |= 3; }
        break;
    case 0x3E: sm_set_byte(out, DP_SM83R_A, 0, b[1]); break;   // ld a,n
    case 0x40 ... 0x7F:   // ld d,s (HALT excluded below)
        if (op == 0x76) break;
        dslot = sm_byte_slot((op >> 3) & 7, &by);
        if ((op & 7) == 6) {   // ld r,(hl): proven only through the ROM view
            uint8_t v;
            if ((out->known[DP_SM83R_HL] & 3) == 3 && rom->read(rom->ctx, out->v[DP_SM83R_HL], &v))
                sm_set_byte(out, dslot, by, v);
            else sm_kill_byte(out, dslot, by);
        } else {   // ld r,r': copy the known byte
            unsigned sby;
            sslot = sm_byte_slot(op & 7, &sby);
            if (in->known[sslot] & (1u << sby)) sm_set_byte(out, dslot, by, (uint8_t)(in->v[sslot] >> (8 * sby)));
            else sm_kill_byte(out, dslot, by);
        }
        break;
    case 0x22: case 0x32: sm_step_pair(out, DP_SM83R_HL, op == 0x22 ? 1 : -1); break;   // ld (hl±),a: the pointer steps
    case 0x2A: case 0x3A: {   // ld a,(hl±): proven read, then the pointer steps
        uint8_t v;
        if ((out->known[DP_SM83R_HL] & 3) == 3 && rom->read(rom->ctx, out->v[DP_SM83R_HL], &v)) sm_set_byte(out, DP_SM83R_A, 0, v);
        else sm_kill_byte(out, DP_SM83R_A, 0);
        sm_step_pair(out, DP_SM83R_HL, op == 0x2A ? 1 : -1);
        break; }
    case 0x23: sm_step_pair(out, DP_SM83R_HL, 1); break;
    case 0x2B: sm_step_pair(out, DP_SM83R_HL, -1); break;
    case 0x09: case 0x19: case 0x29: case 0x39:   // add hl,rr: exact only from two fully known pairs
        { unsigned s2 = op == 0x39 ? DP_SM83R_SP : op >> 4;
          if ((out->known[DP_SM83R_HL] & 3) == 3 && (out->known[s2] & 3) == 3) {
              out->v[DP_SM83R_HL] = (uint32_t)((uint16_t)(out->v[DP_SM83R_HL] + out->v[s2])); out->known[DP_SM83R_HL] = 3;
          } else sm_kill_pair(out, DP_SM83R_HL); }
        break;
    case 0x0A: case 0x1A: {   // ld a,(bc)/(de): proven only through the ROM view
        uint8_t v; unsigned s2 = op == 0x0A ? DP_SM83R_BC : DP_SM83R_DE;
        if ((out->known[s2] & 3) == 3 && rom->read(rom->ctx, out->v[s2], &v)) sm_set_byte(out, DP_SM83R_A, 0, v);
        else sm_kill_byte(out, DP_SM83R_A, 0);
        break; }
    case 0x03: case 0x13: case 0x33: sm_step_pair(out, (op >> 4) & 3, 1); break;   // rp code = slot (BC DE HL SP)
    case 0x0B: case 0x1B: case 0x3B: sm_step_pair(out, (op >> 4) & 3, -1); break;
    case 0x34: case 0x35: break;   // inc/dec (hl): memory only
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C:
        sm_byte_slot((op >> 3) & 7, &by); sm_step_byte(out, sm_byte_slot((op >> 3) & 7, &by), by, 1);
        break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D:
        sm_byte_slot((op >> 3) & 7, &by); sm_step_byte(out, sm_byte_slot((op >> 3) & 7, &by), by, -1);
        break;
    case 0xFA: {   // ld a,(nn): a proven absolute read
        uint8_t v;
        if (rom->read(rom->ctx, (uint32_t)(b[1] | (b[2] << 8)), &v)) sm_set_byte(out, DP_SM83R_A, 0, v);
        else sm_kill_byte(out, DP_SM83R_A, 0);
        break; }
    case 0xF0: case 0xF2: sm_kill_byte(out, DP_SM83R_A, 0); break;   // ldh a,(n)/(C): I/O, never proven
    case 0xF8: sm_kill_pair(out, DP_SM83R_HL); break;   // ld hl,sp+r8
    case 0xE8: sm_kill_pair(out, DP_SM83R_SP); break;   // add sp,r8
    case 0xF9:   // ld sp,hl
        out->v[DP_SM83R_SP] = in->v[DP_SM83R_HL] & 0xffff; out->known[DP_SM83R_SP] = in->known[DP_SM83R_HL] & 3;
        break;
    case 0xF1: case 0xC1: case 0xD1: case 0xE1: {   // pop af/bc/de/hl
        unsigned s2 = (op >> 4) & 3;   // rp code 3 = AF (pop only)
        if (s2 == 3) s2 = DP_SM83R_AF;
        if ((out->known[DP_SM83R_SP] & 3) == 3) {   // the concrete stack proves the popped pair
            uint8_t lo, hi;
            if (rom->read(rom->ctx, out->v[DP_SM83R_SP] & 0xffff, &lo) && rom->read(rom->ctx, (out->v[DP_SM83R_SP] + 1) & 0xffff, &hi)) {
                out->v[s2] = (uint32_t)(lo | hi << 8); out->known[s2] |= 3;
            } else sm_kill_pair(out, s2);
        } else sm_kill_pair(out, s2);
        if (s2 == DP_SM83R_AF) sm_kill_byte(out, DP_SM83R_A, 0);   // the byte slots ride separately
        if ((out->known[DP_SM83R_SP] & 3) == 3)   // SP advances even when the read failed
            out->v[DP_SM83R_SP] = (uint32_t)((out->v[DP_SM83R_SP] + 2) & 0xffff);
        break; }
    case 0xC5: case 0xD5: case 0xE5: case 0xF5:   // push: nothing to know, SP steps down
        if ((out->known[DP_SM83R_SP] & 3) == 3) out->v[DP_SM83R_SP] = (uint32_t)((out->v[DP_SM83R_SP] - 2) & 0xffff);
        break;
    case 0x07: case 0x0F: case 0x17: case 0x1F:   // rotates through A
    case 0x27: case 0x2F:                         // daa, cpl
        sm_kill_byte(out, DP_SM83R_A, 0);
        break;
    case 0xCB:   // rot/shift/bit/res/set: BIT writes nothing, the rest kills their target byte
        if (b[1] < 0x40 || b[1] >= 0x80) {   // rot/shift (0x00-0x3F) and res/set (0x80-0xFF)
            if ((b[1] & 7) != 6) { sm_byte_slot(b[1] & 7, &by); sm_kill_byte(out, sm_byte_slot(b[1] & 7, &by), by); }
        }
        break;
    default:
        if (op >= 0x80 && op <= 0xB7) sm_kill_byte(out, DP_SM83R_A, 0);   // alu a,r/(hl) (CP 0xB8-0xBF keeps A)
        else if (op == 0xC6 || op == 0xD6 || op == 0xE6 || op == 0xF6 || op == 0xCE || op == 0xDE || op == 0xEE)
            sm_kill_byte(out, DP_SM83R_A, 0);   // alu a,n (cp n keeps A)
        break;
    }
}

static int isa_length(uint8_t op) { return sm83_len(op); }

static int isa_flow(const uint8_t *b, uint32_t pc, DpFlowInfo *o) {
    uint8_t op = b[0];
    int len = sm83_len(op);
    o->kind = DP_FLOW_SEQ; o->length = (uint8_t)len; o->fall = 1; o->call = 0;
    o->target = -1; o->cond = (int8_t)sm83_cond_of(op); o->rst = 0; o->halt = op == 0x76 || op == 0x10;
    o->pair = DP_REG_PAIR_NONE;
    if (is_illegal(op)) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return len; }
    switch (op) {
    case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: // JR [cc]
        o->target = (int32_t)((pc + 2 + (int8_t)b[1]) & 0xFFFF);
        o->kind = op == 0x18 ? DP_FLOW_JUMP : DP_FLOW_COND;
        o->fall = op != 0x18;
        break;
    case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA: // JP [cc]
        o->target = b[1] | (b[2] << 8);
        o->kind = op == 0xC3 ? DP_FLOW_JUMP : DP_FLOW_COND;
        o->fall = op != 0xC3;
        break;
    case 0xCD: case 0xC4: case 0xCC: case 0xD4: case 0xDC: // CALL [cc]
        o->target = b[1] | (b[2] << 8);
        o->kind = DP_FLOW_CALL; o->call = 1;
        o->fall = 1;
        break;
    case 0xC9: case 0xD9: // RET, RETI
        o->kind = DP_FLOW_RET; o->fall = 0;
        break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: // RET cc
        o->kind = DP_FLOW_COND;
        break;
    case 0xE9: // JP HL
        o->kind = DP_FLOW_INDIRECT; o->fall = 0;
        o->pair = DP_SM83R_HL;
        break;
    default:
        if ((op & 0xC7) == 0xC7) { // RST
            o->target = op & 0x38; o->kind = DP_FLOW_CALL; o->call = 1; o->rst = 1;
        }
        break;
    }
    return len;
}

// data operand of an SM83 instruction (spec §7.3 mem_operand)
static int isa_mem_operand(const uint8_t *b, DpMemOp *o) {
    uint8_t op = b[0];
    *o = (DpMemOp){ 0 };
    o->width = 1;
    #define PAIR(acc, pr, po) do { o->access = (acc); o->mode = DP_AM_PAIR; o->pair = (pr); o->post = (po); return 1; } while (0)
    switch (op) {
    case 0x02: PAIR(DP_MA_WRITE, 0, 0);
    case 0x12: PAIR(DP_MA_WRITE, 1, 0);
    case 0x0A: PAIR(DP_MA_READ, 0, 0);
    case 0x1A: PAIR(DP_MA_READ, 1, 0);
    case 0x22: PAIR(DP_MA_WRITE, 2, 1);
    case 0x32: PAIR(DP_MA_WRITE, 2, -1);
    case 0x2A: PAIR(DP_MA_READ, 2, 1);
    case 0x3A: PAIR(DP_MA_READ, 2, -1);
    case 0x34: case 0x35: PAIR(DP_MA_RW, 2, 0);
    case 0x36: PAIR(DP_MA_WRITE, 2, 0);
    case 0xEA: o->access = DP_MA_WRITE; o->mode = DP_AM_ABS; o->addr = (uint16_t)(b[1] | b[2] << 8); return 1;
    case 0xFA: o->access = DP_MA_READ; o->mode = DP_AM_ABS; o->addr = (uint16_t)(b[1] | b[2] << 8); return 1;
    case 0x08: o->access = DP_MA_WRITE; o->mode = DP_AM_ABS; o->width = 2; o->addr = (uint16_t)(b[1] | b[2] << 8); return 1;
    case 0xE0: o->access = DP_MA_WRITE; o->mode = DP_AM_HIGH_IMM; o->addr = (uint16_t)(0xFF00 + b[1]); return 1;
    case 0xF0: o->access = DP_MA_READ; o->mode = DP_AM_HIGH_IMM; o->addr = (uint16_t)(0xFF00 + b[1]); return 1;
    case 0xE2: o->access = DP_MA_WRITE; o->mode = DP_AM_HIGH_REG; return 1;
    case 0xF2: o->access = DP_MA_READ; o->mode = DP_AM_HIGH_REG; return 1;
    case 0xC5: case 0xD5: case 0xE5: case 0xF5:
    case 0xC4: case 0xCC: case 0xCD: case 0xD4: case 0xDC:
        o->access = DP_MA_WRITE; o->mode = DP_AM_STACK; o->pair = 3; o->width = 2; return 1;
    case 0xC1: case 0xD1: case 0xE1: case 0xF1:
    case 0xC9: case 0xD9: case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        o->access = DP_MA_READ; o->mode = DP_AM_STACK; o->pair = 3; o->width = 2; return 1;
    case 0xCB: // (HL) forms: BIT only reads, the rest read-modify-write
        if ((b[1] & 7) != 6) return 0;
        PAIR((b[1] >= 0x40 && b[1] < 0x80) ? DP_MA_READ : DP_MA_RW, 2, 0);
    }
    if (op >= 0xC7 && (op & 7) == 7) { // rst
        o->access = DP_MA_WRITE; o->mode = DP_AM_STACK; o->pair = 3; o->width = 2; return 1;
    }
    if (op >= 0x40 && op < 0xC0 && (op & 7) == 6 && op != 0x76) // ld r,(hl) / alu a,(hl)
        PAIR(DP_MA_READ, 2, 0);
    if (op >= 0x70 && op <= 0x77 && op != 0x76) PAIR(DP_MA_WRITE, 2, 0);
    #undef PAIR
    return 0;
}

static int isa_imm_ptr(const uint8_t *b, uint32_t *imm) {   // ld bc/de/hl,nn
    if (b[0] != 0x01 && b[0] != 0x11 && b[0] != 0x21) return 0;
    *imm = (uint32_t)b[1] | (uint32_t)b[2] << 8; return 1;
}
static int isa_acc_imm(const uint8_t *b, uint8_t *v) { if (b[0] != 0x3E) return 0; *v = b[1]; return 1; }   // ld a,n
static int isa_st_acc_abs(const uint8_t *b, uint32_t *a) { if (b[0] != 0xEA) return 0; *a = (uint32_t)b[1] | (uint32_t)b[2] << 8; return 1; }   // ld (nn),a
// "zero data marked code" point 3: tile/index dumps decode as valid
// instructions.  Two consecutive rst $38 (a run of $FF) or two consecutive
// ld (bc),a / ld (de),a (tile writes through a pointer that never moves) are
// never the head of real code.
static int isa_entry_data_sig(const uint8_t *w, unsigned n) {
    for (unsigned i = 1; i < n; i++)
        if (w[i] == w[i - 1] && (w[i] == 0xFF || w[i] == 0x02 || w[i] == 0x12)) return 1;
    return 0;
}
// M1: direct RAM loads into the accumulator, `ld a,(nn)` and the high-register `ldh a,(n)` (techniques 3, 4, 7)
static int isa_ram_load(const uint8_t *b, uint32_t *addr, int *idx_reg, int *width, int *dreg) {
    (void)idx_reg; (void)width; (void)dreg;
    if (b[0] == 0xFA) { *addr = b[1] | (b[2] << 8); return 3; }           // ld a,(nn)
    if (b[0] == 0xF0) { *addr = 0xFF00u + b[1]; return 2; }               // ldh a,(n)
    return 0;
}
// M1: proven length (in entries) of the word table whose base `ld hl,nn` follows the load;
// an entry is proven while its word designates the ROM at or above code_min (a legal code target)
static unsigned isa_table_proven(const DpIsa *isa, DpMachine *m, uint32_t pc, int len) {
    for (unsigned i = 0; i < 13; i++) {
        if (m->fetch(m, pc + len + i) != 0x21) continue;
        uint32_t base = m->fetch(m, pc + len + i + 1) | (uint32_t)m->fetch(m, pc + len + i + 2) << 8;
        if (m->rom_index(m, base) < 0) continue;
        unsigned n = 0;
        while (n < 128) {
            uint32_t t = m->fetch(m, base + 2 * n) | (uint32_t)m->fetch(m, base + 2 * n + 1) << 8;
            if (t < isa->code_min || m->rom_index(m, t) < 0) break;
            n++;
        }
        return n;
    }
    return 0;
}
// M1: a ROM table access indexed by the loaded value follows within 16 bytes:
// `ld hl,nn` from a ROM base, then an addition folding the loaded value in
static int isa_rom_table_follows(const DpIsa *isa, DpMachine *m, uint32_t pc, int len) {
    (void)isa;
    uint8_t win[16];
    for (unsigned i = 0; i < sizeof win; i++) win[i] = m->fetch(m, pc + len + i);
    int tab = 0;
    for (unsigned i = 0; i + 2 < sizeof win; i++) {
        if (win[i] == 0x21 && m->rom_index(m, win[i + 1] | (win[i + 2] << 8)) >= 0) tab = 1;
        if (tab && (win[i] == 0x19 || win[i] == 0x09 || win[i] == 0x85 || win[i] == 0x83)) return 1;
    }
    return 0;
}
static int isa_table_dispatch(const uint8_t *b, uint32_t *p) { if (b[0] != 0xe9) return 0; *p = 0; return 1; }
static const DpIsa isa = { "sm83", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag, isa_reg_effect, isa_illegal, sm83_disasm, isa_mem_operand, 0x150u, 1, 0, isa_imm_ptr, NULL, isa_acc_imm, isa_st_acc_abs,
                           .sp_slot = DP_SM83R_SP, .sp_width = 2, .sp_addr_mask = 0xFFFFu, .sp_base = 0, .entry_data_sig = isa_entry_data_sig,
                           .ram_load = isa_ram_load, .cmp_imm = 0xFE, .table_proven = isa_table_proven, .rom_table_follows = isa_rom_table_follows, .table_dispatch = isa_table_dispatch };
const DpIsa *dp_isa_sm83(void) { return &isa; }
