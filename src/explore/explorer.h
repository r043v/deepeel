// explorer.h: the forking exploration engine and everything it records.
//
// The engine pops machine states from a FIFO, runs them one instruction at a
// time and forks on every possibility: each side of a conditional branch, and
// each pending IRQ at points where interrupts are enabled.  Dedup evicts
// states that were already explored exactly, and section re-entries whose
// incoming context is indistinguishable for what the section reads ("même
// contexte").  Loops die on per-path step budgets, backward-edge caps and the
// global step wall; nothing can run away.
#ifndef MS_EXPLORER_H
#define MS_EXPLORER_H

#include "../core/machine.h"

// --- execution flags per ROM byte -------------------------------------------
enum {
    MS_XF_NATURAL = 1 << 0, // executed by natural flow (no forced fork)
    MS_XF_FORCED = 1 << 1,  // executed through a forked/speculative path
    MS_XF_ENTRY = 1 << 2,   // reached non-sequentially (jump/call/ret/entry)
    MS_XF_IRQ = 1 << 3,     // inside an IRQ handler execution
};
enum {
    MS_DF_READ = 1 << 0, // read as data through the ROM windows
    MS_DF_COPIED = 1 << 1, // source range of a copy into VRAM/OAM
};
enum {
    MS_SF_POINTER = 1 << 0, // static: part of a plausible pointer table
    MS_SF_PAD = 1 << 1,     // static: padding run (8+ identical 00/FF bytes)
};

// --- open-addressing u64 set -------------------------------------------------
typedef struct MsHashSet {
    uint64_t *keys;
    uint8_t *used;
    size_t cap, cnt, cap_abs;
} MsHashSet;

void ms_set_init(MsHashSet *s, size_t cap_abs);
void ms_set_free(MsHashSet *s);
// returns 1 if inserted, 0 if already present; returns -1 when full
int ms_set_insert(MsHashSet *s, uint64_t key);

// --- open-addressing u32 -> payload map --------------------------------------
typedef struct MsMap32 {
    uint32_t *keys;
    uint8_t *used;
    uint8_t *vals;
    size_t stride, cap, cnt, cap_abs;
} MsMap32;

void ms_map_init(MsMap32 *m, size_t stride, size_t cap_abs);
void ms_map_free(MsMap32 *m);
// returns payload slot (zeroed on first insert) or NULL when full
void *ms_map_get_or_add(MsMap32 *m, uint32_t key);
void *ms_map_find(MsMap32 *m, uint32_t key);

// --- io usage per registry slot ----------------------------------------------
typedef struct MsIoStat {
    uint32_t rd, wr;
    uint8_t values[8];
    uint8_t nvalues;
    uint16_t pcs[4]; // last write sites
    uint8_t npcs;
    uint32_t copies, bank_events;
} MsIoStat;

// --- per-branch-site outcome counts (completeness of both sides) -------------
typedef struct MsSiteStat {
    uint32_t taken, nottaken;
} MsSiteStat;

// --- caller lists per call target --------------------------------------------
typedef struct MsCallList {
    uint8_t n;
    uint32_t site[8]; // rom key of the call site
} MsCallList;

// --- per-section record -------------------------------------------------------
enum { MS_V_FORCED = 0, MS_V_REQUIRED = 1, MS_V_SPECULATIVE = 2 };

// liveness bits: which incoming state components the section actually reads
enum {
    MS_CTX_A = 0, MS_CTX_F, MS_CTX_B, MS_CTX_C, MS_CTX_D, MS_CTX_E,
    MS_CTX_H, MS_CTX_L, MS_CTX_SP, MS_CTX_IME, MS_CTX_IE,
    MS_CTX_ROMN, MS_CTX_RAMG, MS_CTX_VBK,
    MS_CTX_BITS
};

