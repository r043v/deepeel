// results.h: the per-byte results model of a ROM (execution/data/static flags, sections,
// discovery log, configuration) shared by the report layer, the map renderer and the static scan.
// The exploration itself is the generic kernel (core.h).
#ifndef DP_RESULTS_H
#define DP_RESULTS_H

#include <signal.h>
#include "../core/machine.h"
#include "../mach/m0boy.h"
#include "../core/coverage.h"
#include "../platform/platform.h"
#include "frontier.h"

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
    DP_DF_STATIC = 1 << 3, // read by statically proven code (provenance pass)
    DP_DF_SIBLING = 1 << 4, // inferred: sibling entry of a verbatim copy's source table
    DP_DF_SAMPLE = 1 << 5, // read, then written to the APU wave RAM ($FF30-$FF3F)
    DP_DF_PALETTE = 1 << 7, // read, then written to BCPD/OCPD ($FF69 / $FF6B)
    DP_DF_MUSIC = 1 << 6, // read, then written to an APU register ($FF10-$FF25)
};
enum {
    DP_SF_POINTER = 1 << 0, // static: part of a plausible pointer table
    DP_SF_PAD = 1 << 1,     // static: padding run (8+ identical 00/FF bytes)
};

// legacy GB flags -> common nature (docs/COUVERTURE.fr.md annexe A)
static inline uint16_t dp_gb_nature(uint8_t x, uint8_t d, uint8_t s) {
    uint16_t n = 0;
    if (x & DP_XF_NATURAL) n |= DP_NAT_CODE_NAT;
    if (x & DP_XF_FORCED) n |= DP_NAT_CODE_FORCED;
    if (x & DP_XF_STATIC) n |= DP_NAT_CODE_STATIC;
    if (d & (DP_DF_COPIED | DP_DF_VERBATIM)) n |= DP_NAT_DATA_COPIED;
    if (d & DP_DF_STATIC) n |= DP_NAT_DATA_STATIC;
    if (d & DP_DF_READ) n |= DP_NAT_DATA_READ;
    if (d & DP_DF_SIBLING) n |= DP_NAT_DATA_INF;
    if (s & DP_SF_POINTER) n |= DP_NAT_TABLE;
    if (s & DP_SF_PAD) n |= DP_NAT_PAD;
    return n;
}

// --- open-addressing u64 set (shared implementation) ---------------------------
#include "../core/hashset.h"

// --- open-addressing u32 -> payload map --------------------------------------
typedef struct DpMap32 {
    uint32_t *keys;
    uint8_t *used;
    uint8_t *vals;
    size_t stride, cap, cnt, cap_abs;
    // parallel drain workers share the result maps: one lock per map, and the arrays a growth replaces are kept
    // until dp_map_free so a slot pointer another worker still holds never dangles (a count bumped on a retired
    // array after a growth is lost: bounded, rare)
    pthread_mutex_t mtx;
    void **retired;
    size_t nretired;
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
    uint32_t spec_hits;           // forced hits descended from a synthetic seed only
                                  // (pointer/closure/vector seeds, no real-flow ancestry)
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
    uint32_t frame;      // play lane frame when the credit was gained (0 = not a lane entry)
} DpDiscEntry;

// one distinct instruction executed from RAM (pc >= $8000), see rtcode.c
typedef struct DpRtCode {
    uint64_t key;     // 0 = empty slot
    uint32_t hits;
    int32_t rom_src;  // ROM index of the verbatim-copy source, -1 when unattached
    int32_t derived;  // unattached code: ROM index of the last ROM byte read before its RAM byte was written (approximate producer), -1 none
    uint16_t pc;
    uint8_t len, b[4];
    uint8_t natural;  // executed at least once on a non-forced path (no fork decision made it reachable)
} DpRtCode;

