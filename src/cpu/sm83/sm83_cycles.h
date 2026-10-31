// sm83_cycles.h: T-cycle cost of one executed instruction (play sessions).
#ifndef MS_SM83_CYCLES_H
#define MS_SM83_CYCLES_H

#include "../../core/machine.h"

#include <stdint.h>

// cost in T-cycles of the instruction the step trace describes; cb_op is the
// operand byte when info->opcode == 0xCB (ignored otherwise)
uint32_t sm83_step_cycles(const MsStepInfo *info, uint8_t cb_op);

#endif
