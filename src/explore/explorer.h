// explorer.h: the forking exploration engine and everything it records.
//
// The engine pops machine state images from a FIFO, runs them on the m0boy
// machine (execution continues until a hook cuts the path) and forks on every
// possibility: each side of a conditional branch, and a no-IRQ continuation
// at points where interrupts are pending and enabled.  Dedup evicts states
// that were already explored exactly, and section re-entries whose incoming
// context is indistinguishable for what the section reads ("même contexte").
// Loops die on per-path step budgets, backward-edge caps and the global step
// wall; the halt watchdog cuts paths parked in a HALT nothing wakes.
#ifndef DP_EXPLORER_H
#define DP_EXPLORER_H

#include "../core/machine.h"
#include "../mach/m0boy.h"
#include "../platform/platform.h"

#include <pthread.h>
#include <stdio.h>

// --- execution flags per ROM byte -------------------------------------------
enum {
    DP_XF_NATURAL = 1 << 0, // executed by natural flow (no forced fork)
    DP_XF_FORCED = 1 << 1,  // executed through a forked/speculative path
    DP_XF_ENTRY = 1 << 2,   // reached non-sequentially (jump/call/ret/entry)
    DP_XF_IRQ = 1 << 3,     // inside an IRQ handler execution
    DP_XF_START = 1 << 4,   // opcode byte of an executed instruction (not an operand)
    DP_XF_STATIC = 1 << 5,  // reached by the static flow closure only (never executed)
};
enum {
    DP_DF_READ = 1 << 0, // read as data through the ROM windows
    DP_DF_COPIED = 1 << 1, // source range of a copy into VRAM/OAM
    DP_DF_VERBATIM = 1 << 2, // source range of a verbatim copy (byte for byte)
};
enum {
    DP_SF_POINTER = 1 << 0, // static: part of a plausible pointer table
    DP_SF_PAD = 1 << 1,     // static: padding run (8+ identical 00/FF bytes)
};

// --- open-addressing u64 set -------------------------------------------------
typedef struct DpHashSet {
    uint64_t *keys;
    uint8_t *used;
    size_t cap, cnt, cap_abs;
} DpHashSet;

void dp_set_init(DpHashSet *s, size_t cap_abs);
void dp_set_free(DpHashSet *s);
// returns 1 if inserted, 0 if already present; returns -1 when full
int dp_set_insert(DpHashSet *s, uint64_t key);

// --- open-addressing u32 -> payload map --------------------------------------
typedef struct DpMap32 {
    uint32_t *keys;
    uint8_t *used;
    uint8_t *vals;
    size_t stride, cap, cnt, cap_abs;
} DpMap32;

void dp_map_init(DpMap32 *m, size_t stride, size_t cap_abs);
void dp_map_free(DpMap32 *m);
// returns payload slot (zeroed on first insert) or NULL when full
void *dp_map_get_or_add(DpMap32 *m, uint32_t key);
void *dp_map_find(DpMap32 *m, uint32_t key);

// --- io usage per registry slot ----------------------------------------------
typedef struct DpIoStat {
    uint32_t rd, wr;
    uint8_t values[8];
    uint8_t nvalues;
    uint16_t pcs[4]; // last write sites
    uint8_t npcs;
    uint32_t copies, bank_events;
} DpIoStat;

// --- per-branch-site outcome counts (completeness of both sides) -------------
typedef struct DpSiteStat {
    uint32_t taken, nottaken;
} DpSiteStat;

// --- indirect jump sites (JP HL): the targets actually observed ----------------
typedef struct DpIndirect {
    uint8_t n;            // distinct targets kept (cap 8)
    uint8_t overflow;     // more distinct targets than kept
    uint16_t target[8];
    uint8_t tbank[8];
    uint32_t hits;
} DpIndirect;

// --- caller lists per call target --------------------------------------------
typedef struct DpCallList {
    uint8_t n;
    uint32_t site[8]; // rom key of the call site
} DpCallList;

// --- per-section record -------------------------------------------------------
enum { DP_V_FORCED = 0, DP_V_REQUIRED = 1, DP_V_SPECULATIVE = 2, DP_V_PLAYED = 3 };

