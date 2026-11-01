/* Linear logged run of a Mega Drive ROM (milestone 1: no forks).
 * Everything is an observation of this tool on a concrete machine (RAM at
 * zero, static pads), not a hardware measurement. */
#ifndef DP_MD_TRACE_H
#define DP_MD_TRACE_H
#include <stdint.h>
#include <stdio.h>
#include "../core/cart_md.h"

#define DP_MD_MAX_COPIES 256
#define DP_MD_MAX_BANKEV 256

typedef struct {
    uint32_t pc, src, len, code; /* code: VDP CD bits of the destination */
} DpMdCopy;

typedef struct {
    uint32_t pc, addr;
    uint8_t value;
} DpMdBankEv;

typedef struct {
    uint8_t *exec;      /* per ROM byte: 1 = executed (opcode + extension words) */
    uint8_t *dread;     /* per ROM byte: 1 = read as data */
    size_t rom_size;
    uint64_t steps, max_steps;
    int cut;            /* budget saturated */
    int fault;          /* MdFull fault code (0 = none) */
    uint32_t fault_pc;
    uint64_t exec_outside;      /* instructions run at pc >= ROM size */
    uint64_t ram_exec;          /* subset of exec_outside in work RAM */
    uint64_t reg_r[64], reg_w[64]; /* per dp_md_regs index */
    uint64_t unmapped_r, unmapped_w;
    DpMdCopy copies[DP_MD_MAX_COPIES];
    int ncopy;
    uint64_t copy_overflow;     /* copies beyond DP_MD_MAX_COPIES, counted */
    DpMdBankEv bank[DP_MD_MAX_BANKEV];
    int nbank;
    uint64_t bank_overflow;
    uint32_t exec_bytes, dread_bytes;
} DpMdTrace;

int dp_md_trace_run(const DpMdCart *c, uint64_t max_steps, DpMdTrace *t);
void dp_md_trace_free(DpMdTrace *t);
void dp_md_trace_report(const DpMdCart *c, const DpMdTrace *t, FILE *out);
#endif