typedef struct MsSection {
    uint32_t id;
    uint8_t bank;
    uint16_t lo, hi; // inclusive, within the bank
    uint32_t hits;
    uint32_t entries;             // path entries into it
    uint32_t natural_hits, forced_hits;
    uint32_t boot_hits;           // hits from the reset flow specifically
    uint8_t kind_bits;            // MS_XF_* observed inside
    uint16_t io_used[16];
    uint8_t io_used_n;
    uint32_t banks_seen[8];       // ROMN windows while executing
    uint8_t banks_seen_n;
    uint64_t ctx_mask;            // liveness: MS_CTX_* bits
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
    uint8_t verdict;              // MS_V_*
    MsHashSet ctx_seen;           // projected contexts already explored
} MsSection;

// --- results ------------------------------------------------------------------
typedef struct MsResults {
    const MsCart *cart;
    const MsPlatform *plat;
    int banks;
    // per-ROM-byte maps, banks * 0x4000 entries
    uint8_t *exec_flags;
    uint32_t *exec_hits;
    uint8_t *data_flags;
    uint32_t *data_hits;
    uint8_t *static_flags;
    uint32_t *section_of; // rom key -> section id + 1
    // sections
    MsSection *sections;
    int section_count, section_cap;
    // registry-indexed io stats (registry order; IE included when present)
    MsIoStat *io;
    int io_count;
    // event records
    MsCopyRec *copies;
    int copy_count, copy_cap;
    MsBankEvent *bank_events;
    int bank_count, bank_cap;
    // maps
    MsMap32 sites;   // branch site key -> MsSiteStat
    MsMap32 callers; // call target key -> MsCallList
    MsMap32 fork_counts; // branch site key -> forks emitted there
    // diagnostics: undefined opcodes
    struct { uint32_t key; uint8_t op; } *bad_ops;
    int bad_op_count, bad_op_cap;
    // counters
    uint64_t steps_total, paths_run;
    uint32_t exec_outside; // instructions executed outside ROM (VRAM/RAM code)
    uint32_t forks_branch, forks_irq, evicted_exact, evicted_ctx;
    uint32_t forks_capped; // forks dropped by the per-site cap
    uint32_t cycle_cuts, step_cuts, budget_paths, budget_steps, queue_overflows;
    uint32_t seeds_natural, seeds_spec;
    uint64_t vram_write_pages[2], sram_write_pages; // page bitmaps
    uint8_t oam_written;
} MsResults;

// --- exploration config -------------------------------------------------------
typedef struct MsConfig {
    uint32_t max_steps_path;  // per-path step budget
    uint32_t max_paths;       // hard cap on paths run
    uint64_t max_steps_total; // hard cap on steps across all paths
    uint32_t queue_cap;       // max pending states
    uint32_t loop_back_cap;   // iterations of one backward edge before cut
    uint32_t fork_site_cap;   // max forks per branch site (loop bodies fork
                              // every iteration; the interesting variants are
                              // the first few, the rest is data drift)
    uint32_t seed_cap;        // max pointer-scan seeds
    uint32_t max_copies;      // max copy records kept
    int no_context_evict;     // disable the "same context" eviction
    int no_pointer_scan;      // disable pointer-table seeding
    int no_irq_fork;          // disable IRQ forking
    int seed_banks;           // seed every switchable bank at $4000
    int verbose;
} MsConfig;

void ms_config_defaults(MsConfig *cfg);

typedef struct MsExplorer MsExplorer;

// --- copy-loop tracker (copies.c) ---------------------------------------------
typedef struct MsCopyTracker {
    uint8_t src_valid, dst_valid;
    uint8_t src_bank, dst_bank;
    uint16_t src_lo, src_hi, dst_lo, dst_hi;
    uint16_t loop_pc;
    uint32_t loop_key; // rom index of the loop site (section attribution)
    uint32_t iter;
    uint8_t loop_active;
} MsCopyTracker;

// queue entry: a machine snapshot plus path bookkeeping
typedef struct MsPathState {
    MsMachine m;
    uint32_t remaining;
    uint8_t forced;      // reached through a fork or speculative seed
    uint8_t kind;        // MS_SEED_* or MS_PATH_FORK
    uint8_t from_reset;  // descends from the reset flow (necessity verdicts)
} MsPathState;

