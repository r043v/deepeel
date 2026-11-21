/* Static TLCS-900H decoder (the Neo Geo Pocket's CPU): instruction length,
 * control-flow class, targets, memory operand and text.  Pure function of the
 * bytes; no machine state.  The semantics follow the interpreter of the m0boy
 * runtime (targets/ngp/runtime/ngp_cpu.c), which is the model this project
 * explores against; the opcode map agrees byte for byte with MAME's TLCS-900H
 * tables. */
#ifndef DP_TLCS900_H
#define DP_TLCS900_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DP_TLCS_OTHER = 0,
    DP_TLCS_JP,      /* JP nn16/nn24: direct unconditional jump */
    DP_TLCS_JCC,     /* JR cc,d8 / JRL cc,d16: conditional relative jump */
    DP_TLCS_JPMEM,   /* JP cc,(mem): absolute (mem) -> conditional direct, register (mem) -> indirect */
    DP_TLCS_CALL,    /* CALL nn16/nn24, CALR d16 */
    DP_TLCS_CALLCC,  /* CALL cc,(mem) */
    DP_TLCS_RET,     /* RET / RETD / RETI */
    DP_TLCS_RETCC,   /* RET cc */
    DP_TLCS_DJNZ,    /* DJNZ r,d8 (condition index TLCS_DJNZ_COND) */
    DP_TLCS_SWI,     /* SWI n: a software interrupt, vector through the BIOS */
    DP_TLCS_HALT,
    DP_TLCS_ILLEGAL
} DpTlcsClass;

/* the pseudo condition index of DJNZ (its counter is not a flag); the native
 * conditions are the TLCS's own cc 0..15 (F LT LE ULE OV MI EQ ULT T GE GT
 * UGT NOV PL NE UGE) */
#define TLCS_DJNZ_COND 16

typedef struct {
    unsigned len;          /* bytes, >= 1 */
    DpTlcsClass cls;
    int has_target;        /* target field is a proven static address */
    uint32_t target;       /* 24-bit masked */
    int cond;              /* 0..15, TLCS_DJNZ_COND, -1 otherwise */
    unsigned size;         /* 0 b, 1 w, 2 l (the group forms); 3 = no size */
    unsigned pair;         /* register slot of a register-based mem operand, 255 none */
    int mem_is_abs;        /* the mem operand is an absolute address */
    uint32_t mem_addr;     /* mem_is_abs: the address */
    uint8_t mem_access;    /* 0 none, 1 read, 2 write, 3 rw (the data operand) */
    unsigned op_pos;       /* byte index of the operation byte (after the (mem) operand) */
    uint8_t op_byte;       /* the operation byte of the group forms */
    uint8_t reg_code;      /* the register-code operand of the reg group's mode-7 forms */
    char text[96];
} DpTlcsInsn;

/* the 24-bit register slots the ISA's DpRegState uses (also the `pair` codes
 * above): 0-3 the current bank's WA/BC/DE/HL (bytes: W A | B C | D E | H L,
 * high first), 4-7 XIX/XIY/XIZ/XSP; the banked copies live in 8-19 */
enum { DP_TLCSR_WA = 0, DP_TLCSR_BC, DP_TLCSR_DE, DP_TLCSR_HL,
       DP_TLCSR_IX, DP_TLCSR_IY, DP_TLCSR_IZ, DP_TLCSR_SP,
       DP_TLCSR_BANK0 = 8, DP_TLCSR_BANK3 = DP_TLCSR_BANK0 + 12,
       DP_TLCSR_NREGS = DP_TLCSR_BANK3 + 4 };

/* Decodes the instruction at `pc`; `code` points at its first byte and `avail`
 * bytes may be read.  Returns len, or 0 if the bytes run out. */
unsigned dp_tlcs900_decode(const uint8_t *code, size_t avail, uint32_t pc, DpTlcsInsn *out);

/* length bound from the first byte only (flow()'s decode is the exact source) */
int dp_tlcs900_length(uint8_t op);

#endif
