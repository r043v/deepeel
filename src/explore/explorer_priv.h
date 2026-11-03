// explorer_priv.h: the internals of the exploration engine shared by its
// translation units (explorer.c, forks.c, seeds.c, ctxproj.c).  Not an API.
#ifndef DP_EXPLORER_PRIV_H
#define DP_EXPLORER_PRIV_H
#include "explorer.h"

void *xrealloc(void *p, size_t n);
uint16_t cur_bank(DpExplorer *ex, uint16_t pc);
int queue_push(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                      uint32_t choice);
uint32_t choice_new(DpExplorer *ex, uint32_t parent, int kind, uint32_t site, int bank,
                           int pc);
uint8_t *seed_base(DpExplorer *ex);
int defer_room(DpExplorer *ex, int reason);
void fork_deferred(DpExplorer *ex, uint8_t *fimg, int reason, uint32_t site, uint16_t tpc);
void pseed_add(DpExplorer *ex, int bank, uint16_t pc, int kind, int forced);
uint64_t project_ctx(DpExplorer *ex, const DpSection *s);
void fork_branch(DpExplorer *ex, int cond, uint32_t site, uint16_t pc, uint8_t op);
void fork_irq(DpExplorer *ex);
void fork_key(DpExplorer *ex, uint32_t site);
void fork_p1(DpExplorer *ex, uint32_t site, uint16_t pc, uint8_t op);
int both_sides_covered(DpExplorer *ex, uint16_t pc, uint8_t op);
int is_padding_at(const DpCart *c, uint32_t bank, uint16_t addr);
int is_illegal_op(uint8_t op);

uint16_t image_pc(const uint8_t *img);
int defer_push(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                      uint32_t choice, int reason);
static inline void u32_add(uint32_t *p, uint32_t v) { __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
int frontier_refill(DpExplorer *ex);
int reason_eligible(const DpExplorer *ex, int reason);
void spool_feed(DpExplorer *ex);
int spool_write_rec(DpExplorer *ex, const uint8_t *img, int forced, int kind,
                           int from_reset, uint32_t choice);

#endif
