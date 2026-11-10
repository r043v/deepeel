// core.h: the generic exploration kernel (docs/PLAN-MUTUALISATION.fr.md, step 1).
// Drives any DpMachine + DpIsa: executes paths instruction by instruction,
// takes both sides of every conditional (the natural side by execution, the
// other one as a forced fork from the saved pre-instruction state), keys
// coverage by ROM index and reports every cap it hits.  Platform specifics are
// optional DpPlatformHooks.
#ifndef DP_EXPLORE_CORE_H
#define DP_EXPLORE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "../core/coverage.h"

#include "../core/dpmachine.h"

enum { DP_CORE_NATURAL = 1, DP_CORE_FORCED = 2, DP_CORE_COPIED = 4 };   // COPIED: a ROM byte later written verbatim to RAM

// optional platform hooks; every member may be NULL
typedef struct DpPlatformHooks {
    void *user;
    // called before each instruction; return 0 to stop the current path
    int (*before_step)(void *user, DpMachine *m, uint32_t pc);
    // called after each executed instruction (platform events: mapper, traps)
    void (*after_step)(void *user, DpMachine *m, uint32_t pc);
    // asked before a forced side is queued; return 0 to refuse it (counted in
    // DpCoreResult.hook_vetoed)
    int (*veto_fork)(void *user, DpMachine *m, uint32_t site, uint32_t other);
} DpPlatformHooks;

typedef struct DpCoreConfig {
    uint64_t max_total;        // total instruction budget (0: unbounded)
    uint32_t max_path_steps;   // per-path instruction cap (0: unbounded)
    uint32_t fork_site_cap;    // forks per conditional site (0: unbounded)
    uint32_t visits_per_pc;    // per-path visits of one pc before the path stops (0: unbounded)
    unsigned jobs;             // parallel workers (<=1: single); needs DpMachine.clone
    int no_forced_ram_cut;     // 1: let forced paths run unfed RAM
    const uint32_t *seeds;     // extra entry points (vectors): queued as forced paths from the start state
    size_t n_seeds;
    int no_copies;             // 1: do not observe data accesses
    int no_novelty_order;      // 1: plain LIFO instead of uncovered-target-first
    size_t queue_max;          // pending forked states held in RAM (0: unbounded)
    const DpPlatformHooks *hooks;
} DpCoreConfig;

typedef struct DpCoreResult {
    uint32_t *src_flags;       // per ROM index: DP_SRC_* (core/coverage.h), OR-ed over every proving source
    uint64_t *first_seen;      // per ROM index: global step count when first covered (0: never)
    uint8_t *flags;            // per ROM index: DP_CORE_NATURAL | DP_CORE_FORCED
    size_t rom_size;
    uint64_t steps, paths, forks;
    uint64_t sat_total, sat_path, sat_loop, sat_site, sat_queue;   // reported caps, never silent
    uint64_t pruned_covered;   // forks skipped because both sides were already covered
    uint64_t copy_bytes;       // verbatim ROM->RAM byte copies seen (a ROM read written unchanged to RAM)
    uint64_t copy_forced;      // the same on forced paths (speculative, not marked on the ROM)
    uint64_t forced_ram_cuts;  // forced paths cut when executing RAM no verbatim ROM copy fed
    uint64_t workers;          // workers actually started
    uint64_t workers_short;    // workers requested but unavailable (no clone / init failure)
    uint64_t seeded;           // seed entry points queued
    uint64_t novelty_picks;    // pending states chosen ahead of LIFO order because their target was uncovered
    uint64_t hook_vetoed;      // forced sides refused by DpPlatformHooks.veto_fork
    uint64_t exec_outside;     // instructions executed where rom_index < 0 (RAM/VRAM code)
    uint64_t no_pc;            // 1 when the machine has no pc/set_pc: nothing explored
} DpCoreResult;

// a section is a maximal run of ROM indexes covered by execution; its verdict
// is "required" when any byte of it ran on a natural path, "forced" otherwise
typedef struct DpCoreSection {
    size_t lo, hi;             // ROM index range, inclusive
    size_t natural, forced;    // bytes covered by each kind of path
    int required;
} DpCoreSection;
// fills up to `cap` sections, returns the total count (may exceed cap)
size_t dp_core_sections(const DpCoreResult *r, DpCoreSection *out, size_t cap);

// Common kernel options, parsed once for every platform CLI: --no-copies,
// --no-forced-ram-cut, --no-novelty-order, --visits-per-pc N.  Returns the
// number of argv entries consumed (0: not a kernel option).
int dp_core_parse_option(DpCoreConfig *c, int argc, char **argv, int i);

void dp_core_config_default(DpCoreConfig *cfg);
// explores from the machine's current state; rom_size = number of ROM index slots
int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg, DpCoreResult *out);
void dp_core_result_free(DpCoreResult *r);

#endif
