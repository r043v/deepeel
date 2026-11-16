// z80_isa.c: the Z80 as a DpIsa (spec §7.3).  Wraps the existing decoder
// (z80.c, the m0boy runtime's own); the classification mirrors what the sega
// static closure used to open-code over DpZ80Instruction.  The condition
// encoding is the z80 native one (dp_z80_condition: 0..7 flags, 8 = DJNZ).
#include "../../core/dpmachine.h"
#include "z80.h"
#include "provenance.h"
#include <string.h>

// instruction length from the first byte: a bound only (prefix chains and
// DD/FD+CB need the bytes; flow()'s decode is the exact source)
static int isa_length(uint8_t op) {
    switch (op) {
    case 0x01: case 0x11: case 0x21: case 0x31:   // ld rr,nn
        return 3;
    case 0xCB: case 0xED:                          // CB, ED + one more byte (chains: flow() knows)
        return 2;
    case 0xDD: case 0xFD:                          // DD/FD + d + op
        return 3;
    default:
        return (op & 7) == 6 ? 2 : 1;              // (r),n and (HL) byte ops
    }
}

// conditional opcodes, z80-native condition index (dp_z80_condition encoding):
// 0 NZ, 1 Z, 2 NC, 3 C, 4 PO, 5 PE, 6 P, 7 M, 8 DJNZ
static int isa_cond_of(uint8_t op) {
    switch (op) {
    case 0x20: case 0x28: case 0x30: case 0x38:                       // JR cc
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:                       // RET cc
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:                       // JP cc
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:                       // CALL cc
        return (op >> 3) & 3;
    case 0x10: return 8;                                              // DJNZ B
    default:
        return -1;   // E0/E8/F0/F8 (RET PO/PE/P/M) and E2..FC: ED-prefixed or same low bits
    }
}

// state mask of the tested flag; polarity in the low bit (0 = negated, like
// sm83): even index NZ/NC/PO/P, odd index Z/C/PE/M.  DJNZ tests B: no flag.
static uint8_t isa_cond_flag(int cond) {
    if (cond == 8) return 0;
    static const uint8_t flag[4] = { 0x40, 0x01, 0x04, 0x80 };  // Z, C, P/V, S
    return flag[(cond >> 1) & 3];
}

// the decoder is the undefined-opcode authority (prefix chains included)
static int isa_illegal(uint8_t op) { (void)op; return 0; }

static int isa_flow(const uint8_t *bytes, uint32_t pc, DpFlowInfo *o) {
    DpZ80Instruction in;
    o->kind = DP_FLOW_SEQ; o->fall = 1; o->call = 0; o->rst = 0; o->halt = 0;
    o->target = -1; o->cond = -1; o->length = 1; o->pair = DP_REG_PAIR_NONE;
    if (!dp_z80_decode(bytes, 6, pc, &in)) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return in.raw.length ? (int)in.raw.length : 1; }
    o->length = in.raw.length;
    o->cond = in.condition;
    switch (in.flow) {
    case DP_Z80_NEXT:
        break;
    case DP_Z80_JUMP:
        o->kind = DP_FLOW_JUMP; o->fall = 0; o->target = in.target;
        break;
    case DP_Z80_CALL:
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->target = in.target;
        break;
    case DP_Z80_RETURN:
        o->kind = in.condition >= 0 ? DP_FLOW_COND : DP_FLOW_RET;
        o->fall = in.condition >= 0;
        break;
    case DP_Z80_INDIRECT:
        o->kind = DP_FLOW_INDIRECT; o->fall = 0;
        o->pair = in.raw.index == 1 ? DP_Z80R_IX : in.raw.index == 2 ? DP_Z80R_IY : DP_Z80R_HL;   // jp (hl)/(ix)/(iy)
        break;
    case DP_Z80_HALT:
        o->halt = 1;   // HALT/STOP: waits for an interrupt, then continues below
        break;
    case DP_Z80_REPEAT:
        // ED block repetition (LDIR...): loops on itself until the counter ends
        o->kind = DP_FLOW_COND; o->fall = 1; o->target = pc;
        break;
    }
    // JR cc/DJNZ targets come from the decoder; JR is a JUMP with a condition
    if (in.flow == DP_Z80_JUMP && in.condition >= 0) { o->kind = DP_FLOW_COND; o->fall = 1; }
    return o->length;
}