// liveness bits: which incoming state components the section actually reads
enum {
    DP_CTX_A = 0, DP_CTX_F, DP_CTX_B, DP_CTX_C, DP_CTX_D, DP_CTX_E,
    DP_CTX_H, DP_CTX_L, DP_CTX_SP, DP_CTX_IME, DP_CTX_IE,
    DP_CTX_ROMN, DP_CTX_RAMG, DP_CTX_VBK,
    DP_CTX_BITS
};

typedef struct DpSection {
    uint32_t id;
    uint8_t bank;
    uint16_t lo, hi; // inclusive, within the bank
    uint32_t hits;
    uint32_t entries;             // path entries into it
    uint32_t natural_hits, forced_hits;
    uint32_t boot_hits;           // hits from the reset flow specifically
    uint8_t kind_bits;            // DP_XF_* observed inside
    uint16_t io_used[16];
    uint8_t io_used_n;
    uint32_t banks_seen[8];       // ROMN windows while executing
    uint8_t banks_seen_n;
    uint64_t ctx_mask;            // liveness: DP_CTX_* bits
    uint64_t page_mask_wram;      // liveness: pages read (bit = page)
    uint64_t page_mask_vram[2];
    uint64_t page_mask_sram;
    uint8_t ctx_hram, ctx_oam;
    uint32_t io_read_mask;        // bit i: FF00+i read inside
    uint32_t cond_sites, cond_both;
    uint32_t copies;
    uint32_t ctx_count;           // distinct incoming contexts explored
    uint32_t callers[8];          // rom keys of call sites into this section
    uint8_t callers_n;
    uint8_t verdict;              // DP_V_*
    DpHashSet ctx_seen;           // projected contexts already explored
} DpSection;

// --- results ------------------------------------------------------------------
// The per-byte maps and the record lists are shared by the forking explorer
// (main thread) and the play-session workers.  Flags and hits go through
// GCC/Clang __atomic builtins; the section records and the append-only record
// lists are guarded by res->sec_mtx / res->rec_mtx whenever res->mt is set
// (sessions running).  The fork explorer only runs while the sessions wait at
// the epoch barrier, so its own updates stay lock-free.
// One line of the discovery log: who found how much, and when.
typedef struct DpDiscEntry {
    double t;            // seconds since the first entry (wall clock)
    char source[48];     // "tas:<file>", "rng", "fork-explorer", "total-search", "closure-N"...
    uint64_t credit;     // discovery credit gained by that source in the step
    uint32_t known;      // ROM bytes carrying any evidence after the step
} DpDiscEntry;

