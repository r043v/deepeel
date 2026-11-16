// m6502_isa.c: the 6502 as a DpIsa (spec §7.3).  Wraps the existing decoder
// (m6502.c); the classification mirrors what the NES explorer's static layer
// used to reason about.  Conditions are the m6502 compact encoding
// (0..3 N V C Z clear, 4..7 set, m6502.h).
#include "../../core/dpmachine.h"
#include "m6502.h"
#include <string.h>

static int isa_length(uint8_t op) {
    DpM6502Instruction in;
    uint8_t bytes[3] = { op, 0, 0 };
    if (!dp_m6502_decode(bytes, sizeof bytes, 0, &in)) return 1;
    return in.length;
}

static int isa_flow(const uint8_t *bytes, uint32_t pc, DpFlowInfo *o) {
    DpM6502Instruction in;
    o->kind = DP_FLOW_SEQ; o->fall = 1; o->call = 0; o->rst = 0; o->halt = 0;
    o->target = -1; o->cond = -1; o->length = 1; o->pair = DP_REG_PAIR_NONE;
    if (!dp_m6502_decode(bytes, 3, pc, &in)) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return 1; }
    o->length = in.length;
    o->cond = in.condition;
    switch (in.flow) {
    case DP_M6502_NEXT:
        break;
    case DP_M6502_JUMP:
        o->kind = DP_FLOW_JUMP; o->fall = 0; o->target = in.target;
        break;
    case DP_M6502_INDIRECT:
        o->kind = DP_FLOW_INDIRECT; o->fall = 0;
        break;
    case DP_M6502_CALL:
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->target = in.target;
        break;
    case DP_M6502_RETURN:
        o->kind = DP_FLOW_RET; o->fall = 0;
        break;
    case DP_M6502_BRANCH:
        o->kind = DP_FLOW_COND; o->fall = 1; o->target = in.target;
        break;
    case DP_M6502_JAM:
        o->kind = DP_FLOW_ILLEGAL; o->fall = 0;
        break;
    }
    return o->length;
}

static int isa_cond_of(uint8_t op) {
    DpM6502Instruction in;
    uint8_t bytes[3] = { op, 0, 0 };
    if (!dp_m6502_decode(bytes, sizeof bytes, 0, &in)) return -1;
    return in.flow == DP_M6502_BRANCH ? in.condition : -1;
}

// the tested flag (m6502.h: 0..3 N V C Z clear, 4..7 set); polarity is in
// bit 2 like the encoding itself, the low bit selects the flag
static uint8_t isa_cond_flag(int cond) {
    static const uint8_t flag[4] = { 0x80, 0x40, 0x01, 0x02 };  // N, V, C, Z
    return flag[cond & 3];
}

static int isa_illegal(uint8_t op) {
    DpM6502Instruction in;
    uint8_t bytes[3] = { op, 0, 0 };
    if (!dp_m6502_decode(bytes, sizeof bytes, 0, &in)) return 1;
    return in.flow == DP_M6502_JAM;
}

