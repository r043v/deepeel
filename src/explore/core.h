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
#include <signal.h>
#include "../core/coverage.h"
#include "ramarch.h"

#include "../core/dpmachine.h"

enum { DP_CORE_NATURAL = 1, DP_CORE_FORCED = 2, DP_CORE_COPIED = 4, DP_CORE_COMPRESSED = 8, DP_CORE_DATA = 16, DP_CORE_STATIC = 32 };   // STATIC: reached by the static closure from a natural direct transfer   // DATA: read as data by a natural path   // COPIED: a ROM byte later written verbatim to RAM
enum { DP_CORE_START_NATURAL = 64, DP_CORE_START_FORCED = 128 };
// DpSeedRegs lives in core/dpmachine.h (the seed_regs contract is on DpMachine)

struct DpCoreResult;
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
    // Optional independently enumerable interrupt sources. Source numbers are stable
    // within a backend; a missing or disabled source returns 0 without consuming
    // the site key. When supplied, this replaces irq_alt for this backend.
    unsigned irq_source_count;
    int (*irq_source_alt)(void *user, DpMachine *m, unsigned source, unsigned delay, uint8_t *save);
    // called once when the workers are done, before the speculative gaps: the platform folds
    // its own evidence into the result (Sega: the provenance closure's static bytes, C3)
    void (*finish)(void *user, struct DpCoreResult *r);
} DpPlatformHooks;

