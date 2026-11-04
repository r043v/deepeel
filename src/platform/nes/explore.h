// explore.h: the bounded NES explorer contract, mirroring the GB/Z80
// explorer records (sections with necessity verdicts, copy records, bank
// timeline, guard saturations reported, never silent).  The machine is
// m0boy's NES runtime (src/mach/nesmach.h); this header carries only the
// explorer's own records.
#ifndef DP_NES_EXPLORE_H
#define DP_NES_EXPLORE_H
#include "../../core/hashset.h"
#include "../../explore/ramarch.h"
#include <stdint.h>
#include <stdio.h>

enum { DP_NES_NATURAL = 1, DP_NES_FORCED = 2, DP_NES_DATA = 4, DP_NES_COPIED = 8,
       DP_NES_IRQ = 16, DP_NES_PAD = 32,
       DP_NES_SAMPLE = 64, // read by the DMC channel (delta-modulation sample DMA)
       DP_NES_STATIC = 128 }; // reached by the static flow closure, never executed

enum { DP_NES_X_ENTRY = 1, DP_NES_X_IRQ = 2 };

// map classes mirror the GB names so maprender renders nes maps unchanged
enum {
    DP_NMAP_UNKNOWN, DP_NMAP_CODE_NAT, DP_NMAP_CODE_FORCED, DP_NMAP_CODE_STATIC,
    DP_NMAP_DATA_COPIED, DP_NMAP_DATA_READ, DP_NMAP_DATA_STATIC, DP_NMAP_TABLE,
    DP_NMAP_PAD, DP_NMAP_INF_DATA, DP_NMAP_INF_CODE, DP_NMAP_INF_PAD, DP_NMAP_COUNT
};
static const char *const dp_nmap_names[DP_NMAP_COUNT] = { "unknown", "code-natural",
    "code-forced", "code-static", "data-copied", "data-read", "data-static",
    "table", "padding", "inferred-data", "inferred-code", "inferred-padding" };

static inline int dp_nes_io_slot(unsigned addr) {
    if (addr >= 0x2000 && addr < 0x4000) return (int)(addr & 7);
    if (addr >= 0x4000 && addr < 0x4020) return 8 + (int)(addr & 0x1F);
    return -1;
}

enum { DP_NES_COPY_CAP = 4096 };
enum { DP_NES_BANK_LOG_CAP = 1024 };
enum { DP_NES_IRQ_LOG_CAP = 1024 };
enum { DP_NES_VERDICT_NONE = 0, DP_NES_VERDICT_REQUIRED, DP_NES_VERDICT_FORCED };
static const char *const dp_nes_verdict_names[] = { "none", "required", "forced" };

typedef struct {
    uint64_t path, step;
    int64_t target_rom;
    uint16_t pc;
    uint8_t kind; // 0 NMI, 1 IRQ
} DpNesInterrupt;

typedef struct {
    uint64_t path, step;
    uint16_t pc, addr;
    uint8_t reg, value;
} DpNesRegisterWrite;

typedef struct {
    uint64_t path, first_step, last_step, occurrences;
    uint32_t source, destination; // physical PRG/RAM cells; source is the page base
    uint16_t pc;
    uint32_t length;   // bytes covered by a coalesced CPU loop copy (channel 3)
    uint8_t channel;   // 1 = OAM DMA ($4014), 2 = DMC, 3 = CPU loop to PPUDATA ($2007)
    uint8_t verbatim;
} DpNesCopy;

typedef struct {
    uint64_t seq, path, step;
    uint16_t pc;
    uint8_t window;    // 0: $8000-$BFFF, 1: $C000-$FFFF
    uint8_t old_bank, new_bank;
} DpNesBankEvent;

// choice tree (F2): one node per queued state, parented on the path that queued it
enum { DP_NES_TREE_RESET = 1, DP_NES_TREE_ENTRY, DP_NES_TREE_VECTOR, DP_NES_TREE_BRANCH,
       DP_NES_TREE_IRQ, DP_NES_TREE_PROBE, DP_NES_TREE_PASS2 };
enum { DP_NES_ISSUE_NONE = 0, DP_NES_ISSUE_DONE, DP_NES_ISSUE_EVICTED, DP_NES_ISSUE_SPOOLED, DP_NES_ISSUE_BUDGET };
typedef struct {
    uint32_t parent;   // 0 for roots
    uint32_t site;     // PRG cell of the entry pc (0xFFFFFFFF outside the ROM)
    uint16_t pc;
    uint8_t kind, issue;
} DpNesTreeNode;