// --- C3: constant transfer over the DpRegState lattice ----------------------
// Sound subset: immediates, transfers, deterministic inc/dec, and the listed
// no-write space (branches, flags, stores, compares) keep their registers;
// anything written from memory, arithmetic, or not modelled loses them, never
// guessed.  Documented opcodes only: the undocumented space is killed.
static void r_set(DpRegState *s, unsigned slot, uint8_t v) { s->v[slot] = v; s->known[slot] |= 1; }
static void r_kill(DpRegState *s, unsigned slot) { s->known[slot] = 0; s->v[slot] = 0; }
static void r_step(DpRegState *s, unsigned slot, int delta) {
    if (s->known[slot] & 1) r_set(s, slot, (uint8_t)(s->v[slot] + delta));
    else r_kill(s, slot);
}
static void isa_reg_effect(const uint8_t *b, uint32_t pc, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    (void)pc; (void)rom;
    *out = *in;
    uint8_t op = b[0];
    switch (op) {
    case 0xA9: r_set(out, DP_M6502R_A, b[1]); return;   // lda #
    case 0xA2: r_set(out, DP_M6502R_X, b[1]); return;   // ldx #
    case 0xA0: r_set(out, DP_M6502R_Y, b[1]); return;   // ldy #
    case 0xAA: if (out->known[DP_M6502R_A] & 1) r_set(out, DP_M6502R_X, (uint8_t)out->v[DP_M6502R_A]); else r_kill(out, DP_M6502R_X); return;   // tax
    case 0xA8: if (out->known[DP_M6502R_A] & 1) r_set(out, DP_M6502R_Y, (uint8_t)out->v[DP_M6502R_A]); else r_kill(out, DP_M6502R_Y); return;   // tay
    case 0x8A: if (out->known[DP_M6502R_X] & 1) r_set(out, DP_M6502R_A, (uint8_t)out->v[DP_M6502R_X]); else r_kill(out, DP_M6502R_A); return;   // txa
    case 0x98: if (out->known[DP_M6502R_Y] & 1) r_set(out, DP_M6502R_A, (uint8_t)out->v[DP_M6502R_Y]); else r_kill(out, DP_M6502R_A); return;   // tya
    case 0xBA: if (out->known[DP_M6502R_S] & 1) r_set(out, DP_M6502R_X, (uint8_t)out->v[DP_M6502R_S]); else r_kill(out, DP_M6502R_X); return;   // tsx
    case 0x9A: if (out->known[DP_M6502R_X] & 1) r_set(out, DP_M6502R_S, (uint8_t)out->v[DP_M6502R_X]); else r_kill(out, DP_M6502R_S); return;   // txs
    case 0xE8: r_step(out, DP_M6502R_X, 1); return;   // inx
    case 0xCA: r_step(out, DP_M6502R_X, -1); return;  // dex
    case 0xC8: r_step(out, DP_M6502R_Y, 1); return;   // iny
    case 0x88: r_step(out, DP_M6502R_Y, -1); return;  // dey
    }
    unsigned write = 0;
    if (op == 0x68) write |= 1u << DP_M6502R_A;                       // pla
    else if ((op & 0x1F) == 0x01) {                                  // the ORA/AND/EOR/ADC/STA/LDA/CMP/SBC group
        unsigned grp = (op >> 5) & 7;
        if (grp == 5) write |= 1u << DP_M6502R_A;                     // lda from memory: killed
        else if (grp < 4 || grp == 7) write |= 1u << DP_M6502R_A;     // arithmetic: A killed (cmp keeps it)
        // sta (4) and cmp (6) write nothing
    }
    if ((op & 0x9F) == 0x0A) write |= 1u << DP_M6502R_A;              // asl/lsr/rol/ror A
    if (op == 0xAE || op == 0xA6 || op == 0xBE || op == 0xB6) write |= 1u << DP_M6502R_X;   // ldx from memory
    if (op == 0xAC || op == 0xA4 || op == 0xBC || op == 0xB4) write |= 1u << DP_M6502R_Y;   // ldy from memory
    if (write & (1u << DP_M6502R_A)) r_kill(out, DP_M6502R_A);
    if (write & (1u << DP_M6502R_X)) r_kill(out, DP_M6502R_X);
    if (write & (1u << DP_M6502R_Y)) r_kill(out, DP_M6502R_Y);
    else {
        static const uint16_t keeps[] = {   // the documented no-write space
            0xEA, 0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA,   // NOP family
            0x18, 0x38, 0x58, 0x78, 0xB8, 0xD8, 0xF8,   // CLC SEC CLI SEI CLV CLD SED
            0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0,   // branches
            0x4C, 0x6C, 0x20, 0x60, 0x40, 0x00,         // jmp (ind) jsr rts rti brk
            0x24, 0x2C, 0xC0, 0xE0, 0xC4, 0xE4,         // bit cpx cpy
            0x81, 0x91, 0x85, 0x86, 0x84, 0x8C, 0x8E, 0x8D,   // stores
            0xC1, 0xC5, 0xCD, 0xD1, 0xD5, 0xD9, 0xDD,   // cmp
        };
        unsigned keep = 0;
        for (size_t i = 0; i < sizeof keeps / sizeof *keeps; i++) if (keeps[i] == op) keep = 1;
        if (!keep) { r_kill(out, DP_M6502R_A); r_kill(out, DP_M6502R_X); r_kill(out, DP_M6502R_Y); r_kill(out, DP_M6502R_S); }   // unmodelled: never guessed
    }
}

