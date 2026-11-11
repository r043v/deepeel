// huc_isa.c: the HuC6280 as a DpIsa (spec §7.3).  Wraps the PCE decoder
// (src/platform/pce/hucdec.c); conditions are the decoder's own encoding
// (0..7 flag branches, 8+bit BBR/BBS, 16+bit BBS — see DpHucIns.cond).
#include "../core/dpmachine.h"
#include "../platform/pce/pce.h"
#include <string.h>

static int isa_length(uint8_t op) { return dp_huc_length(op); }

static int isa_flow(const uint8_t *bytes, uint32_t pc, DpFlowInfo *o) {
    DpHucIns in;
    o->kind = DP_FLOW_SEQ; o->fall = 1; o->call = 0; o->rst = 0; o->halt = 0;
    o->target = -1; o->cond = -1; o->length = 1;
    if (!dp_huc_decode(bytes, 7, pc, &in)) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return 1; }
    o->length = in.length;
    o->cond = in.cond;
    switch (in.flow) {
    case DP_HUC_NEXT:
        break;
    case DP_HUC_JUMP:
        o->kind = DP_FLOW_JUMP; o->fall = 0; o->target = in.target;
        break;
    case DP_HUC_INDIRECT:
        o->kind = DP_FLOW_INDIRECT; o->fall = 0;
        break;
    case DP_HUC_CALL:
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->target = in.target;
        break;
    case DP_HUC_RETURN:
        o->kind = DP_FLOW_RET; o->fall = 0;
        break;
    case DP_HUC_BRANCH:
        o->kind = DP_FLOW_COND; o->fall = 1; o->target = in.target;
        break;
    case DP_HUC_BRK:
        // a software interrupt through $FFF6: the vector is outside the ROM
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1;
        break;
    case DP_HUC_ILLEGAL:
        o->kind = DP_FLOW_ILLEGAL; o->fall = 1;   // a two-cycle NOP on the machine
        break;
    }
    return o->length;
}

static int isa_cond_of(uint8_t op) {
    DpHucIns in;
    uint8_t bytes[7] = { op, 0 };
    if (!dp_huc_decode(bytes, sizeof bytes, 0, &in)) return -1;
    return in.flow == DP_HUC_BRANCH ? in.cond : -1;
}

// the tested flag (dp_huc_taken's own convention); BBR/BBS test zero-page bits
static uint8_t isa_cond_flag(int cond) {
    static const uint8_t flag[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 }; // C Z D I T V N
    if (cond >= 8) return 0;   // a zero-page bit: no flag
    return flag[cond & 7];
}

static int isa_illegal(uint8_t op) {
    DpHucIns in;
    uint8_t bytes[7] = { op, 0 };
    if (!dp_huc_decode(bytes, sizeof bytes, 0, &in)) return 1;
    return in.flow == DP_HUC_ILLEGAL;
}

// the provenance lattice on the HuC6280: not extracted yet (the MPR window
// system makes a static pointer pair a different problem than HL)
static void isa_effect(const uint8_t *b, DpProvRegs *r, const DpProvRom *rom) {
    (void)b; (void)r; (void)rom;
}

static int isa_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    DpHucIns in;
    uint8_t bytes[7] = { 0 };
    unsigned avail = rom_size > addr ? (unsigned)(rom_size - addr) : 0;
    if (!avail) return 0;
    if (avail > sizeof bytes) avail = sizeof bytes;
    memcpy(bytes, rom + addr, avail);
    if (!dp_huc_decode(bytes, sizeof bytes, addr, &in)) return 0;
    if ((unsigned)in.length > avail) return 0;
    dp_huc_format(&in, bytes, out, outsz);
    return in.length;
}

static int isa_mem_operand(const uint8_t *b, DpMemOp *o) {
    o->access = DP_MA_NONE; o->mode = DP_AM_NONE; o->pair = 0; o->post = 0; o->width = 0; o->addr = 0;
    DpHucIns in;
    if (!dp_huc_decode(b, 7, 0, &in)) return 0;
    switch (in.mode) {
    case DP_HUC_M_ZP: case DP_HUC_M_ZPX: case DP_HUC_M_ZPY:
        o->mode = DP_AM_ABS; o->addr = in.operand; o->width = 1; break;
    case DP_HUC_M_ABS: case DP_HUC_M_ABSX: case DP_HUC_M_ABSY: case DP_HUC_M_IND: case DP_HUC_M_INDX:
        o->mode = DP_AM_ABS; o->addr = in.operand; o->width = 1; break;
    case DP_HUC_M_IZX: case DP_HUC_M_IZY: case DP_HUC_M_IZP:
        o->mode = DP_AM_PAIR; o->pair = 2; o->width = 1; break;   // zero-page pointer: no static address
    case DP_HUC_M_IMP: case DP_HUC_M_IMM: case DP_HUC_M_REL: case DP_HUC_M_ZPREL:
    case DP_HUC_M_BLOCK: case DP_HUC_M_TST_ZP: case DP_HUC_M_TST_ABS:
    default:
        return 0;
    }
    // the read/write split comes from the opcode class the decoder knows;
    // conservative here: reads only, writes via the runtime observer
    o->access = DP_MA_READ;
    return 1;
}

static const DpIsa isa = { "huc6280", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_effect, isa_illegal, isa_disasm, isa_mem_operand };
const DpIsa *dp_isa_huc6280(void) { return &isa; }