typedef struct DpCoreConfig {
    uint64_t max_total;        // total instruction budget (0: unbounded)
    uint32_t max_path_steps;   // per-path instruction cap (0: unbounded)
    uint32_t fork_site_cap;    // forks per conditional site (0: unbounded)
    uint32_t visits_per_pc;    // per-path visits of one pc before the path stops (0: unbounded)
    unsigned jobs;             // parallel workers (<=1: single); needs DpMachine.clone
    int adaptive;              // 1: `jobs` is a ceiling; the number of active workers follows the machine's free CPU and RAM
    int no_taint_cut;          // 1: do not cut forced paths on tainted control transfers (technique 1.b)
    unsigned index_max;        // technique 7: highest index replayed (0 = default 15, at most 255)
    int no_index_replay;       // 1: do not replay table-index reads (technique 7)
    int no_rng_learn;          // 1: do not fork bytes that change between visits (technique 4)
    int no_solve;              // 1: no concolic solving of conditionals (technique 1)
    int no_learn_sig;          // 1: do not learn `ld a,(nn); cp n` signatures (technique 3)
    int no_site_score;         // 1: novelty pick ignores the per-site yield score (technique 5)
    int no_forced_ram_cut;     // 1: let forced paths run unfed RAM
    int no_nonsense;           // 1: no nonsense cuts (echo/OAM pc, stack outside RAM, padding slide)
    int no_vectors;            // 1: do not seed the platform registry's RST/IRQ vector entries (G1)
    const uint8_t *rom_image;  // optional: the whole ROM (bank*0x4000 layout); enables bank-naming tables (S4 stride 3)
    size_t rom_image_size;
    const uint32_t *seeds;     // extra entry points (vectors): queued as forced paths from the start state
    size_t n_seeds;
    const uint16_t *seed_banks;   // optional, parallel to seeds: 16 KiB ROM block mapped into the seed window (DpMachine.bank_view) before the seed runs
    // optional, parallel to seeds: register seeds applied to the start state
    // before the seed runs (music rips: the init call carries the song number
    // in A and starts on the rip's stack).  a == 0xFF leaves A, sp == 0xFFFF
    // leaves SP; needs DpMachine.seed_regs, else counted in seed_regs_absent.
    const DpSeedRegs *seed_regs;
    int no_event_probes;
    unsigned pad_full;        // D10: 1 probes every pad mask at each pad-read site, 2 also the extended (six-button) keys
    int no_auto_seed;          // B10: off switch of the automatic cart-RAM seeding
    int no_tas_ckpt;           // A22/B3: off switch of the movie checkpoints
    unsigned ckpt_burst_frames;  // B3: frames of a random-pad burst run from each session checkpoint (0: none)
    unsigned tas_ckpt_every, tas_ckpt_cap;   // movie frames between checkpoints; total cap
    int no_held_budget;        // A26: held-pad sessions share the global budget again
    uint64_t held_steps;       // A26: step cap of one held-pad session path (0: unbounded)
    int no_exact;              // B5: off switch of the exact-state loop cut (the run then ends only by exhaustion or a cap)
    int no_spec, no_live_seeds;               // B8: off switch of the speculative gap disassembly
    int no_closure_ret;        // C3: off switch of the walk-time return stack (a direct call continues where its callee returns, not blindly after it)
    int no_closure_banks;      // C3: off switch of the walk-time bank simulation (closure follows `ld a,n; ld (bankreg),a`)
    int no_dispatch_forks;     // off switch of the dispatch forks (every unexecuted target of a code table met by an executed path)
    int no_closure_regs;       // C3: off switch of the regstate-aware closure (concrete bootstrapping + the ISA constant-transfer lattice)
    unsigned closure_seed_cap; // C3: distinct regstate seeds accepted per site (0: no cap; --unbounded lifts it)
    int no_bank_forks;         // B7: off switch of the bank forks at mapper writes
    unsigned bank_fork_cap;    // B7: bank forks made per site visit (0: unbounded)
    unsigned probe_ctx_cap;    // B1: contexts one probe site is armed under (1: the historical once-per-site)
    const char *state_dir;     // --state-dir: pending forked states are saved to <dir>/frontier.bin at exit
    int frontier_lz4;          // --frontier-lz4: persist pending states in compressed records
    int resume;                // --resume: states saved there by a previous run are queued first
    const char *cov_hits_path; // --cov-hits: a coverage v1 file whose per-line hits seed the heat (the max wins; the report then shows the played usage of that campaign)
    const char *cov_transfer_rom, *cov_transfer_cov;   // --cov-transfer ROM COV: another revision's (or this one's own) coverage replayed as forced-path seeds where the four-byte instruction windows match byte for byte
    int unbounded;             // --unbounded: no step/path/site-fork ceilings; the RAM queue spills to the disk spool
    int campaign;              // --campaign: a bounded phase 1 (the platform's width-first shape, --max-time/--max-total bound, default 120 s) then --resume --unbounded in the same process; one report per phase
    const volatile sig_atomic_t *stop_requested; // cooperative interruption, checked between instructions
    int no_closure;            // static closure off (no STATIC marking from natural direct transfers)
    int no_proj;               // C11 off: no context-projection eviction of forks
    int no_cells;              // technique 8 off: no (target, RAM cell) pruning of forced forks
    uint32_t seed_lo, seed_hi;  /* technique 3: RAM range pre-filled in extra start states (needs DpMachine.poke) */
    uint8_t seed_vals[8]; unsigned seed_n;   /* values (default 00 FF 55 AA) */
    uint32_t sessions;          /* held-pad sessions started from power-on (0: off); default 16 when the machine has set_input */
    int no_sessions;
    int no_start_attract;   /* --no-start-attract: no no-input attract session from power-on (the movie's end-state attract stays) */
    uint32_t attract_frames;    /* generic attract session: no input for this many frames from power-on (0: off; needs DpMachine.frame) */
    uint32_t min_secs, max_secs, stall_secs;   /* wall-clock limits, 0 = off: min before a stagnation stop; hard max; seconds without a new ROM byte */
    uint32_t attract_min;       /* attract runs at least this many frames before a stagnation stop (default 36000: 10 min of game time); attract_frames is the hard max */
    uint32_t attract_stall;     /* attract stops after this many frames without a newly covered ROM byte (0: never) */
    uint32_t play_frames;       /* generic play session: nine pad epochs over this many frames (0: off; needs frame + set_input) */
    uint32_t play_sessions;     /* number of play sessions (each starts at another catalog pad; default 1) */
    const uint8_t *tas_masks;   /* TAS replay (techniques 9): one kernel pad mask per frame, needs DpMachine.frame */
    const uint32_t *tas_masks32; /* C12: the wide form (DpInput bits, core/dpmachine.h); wins over tas_masks */
    uint32_t tas_frames;
    unsigned watch_every;       /* --watch: the unshifted movie replay publishes its game screen every N frames (DpMachine.render; 0 = off) */
    int replay_only;            /* --replay-only: play the movie lean (no fork, scan or spool work), stop at its end or after the last --save-frames frame */
    const char *save_frames;    /* --save-frames SPEC (N, A-B, comma list): full machine state + screen + work RAM of those movie frames, written to save_dir */
    const char *save_dir;       /* --save-dir D (default .) */
    const char *dsy_path;       /* --dsy FILE: the unshifted TAS replay writes work RAM per frame as DSY1 */
    int no_tas_jitter;          /* 1: replay the movie unshifted only (no +-1..3 frame shifts) */
    int no_irq_jitter;          /* technique 9: only the 1-instruction IRQ delay, no 2..3 */       // 1: ignore DpPlatformHooks.event_probe (technique 2)
    int no_decompress;         // 1: do not look for decompressors (technique 6)
    int no_infer;              // 1: skip the static inference (padding, gaps, copy-table siblings)
    int no_copies;             // 1: do not observe data accesses
    int no_novelty_order;      // 1: plain LIFO instead of uncovered-target-first
    uint32_t cell_lo, cell_hi; // technique 8: RAM range hashed into a semantic cell (cell_hi > cell_lo enables it)
    uint64_t spool_max;        // bytes the disk spool may hold (0: no cap); past it a pushed fork is dropped and counted in pending_dropped
    int no_frontier;           // do not export frontier.bin at the end (the caller owns the continuation)
    size_t queue_max;          // pending forked states held in RAM (0: unbounded)
    int queue_auto;            // default 1: queue_max is lowered at launch to what the free RAM (and the cgroup limit) can hold; an explicit --queue-max clears it
    const DpPlatformHooks *hooks;
    // live view: called once, right after the result arrays are allocated and before any path runs, with the
    // result being filled (flags/src_flags are updated atomically while the workers run, so a viewer may read them)
    void (*on_live)(void *user, const struct DpCoreResult *live);
    void *on_live_user;
    unsigned progress_secs;   // >0: a progress line on stderr every N seconds (0 = silent)
    const char *snap_cov;     // path of the periodic coverage snapshot (written to <path>.tmp then renamed); NULL = none
    unsigned snap_secs;       // snapshot period in seconds (default 600; 0 = none)
    // autonomous campaign (src/explore/auto.c): movie stagnation, machine snapshots, restarts from a snapshot, random runs
    const char *from_state;   // --from-state FILE: explore from this saved machine state (DpMachine.save layout, e.g. a movie snapshot) instead of power-on
    uint32_t tas_stall_frames;   // --tas-stall N: the unshifted movie replay stops after N frames without a ROM byte new to it (a desynced or idle movie); reported in tas_report (0: off)
    unsigned snap_every;      // --snap-every N: the unshifted movie replay writes <snap_dir>/s<frame>.state every N frames (0: off)
    const char *snap_dir;     // --snap-dir D
    unsigned snap_min;        // --snap-min N: a random run writes <snap_dir>/d<found>-<run>-<frame>.state after finding N new ROM bytes, at most one per snap_every frames (0: off)
    unsigned live_frame_ms;   // --live-frame-ms N: minimum wall-clock gap between two screen publications (default 500)
    const char *live_frame;   // --live-frame FILE: the unshifted movie replay writes its screen there (w,h,frame,total + ARGB) about twice a second, for the supervisor's live window
    const char *tas_summary;  // --tas-summary FILE.png: one picture (<=1920x1080) of the movie replay: key screens, identity, verdict
    const char *tas_name, *tas_emu, *tas_profile, *tas_rom;   // --tas-name/--tas-emu/--tas-profile: what the summary prints (emu: TASVideos API)
    const char *tas_report;   // --tas-report FILE: one line "frames=.. end=.. stalled=0|1 stall_frame=.. last_new=.. new_bytes=.." written when the unshifted replay ends
    const char *live_flags;   // --live-flags FILE: the coverage flag bytes (coverage.bin layout) rewritten at every progress tick, for a supervising process
    unsigned random_runs;     // --random N: N seeded random-pad runs started from the start state (each stops on random_stall frames without a new ROM byte)
    uint32_t random_frames;   // --random-frames N: frame cap of one random run (0 = none: only stagnation stops a run)
    int observe;              // --observe: sessions only (movie, attract, random): no fork, no deferred state; the movie's end state runs on as attract
    uint32_t random_min;      // --random-min N: a random run lasts at least N frames before stagnation may end it
    unsigned random_first;    // --random-first N: seed rank of the first random run (parallel processes play different pads)
    unsigned random_no_start; // --random-no-start N: the first N random runs never press Start (no pause or menu); the others use every button
    unsigned random_no_select; // --random-no-select N: the first N random runs never press Select (GB/GBC pad bit)
    int fabricated_forced;    // --fabricated-forced: what runs in a state the explorer made up (DP_SRC_DYN_FABRICATED) is recorded as forced, not natural
    unsigned random_hold;     // --random-hold N: one press in N of a random run is held 0.5 to 4 s instead of 8 frames (0: none)
    unsigned random_movie;    // --random-movie N: the first N random runs play 5 s segments of the --tas movie, drawn at random, instead of random pads
    uint32_t random_stall;    // --random-stall N: frames without a new ROM byte that end a random run (default 36000 = 10 min of game time)
} DpCoreConfig;

