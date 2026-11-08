// explore.h: the bounded PC Engine explorer contract, mirroring the NES and
// GB records: per-cell flags on the physical HuCard image, sections with
// necessity verdicts, copy records (ROM -> VDC), MPR timeline, interrupt
// and register logs, every guard saturation counted.
#ifndef DP_PCE_EXPLORE_H
#define DP_PCE_EXPLORE_H
#include "../../explore/ramarch.h"
#include <stdint.h>
#include <stdio.h>

enum { DP_PCE_NATURAL = 1, DP_PCE_FORCED = 2, DP_PCE_DATA = 4, DP_PCE_COPIED = 8,
       DP_PCE_IRQ = 16, DP_PCE_PAD = 32, DP_PCE_STATIC = 64, DP_PCE_TABLE = 128 };

enum { DP_PCE_X_ENTRY = 1, DP_PCE_X_IRQ = 2 };

// map classes: the GB names, so maprender renders pce maps unchanged
enum {
    DP_PMAP_UNKNOWN, DP_PMAP_CODE_NAT, DP_PMAP_CODE_FORCED, DP_PMAP_CODE_STATIC,
    DP_PMAP_DATA_COPIED, DP_PMAP_DATA_READ, DP_PMAP_DATA_STATIC, DP_PMAP_TABLE,
    DP_PMAP_PAD, DP_PMAP_INF_DATA, DP_PMAP_INF_CODE, DP_PMAP_INF_PAD, DP_PMAP_COUNT
};
static const char *const dp_pmap_names[DP_PMAP_COUNT] = { "unknown", "code-natural",
    "code-forced", "code-static", "data-copied", "data-read", "data-static",
    "table", "padding", "inferred-data", "inferred-code", "inferred-padding" };

enum { DP_PCE_COPY_CAP = 4096, DP_PCE_BANK_LOG_CAP = 1024, DP_PCE_IRQ_LOG_CAP = 1024,
       DP_PCE_REG_LOG_CAP = 256 };
// hardware-page register slot: the registry offsets one by one, 27 = anything else
enum { DP_PCE_IO_SLOTS = 28 };
static inline int dp_pce_io_slot(unsigned reg) {
    reg &= 0x1FFFu;
    if (reg < 4) return (int)reg;
    if (reg >= 0x400 && reg < 0x408) return 4 + (int)(reg - 0x400);
    if (reg >= 0x800 && reg < 0x80A) return 12 + (int)(reg - 0x800);
    if (reg == 0xC00 || reg == 0xC01) return 22 + (int)(reg - 0xC00);
    if (reg == 0x1000) return 24;
    if (reg == 0x1402) return 25;
    if (reg == 0x1403) return 26;
    return 27;
}
static inline unsigned dp_pce_io_slot_reg(int k) {
    static const unsigned other = 0xFFFF;
    if (k < 4) return (unsigned)k;
    if (k < 12) return 0x400u + (unsigned)(k - 4);
    if (k < 22) return 0x800u + (unsigned)(k - 12);
    if (k < 24) return 0xC00u + (unsigned)(k - 22);
    if (k == 24) return 0x1000u;
    if (k == 25) return 0x1402u;
    if (k == 26) return 0x1403u;
    return other;
}

enum { DP_PCE_VERDICT_NONE = 0, DP_PCE_VERDICT_REQUIRED, DP_PCE_VERDICT_FORCED, DP_PCE_VERDICT_SPECULATIVE };
static const char *const dp_pce_verdict_names[] = { "none", "required", "forced", "speculative" };

typedef struct {
    uint64_t path, step;
    uint16_t pc;
    uint8_t kind;          // 0 IRQ2/BRK, 1 IRQ1 (VDC), 2 timer
} DpPceInterrupt;

typedef struct {
    uint64_t path, step;
    uint16_t pc, reg;
    uint8_t value;
} DpPceRegisterWrite;

typedef struct {
    uint64_t path, first_step, occurrences;
    uint32_t source;       // first physical ROM cell of the run
    uint32_t length;       // bytes copied in the run
    uint16_t pc;
    uint8_t channel;       // 1 CPU store to VDC data, 2 block move to VDC data,
                           // 3 CPU store to VCE colour data, 4 block move to it (palette)
} DpPceCopy;

typedef struct {
    uint64_t seq, path, step;
    uint16_t pc;
    uint8_t window;        // MPR index
    uint8_t old_page, new_page;
} DpPceBankEvent;

