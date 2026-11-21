/* Static V30MZ decoder (the WonderSwan's CPU, an 8086/80186 subset as the
 * BizHawk core and m0boy's runtime model it): instruction length, control
 * flow class, condition codes and data operands for the exploration kernel.
 * The runtime's own dispatch (m0boy targets/wonderswan ws_cpu.c) is the
 * truth: an opcode it reports unsupported decodes as illegal here. */
#ifndef DP_V30MZ_H
#define DP_V30MZ_H

#include <stdint.h>

/* control-flow class of a decoded instruction */
enum {
    DP_V30_SEQ = 0,     /* falls through */
    DP_V30_JCC,         /* conditional short jump (70-7F) */
    DP_V30_JMP,         /* unconditional jump (e9/eb/ea) */
    DP_V30_CALL,        /* call (e8/9a) */
    DP_V30_RET,         /* return (c2/c3/ca/cb/cf) */
    DP_V30_INDIRECT,    /* register/memory-indirect call/jmp (ff /2 /4 /5) */
    DP_V30_INT,         /* software interrupt (cc/cd/ce) */
    DP_V30_HALT,        /* f4 */
    DP_V30_ILLEGAL,
};

/* condition indices: the 8086 order (the opcode's low nibble); 16/17 are the
 * CX conditions (loop/loopz/loopnz), 18 jcxz — they test CX, not a flag */
enum { DP_V30_CX_COND = 16, DP_V30_JCXZ_COND = 18 };

/* register slots agreed between dp_isa_v30mz (ws_isa.c) and the machine's
 * get_regs (ws_machine.c): byte order low byte first */
enum {
    DP_V30R_AX, DP_V30R_CX, DP_V30R_DX, DP_V30R_BX,
    DP_V30R_SP, DP_V30R_BP, DP_V30R_SI, DP_V30R_DI,
    DP_V30R_FL,                       /* the flag byte's slot (2 bytes wide) */
    DP_V30R_NREGS
};

typedef struct {
    unsigned len;
    uint8_t cls;              /* DP_V30_* */
    int8_t cond;              /* condition index, -1 when unconditional */
    uint32_t target;          /* linear transfer target, UINT32_MAX if none */
    uint8_t mod, reg, rm;     /* the ModRM fields (0 when absent) */
    uint8_t has_modrm;
    uint8_t op_byte;
} DpV30Insn;

/* full decode (also the disassembler's engine); returns 0 on truncation,
 * still fills `out` (cls ILLEGAL) when the opcode is unsupported */
unsigned dp_v30mz_decode(const uint8_t *bytes, unsigned avail, uint32_t pc, DpV30Insn *out);
/* length bound from the first byte only (flow()'s decode is the exact source);
 * ModRM instructions assume the widest displacement */
int dp_v30mz_length(uint8_t op);
/* textual form; returns the length or 0 when unsupported */
int dp_v30mz_disasm(const uint8_t *bytes, unsigned avail, uint32_t pc, char *out, unsigned outsz);

#endif