typedef struct DpResults {
    const DpCart *cart;
    const DpPlatform *plat;
    int banks;
    int mt; // sessions running: take the mutexes
    DpDiscEntry *disc; // discovery log (appended from one thread at a time)
    int disc_n, disc_cap;
    double disc_t0;
    pthread_mutex_t sec_mtx, rec_mtx;
    // per-ROM-byte maps, banks * 0x4000 entries
    uint8_t *exec_flags;
    uint32_t *exec_hits;
    uint8_t *data_flags;
    uint32_t *data_hits;
    uint8_t *static_flags;
    uint32_t *section_of; // rom key -> section id + 1
    // sections
    DpSection *sections;
    int section_count, section_cap;
    // registry-indexed io stats (registry order; IE included when present)
    DpIoStat *io;
    int io_count;
    // event records
    DpCopyRec *copies;
    int copy_count, copy_cap;
    DpBankEvent *bank_events;
    int bank_count, bank_cap;
    // maps
    DpMap32 sites;   // branch site key -> DpSiteStat
    DpMap32 callers; // call target key -> DpCallList
    DpMap32 fork_counts; // branch site key -> forks emitted there
    DpMap32 indirect;    // JP HL site key -> DpIndirect (observed targets)
    // static flow closure (closure.c): code reached by following direct
    // control flow out of executed instructions, never executed itself
    uint32_t closure_rounds, closure_starts, closure_bytes;
    uint32_t closure_unresolved_bank;   // bank-0 edge into $4000+ with no bank evidence
    uint32_t closure_stopped_data;      // flow stopped by padding / proven data
    uint32_t closure_stopped_illegal;   // flow stopped by an undefined opcode
    uint32_t closure_seeds, closure_seeds_dropped;
    uint32_t closure_seeds_refused;     // seeds the full queue sent back (retried next round)
    uint32_t closure_bank_seeds;        // bank candidates seeded on ambiguous edges
    uint32_t *clo_entries;              // rom keys of static entry points never executed
    int clo_entry_n, clo_entry_cap;
    uint32_t *clo_bank_entries;         // rom keys: every bank's view of an unresolved edge target
    int clo_bank_n, clo_bank_cap;
    uint32_t indirect_seeds;            // JP HL observed targets handed to the explorer
    DpHashSet bad_seen;                 // undefined-opcode sites already recorded
    uint32_t forks_covered_pruned;      // forks dropped: both sides known, target covered
    // diagnostics: undefined opcodes (rom key + byte)
    struct { uint32_t key; uint8_t op; } *bad_ops;
    int bad_op_count, bad_op_cap;
    // counters (fork-explorer ones are main-thread only; steps_total and the
    // discovery counters move under every session worker)
    uint64_t steps_total, paths_run;
    uint32_t exec_outside; // instructions executed outside ROM (VRAM/RAM code)
    uint32_t forks_branch, forks_irq, forks_key, evicted_exact, evicted_ctx;
    uint32_t forks_capped; // forks dropped by the per-site cap
    uint32_t cycle_cuts, step_cuts, budget_paths, budget_steps, queue_overflows;
    uint32_t queue_growths;   // times the full worklist grew instead of dropping paths
    // disk spool (state-dir/spool.bin): states the RAM worklist could not hold
    // (queue full under the mem-cap/q_max) are written there instead of being
    // dropped, and fed back when the worklist runs low; a graceful stop flushes
    // the remaining worklist into it, --resume picks everything up
    uint32_t spool_writes, spool_reads, spool_refused; // refused = spool cap hit
    uint64_t spool_bytes;     // pending spooled bytes (written minus read back)
    uint32_t pad_cuts; // paths stopped for executing a padding run
    // nonsense evictions (the path proved senseless): echo/OAM execution,
    // execution inside verbatim-proven data, RET with a stack outside
    // WRAM/HRAM — the bytes stay out of the code map
    uint32_t nonsense_echo, nonsense_data, nonsense_stack;
    uint32_t halt_cuts; // paths parked in a HALT nothing wakes (watchdog)
    uint32_t seeds_natural, seeds_spec;
    uint32_t seeds_skipped_data; // pointer-scan targets inside proven data/static zones
    uint64_t vram_write_pages[2], sram_write_pages; // page bitmaps
    uint8_t oam_written;
    // data proven by exploration, for the m0boy bridge
    uint64_t data_verbatim_bytes; // bytes with DP_DF_VERBATIM
    uint32_t data_contradictions; // bytes both executed and proven data (kept code)
} DpResults;

// --- exploration config -------------------------------------------------------
typedef struct DpConfig {
    uint32_t max_steps_path;  // per-path step budget
    uint32_t max_paths;       // hard cap on paths run
    uint64_t max_steps_total; // hard cap on steps across all paths
    uint32_t queue_cap;       // max pending states
    uint32_t queue_max;       // adaptive growth cap of the worklist (0 = 4x queue_cap):
                              // a full queue grows by doubling up to this instead of
                              // dropping whole paths
    uint64_t mem_cap_mb;      // queue growth stops once RSS exceeds this MiB (0 = off;
                              // same wall as the sessions' --mem-cap)
    uint64_t spool_cap_mb;    // disk budget of the spool file (0 = off; default 4096)
    uint32_t loop_back_cap;   // iterations of one backward edge before cut
    uint32_t fork_site_cap;   // max forks per branch site (loop bodies fork
                              // every iteration; the interesting variants are
                              // the first few, the rest is data drift)
    uint32_t seed_cap;        // max pointer-scan seeds
    uint32_t max_copies;      // max copy records kept
    int no_context_evict;     // disable the "same context" eviction
    int no_pointer_scan;      // disable pointer-table seeding
    int no_irq_fork;          // disable IRQ forking
    int no_key_fork;          // disable the simulated joypad-IRQ key forks
    int no_closure;           // disable the static flow closure
    int closure_rounds;       // closure -> seed -> explore loops (default 4)
    int no_covered_prune;     // keep forking sites whose both sides are known
    uint32_t covered_fork_keep; // forks a fully-known site still gets (default 2)
    int seed_banks;           // seed every switchable bank at $4000
    int jobs;                 // parallel drain workers (1 = single core)
    int dfs;                  // depth-first frontier: newest path first (the
                              // total-search phase; default breadth-first)
    int verbose;
} DpConfig;