// --- C3: constant transfer over the DpRegState lattice ----------------------
// The transfer table is the shared provenance engine (provenance.c, the same
// one the Sega closure's target_constants runs on): input constants ride as
// origin = value + 1, the engine's pure rules keep or kill them per
// instruction, and only the instruction's *explicit* constant operand is
// provable (the planned read below, exactly the Sega closure's read plan).
// On top of the engine the port re-derives what the engine kills but the
// Sega closure proves itself (inc/dec of pairs and bytes, DJNZ, the ED block
// steps): same rules, value arithmetic instead of origin arithmetic.
typedef struct { const DpProvRom *rom; uint16_t addr; unsigned bytes; uint16_t saddr; unsigned sbytes; } ZConstRead;
static uint32_t zconst_read(void *u, uint16_t a) {
    ZConstRead *r = u;
    int in_main = (uint16_t)(a - r->addr) < r->bytes;            // the instruction's explicit operand
    int in_stack = (uint16_t)(a - r->saddr) < r->sbytes;         // the stack words a pop / ex (sp) touches
    if (!in_main && !in_stack) return 0;                          // anything else: never proven
    uint8_t v;
    if (!r->rom->read(r->rom->ctx, a, &v)) return 0;
    return (uint32_t)v + 1;
}
static unsigned zpair_slot(unsigned p, unsigned index) {   // the rp/p slot of pair code p (IX/IY replace HL)
    if (p < 2) return p;                                   // BC, DE
    if (p == 2) return index == 1 ? DP_Z80R_IX : index == 2 ? DP_Z80R_IY : DP_Z80R_HL;
    return DP_Z80R_SP;
}
// byte-register slot of register code y (DD/FD: H/L become IX/IY)
// provenance tag of (slot, byte); byte-only slots read their byte 0 twice
static unsigned ztag_of(unsigned slot, unsigned byte) {
    static const unsigned t[DP_Z80R_NREGS][2] = {
        [DP_Z80R_BC] = { DP_Z80_TAG_C, DP_Z80_TAG_B },
        [DP_Z80R_DE] = { DP_Z80_TAG_E, DP_Z80_TAG_D },
        [DP_Z80R_HL] = { DP_Z80_TAG_L, DP_Z80_TAG_H },
        [DP_Z80R_IX] = { DP_Z80_TAG_IXL, DP_Z80_TAG_IXH },
        [DP_Z80R_IY] = { DP_Z80_TAG_IYL, DP_Z80_TAG_IYH },
        [DP_Z80R_SP] = { DP_Z80_TAG_SPL, DP_Z80_TAG_SPH },
        [DP_Z80R_A] = { DP_Z80_TAG_A, DP_Z80_TAG_A },
        [DP_Z80R_F] = { DP_Z80_TAG_F, DP_Z80_TAG_F },
        [DP_Z80R_B] = { DP_Z80_TAG_B, DP_Z80_TAG_B },
        [DP_Z80R_C] = { DP_Z80_TAG_C, DP_Z80_TAG_C },
        [DP_Z80R_D] = { DP_Z80_TAG_D, DP_Z80_TAG_D },
        [DP_Z80R_E] = { DP_Z80_TAG_E, DP_Z80_TAG_E },
        [DP_Z80R_H] = { DP_Z80_TAG_H, DP_Z80_TAG_H },
        [DP_Z80R_L] = { DP_Z80_TAG_L, DP_Z80_TAG_L },
        [DP_Z80R_BC2] = { DP_Z80_TAG_C2, DP_Z80_TAG_B2 },
        [DP_Z80R_DE2] = { DP_Z80_TAG_E2, DP_Z80_TAG_D2 },
        [DP_Z80R_HL2] = { DP_Z80_TAG_L2, DP_Z80_TAG_H2 },
        [DP_Z80R_AF2] = { DP_Z80_TAG_F2, DP_Z80_TAG_A2 },
    };
    return t[slot][byte];
}
// derive one byte +/- delta from the input state (the engine's verdict already
// killed the written bytes: the Sega closure derives from `before`, so the
// exact arithmetic must too — an unknown input byte stays unknown)
static void zstep_byte(const DpRegState *in, DpRegState *out, unsigned slot, unsigned byte, int delta) {
    unsigned bit = 1u << byte;
    if (in->known[slot] & bit) {
        out->known[slot] |= (uint8_t)bit;
        out->v[slot] = (out->v[slot] & ~(0xffu << (8 * byte))) | (uint32_t)(((in->v[slot] >> (8 * byte)) + delta) & 0xff) << (8 * byte);
    } else { out->known[slot] &= (uint8_t)~bit; out->v[slot] &= ~(0xffu << (8 * byte)); }
}
static void zstep_pair(const DpRegState *in, DpRegState *out, unsigned slot, int delta) {
    if ((in->known[slot] & 3) == 3) {
        out->known[slot] |= 3;
        out->v[slot] = (out->v[slot] & 0xffff0000u) | (uint32_t)((uint16_t)((in->v[slot] & 0xffffu) + delta));
    } else { zstep_byte(in, out, slot, 0, delta); out->known[slot] &= (uint8_t)~2; out->v[slot] &= 0xffff00ffu; }
}
static void isa_reg_effect(const uint8_t *b, uint32_t pc, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    DpZ80Instruction ins;
    if (!dp_z80_decode(b, 8, (uint16_t)pc, &ins)) { *out = *in; return; }   // undecodable: flow already rejected, keep the state
    const z80_decoded_t *d = &ins.raw;
    unsigned op = d->opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    // seed the engine: every known input byte rides as origin = value + 1
    DpZ80Provenance pv; memset(&pv, 0, sizeof pv);
    z80_t cpu; memset(&cpu, 0, sizeof cpu);
    cpu.pc = (uint16_t)pc;
    for (unsigned s = 0; s < DP_Z80R_NREGS; s++)
        for (unsigned by = 0; by < 2; by++)
            if (in->known[s] & (1u << by)) pv.reg[ztag_of(s, by)] = ((in->v[s] >> (8 * by)) & 0xff) + 1;
    cpu.bc = (uint16_t)in->v[DP_Z80R_BC]; cpu.de = (uint16_t)in->v[DP_Z80R_DE]; cpu.hl = (uint16_t)in->v[DP_Z80R_HL];
    cpu.ix = (uint16_t)in->v[DP_Z80R_IX]; cpu.iy = (uint16_t)in->v[DP_Z80R_IY]; cpu.sp = (uint16_t)in->v[DP_Z80R_SP];
    cpu.f = (uint8_t)((in->known[DP_Z80R_F] & 1) ? in->v[DP_Z80R_F] : 0);
    // the planned read: only the instruction's explicit constant operand, plus
    // the stack words a pop / ex (sp) touches when the concrete SP is known
    // (the Sega closure's peek — the machine's stack proves the popped value)
    ZConstRead rd = { rom, 0, 0, 0, 0 };
    if ((in->known[DP_Z80R_SP] & 3) == 3 && !d->group && x == 3 && (z == 1 ? !q : (z == 3 && y == 4))) {
        rd.saddr = (uint16_t)in->v[DP_Z80R_SP]; rd.sbytes = 2;
    }
    unsigned pair_known[3] = { (in->known[DP_Z80R_HL] & 3) == 3, (in->known[DP_Z80R_IX] & 3) == 3, (in->known[DP_Z80R_IY] & 3) == 3 };
    unsigned ipair = d->index ? d->index : 0;   // 0 HL, 1 IX, 2 IY
    uint16_t hla = (uint16_t)(d->index == 1 ? cpu.ix : d->index == 2 ? cpu.iy : cpu.hl);
    uint16_t address = (uint16_t)(hla + (d->index ? (int8_t)d->displacement : 0));
    uint16_t immediate = (uint16_t)(pc + d->length - 1);
    if (!d->group && x == 1 && z == 6 && op != 0x76 && pair_known[ipair]) { rd.addr = address; rd.bytes = 1; }
    else if (!d->group && op == 0x3a) { rd.addr = d->immediate; rd.bytes = 1; }
    else if (!d->group && (op == 0x0a || op == 0x1a) && ((in->known[op == 0x0a ? DP_Z80R_BC : DP_Z80R_DE] & 3) == 3)) { rd.addr = (uint16_t)(op == 0x0a ? cpu.bc : cpu.de); rd.bytes = 1; }
    else if ((!d->group && op == 0x2a) || (d->group == 2 && x == 1 && z == 3 && q && p < 3)) { rd.addr = d->immediate; rd.bytes = 2; }
    else if (!d->group && x == 0 && z == 1 && !q && p <= 3) { rd.addr = (uint16_t)(immediate - 1); rd.bytes = 2; }   // ld rr,nn / ld sp,nn: the engine loads the pair from pc+1..pc+2 (DD/FD included)
    else if (!d->group && x == 0 && z == 6 && y != 6) { rd.addr = immediate; rd.bytes = 1; }        // ld r,n (DD/FD included)
    dp_z80_provenance_begin(&pv, &cpu, d, zconst_read, &rd);
    // read the engine's verdict back into the slot lattice
    for (unsigned s = 0; s < DP_Z80R_NREGS; s++) {
        out->v[s] = 0; out->known[s] = 0;
        unsigned bytes = (s == DP_Z80R_A || s == DP_Z80R_F || (s >= DP_Z80R_B && s <= DP_Z80R_L)) ? 1 : 2;
        for (unsigned by = 0; by < bytes; by++) {
            DpZ80Origin o = pv.reg[ztag_of(s, by)];
            if (o) { out->v[s] |= (uint32_t)((o - 1) & 0xff) << (8 * by); out->known[s] |= (uint8_t)(1u << by); }
        }
    }
    // what the engine kills but an exact derivation proves (the Sega closure's
    // own rules): inc/dec of pairs and bytes (SP included), DJNZ, the ED block
    // steps — the values come from the input state, the engine's verdict has
    // already killed the written bytes
    if (!d->group && x == 0) {
        if (z == 3 && p <= 3) zstep_pair(in, out, zpair_slot(p, d->index), q ? -1 : 1);
        else if ((z == 4 || z == 5) && y != 6) {
            if (y == 7) zstep_byte(in, out, DP_Z80R_A, 0, z == 5 ? -1 : 1);
            else zstep_byte(in, out, zpair_slot(y >> 1, d->index), (y & 1) ^ 1, z == 5 ? -1 : 1);   // B,D,H high; C,E,L low
        } else if (op == 0x10) zstep_byte(in, out, DP_Z80R_B, 0, -1);   // DJNZ
    } else if (d->group == 2 && x == 2 && y >= 4 && z <= 3) {
        int dir = (y & 1) ? -1 : 1;
        zstep_pair(in, out, DP_Z80R_HL, dir);
        if (z == 0) zstep_pair(in, out, DP_Z80R_DE, dir);
        if (z <= 1) zstep_pair(in, out, DP_Z80R_BC, -1);
    }
    // the stack pointer of the unconditional stack words (a call or a ret moves
    // SP differently on each of its two sides: the walk applies those)
    if ((in->known[DP_Z80R_SP] & 3) == 3 && !d->group && x == 3 && ((z == 1 && !q) || (z == 5 && !q))) {
        out->known[DP_Z80R_SP] |= 3;
        out->v[DP_Z80R_SP] = (out->v[DP_Z80R_SP] & 0xffff0000u) | (uint32_t)((uint16_t)(in->v[DP_Z80R_SP] + (z == 1 ? 2 : -2)));
    }
}
static int isa_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    DpZ80Instruction in;
    uint8_t bytes[6] = { 0 };
    unsigned avail = rom_size > addr ? (unsigned)(rom_size - addr) : 0;
    if (avail < 1 || avail > sizeof bytes) avail = avail ? sizeof bytes : 0;
    if (!avail) return 0;
    memcpy(bytes, rom + addr, avail < sizeof bytes ? avail : sizeof bytes);
    if (!dp_z80_decode(bytes, sizeof bytes, addr, &in)) return 0;
    if (in.raw.length > avail) return 0;   // the instruction would read past the ROM
    dp_z80_format(&in, out, outsz);
    return in.raw.length;
}

