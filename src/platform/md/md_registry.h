/* Mega Drive hardware registry (32-bit addresses; the GB DpIoReg is 16-bit,
 * so MD gets its own types until the engine is widened). */
#ifndef DP_MD_REGISTRY_H
#define DP_MD_REGISTRY_H
#include <stdint.h>

typedef struct {
    uint32_t lo, hi; /* inclusive */
    const char *name;
    const char *category; /* "video" "sound" "input" "sys" "mapper" "memory" */
} DpMdReg;

typedef struct {
    int vector;     /* vector number (index into the table at $0) */
    const char *name;
} DpMdVector;

extern const DpMdReg dp_md_regs[];
extern const int dp_md_reg_count;
extern const DpMdVector dp_md_vectors[];
extern const int dp_md_vector_count;

/* Index in dp_md_regs of the register covering addr (24-bit, mirrors
 * folded), or -1. */
int dp_md_reg_find(uint32_t addr);
#endif
