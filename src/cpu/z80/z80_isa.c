// z80_isa.c: the Z80 as a DpIsa (spec §7.3).  Wraps the existing decoder
// (z80.c, the m0boy runtime's own); the classification mirrors what the sega
// static closure used to open-code over DpZ80Instruction.  The condition
// encoding is the z80 native one (dp_z80_condition: 0..7 flags, 8 = DJNZ).
#include "../../core/dpmachine.h"
#include "z80.h"
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

static int isa_flow(const uint8_t *bytes, uint16_t pc, DpFlowInfo *o) {
    DpZ80Instruction in;
    o->kind = DP_FLOW_SEQ; o->fall = 1; o->call = 0; o->rst = 0; o->halt = 0;
    o->target = -1; o->cond = -1; o->length = 1;
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

// conservative kill tables over the primary map: anything not proven kept is
// lost, never guessed (the sm83 wrapper's model; DD/FD kill via their ops on HL)
static int hl_writes(uint8_t op) {
    if (op == 0xCB) return 1;                                  // CB: untracked
    switch (op) {
    case 0x21: case 0x22: case 0x2A:                           // ld hl,nn; ldi/ldd (hl)
    case 0x09: case 0x19: case 0x29: case 0x39:                // add hl,rr
    case 0x23: case 0x2B:                                      // inc/dec hl
    case 0xE1: case 0xE5: case 0xE3:                           // pop hl; push hl(=no); ex (sp),hl
    case 0xEB: case 0x08:                                      // ex de,hl; ex af,af'(=no)
    case 0xDD: case 0xFD:                                      // DD/FD prefix: their ops target IX/IY (HL untouched) — kill conservatively anyway
    case 0xED:                                                 // ED block ops touch HL
        return 1;
    default:
        return 0;
    }
}

static int a_writes(uint8_t op) {
    if (op == 0xCB) return 1;
    if (op >= 0x80 && op <= 0xBF) return 1;                    // alu
    if (((op & 0xC7) == 0x04) || ((op & 0xC7) == 0x0C)) return 1;  // inc/dec r (a included)
    switch (op) {
    case 0x07: case 0x0F: case 0x17: case 0x1F:                // rotates through a
    case 0x27: case 0x2F:                                      // daa, cpl
    case 0x37: case 0x3F:                                      // scf, ccf (a kept! cf. sm83: kill conservatively)
    case 0x3A:                                                 // ld a,(nn) — proven only via rom->read
        return 1;
    default:
        return 0;
    }
}

static void load_a(DpProvRegs *r, const DpProvRom *rom, uint16_t addr) {
    uint8_t v;
    if (rom->read(rom->ctx, addr, &v)) { r->a = v; r->aknown = 1; } else r->aknown = 0;
}

static void isa_effect(const uint8_t *b, DpProvRegs *r, const DpProvRom *rom) {
    uint8_t op = b[0];
    if (hl_writes(op)) {
        if (op == 0x21) { r->p = (uint16_t)(b[1] | (b[2] << 8)); r->pmask = 3; }   // ld hl,nn
        else if (op == 0x2A && rom->read(rom->ctx, (uint16_t)(b[1] | (b[2] << 8)), (uint8_t *)&r->p)) {
            uint8_t hi;
            r->pmask = rom->read(rom->ctx, (uint16_t)((b[1] | (b[2] << 8)) + 1), &hi) ? 3 : 1;
            r->p |= (uint16_t)(hi << 8);
        } else r->pmask = 0;
    }
    if (op == 0x3E) { r->a = b[1]; r->aknown = 1; }                                // ld a,n
    else if (op == 0x3A) load_a(r, rom, (uint16_t)(b[1] | (b[2] << 8)));           // ld a,(nn)
    else if (op == 0x7E) { r->aknown = 0; (void)rom; }                             // ld a,(hl): untracked here
    else if (a_writes(op)) r->aknown = 0;
}

// disasm through the shared formatter
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

static const DpIsa isa = { "z80", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_effect, isa_illegal, isa_disasm, isa_mem_operand };
const DpIsa *dp_isa_z80(void) { return &isa; }
