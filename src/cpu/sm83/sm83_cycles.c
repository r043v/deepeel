// sm83_cycles.c: T-cycle cost of every instruction.  Base tables are the
// standard SM83 timings (conditional rows hold the not-taken cost; the taken
// correction is applied from the step trace).  Used only by the play
// sessions, where the tick model needs a clock; the forking explorer is
// cycle-free by design.
#include "sm83_cycles.h"

#define X 4 // undefined opcodes: cost is irrelevant, they stop the path

static const uint8_t base[256] = {
    // 0x00
    4, 12, 8, 8, 4, 4, 8, 4, 20, 8, 8, 8, 4, 4, 8, 4,
    // 0x10
    4, 12, 8, 8, 4, 4, 8, 4, 12, 8, 8, 8, 4, 4, 8, 4,
    // 0x20
    8, 12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
    // 0x30
    8, 12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
    // 0x40
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    // 0x50
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    // 0x60
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    // 0x70 (0x76 HALT)
    8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
    // 0x80
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    // 0x90
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    // 0xA0
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    // 0xB0
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    // 0xC0
    8, 12, 12, 16, 12, 16, 8, 16, 8, 16, 12, 4, 12, 24, 8, 16,
    // 0xD0
    8, 12, 12, X, 12, 16, 8, 16, 8, 16, 12, X, 12, X, 8, 16,
    // 0xE0
    12, 12, 8, X, X, 16, 8, 16, 16, 4, 16, X, X, X, 8, 16,
    // 0xF0
    12, 12, 8, 4, X, 16, 8, 16, 12, 8, 16, 4, X, X, 8, 16,
};

static const uint8_t cb[256] = {
    // 0x00-0x3F rotates/shifts: 8, 16 through (HL)
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
    // 0x40-0x7F BIT: 8, 12 through (HL)
    8, 8, 8, 8, 8, 8, 12, 8, 8, 8, 8, 8, 8, 8, 12, 8,
    8, 8, 8, 8, 8, 8, 12, 8, 8, 8, 8, 8, 8, 8, 12, 8,
    8, 8, 8, 8, 8, 8, 12, 8, 8, 8, 8, 8, 8, 8, 12, 8,
    8, 8, 8, 8, 8, 8, 12, 8, 8, 8, 8, 8, 8, 8, 12, 8,
    // 0x80-0xFF RES/SET: 8, 16 through (HL)
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
    8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 16, 8,
};

uint32_t sm83_step_cycles(const MsStepInfo *info, uint8_t cb_op) {
    uint32_t c;
    if (info->opcode == 0xCB)
        c = cb[cb_op];
    else
        c = base[info->opcode];
    switch (info->ctrl) {
    case MS_CTRL_JR_CC: if (info->cond_taken) c += 4; break;
    case MS_CTRL_JP_CC: if (info->cond_taken) c += 4; break;
    case MS_CTRL_CALL_CC: if (info->cond_taken) c += 12; break;
    case MS_CTRL_RET_CC: if (info->cond_taken) c += 12; break;
    default: break;
    }
    return c;
}