void dp_config_defaults(DpConfig *cfg);

// resident memory of this process, in MiB (0 when unreadable) — the shared
// memory-cap probe: long runs must not eat the machine's RAM and swap
uint64_t dp_rss_mb(void);

typedef struct DpExplorer DpExplorer;

// --- copy-loop tracker (copies.c) ---------------------------------------------
typedef struct DpCopyTracker {
    uint8_t src_valid, dst_valid;
    uint8_t src_bank, dst_bank;
    uint16_t src_lo, src_hi, dst_lo, dst_hi;
    uint16_t loop_pc;
    uint32_t loop_key; // rom index of the loop site (section attribution)
    uint32_t iter;
    uint8_t loop_active;
    // verbatim evidence: running hash of the bytes read from ROM and of the
    // bytes written to VRAM inside the accumulation window; a record is
    // verbatim when both counts and hashes match and the ranges have equal
    // length (an unrolled or transformed copy cannot match)
    uint64_t src_hash, dst_hash;
    uint32_t src_n, dst_n;
} DpCopyTracker;

// queue entry: a machine state image plus path bookkeeping
typedef struct DpPathState {
    uint8_t *img;        // gbl state image (dp_mach_state_bytes bytes)
    uint8_t forced;      // reached through a fork or speculative seed
    uint8_t kind;        // DP_SEED_* or DP_PATH_FORK
    uint8_t from_reset;  // descends from the reset flow (necessity verdicts)
    uint32_t choice;     // the tree choice this path answers
} DpPathState;

// disk spool of the overflow: appended at wr, fed back from rd, both cursors
// in one file so a stopped run resumes without a rewrite.  Shared (mutex, one
// struct) by the parallel workers: the spool doubles as the shared worklist.
typedef struct DpSpool {
    FILE *f;
    uint64_t rd, wr;
    pthread_mutex_t mtx;
} DpSpool;

// shared work pool of a parallel drain (--jobs N): ordinary pushes go here,
// pops come from here; the exact dedup and the spool are shared alongside
typedef struct DpSharedPool {
    pthread_mutex_t mtx;
    DpPathState *q;
    uint32_t cap, head, len, max;
} DpSharedPool;

// the engine state is shared with the sibling modules (sections.c, copies.c)
struct DpExplorer {
    DpConfig cfg;
    DpResults *res;
    DpMach *mach;      // the live machine (instance 0 during fork drains)
    uint32_t sb;       // state image size
    // worklist
    DpPathState *queue;
    uint32_t q_cap, q_head, q_len;
    uint32_t q_max;    // growth ceiling (resolved from cfg.queue_max)
    // priority inbox: closure / indirect / TAS-shoot seeds.  The main worklist
    // runs saturated in steady state (every popped path forks new ones), so
    // late seeds would starve there; these are served first, the slot cap
    // bounds them and refusals are counted.
    DpPathState *pqueue;
    uint32_t pq_cap, pq_head, pq_len;
    DpHashSet exact;
    pthread_mutex_t *exact_mtx; // guards the set when shared by parallel workers
    DpHashSet *exactp;         // &exact, or the leader's set when shared
    // disk spool of the overflow (see DpResults): shared with the parallel
    // workers, which makes it the shared worklist of a parallel drain
    DpSpool *spool;
    int spool_owned; // this explorer allocated the spool (frees it)
    // parallel drain (--jobs N): ordinary pushes/pops go through this pool
    DpSharedPool *sh;
    pthread_mutex_t tmtx;      // guards the tree when shared (parallel workers)
    pthread_mutex_t *tree_mtx; // -> own tmtx, or the leader's when shared
    // per-path state
    int cur_forced;
    uint8_t cur_kind, cur_from_reset;
    uint8_t cur_pad;           // the step runs inside a padding slide: mark nothing
    int pad_slide;              // consecutive padding ops on the current path
    uint32_t cur_bankN;
    uint16_t last_pc;
    uint8_t last_len, have_last;
    uint16_t last_bank;
    // seed construction: the reset image and the last bank window image
    uint8_t *seed_base, *seed_bank_img;
    int seed_bank;
    // branch-site outcome attribution: the previous step was a conditional
    uint8_t prev_was_cond;
    uint16_t prev_target, prev_site_pc;
    uint16_t prev_site_bank;
    uint32_t cur_sec;
    DpCopyTracker ct;
    struct { uint16_t pc; uint32_t count; } back_ring[8];
    int back_idx;
    uint32_t steps_this_path;
    uint64_t steps_own;  // fork-explorer steps (the wall); sessions share res->steps_total
    int last_io_slot; // io registry slot of the last register write (copy attribution)
    int stop; // set when a hard budget wall is hit
    // play-session mode: set by a session worker that owns this explorer
    // (one explorer per worker, sharing res); the fork explorer runs with it
    // cleared.  Sessions record naturally, never fork, and attribute every
    // first discovery to their credit counters below.
    int session;
    void *session_ctx;         // the owning worker (sessions.c)
    int irq_depth;             // natural IRQ handlers currently executing
    int reti_pending;          // a RETI executes this step: drop the depth after
    // mapper write seen on the previous step: resolve old->new banks now
    uint8_t pend_map;
    uint16_t pend_pc;
    uint16_t pend_addr; // the written address (mapper register)
    uint8_t pend_window;
    uint32_t pend_old;
    // discovery credit since the last reset (epoch bookkeeping)
    uint64_t credit_exec, credit_data, credit_copies, credit_io, credit_banks;
    // common tree: owned by the caller (NULL = no bookkeeping, e.g. sessions);
    // cur_choice is the choice of the path currently running
    struct DpTree *tree;
    uint32_t cur_choice;
    // per-path outcome for the tree: steps without any first discovery, and
    // the dead/reset/nonsense flags the step hook raises
    uint32_t steps_no_credit;
    uint64_t credit_shown;
    uint8_t path_dead, path_reset;
    uint8_t path_nonsense; // 0 none, else an DP_NS_* reason
};