#define DP_IX_CAP 2048
// absolute caps of the record sets a long unbounded run could otherwise grow
// for days: the sets keep their semantics (first occurrence / curve shape),
// only the resolution stops, and every refused item is counted (never silent)
#define DP_BANK_SEEN_CAP_MAX (1u << 22) // distinct bank switches kept (32 MB of set)
#define DP_DISC_CAP_MAX (1u << 16)      // discovery-log entries before compaction
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
    // real usage: the same counts from played sessions only (TAS, random,
    // attract, human), never the fork explorer's forced paths; the heat map
    // and the hot-zone statistics read these
    uint32_t *play_exec_hits;
    uint32_t *play_data_hits;
    uint8_t *static_flags;
    uint8_t *map_sticky; // per ROM byte: the inferred map class once given (never taken back by a later pass)
    uint64_t defer_by[DP_R_COUNT];   // deferred per reason
    uint64_t defer_final[DP_R_COUNT]; // cut for good per reason (tier cap / holding area full)
    uint64_t seeds_deferred, seeds_resumed, seeds_lost, seeds_pending_disk, defer_on_disk, inbox_dropped;
    uint64_t steps_deferred, loop_deferred, defer_resumed, steps_final, loop_final, defer_dropped, defer_pending; // §6 frontier
    uint32_t *src_flags;  // DP_SRC_* per ROM byte, OR-ed (every proving source)
    DpFirstLog first;     // sparse first-discovery table (bounded)
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
    uint64_t bank_total;     // every bank switch seen; the timeline keeps the first of each distinct (pc, window, old, new)
    uint64_t *bank_seen;     // open-addressing set of those tuples (0 = empty slot)
    uint32_t bank_seen_cap, bank_seen_n;
    // maps
    DpMap32 sites;   // branch site key -> DpSiteStat
    DpMap32 callers; // call target key -> DpCallList
    DpMap32 fork_counts; // branch site key -> forks emitted there
    DpMap32 indirect;    // JP HL site key -> DpIndirect (observed targets)
    // static flow closure (closure.c): code reached by following direct
    // control flow out of executed instructions, never executed itself
    uint32_t closure_rounds, closure_starts, closure_bytes;
    uint32_t closure_unresolved_bank;   // bank-0 edge into $4000+ with no bank evidence
    uint32_t closure_jp_tables, closure_jp_entries; // S4: operand tables of executed jp hl
    uint32_t closure_stopped_data;      // flow stopped by padding / proven data
    uint32_t closure_stopped_illegal;   // flow stopped by an undefined opcode
    uint32_t closure_seeds, closure_seeds_dropped;
    uint32_t closure_seeds_refused;     // seeds the full queue sent back (retried next round)
    uint32_t closure_bank_seeds;        // bank candidates seeded on ambiguous edges
    uint32_t residual_targets, residual_seeded;     // residual ranges tried, forced entries seeded
    uint32_t residual_static, residual_refused;     // ranges re-proven statically, refusals (pointer, no entry)
    DpHashSet residual_tried;           // rom keys of ranges already targeted (never retried)
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
    uint32_t exec_wram_rom; // WRAM-executed instructions traced to the ROM bytes a verbatim copy loop put there
    uint32_t exec_outside; // instructions executed outside ROM (VRAM/RAM code)
    uint64_t hits_saturated; // per-byte hit counters held at UINT32_MAX (never wrap)
    uint32_t forks_branch, forks_irq, forks_key, forks_p1, evicted_exact, evicted_ctx;
    uint32_t ctx_set_full;   // paths run: the section context set was at its cap (kept, not evicted)
    uint32_t bank_seen_full; // bank switches not kept in the timeline: the distinct-switch set is full
    uint32_t disc_dropped;   // discovery-log entries folded away by compaction
    uint32_t decoders, decoders_without_snapshot, decoder_overflow, start_overflow, frame_overflow; // S6 stream consumers
    uint32_t replay_tables, replay_candidates, replay_runs, sat_replay_total; // S6 replays
    struct { uint32_t at; uint16_t stride, count; } tb[128]; uint32_t tb_n;   // S6 pointer tables found (dpTb)
    // dpIx: ROM data-read sites (lock-free open-addressed table): site = rom index of the reading pc,
    // [kmin,kmax] = rom index range read, n = reads. Observation only: no base/stride is inferred.
    struct { uint32_t site1, kmin, kmax, n, last, g; } ix[DP_IX_CAP]; // g = gcd of successive read-index deltas (0 = none yet)
     uint32_t ix_full; // site1 = site + 1 (0 = free slot)
    uint32_t forks_capped; // forks dropped by the per-site cap
    uint32_t forks_table_full; // forks refused: the per-site counter table is full (also in forks_capped)
    uint32_t cycle_cuts, step_cuts, budget_paths, budget_steps, queue_overflows;
    uint32_t queue_growths;   // times the full worklist grew instead of dropping paths
    uint32_t copy_window_breaks; // copy windows emitted when a touch left them (not at path end)
    uint32_t copies_dropped;  // copy records refused by the max_copies cap (saturation)
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
    uint32_t nocredit_cuts; // paths ended by --nocredit-cut (nothing new learned for N steps)
    uint32_t seeds_natural, seeds_spec;
    uint32_t seeds_dedup_bypass; // seeds deposited despite the saturated dedup set
    uint32_t logo_data;         // $0104-$0133 proven data (canonical logo present)
    uint32_t seeds_skipped_data; // pointer-scan targets inside proven data/static zones
    uint64_t vram_write_pages[2], sram_write_pages; // page bitmaps
    uint8_t oam_written;
    // data proven by exploration, for the m0boy bridge
    uint32_t serial_starts, rtc_selects; // distinct code sites: $FF02 bit-7 starts, MBC3 RAM-bank selects of 8-$C (RTC)
    uint32_t div_reads, stat_reads, ly_reads; // distinct code sites reading $FF04 / $FF41 / $FF44 (timing probes)
    uint16_t probe_pc[5][16]; // site tables behind the two counters above (cap 16 each)
    uint64_t palette_bytes; // bytes with DP_DF_PALETTE
    uint64_t apu_sample_bytes, apu_music_bytes; // bytes with DP_DF_SAMPLE / DP_DF_MUSIC
    uint64_t data_verbatim_bytes; // bytes with DP_DF_VERBATIM
    uint32_t seeds_refused_budget; // seeds dropped: the phase's cascade solde was spent
    uint32_t sib_tables, sib_ranges, sib_bytes, sib_refused, sib_caps; // copy-sibling inference
    uint32_t prov_jp_hl, prov_data;  // provenance: jp-hl targets proven, bytes proven data
    uint32_t prov_table_targets;     // indexed-dispatch table entries proven code (tool inference)
    uint32_t prov_unresolved, prov_sat, prov_steps; // partial proofs, caps hit, walk steps
    uint32_t sgb_packets, sgb_cmd[32]; // SGB packets decoded from $FF00 writes, per command (code = byte0 >> 3)
    uint32_t forks_event;         // read-value probes on LY / STAT / DIV (3.7)
    uint32_t data_contradictions; // bytes both executed and proven data (kept code)
    DpRtCode *rt;                 // runtime-code archive (open addressing), guarded by rec_mtx
    uint8_t *rt_pcseen;           // variants seen per pc (saturating), for the polymorphic count
    uint32_t smc_writes, smc_first_pc; // writes into WRAM bytes already executed (self-modifying code)
    uint32_t rt_poly;             // variants that replace an earlier one at the same pc (SMC)
    uint32_t rt_natural;          // generated variants executed at least once on a non-forced path
    uint32_t rt_derived;          // generated variants with an approximate ROM producer
    uint32_t rt_cap, rt_n, rt_attached; // table size, distinct (pc,bytes) variants, those with a ROM source
} DpResults;