typedef struct {
    uint32_t id;
    uint8_t bank;          // physical 16 KiB bank
    uint16_t lo, hi;       // bank offsets, inclusive
    uint64_t hits, natural_hits, forced_hits, boot_hits, entries;
    uint64_t copies;
    uint8_t kind_bits;     // DP_NES_NATURAL | DP_NES_FORCED | DP_NES_IRQ
    uint8_t verdict;       // DP_NES_VERDICT_*
} DpNesSection;

struct DpNesResults_;
typedef struct {
    uint64_t max_total;
    uint32_t max_steps, max_paths, queue_cap, queue_max, loop_cap, fork_cap;   // queue_max: adaptive growth ceiling (0: fixed queue_cap)
    int seed_irq;          // fork the NMI/IRQ entries when enabled (default on)
    // play sessions: random-input frames before the fork exploration.  The
    // visited bytes land in the same physical map as the exploration.
    uint32_t play_frames;  // frames per session (0: no play pass)
    uint32_t demo_frames;  // idle-pad run from power-on before the TAS and play passes (0: none)
    uint32_t play_sessions; // restart count
    uint32_t play_seed;    // RNG seed
    uint32_t play_pool;    // shared checkpoint pool size: random sessions restart from earlier sessions' checkpoints (0: from power-on)
    // TAS movies (fm2/bk2), played from power-on before the fork drain;
    // each movie is one session, its end state seeds the fork queue
    char tas_paths[8][512];
    int tas_count;
    uint8_t entry_bank[32]; uint16_t entry_addr[32]; int entry_count; // --entry bank:addr seeds
    int no_pad_probe;      // disable the strobe-site pad probes
    int pad_full;          // D10: also probe every one of the 256 port-1 button masks at each strobe site
    int bank_dispatch;     // second pass also scans the fixed bank for jump tables and seeds every bank view (G2, UxROM)
    unsigned ctx_cap;      // max admitted states per coarse context (0 = off, C2)
    int second_pass;       // after the drain, enter static-closure roots that no path executed and drain again (G4)
    int seed_vectors;      // also seed the NMI and IRQ/BRK vector targets as roots (G1)
    int seed_banks;        // also seed every switchable bank at $8000 (UxROM)
    int lot_drain;         // with a lot backlog: run only the saved states (no root, no seeds)
    const char *tree_path; // write the choice tree here (binary DPTZ records), NULL: none
    const char *lot_path;  // disk lot of deferred states (NULL: memory only); an existing file is resumed
    // --jobs 2 partition (set by dp_nes_explore_jobs): the secondary instance skips the
    // power-on root, the entries and the finalization; the primary calls
    // pre_finalize(results, user) once its drain is done, before the sections, to fold
    // the secondary's findings in
    // novelty search (Go-Explore): archive of screen cells, random trials from the least
    // played ones; the freshest cells seed the fork queue
    uint64_t novelty_steps;     // instruction budget of the search (0: off)
    uint32_t novelty_frames;    // frames per trial
    uint32_t novelty_archive;   // archive capacity
    int novelty_cells;          // key the archive on the screen fingerprint and lives
    const char *export_state;   // write the deepest novelty cell here (DPST file, needs --novelty)
    unsigned watch_every;       // publish a frame to the watch buffer every N frames (0: off)
    int skip_root;
    int no_finalize;
    void (*pre_finalize)(struct DpNesResults_ *r, void *user);
    void *pre_user;
} DpNesOptions;

enum { DP_NES_CHR_BG = 1, DP_NES_CHR_SPR = 2, DP_NES_CHR_READ = 4 };