// the engine state is shared with the sibling modules (sections.c, copies.c)
struct MsExplorer {
    MsConfig cfg;
    MsResults *res;
    // worklist
    MsPathState *queue;
    uint32_t q_cap, q_head, q_len;
    MsHashSet exact;
    // per-path state
    MsMachine *cur;
    int cur_forced;
    uint8_t cur_kind, cur_irq, cur_from_reset;
    uint32_t cur_bankN;
    uint16_t last_pc;
    uint8_t last_len, last_bank, have_last;
    uint32_t cur_sec;
    MsCopyTracker ct;
    struct { uint16_t pc; uint32_t count; } back_ring[8];
    int back_idx;
    uint32_t steps_this_path;
    int last_io_slot; // io registry slot of the last register write (copy attribution)
    int stop; // set when a hard budget wall is hit
    MsTraceVt vt;
};

// --- section bookkeeping (sections.c) -------------------------------------------
MsSection *ms_section_touch(MsResults *res, uint8_t bank, uint16_t pc, uint16_t hi, int natural, int is_entry, uint32_t cur_sec);
void ms_section_live_reg(MsSection *s, int bit);
void ms_section_live_wram(MsSection *s, uint32_t page);
void ms_section_live_vram(MsSection *s, uint32_t bank, uint32_t page);
void ms_section_live_sram(MsSection *s, uint32_t page);
void ms_section_live_small(MsSection *s, int what); // 0 hram 1 oam 2 ie 3 romN 4 ramg 5 vbk
void ms_section_live_io(MsSection *s, uint16_t io_off);
void ms_section_io_used(MsSection *s, int slot);
void ms_section_banks_seen(MsSection *s, uint32_t bank);

void ms_results_init(MsResults *res, const MsCart *cart, const MsPlatform *plat);
void ms_results_free(MsResults *res);
// static byte classification (pointer tables, padding), independent of exploration
void ms_static_scan(MsResults *res);
// registry slot for an address (mapper windows and IO), or -1
int ms_io_slot(const MsResults *res, uint16_t addr);

void ms_copy_reset(MsCopyTracker *ct);
void ms_record_copy(MsExplorer *ex, const MsCopyRec *rec, uint32_t attrib_key);
void ms_copy_on_rom_read(MsCopyTracker *ct, uint8_t bank, uint16_t addr);
void ms_copy_on_vram_write(MsCopyTracker *ct, uint8_t bank, uint16_t addr);
void ms_copy_on_back_edge(MsCopyTracker *ct, uint16_t site, uint32_t site_key);
void ms_copy_flush_path(MsExplorer *ex, MsCopyTracker *ct);

// --- engine -------------------------------------------------------------------

// queue entry kind: MS_SEED_* from platform.h, plus internal forks
enum { MS_PATH_FORK = 4 };

MsExplorer *ms_explorer_new(const MsConfig *cfg, MsResults *res);
void ms_explorer_free(MsExplorer *ex);
// queue a starting state (pc set by caller on the machine); returns 0 if dropped
int ms_explorer_seed(MsExplorer *ex, const MsMachine *m, int kind, int forced);
// standard GB seeding: reset, non-padding RST/IRQ vectors, pointer scan
void ms_seed_gb(MsExplorer *ex);
// pop and run one path; returns 0 when the worklist is empty or a budget wall
int ms_explore_step(MsExplorer *ex);
// run paths for ~ms milliseconds (GUI slices); returns 0 when nothing left
int ms_explore_slice(MsExplorer *ex, int ms);
// branch-site completeness, callers, verdicts — call once exploration is done
void ms_explore_finalize(MsExplorer *ex);
// run the whole worklist to completion (slice loop + finalize)
int ms_explore(MsExplorer *ex);

// progress snapshot for the GUI / verbose prints
typedef struct MsProgress {
    uint32_t queue_len;
    uint64_t steps_total;
    uint32_t paths_run;
    uint32_t sections;
} MsProgress;
void ms_explorer_progress(MsExplorer *ex, MsProgress *out);

#endif