static int isa_mem_operand(const uint8_t *b, DpMemOp *o) {
    o->access = DP_MA_NONE; o->mode = DP_AM_NONE; o->pair = 2; o->post = 0; o->width = 0; o->addr = 0;
    uint8_t op = b[0];
    switch (op) {
    case 0x22: o->access = DP_MA_WRITE; o->mode = DP_AM_PAIR; o->pair = 2; o->post = 1; o->width = 2; return 1;   // ldi (hl),a
    case 0x2A: o->access = DP_MA_READ; o->mode = DP_AM_PAIR; o->pair = 2; o->post = 1; o->width = 2; return 1;    // ldi a,(hl)
    case 0x02: case 0x12: o->access = DP_MA_WRITE; o->mode = DP_AM_PAIR; o->pair = op == 0x02 ? 0 : 1; o->width = 1; return 1;
    case 0x0A: case 0x1A: o->access = DP_MA_READ; o->mode = DP_AM_PAIR; o->pair = op == 0x0A ? 0 : 1; o->width = 1; return 1;
    case 0x77: o->access = DP_MA_WRITE; o->mode = DP_AM_PAIR; o->pair = 2; o->width = 1; return 1;                // ld (hl),a
    case 0x7E: o->access = DP_MA_READ; o->mode = DP_AM_PAIR; o->pair = 2; o->width = 1; return 1;                 // ld a,(hl)
    case 0xE0: case 0xF0: o->access = op == 0xE0 ? DP_MA_WRITE : DP_MA_READ; o->mode = DP_AM_HIGH_IMM; o->addr = (uint16_t)(0xFF00 + b[1]); o->width = 1; return 1;
    case 0x32: case 0x3A: o->access = op == 0x32 ? DP_MA_WRITE : DP_MA_READ; o->mode = DP_AM_ABS; o->addr = (uint16_t)(b[1] | (b[2] << 8)); o->width = 1; return 1;
    default: return 0;
    }
}