typedef struct DpNesResults_ {
    uint8_t *inferred;         // per PRG cell: DP_NMAP_INF_* for unclassified gaps, 0 otherwise
    uint8_t *chr_flags;        // one byte per CHR ROM cell (DP_NES_CHR_*), NULL on CHR-RAM
    size_t chr_size;
    uint64_t chr_tiles_bg, chr_tiles_spr, chr_reads; // distinct 16-byte tiles / $2007 reads
    uint32_t *exec_hits, *data_hits; // per PRG cell: executions of the opcode byte / data reads (saturating)
    DpNesTreeNode *tree_nodes; uint32_t tree_count, tree_capacity;
    uint8_t *flags;            // one byte per physical PRG cell
    uint8_t *exec_kind;        // per PRG cell, opcode byte: DP_NES_X_ENTRY (non-sequential arrival) | DP_NES_X_IRQ (ran inside a handler)
    size_t prg_size;
    uint64_t s6_decoders, s6_tables, s6_candidates, s6_runs, s6_sat;   // S6 decoder replay
    uint64_t pool_saved, pool_starts;   // J1 checkpoint pool: checkpoints kept / sessions started from one
    uint64_t steps, paths, forks, irq_forks, exec_outside, data_reads, oam_copies;
    uint64_t dmc_starts, dmc_bytes; // $4015 bit 4 starts (copy channel 2) / distinct sample bytes
    uint64_t forced_paths;
    uint64_t queue_grown;
    uint64_t step_cuts, loop_cuts, fork_cuts, queue_cuts, global_cuts, path_cuts,
             faults, decode_cuts;
    uint64_t ctx_evicted, dispatch_tables, dispatch_seeds, dispatch_refused_view, prov_sites, prov_roots, pass2_seeds, pass2_refused, pass2_capped, sib_tables, sib_ranges, sib_bytes, sib_refused, sib_caps, vectors_seeded, tas_same, tas_different, tas_unrecorded, pad_cuts, table_bytes, table_count, static_bytes, static_roots, static_cap_hits, padding_bytes, inferred_code, inferred_data, inferred_pad, cpu_copy_bytes, indirect_sites, indirect_targets, indirect_overflow, pad_probes, pad_probe_sites, pad_probe_refused, entries_seeded, entries_refused, seeds_reset, seeds_irq, seeds_bank, seeds_refused_queue;
    uint64_t demo_frames_run, play_frames_run, play_sessions_run, play_steps_accounted, seeds_play;
    uint64_t evicted;          // exact-state dedup admissions refused
    uint64_t lot_written, lot_read, lot_pending; // disk lot (--lot): spilled / read back / left on disk
    uint64_t parked, parked_resumed, parked_lost; // queue-full states held aside (§6), resumed, lost at the holding cap
    uint64_t pending_dropped;  // queued states left undrained at a wall
    uint64_t ram_writes, io_writes, io_reads;
    uint64_t smc_writes; uint16_t smc_first_pc; // writes into already executed RAM bytes
    // per-register access counts: slots 0-7 PPU $2000-$2007 (mirrors folded), 8-39 $4000-$401F
    uint64_t io_reg_writes[40], io_reg_reads[40];
    // sections
    DpNesSection *sections;
    uint32_t section_count, section_capacity;
    uint32_t *section_of;      // physical cell -> section id + 1 (0: none)
    // hardware logs
    DpNesCopy copies[DP_NES_COPY_CAP];
    unsigned copy_count;
    uint64_t copy_overflow;
    DpNesBankEvent banks[DP_NES_BANK_LOG_CAP];
    unsigned bank_count;
    uint64_t bank_overflow;
    DpNesInterrupt interrupts[DP_NES_IRQ_LOG_CAP];
    unsigned interrupt_count;
    uint64_t interrupt_overflow;
    DpNesRegisterWrite register_writes[256];
    unsigned register_count;
    uint64_t register_overflow;
    uint64_t irq_counts[2];    // NMI, IRQ dispatched
    uint64_t novelty_trials, novelty_hits, novelty_archived, novelty_replaced, novelty_frames_run, novelty_deaths;
    uint32_t novelty_depth;
    uint64_t watch_frames;
    uint32_t state_exported_depth; int state_exported;
    unsigned jobs_folded;      // --jobs 2: secondary instances folded into this report
    DpNesOptions options;
    DpRamArch ra;          // code executed from RAM (archive)
} DpNesResults;

DpNesOptions dp_nes_options(void);
int dp_nes_explore(const uint8_t *data, size_t size, int mapper,
                   const DpNesOptions *options, DpNesResults *results);
void dp_nes_rtcode(FILE *out, const DpNesResults *r);
void dp_nes_results_free(DpNesResults *results);
// --jobs 2: TAS/play phases on a second instance (own machine), see jobs.c
int dp_nes_explore_jobs(const uint8_t *data, size_t size, int mapper, const DpNesOptions *options,
                        DpNesResults *results, int jobs);
uint8_t *dp_nes_map_build(const DpNesResults *r);
void dp_nes_report(FILE *out, const DpNesResults *results);
void dp_nes_map(FILE *out, const DpNesResults *results);
// coverage (.cov: "bank offset flags hits") and the 256-byte-page data-read profile (romdata)
void dp_nes_cov(FILE *out, const DpNesResults *results);
void dp_nes_romdata(FILE *out, const DpNesResults *results);
// per-run evidence ranges, "bank start end mask" in 16 KiB PRG banks (mask = the cell flags)
void dp_nes_evidence(FILE *out, const DpNesResults *results);

// the hardware registry (src/platform/nes/registry.c)
struct DpPlatform;
const struct DpPlatform *dp_nes_platform(void);
void dp_nes_regs(FILE *out);
#endif