typedef struct DpCoreResult {
    uint32_t *src_flags;       // per ROM index: DP_SRC_* (core/coverage.h), OR-ed over every proving source
    uint32_t *hits;            // P13/A9: per ROM index: execution hits of the instruction head byte (the kernel's heat; shared by the workers like flags)
    uint64_t *first_seen;      // per ROM index: global step count when first covered (0: never)
    uint8_t *first_actor;      // P13/A9: actor of the first cover (DP_ACTOR_*, 0 unknown): a session replay discovery carries its play mode
    uint8_t *flags;            // per ROM index: DP_CORE_NATURAL | DP_CORE_FORCED
    uint64_t closure_bank_sims; uint8_t closure_banks, closure_ret; uint64_t closure_rets, closure_ptr_stores;   // C3: mapper writes replayed during a walk; walk-time bank simulation on
    uint64_t closure_unresolved; // C3: static edges (jump/branch/call) whose target is outside the mapped ROM view (RAM, other bank): counted, never silent
    uint64_t closure_stopped_data;   // A13: untrusted static walks stopped on data/copied bytes
    uint8_t *prov;             // per ROM index, 1 = reached only by an untrusted static walk (call fall-through,
                               // concrete-stack return): provisional code, counted as code once confirmed only
                               // ("zero data marked code" point 2); projected to the map's probable-code class
    uint64_t closure_entry_refused;   // point 3: untrusted continuations whose entry decodes like data (tiles/pointers/no-op soup), refused
    uint64_t closure_stack_invalidations;   // C3 port_effect: ghost returns discarded because an OUT to a memory-control port can hide the stack
    uint64_t live_seeds;       // B3: vector seeds started from a live checkpoint
    uint8_t *inferred;         // C4: DP_MAP_* inference per ROM byte (padding, gap fill, copy-table siblings); never an observation
    uint64_t inferred_code, inferred_pad, inferred_data, padding_bytes, sib_tables, sib_bytes, sib_refused, jt_tables, jt_bytes;
    uint8_t *page_win;
    uint32_t *live;       // C11 liveness: per 256-byte ROM page, a bitmap of the 256-byte RAM pages its natural-path instructions read
    // typed register accesses (natural paths only): per platform-registry slot, CPU reads, CPU writes and the set of
    // distinct byte values written; reg_slot maps an address to its slot + 1 (0 = not a register)
    const struct DpPlatform *reg_plat; uint16_t *reg_slot; uint64_t *reg_reads, *reg_writes; uint32_t *reg_vals; int reg_n;
    uint32_t live_w;      // words of that bitmap per ROM page         // B8: per ROM page (DpMachine.page_shift), 1 + the 8 KiB window it was executed at (0 = never executed)
    uint8_t *refs;             // B9: per ROM index, 1 = designated by an executed immediate pointer
    uint64_t imm_refs, spec_ref_runs;
    uint8_t *spec;             // B8: per ROM index, 1 = covered only by the speculative gap disassembly (never known)
    uint64_t spec_runs, spec_bytes;
    uint8_t *dest;             // per ROM index: DP_DEST_* where this byte's data ends up (0: untyped)
    size_t rom_size;
    uint64_t steps, paths, forks;
    uint64_t cooperative_yields; // active paths requeued to let pending work run
    uint64_t tas_paths;        // TAS replay paths started (unshifted + shifted)
    uint64_t sat_total, sat_path, sat_loop, sat_site;   // reported caps, never silent (the queue never saturates: it spills, counted by the spool line)
    uint64_t pruned_covered;   // forks skipped because both sides were already covered
    uint64_t copy_bytes;       // verbatim ROM->RAM byte copies seen (a ROM read written unchanged to RAM)
    uint64_t copy_forced;      // the same on forced paths (speculative, not marked on the ROM)
    uint64_t taint_cut_indirect, taint_cut_ret;   // forced paths cut on a computed jump / a ret on a stack the path did not build
    uint64_t forced_ram_cuts;  // forced paths cut when executing RAM no verbatim ROM copy fed
    uint64_t nonsense_pc;      // paths cut executing in echo/OAM/I-O space (machine cut_pc, audit E2)
    uint64_t nonsense_sp;      // paths cut on a RET whose stack left the RAM space (machine cut_sp)
    uint64_t pad_cuts;         // paths cut after sliding through a long 00/FF run (padding is not code)
    uint64_t pad_unmarked;     // executed 1-byte instructions inside a long 00/FF ROM run left unmarked (a ramp of rst $38 / nop is padding, not code)
    uint64_t workers;          // workers actually started
    uint64_t workers_min, workers_max;   // adaptive mode: lowest / highest number of workers allowed
    uint64_t workers_short;    // workers requested but unavailable (no clone / init failure)
    uint64_t seeded;           // seed entry points queued
    uint64_t seed_refused_cap; // unique pointer seeds not queued due to the GB scan cap
    uint64_t seed_regs_absent; // register seeds requested but the machine has no seed_regs hook
    uint64_t seed_banks_absent; // bank seeds requested but the machine cannot show the block (no bank_view hook, or the block is not switchable)
    uint64_t vector_seeds;     // the platform registry's RST/IRQ vector entries queued as forced starts (G1)
    uint64_t novelty_picks;    // pending states chosen ahead of LIFO order because their target was uncovered
    uint64_t index_replays;    // states queued with a RAM index byte set to 0..15 at a ROM-table read
    uint64_t rng_forks;        // states queued with a volatile RAM byte forced to 00/01/80/FF
    DpRamArch ra;              // distinct (pc, bytes) variants executed outside ROM, with their ROM copy source / approximate producer
    uint64_t promoted_forced;             // A9: forced-only sides proven natural by the resolution
    uint64_t solved_forks, solve_tries;   // technique 1: natural states proving the other side / replays tried
    uint64_t learned_sigs;     // signature states queued from observed `ld a,(nn); cp n`
    uint64_t site_scored_picks; // picks that the per-site score moved away from the plain novelty choice
    uint64_t novelty_new_bytes; // ROM bytes first covered by paths started from a novelty pick (yield of technique 5)
    uint64_t plain_new_bytes;   // the same for paths started in plain LIFO order
    int in_novelty_path;        // internal: the path being run was a novelty pick
    uint64_t seeded_ram, event_probe_sites, irq_forks, jitter_forks, sessions; // probe sites visited (technique 2)
    uint64_t event_probe_arms;  // B1: arms (a fresh site, or a re-armed context)
    uint64_t event_probes;      // forced variants queued by the probe arms
    uint64_t epoch_forks, ckpt_bursts;   // B3: pad bursts started from session checkpoints
    uint64_t proof_roots, proof_bytes, proof_forced;   // end-of-run static proof: seeds, bytes newly static, of which only forced before
    uint64_t closure_roots, closure_bytes, closure_capped, closure_jp_tables, closure_jp_entries, prov_data_bytes; uint32_t *jp_seen; unsigned jp_seen_n, jp_seen_cap; uint64_t closure_stack_dropped, pending_dropped, deferred, state_repeats, exact_states, probe_ctx_sat, probe_rearms, bank_forks, bank_fork_sat, tas_ckpts, sess_ckpts, held_stops;   // static closure: roots walked, bytes newly marked, walks stopped by the cap
    uint8_t closure_regs;              // C3: the regstate-aware closure is on
    uint64_t closure_reg_roots;        // C3: regstate seed walks started from an executed step
    uint64_t closure_reg_seed_sat;     // C3: seeds refused past the per-site cap
    uint64_t closure_reg_set_sat;      // C3: seeds refused because the per-worker seed set is full
    uint64_t closure_reg_indirect;     // C3: indirect transfers resolved through a known register constant
    uint64_t closure_reg_indirect_sat; // C3: resolutions refused at an indirect site past the distinct-value cap
    uint64_t closure_reg_miss;         // C3: indirect transfers the walk left unresolved (register unknown)
    uint64_t closure_reg_stackrets;    // C3: returns followed through the machine's concrete stack (SP fully known)
    uint64_t cells;             // distinct (fork target, RAM cell) pairs queued (technique 8)
    uint64_t proj_pruned;       // C11: forks evicted because the same target was already queued under the same projected context (registers + RAM pages the section reads)
    uint64_t cell_pruned;       // forced forks dropped because their (target, RAM cell) pair was already queued
    uint64_t compressed_bytes;  // ROM bytes read as a sequential stream that fed unequal RAM writes (technique 6)
    uint64_t decompress_runs;   // such streams detected
    uint64_t resumed, saved; uint64_t spooled;          // forked states that overflowed the RAM queue onto disk (none is dropped)
    uint64_t spool_left;       // spilled states never run (the exploration ended first)
    unsigned halted;   /* 1: max-time, 2: stall-time, 3: spool error, 4: interruption */
    uint64_t dispatch_forks, attract_paths, play_paths, play_epochs, session_frames, session_steps, session_stalls;   // generic long sessions run
    uint64_t hook_vetoed;      // forced sides refused by DpPlatformHooks.veto_fork
    uint64_t exec_outside, exec_wram_rom;     // instructions executed where rom_index < 0 (RAM/VRAM code)
    uint64_t no_pc;            // 1 when the machine has no pc/set_pc: nothing explored
    char rom_title[64];        // the cartridge header's game title when the platform has one (shown in the map PNG); empty otherwise
} DpCoreResult;