static int isa_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    DpM6502Instruction in;
    uint8_t bytes[3] = { 0 };
    unsigned avail = rom_size > addr ? (unsigned)(rom_size - addr) : 0;
    if (!avail) return 0;
    if (avail > 3) avail = 3;
    memcpy(bytes, rom + addr, avail);
    if (!dp_m6502_decode(bytes, sizeof bytes, addr, &in)) return 0;
    if ((unsigned)in.length > avail) return 0;
    dp_m6502_format(&in, bytes[1], bytes[2], out, outsz);
    return in.length;
}

static int isa_mem_operand(const uint8_t *b, DpMemOp *o) {
    o->access = DP_MA_NONE; o->mode = DP_AM_NONE; o->pair = 0; o->post = 0; o->width = 0; o->addr = 0;
    DpM6502Instruction in;
    if (!dp_m6502_decode(b, 3, 0, &in)) return 0;
    if (in.access == 3 || in.access == 4) return 0;   // no access, jump/call/return
    switch (in.addr_mode) {
    case DP_M6502_ZP: case DP_M6502_ZPX: case DP_M6502_ZPY:
        o->mode = DP_AM_ABS; o->addr = b[1]; o->width = 1; break;
    case DP_M6502_ABS: case DP_M6502_ABSX: case DP_M6502_ABSY:
        o->mode = DP_AM_ABS; o->addr = (uint16_t)(b[1] | (b[2] << 8)); o->width = 1; break;
    case DP_M6502_INDX: case DP_M6502_INDY: case DP_M6502_IND:
        o->mode = DP_AM_PAIR; o->pair = 2; o->width = 1; break;   // zp pointer: no static address
    case DP_M6502_IMM: case DP_M6502_IMP: default:
        return 0;
    }
    o->access = (uint8_t)(in.access == 1 ? DP_MA_WRITE : in.access == 2 ? DP_MA_RW : DP_MA_READ);
    return 1;
}

static int isa_imm_byte(const uint8_t *b, uint8_t *v) {   // B9: lda/ldx/ldy #imm (a split pointer is loaded in two halves)
    if (b[0] != 0xA9 && b[0] != 0xA2 && b[0] != 0xA0) return 0;
    *v = b[1]; return 1;
}

static int isa_acc_imm(const uint8_t *b, uint8_t *v) { if (b[0] != 0xA9) return 0; *v = b[1]; return 1; }   // lda #n
static int isa_st_acc_abs(const uint8_t *b, uint32_t *a) { if (b[0] != 0x8D) return 0; *a = (uint32_t)b[1] | (uint32_t)b[2] << 8; return 1; }   // sta abs
// C3: `rts` pulls the 16-bit return address from S+1..S+2 (pushes pre-decrement)
// and adds one on the fly; `rti` also pulls the status byte — not modeled.
static int isa_sp_ret(const uint8_t *b, unsigned *w, unsigned *bias, unsigned *off) {
    if (b[0] != 0x60) return 0;
    *w = 2; *bias = 1; *off = 1; return 1;
}
static const DpIsa isa = { "m6502", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_reg_effect, isa_illegal, isa_disasm, isa_mem_operand, 0, 0, 1, NULL, isa_imm_byte, isa_acc_imm, isa_st_acc_abs,
                           .sp_slot = DP_M6502R_S, .sp_width = 2, .sp_addr_mask = 0xFFu, .sp_base = 0x0100u, .entry_data_sig = NULL, .sp_ret = isa_sp_ret };
const DpIsa *dp_isa_m6502(void) { return &isa; }
