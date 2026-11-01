/* Static 68000 decoder: instruction length, control-flow class, targets and
 * text.  Pure function of the bytes; no machine state. */
#ifndef DP_M68K_H
#define DP_M68K_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DP_M68K_OTHER = 0,
    DP_M68K_BRA,     /* unconditional branch (target known) */
    DP_M68K_BSR,     /* call (target known) */
    DP_M68K_BCC,     /* conditional branch (target known, falls through) */
    DP_M68K_DBCC,    /* decrement-and-branch (target known, falls through) */
    DP_M68K_JMP,     /* jump; target known only for absolute / pc-relative */
    DP_M68K_JSR,     /* call; same */
    DP_M68K_RTS,
    DP_M68K_RTE,
    DP_M68K_RTR,
    DP_M68K_TRAP,    /* trap #n / trapv: vector call, falls through for dis */
    DP_M68K_STOP,
    DP_M68K_ILLEGAL  /* illegal / line A / line F */
} DpM68kClass;

typedef struct {
    unsigned len;        /* bytes, even, >= 2 */
    DpM68kClass cls;
    int has_target;      /* target field is a proven static address */
    uint32_t target;     /* 24-bit masked */
    int cond;            /* BCC/DBCC condition (0..15), -1 otherwise */
    char text[96];
} DpM68kInsn;

/* Decodes the instruction at `pc`; `code` points at its first byte and
 * `avail` bytes may be read.  Returns len, or 0 if the bytes run out. */
unsigned dp_m68k_decode(const uint8_t *code, size_t avail, uint32_t pc, DpM68kInsn *out);

#endif