typedef struct {
    uint32_t id;
    uint32_t lo, hi;       // physical cells, inclusive, within one 8 KiB page
    uint64_t natural_hits, forced_hits;
    uint8_t io_reads;      // C1 liveness: hardware blocks the section's code read, bit = (register >> 10): VDC VCE PSG timer pad IRQ
    uint8_t verdict;
} DpPceSection;

struct DpPceResults_;
typedef struct {
    uint64_t max_total;
    uint32_t max_steps, queue_cap, queue_max, loop_cap, fork_cap, pad_fork_cap, irq_fork_cap;
    unsigned defer_tiers;   // frontier §6 (shared policy): cut continuations resume with a 4x budget, N tiers (0: cuts stay final)
    int seed_vectors;      // also seed the IRQ2/IRQ1/TIMER/NMI vector targets as roots (G1)
    int seed_banks;        // also seed every 8 KiB ROM page in the $4000 window as a root (G3)
    int seed_irq;          // IRQ forks (VDC vblank, timer) at their enabling points
    int bank_dispatch;     // seed the targets of dispatch tables (implies a second drain)
    int second_pass;       // after the drain, seed the unexecuted static successors of executed code
    int static_closure;    // static decode from observed code (default on)
    int infer;             // padding and gap inference (default on)
    uint32_t play_frames, play_sessions, play_seed, demo_frames, play_pool;
    // novelty search (Go-Explore): restore archived checkpoints that found
    // new cells and play on with random held inputs
    uint64_t novelty_steps;  // step budget (0: off)
    uint32_t novelty_frames; // frames per trial
    uint32_t novelty_archive; // archive cap (the least productive leaves first)
    int novelty_cells;       // Go-Explore cells: screen fingerprints, learned lives, depth bias
    const char *export_state; // write the deepest novelty cell here (DPST file, needs --novelty)
    unsigned watch_every;    // publish a frame to the watch buffer every N frames (0: off)
    char tas_paths[8][512];
    int tas_count;
    uint16_t entries[16];   // explicit entry points (CPU addresses in the power-on mapping)
    unsigned entry_count;
    unsigned ctx_cap;       // max admitted states per coarse context (0 = off, C2)
    int lot_drain;         // with a lot backlog: run only the saved states (no root, no seeds)
    const char *tree_path; // write the choice tree (DpTree file) here, NULL: none
    const char *lot_path;  // disk lot of deferred states (NULL: memory only); an existing file is resumed
    // --jobs 2 partition (set by dp_pce_explore_jobs): the secondary instance
    // skips the power-on root and the finalization; the primary calls
    // pre_finalize(results, user) once its drain is done, before the static
    // closure, to fold the secondary's findings in
    int skip_root;
    int no_finalize;
    void (*pre_finalize)(struct DpPceResults_ *r, void *user);
    void *pre_user;
} DpPceOptions;

// An indexed read site (lda/adc/... abs,X or abs,Y): the base is the operand,
// the index is the 8-bit X/Y. See docs/COUVERTURE.fr.md section 5.
typedef struct {
    uint32_t site;        // cell of the instruction
    uint16_t base;        // CPU address operand
    int32_t cell0;        // cell of base (element 0), same 8 KiB page as base
    uint16_t idx_min, idx_max;
    uint64_t n_obs;
} DpPceIndexed;
enum { DP_PCE_IDX_CAP = 256 };

