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

static int isa_flow(const uint8_t *bytes, uint16_t pc, DpFlowInfo *o) {
    DpM68kInsn in;
    o->kind = DP_FLOW_SEQ; o->fall = 1; o->call = 0; o->rst = 0; o->halt = 0;
    o->target = -1; o->cond = -1; o->length = 2;
    unsigned len = dp_m68k_decode(bytes, 10, pc, &in);
    if (!len) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return 2; }
    o->length = (uint8_t)len;
    o->cond = in.cond;
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

static void isa_effect(const uint8_t *b, DpProvRegs *r, const DpProvRom *rom) {
    (void)b; (void)r; (void)rom;   // the An/Dn provenance lattice is not extracted yet
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

static const DpIsa isa = { "68000", 3, 1, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_effect, isa_illegal, isa_disasm, isa_mem_operand };
const DpIsa *dp_isa_68000(void) { return &isa; }