// a section is a maximal run of ROM indexes covered by execution; its verdict
// is "required" when any byte of it ran on a natural path, "forced" otherwise
typedef struct DpCoreSection {
    size_t lo, hi;             // ROM index range, inclusive
    size_t natural, forced;    // bytes covered by each kind of path
    size_t live_pages;         // C11: distinct 256-byte RAM pages the section read on natural paths (its liveness)
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
uint64_t dp_mem_budget(void);   // bytes the explorer may still take (MemAvailable, lowered to the cgroup limit); 0 if unknown
// explores from the machine's current state; rom_size = number of ROM index slots
int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg, DpCoreResult *out);
// CLI wrapper: install a temporary SIGINT handler and provide a state directory
// for unbounded runs when none was specified.  output_hint may be NULL.
// process-wide default for DpCoreConfig.on_live, used when a run's config sets none (the CLI installs the --mapwin
// window here once, so every platform gets the live map without touching its own driver)
void dp_core_live_default(void (*cb)(void *, const struct DpCoreResult *), void *user);
int dp_core_explore_cli(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg,
                        DpCoreResult *out, const char *output_hint);
void dp_core_result_free(DpCoreResult *r);

void dp_core_write_cov(const char *path, const char *id, const struct DpMachine *m, const DpCoreResult *r);   // P10: the machine geometry decides the offsets
/* --cov-transfer ROM COV: replay another revision's executed coverage as speculative
 * seeds where the two images' four-byte instruction windows match exactly.  Returns
 * the seed count; the arrays are the kernel's cfg->seeds / seed_banks format. */
size_t dp_core_transfer_seeds(const uint8_t *rom, size_t rom_size,
                              const char *other_rom_path, const char *cov_path,
                              uint32_t **pcs, uint16_t **banks,
                              uint64_t *lines_out, uint64_t *matched_out);
// generic map/summary outputs (core_map.c): class vector, PNG-or-text map, JSON summary; 0 = ok, -1 = failure
uint8_t *dp_core_map_build(const DpCoreResult *r);
int dp_core_write_map(const char *path, const char *id, const char *rom_name, const DpCoreResult *r);
int dp_core_write_summary(const char *path, const char *id, const DpCoreResult *r);

#endif

// GB pointer-table entry discovery: seeds {bank, pc} for cfg->seeds / cfg->seed_banks (free both arrays)
size_t dp_core_gb_pointer_seeds(const uint8_t *rom, size_t size, size_t cap, uint32_t **pcs, uint16_t **banks,
                                uint64_t *refused);
