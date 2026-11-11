// core.h: the generic exploration kernel (docs/PLAN-MUTUALISATION.fr.md, step 1).
// Drives any DpMachine + DpIsa: executes paths instruction by instruction,
// takes both sides of every conditional (the natural side by execution, the
// other one as a forced fork from the saved pre-instruction state), keys
// coverage by ROM index and reports every cap it hits.  Platform specifics are
// optional DpPlatformHooks.
#ifndef DP_EXPLORE_CORE_H
#define DP_EXPLORE_CORE_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include "../core/coverage.h"

#include "../core/dpmachine.h"

enum { DP_CORE_NATURAL = 1, DP_CORE_FORCED = 2, DP_CORE_COPIED = 4, DP_CORE_COMPRESSED = 8 };   // COPIED: a ROM byte later written verbatim to RAM

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
    // technique 2, event-register probes: asked once per ROM site before the instruction
    // at `pc` runs.  For alternative k = 0,1,... the hook plants a status value the
    // hardware can present, saves the machine into `save` (m->save), restores the live
    // state and returns 1; it returns 0 when k is out of range (or `pc` is no probe site).
    int (*event_probe)(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save);
    // IRQ fork: asked once per ROM site.  When an interrupt would be serviced at the next
    // run(), the hook saves into `save` the state where that service is suppressed for
    // `delay` instructions (the line stays pending) and returns 1; otherwise returns 0.  The
    // live machine is left unchanged.  Counted in DpCoreResult.irq_forks.
    int (*irq_alt)(void *user, DpMachine *m, unsigned delay, uint8_t *save);
} DpPlatformHooks;

typedef struct DpCoreConfig {
    uint64_t max_total;        // total instruction budget (0: unbounded)
    uint32_t max_path_steps;   // per-path instruction cap (0: unbounded)
    uint32_t fork_site_cap;    // forks per conditional site (0: unbounded)
    uint32_t visits_per_pc;    // per-path visits of one pc before the path stops (0: unbounded)
    unsigned jobs;             // parallel workers (<=1: single); needs DpMachine.clone
    int no_taint_cut;          // 1: do not cut forced paths on tainted control transfers (technique 1.b)
    int no_index_replay;       // 1: do not replay table-index reads (technique 7)
    int no_rng_learn;          // 1: do not fork bytes that change between visits (technique 4)
    int no_learn_sig;          // 1: do not learn `ld a,(nn); cp n` signatures (technique 3)
    int no_site_score;         // 1: novelty pick ignores the per-site yield score (technique 5)
    int no_forced_ram_cut;     // 1: let forced paths run unfed RAM
    const uint32_t *seeds;     // extra entry points (vectors): queued as forced paths from the start state
    size_t n_seeds;
    int no_event_probes;
    uint32_t seed_lo, seed_hi;  /* technique 3: RAM range pre-filled in extra start states (needs DpMachine.poke) */
    uint8_t seed_vals[8]; unsigned seed_n;   /* values (default 00 FF 55 AA) */
    uint32_t sessions;          /* held-pad sessions started from power-on (0: off); default 16 when the machine has set_input */
    int no_sessions;
    int no_irq_jitter;          /* technique 9: only the 1-instruction IRQ delay, no 2..3 */       // 1: ignore DpPlatformHooks.event_probe (technique 2)
    int no_decompress;         // 1: do not look for decompressors (technique 6)
    int no_copies;             // 1: do not observe data accesses
    int no_novelty_order;      // 1: plain LIFO instead of uncovered-target-first
    uint32_t cell_lo, cell_hi; // technique 8: RAM range hashed into a semantic cell (cell_hi > cell_lo enables it)
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
    uint64_t taint_cut_indirect, taint_cut_ret;   // forced paths cut on a computed jump / a ret on a stack the path did not build
    uint64_t forced_ram_cuts;  // forced paths cut when executing RAM no verbatim ROM copy fed
    uint64_t workers;          // workers actually started
    uint64_t workers_short;    // workers requested but unavailable (no clone / init failure)
    uint64_t seeded;           // seed entry points queued
    uint64_t novelty_picks;    // pending states chosen ahead of LIFO order because their target was uncovered
    uint64_t index_replays;    // states queued with a RAM index byte set to 0..15 at a ROM-table read
    uint64_t rng_forks;        // states queued with a volatile RAM byte forced to 00/01/80/FF
    uint64_t learned_sigs;     // signature states queued from observed `ld a,(nn); cp n`
    uint64_t site_scored_picks; // picks that the per-site score moved away from the plain novelty choice
    uint64_t novelty_new_bytes; // ROM bytes first covered by paths started from a novelty pick (yield of technique 5)
    uint64_t plain_new_bytes;   // the same for paths started in plain LIFO order
    int in_novelty_path;        // internal: the path being run was a novelty pick
    uint64_t seeded_ram, event_probe_sites, irq_forks, jitter_forks, sessions; // probe sites visited (technique 2)
    uint64_t event_probes;      // alternative-status states queued
    uint64_t cells;             // distinct (fork target, RAM cell) pairs queued (technique 8)
    uint64_t cell_pruned;       // forced forks dropped because their (target, RAM cell) pair was already queued
    uint64_t compressed_bytes;  // ROM bytes read as a sequential stream that fed unequal RAM writes (technique 6)
    uint64_t decompress_runs;   // such streams detected
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

// Common kernel options, parsed once for every platform CLI: --cells LO-HI, --no-event-probes, --no-decompress, --no-copies,
// --no-forced-ram-cut, --no-novelty-order, --visits-per-pc N.  Returns the
// number of argv entries consumed (0: not a kernel option).
int dp_core_parse_option(DpCoreConfig *c, int argc, char **argv, int i);

// Prints the kernel's result in the shared report format (the platform-neutral
// part of every `--core` command); `label` names the platform.
void dp_core_report(FILE *out, const char *label, const DpCoreResult *r);

void dp_core_config_default(DpCoreConfig *cfg);
// explores from the machine's current state; rom_size = number of ROM index slots
int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg, DpCoreResult *out);
void dp_core_result_free(DpCoreResult *r);

#endif
