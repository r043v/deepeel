#ifndef DP_SEGA_CLOSURE_H
#define DP_SEGA_CLOSURE_H
#include "backend.h"
enum { DP_SEGA_CLOSURE_STACK=8 };
typedef struct {
    uint32_t nodes,seeds,views,contexts;
} DpSegaClosureOptions;
typedef struct {
    DpSegaClosureOptions limits;
    uint64_t seeds,seed_skips,nodes,processed,duplicates,view_skips,context_cuts,node_cuts;
    uint64_t unmapped,ambiguous,padding,data,decode,indirect,returns_resolved,halted,map_barriers,new_bytes;
    uint64_t stack_invalidations,stack_cuts,indirect_resolved,memory_resolved;
    uint64_t static_data_reads,static_data_bytes;
    uint32_t views;
} DpSegaClosureStats;
typedef struct DpSegaClosure DpSegaClosure;
// Observations capture ROM/RAM visibility, cartridge windows, PC, SP and C.
// The optional target frontier also captures HL/IX/IY. No machine is run by
// the static passes. Mapper writes invalidate knowledge instead of guessing banks.
DpSegaClosure *dp_sega_closure_create(const DpSegaBackend *backend,const DpSegaSystem *system,
    const uint8_t *rom,size_t size,uint8_t *flags,DpSegaClosureOptions options,DpSegaClosureStats *stats);
// Optional independent frontier, sharing only final ROM evidence. Enable before
// observations; each pass has its own complete declared budget and counters.
int dp_sega_closure_targets(DpSegaClosure *closure,DpSegaClosureStats *stats);
// Refine BC/DE and blocks after targets, sharing its remaining node/seed/view
// budgets. Attach before observations; run finalizes the observation window.
int dp_sega_closure_blocks(DpSegaClosure *closure,DpSegaClosureStats *stats);
void dp_sega_closure_invalidate(DpSegaClosure *closure);
void dp_sega_closure_observe(DpSegaClosure *closure);
void dp_sega_closure_run(DpSegaClosure *closure);
void dp_sega_closure_free(DpSegaClosure *closure);
#endif
