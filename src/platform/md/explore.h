/* Forking explorer for the Mega Drive (milestone 2).
 * Both outcomes of every conditional branch are followed (the second one
 * from a whole-machine snapshot); every bound has a counter that is reported.
 * Numbers are observations of this tool on a concrete machine (RAM at zero,
 * static pads), not hardware measurements. */
#ifndef DP_MD_EXPLORE_H
#define DP_MD_EXPLORE_H
#include <stdint.h>
#include <stdio.h>
#include "../../core/cart_md.h"

enum { DP_MDF_NATURAL = 1, DP_MDF_FORCED = 2, DP_MDF_DATA = 4, DP_MDF_QUEUED = 8 };
#define DP_MD_COPY_CAP 4096

typedef struct {
    uint64_t max_total;   /* instructions over all paths */
    uint64_t path_steps;  /* instructions per path */
    uint32_t queue_cap;   /* pending forks (each holds one machine snapshot) */
    uint32_t fork_cap;    /* forks created per branch site */
    uint32_t visit_cap;   /* visits of one instruction per path (loop bound) */
} DpMdOptions;

typedef struct { uint32_t pc, src, len, code; } DpMdCopyRec;

typedef struct {
    uint8_t *flags;       /* per ROM byte, DP_MDF_* */
    size_t rom_size;
    uint64_t steps, paths, forks, irq_forks;
    uint64_t sat_total;        /* paths dropped/cut by the global budget */
    uint64_t sat_path;         /* paths cut by the per-path budget */
    uint64_t sat_visit;        /* paths cut by the loop bound */
    uint64_t sat_queue;        /* forks dropped, queue full */
    uint64_t sat_site;         /* forks dropped, per-site cap */
    uint64_t fork_seen;        /* alternative already executed/queued (not a loss) */
    uint64_t fork_off_rom;     /* alternative target outside the ROM */
    uint64_t dropped_pending;  /* forks left unrun when the total budget ended */
    uint64_t faults;
    uint64_t exec_outside, ram_exec;
    uint64_t reg_r[64], reg_w[64], unmapped_r, unmapped_w;
    DpMdCopyRec *copies;
    int ncopy;
    uint64_t copy_overflow;
    uint32_t exec_bytes, forced_only_bytes, dread_bytes;
} DpMdResults;

void dp_md_options_default(DpMdOptions *o);
int dp_md_explore(const DpMdCart *c, const DpMdOptions *o, DpMdResults *r);
void dp_md_results_free(DpMdResults *r);
void dp_md_explore_report(const DpMdCart *c, const DpMdOptions *o, const DpMdResults *r, FILE *out);
/* One byte per ROM byte: the DP_MDF_* flags. */
int dp_md_explore_map(const DpMdResults *r, const char *path);
/* Text: executed ranges and copies. */
int dp_md_explore_evidence(const DpMdResults *r, const char *path);
#endif