static int isa_imm_ptr(const uint8_t *b, uint32_t *imm) {   // ld bc/de/hl,nn (unprefixed)
    if (b[0] != 0x01 && b[0] != 0x11 && b[0] != 0x21) return 0;
    *imm = (uint32_t)b[1] | (uint32_t)b[2] << 8; return 1;
}
static int isa_acc_imm(const uint8_t *b, uint8_t *v) { if (b[0] != 0x3E) return 0; *v = b[1]; return 1; }   // ld a,n
static int isa_st_acc_abs(const uint8_t *b, uint32_t *a) { if (b[0] != 0x32) return 0; *a = (uint32_t)b[1] | (uint32_t)b[2] << 8; return 1; }   // ld (nn),a
// "zero data marked code" point 3: the classic tile/index dumps decode as valid
// instructions, so the entry check needs their signatures.  Two consecutive
// rst $38 (data of $FF), two consecutive ld (bc),a / ld (de),a (tile writes to
// a pointer that never moves), and a DD/FD prefix whose opcode does not touch
// IX/IY (an undocumented no-op: real code never issues it).
static int isa_entry_data_sig(const uint8_t *w, unsigned n) {
    for (unsigned i = 1; i < n; i++)
        if (w[i] == w[i - 1] && (w[i] == 0xFF || w[i] == 0x02 || w[i] == 0x12)) return 1;
    if ((w[0] == 0xDD || w[0] == 0xFD) && n >= 2 && w[1] != 0xCB && w[1] != 0xDD && w[1] != 0xFD) {
        DpZ80Instruction in;
        if (dp_z80_decode(w, 6, 0, &in) && in.raw.index == 0) return 1;   // the prefix is ignored by the decode: not an IX/IY instruction
    }
    return 0;
}
static const DpIsa isa = { "z80", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_reg_effect, isa_illegal, isa_disasm, isa_mem_operand, 0, 1, 0, isa_imm_ptr, NULL, isa_acc_imm, isa_st_acc_abs,
                           .sp_slot = DP_Z80R_SP, .sp_width = 2, .sp_addr_mask = 0xFFFFu, .sp_base = 0, .entry_data_sig = isa_entry_data_sig };
const DpIsa *dp_isa_z80(void) { return &isa; }