// nonsense eviction reasons (the path proved senseless; the bytes it touched
// stay out of the code map's evidence)
enum { DP_NS_NONE = 0, DP_NS_ECHO, DP_NS_DATA, DP_NS_STACK };

// --- section bookkeeping (sections.c) -------------------------------------------
// creates or extends the section at (bank, pc) and updates its per-run
// counters; returns the section id (pointers would not survive the lock)
uint32_t dp_section_touch(DpResults *res, uint8_t bank, uint16_t pc, uint16_t hi, int natural, int is_entry,
                          uint32_t cur_sec, int from_reset, int irq);
void dp_section_live_reg(DpSection *s, int bit);
void dp_section_live_wram(DpSection *s, uint32_t page);
void dp_section_live_vram(DpSection *s, uint32_t bank, uint32_t page);
void dp_section_live_sram(DpSection *s, uint32_t page);
void dp_section_live_small(DpSection *s, int what); // 0 hram 1 oam 2 ie 3 romN 4 ramg 5 vbk
void dp_section_live_io(DpSection *s, uint16_t io_off);
void dp_section_io_used(DpSection *s, int slot);
void dp_section_banks_seen(DpSection *s, uint32_t bank);

// --- static flow closure (closure.c) ------------------------------------------
// Follows direct control flow (fall-through, JR/JP/CALL/RST targets, both sides
// of conditionals) out of every executed instruction and marks what it reaches
// DP_XF_STATIC.  Executed bytes are never touched; padding, pointer tables and
// proven data stop the flow.  Terminates by construction (a visited map the
// size of the ROM).  Returns the newly marked byte count.
uint32_t dp_static_closure(DpResults *res);
// seed the fork explorer at the closure's entry points that were never
// executed (jump/call/branch targets): the static flow proposes, the machine
// disposes (nonsense eviction, indirect targets).  Returns seeds queued; the
// seeds over cfg.seed_cap are counted in closure_seeds_dropped.
int dp_closure_seed(DpExplorer *ex);

void dp_results_init(DpResults *res, const DpCart *cart, const DpPlatform *plat);
void dp_results_free(DpResults *res);
// Append a discovery-log entry (known = bytes with any evidence right now).
void dp_disc_add(DpResults *res, const char *source, uint64_t credit);
// static byte classification (pointer tables, padding), independent of exploration
void dp_static_scan(DpResults *res);
// registry slot for an address (mapper windows and IO), or -1
int dp_io_slot(const DpResults *res, uint16_t addr);