typedef struct DpPceResults_ {
    uint8_t *idx_flags;    // 1: declared by index inference (gaps inside the observed index range)
    DpPceIndexed idx[DP_PCE_IDX_CAP];
    unsigned nidx;
    uint64_t idx_overflow, idx_cross_page, idx_forced_ignored;      // records dropped (cap), reads on another page than the base
    uint32_t idx_declared, idx_refused_code, idx_biased; // bytes inferred / refused over code / sites starting at index >= 1
    uint8_t *flags;        // one byte per physical cell
    uint8_t *live_io;    // C1: per ROM cell of an executed opcode, hardware blocks it read
    uint32_t *exec_hits, *data_hits;   // per cell: natural-path opcode starts / ROM data reads (saturating)
    uint8_t *exec_kind;    // per cell, opcode byte: DP_PCE_X_ENTRY (non-sequential arrival) | DP_PCE_X_IRQ (ran inside a handler)
    uint8_t *pal_flags;    // 1: the cell was copied into the VCE palette (HuC6260)
    uint64_t vce_data_writes, palette_bytes; // VCE colour-data stores / distinct palette cells
    size_t rom_size;
    uint64_t steps, paths, forks, irq_forks, pad_forks, exec_outside, data_reads;
    uint64_t forced_paths, evicted, faults, illegal_cuts, pad_cuts;
    uint64_t step_cuts, loop_cuts, fork_cuts, pad_fork_cuts, irq_fork_cuts, queue_cuts,
             global_cuts;
    uint64_t lot_written, lot_read, lot_pending; // disk lot (--lot)
    uint64_t parked, parked_resumed, parked_lost; // queue-full states held aside (§6)
    uint64_t steps_deferred, loop_deferred, defer_resumed, steps_final, loop_final, defer_dropped; // frontier §6 shared policy
    uint64_t pending_dropped;  // queued states left undrained at the global wall
    uint64_t disp_tables, disp_seeds, disp_refused_view, pass2_seeds, pass2_refused, pass2_capped, tree_nodes, queue_grown, prov_sites, prov_roots, indirect_sites, indirect_targets, indirect_ptrs, indirect_overflow, sib_tables, sib_entries, sib_bytes, sib_caps, ctx_evicted, entries_seeded, entries_refused, vectors_seeded, seeds_bank, tree_loaded, tas_same, tas_different, tas_unrecorded, demo_frames_run, play_frames_run, play_sessions_run, tas_frames_run, s6_decoders, s6_tables, s6_candidates, s6_runs, s6_sat, pool_saved, pool_starts;
    uint64_t novelty_trials, novelty_hits, novelty_archived, novelty_replaced, novelty_frames_run,
             novelty_deaths;
    uint64_t watch_frames;
    uint32_t state_exported_depth; int state_exported;
    uint32_t novelty_depth;  // deepest archived cell, frames from power-on
    uint64_t io_reads, io_reg_reads[DP_PCE_IO_SLOTS], io_reg_writes[DP_PCE_IO_SLOTS];
    uint64_t io_writes, vdc_data_writes, copied_bytes;
    uint64_t smc_writes; uint16_t smc_first_pc; // stores into already executed WRAM bytes
    uint64_t static_bytes, padding_bytes, inferred_code, inferred_data, inferred_pad,
             table_bytes;
    uint64_t irq_counts[3];
    uint64_t state_bytes_peak;
    DpPceSection *sections;
    uint32_t section_count;
    DpPceCopy copies[DP_PCE_COPY_CAP];
    unsigned copy_count;
    uint64_t copy_overflow;
    DpPceBankEvent banks[DP_PCE_BANK_LOG_CAP];
    unsigned bank_count;
    uint64_t bank_overflow;
    DpPceInterrupt interrupts[DP_PCE_IRQ_LOG_CAP];
    unsigned interrupt_count;
    uint64_t interrupt_overflow;
    DpPceRegisterWrite register_writes[DP_PCE_REG_LOG_CAP];
    unsigned register_count;
    uint64_t register_overflow;
    DpPceOptions options;
    DpRamArch ra;          // code executed from RAM (archive)
} DpPceResults;

DpPceOptions dp_pce_options(void);
// --jobs N (N >= 2): a second complete copy of the explorer (namespaced
// runtime) takes the play/TAS/novelty phases while the primary drains the
// power-on fork tree; findings are merged before the shared finalization.
int dp_pce_explore_jobs(const uint8_t *data, size_t size, const DpPceOptions *options,
                        DpPceResults *results, int jobs);
int dp_pce_explore(const uint8_t *data, size_t size, const DpPceOptions *options,
                   DpPceResults *results);
void dp_pce_rtcode(FILE *out, const DpPceResults *r);
void dp_pce_results_free(DpPceResults *results);
uint8_t *dp_pce_map_build(const DpPceResults *r);
void dp_pce_report(FILE *out, const DpPceResults *results);
// campaign summary (same JSON shape as the MD one, tools/compare_platforms.py input)
int dp_pce_summary(const DpPceResults *results, const char *path);
// the "bank start end class" text maprender --z80 reads (16 KiB physical banks)
void dp_pce_map(FILE *out, const DpPceResults *results);
// physical-bank offset-start offset-end evidence-mask (8 KiB banks), like the NES file
void dp_pce_evidence(FILE *out, const DpPceResults *results);
void dp_pce_cov(FILE *out, const DpPceResults *results);       // O2: cell flags hits, natural paths
void dp_pce_romdata(FILE *out, const DpPceResults *results);   // O2: 256-byte page read counts
#endif
