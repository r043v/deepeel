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

static int isa_length(uint8_t op) { return sm83_len(op); }

static int isa_flow(const uint8_t *b, uint16_t pc, DpFlowInfo *o) {
    uint8_t op = b[0];
    int len = sm83_len(op);
    o->kind = DP_FLOW_SEQ; o->length = (uint8_t)len; o->fall = 1; o->call = 0;
    o->target = -1; o->cond = (int8_t)sm83_cond_of(op);
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
        break;
    default:
        if ((op & 0xC7) == 0xC7) { // RST
            o->target = op & 0x38; o->kind = DP_FLOW_CALL; o->call = 1;
        }
        break;
    }
    return len;
}

static const DpIsa isa = { "sm83", 2, 0, isa_length, isa_flow, sm83_disasm };
const DpIsa *dp_isa_sm83(void) { return &isa; }