void dp_copy_reset(DpCopyTracker *ct);
void dp_record_copy(DpExplorer *ex, const DpCopyRec *rec, uint32_t attrib_key);
void dp_copy_on_rom_read(DpCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v);
void dp_copy_on_vram_write(DpCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v);
void dp_copy_on_back_edge(DpCopyTracker *ct, uint16_t site, uint32_t site_key);
void dp_copy_flush_path(DpExplorer *ex, DpCopyTracker *ct);

// --- engine -------------------------------------------------------------------

// queue entry kind: DP_SEED_* from platform.h, plus internal forks
enum { DP_PATH_FORK = 4 };

DpExplorer *dp_explorer_new(const DpConfig *cfg, DpResults *res, DpMach *mach);
void dp_explorer_free(DpExplorer *ex);
// queue a state image (takes ownership of img); returns 0 if dropped
int dp_explorer_seed(DpExplorer *ex, uint8_t *img, int kind, int forced);
// same, into the priority inbox served before the main worklist
int dp_explorer_seed_pri(DpExplorer *ex, uint8_t *img, int kind, int forced, int pri);
// queue the reset state entered at pc (bank window applied for pc >= $4000);
// returns 0 if dropped
int dp_explorer_seed_pc(DpExplorer *ex, int bank, uint16_t pc, int kind, int forced);
// same, into the priority inbox
int dp_explorer_seed_pc_pri(DpExplorer *ex, int bank, uint16_t pc, int kind, int forced, int pri);
// standard GB seeding: reset, non-padding RST/IRQ vectors, pointer scan
void dp_seed_gb(DpExplorer *ex);
// seed the targets the JP HL sites actually took (bounded: sites x 8, seed cap);
// returns how many were queued
int dp_seed_indirect(DpExplorer *ex);
// the reset state image (built on first use; pool slot 0 starts from it)
uint8_t *dp_explorer_seed_base(DpExplorer *ex);
// pop and run one path; returns 0 when the worklist is empty or a budget wall
int dp_explore_step(DpExplorer *ex);
// run paths for ~ms milliseconds (GUI slices); returns 0 when nothing left
int dp_explore_slice(DpExplorer *ex, int ms);
// branch-site completeness, callers, verdicts — call once exploration is done
void dp_explore_finalize(DpExplorer *ex);
// run the whole worklist to completion (slice loop + finalize)
int dp_explore(DpExplorer *ex);
// disk spool: overflow states survive the run and any stop.  attach() binds
// <state-dir>/spool.bin (resume keeps the existing records), feed-back happens
// inside dp_explore_step; flush() pours the remaining worklist in before the
// process exits so --resume continues exactly where the run stopped.
int dp_explorer_spool_attach(DpExplorer *ex, const char *path, int resume);
// parallel drain: the worker borrows the leader's spool, exact dedup and tree
void dp_explorer_share(DpExplorer *worker, DpExplorer *leader);
// pours queue + priority inbox into the spool; returns states still pending
uint64_t dp_explorer_spool_flush(DpExplorer *ex);
uint64_t dp_explorer_spool_pending(const DpExplorer *ex);
// exact-state dedup persistence: without it a resumed run would re-explore
// every spooled state from scratch
int dp_explorer_exact_save(const DpExplorer *ex, const char *path);
int dp_explorer_exact_load(DpExplorer *ex, const char *path);
// bytes neither executed, nor read as data, nor statically classified
// (padding, pointer tables) — the honest "remaining" gap metric
enum { DP_CLS_EXEC_NATURAL, DP_CLS_EXEC_FORCED, DP_CLS_STATIC_FLOW, DP_CLS_DATA_COPIED,
       DP_CLS_DATA_READ, DP_CLS_POINTER_PAD, DP_CLS_UNEXPLAINED, DP_CLS_COUNT };
void dp_results_breakdown(const DpResults *res, uint64_t out[DP_CLS_COUNT]);
uint64_t dp_results_unexplained(const DpResults *res);

// progress snapshot for the GUI / verbose prints
typedef struct DpProgress {
    uint32_t queue_len;
    uint64_t steps_total;
    uint32_t paths_run;
    uint32_t sections;
} DpProgress;
void dp_explorer_progress(DpExplorer *ex, DpProgress *out);

#endif
