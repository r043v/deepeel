#ifndef DP_SEEDSET_H
#define DP_SEEDSET_H
#include "core.h"
#include "../core/music_index_seeds.h"
#include <stdlib.h>
#include <string.h>

// Extra entry points handed to the kernel: the music-rip seeds, the user's --entry list and, with
// all_banks, every 16 KiB block shown at $8000.  Shared by every platform front end; a bank of
// 0xFFFF means "the current mapping" (the kernel does not re-bank that seed).
typedef struct { uint32_t *pc; uint16_t *bk; DpSeedRegs *rg; } DpSeedSet;
static inline void seedset_free(DpSeedSet *s) { free(s->pc); free(s->bk); free(s->rg); memset(s, 0, sizeof *s); }
static inline int seedset_build(DpCoreConfig *cfg, const DpMusicKernelSeeds *mks, const uint32_t *epc, const uint16_t *ebk,
                         size_t ne, unsigned all_banks, DpSeedSet *o) {
    memset(o, 0, sizeof *o);
    size_t tot = mks->n + ne + all_banks;
    if (!(ne + all_banks)) {   // nothing to add: the music seeds go as they are
        if (mks->n) { cfg->seeds = mks->pcs; cfg->n_seeds = mks->n; cfg->seed_banks = mks->banks; cfg->seed_regs = mks->regs; }
        return 0;
    }
    o->pc = malloc(tot * sizeof *o->pc); o->bk = malloc(tot * sizeof *o->bk); o->rg = malloc(tot * sizeof *o->rg);
    if (!o->pc || !o->bk || !o->rg) { seedset_free(o); return 1; }
    const DpSeedRegs none = { 0xFF, 0xFFFF };
    size_t k = 0;
    for (; k < mks->n; k++) { o->pc[k] = mks->pcs[k]; o->bk[k] = mks->banks ? mks->banks[k] : 0xFFFF; o->rg[k] = mks->regs ? mks->regs[k] : none; }
    for (size_t e = 0; e < ne; e++, k++) { o->pc[k] = epc[e]; o->bk[k] = ebk ? ebk[e] : 0xFFFF; o->rg[k] = none; }
    for (unsigned b = 0; b < all_banks; b++, k++) { o->pc[k] = 0x8000; o->bk[k] = (uint16_t)b; o->rg[k] = none; }
    cfg->seeds = o->pc; cfg->n_seeds = tot; cfg->seed_banks = o->bk; cfg->seed_regs = o->rg;
    return 0;
}

#endif
