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
    o->target = -1; o->cond = -1; o->length = 1;
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

// the provenance lattice on the 6502: A and the pointer pair (zp/X/Y indirect)
static void isa_effect(const uint8_t *b, DpProvRegs *r, const DpProvRom *rom) {
    (void)b; (void)r; (void)rom;   // no proven pointer pair on the 6502 yet: nothing proven, nothing killed
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

static const DpIsa isa = { "m6502", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_effect, isa_illegal, isa_disasm, isa_mem_operand };
const DpIsa *dp_isa_m6502(void) { return &isa; }