// --- exploration config -------------------------------------------------------
typedef struct DpConfig {
    uint32_t max_steps_path;  // per-path step budget
    uint32_t nocredit_cut;    // a path that learned nothing new (no code, data, copy, I/O, bank) for this many steps ends; 0 = off
    uint32_t max_paths;       // hard cap on paths run
    uint64_t max_steps_total; // hard cap on steps across all paths
    uint32_t queue_cap;       // max pending states
    uint32_t queue_mem_mb;    // RAM budget of the worklist images (0 = 1536): beyond it states go to the disk spool
    uint32_t queue_max;       // adaptive growth cap of the worklist (0 = 4x queue_cap):
                              // a full queue grows by doubling up to this instead of
                              // dropping whole paths
    uint64_t mem_cap_mb;      // queue growth stops once RSS exceeds this MiB (0 = off;
                              // same wall as the sessions' --mem-cap)
    uint64_t frontier_cap_mb; // disk budget of the frontier lots (0 = unbounded)
    uint64_t spool_cap_bytes; // finer disk budget (tests); 0 = off
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
    int no_p1_fork;           // disable the P1-polling read forks
    int no_event_fork;        // disable the LY/STAT/DIV read-value probes (docs/COUVERTURE.fr.md 3.7)
    int no_closure;           // disable the static flow closure
    int no_provenance;        // disable the static HL/A provenance pass
    int no_siblings;          // disable the copy-sibling inference
    int closure_rounds;       // closure -> seed -> explore loops (default 4)
    int unbounded;            // coverage over resources: no fork/queue/tier/disk cap, overflow goes to the disk spool
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
void dp_results_init(DpResults *res, const DpCart *cart, const DpPlatform *plat);
void dp_results_free(DpResults *res);
// Append a discovery-log entry (known = bytes with any evidence right now).
void dp_disc_add(DpResults *res, const char *source, uint64_t credit);
// static byte classification (pointer tables, padding), independent of exploration
void dp_static_scan(DpResults *res);
// registry slot for an address (mapper windows and IO), or -1
int dp_io_slot(const DpResults *res, uint16_t addr);
int is_padding_at(const DpCart *c, uint32_t bank, uint16_t addr);

int dp_results_cov_save(const DpResults *res, const char *path);   // coverage flags next to the spool
long dp_results_cov_load(DpResults *res, const char *path);        // OR them back on --resume (-1: none)
int dp_results_disc_save(const DpResults *res, const char *path); // discovery log, same purpose
long dp_results_disc_load(DpResults *res, const char *path);       // prepended on --resume (-1: none)
// bytes neither executed, nor read as data, nor statically classified
// (padding, pointer tables) — the honest "remaining" gap metric
enum { DP_CLS_EXEC_NATURAL, DP_CLS_EXEC_FORCED, DP_CLS_STATIC_FLOW, DP_CLS_DATA_COPIED,
       DP_CLS_DATA_READ, DP_CLS_POINTER_PAD, DP_CLS_UNEXPLAINED, DP_CLS_COUNT };
void dp_results_breakdown(const DpResults *res, uint64_t out[DP_CLS_COUNT]);

#endif
