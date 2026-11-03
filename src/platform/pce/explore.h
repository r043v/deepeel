// explore.h: the bounded PC Engine explorer contract, mirroring the NES and
// GB records: per-cell flags on the physical HuCard image, sections with
// necessity verdicts, copy records (ROM -> VDC), MPR timeline, interrupt
// and register logs, every guard saturation counted.
#ifndef DP_PCE_EXPLORE_H
#define DP_PCE_EXPLORE_H
#include <stdint.h>
#include <stdio.h>

enum { DP_PCE_NATURAL = 1, DP_PCE_FORCED = 2, DP_PCE_DATA = 4, DP_PCE_COPIED = 8,
       DP_PCE_IRQ = 16, DP_PCE_PAD = 32, DP_PCE_STATIC = 64, DP_PCE_TABLE = 128 };

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
enum { DP_PCE_VERDICT_NONE = 0, DP_PCE_VERDICT_REQUIRED, DP_PCE_VERDICT_FORCED };
static const char *const dp_pce_verdict_names[] = { "none", "required", "forced" };

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
    uint8_t channel;       // 1 CPU store to VDC data, 2 block move to VDC data
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
    uint8_t verdict;
} DpPceSection;

typedef struct {
    uint64_t max_total;
    uint32_t max_steps, queue_cap, loop_cap, fork_cap, pad_fork_cap, irq_fork_cap;
    int seed_irq;          // IRQ forks (VDC vblank, timer) at their enabling points
    int static_closure;    // static decode from observed code (default on)
    int infer;             // padding and gap inference (default on)
    uint32_t play_frames, play_sessions, play_seed;
    // novelty search (Go-Explore): restore archived checkpoints that found
    // new cells and play on with random held inputs
    uint64_t novelty_steps;  // step budget (0: off)
    uint32_t novelty_frames; // frames per trial
    uint32_t novelty_archive; // archive cap (the least productive leaves first)
    int novelty_cells;       // Go-Explore cells: screen fingerprints, learned lives, depth bias
    char tas_paths[8][512];
    int tas_count;
} DpPceOptions;

typedef struct {
    uint8_t *flags;        // one byte per physical cell
    size_t rom_size;
    uint64_t steps, paths, forks, irq_forks, pad_forks, exec_outside, data_reads;
    uint64_t forced_paths, evicted, faults, illegal_cuts;
    uint64_t step_cuts, loop_cuts, fork_cuts, pad_fork_cuts, irq_fork_cuts, queue_cuts,
             global_cuts;
    uint64_t play_frames_run, play_sessions_run, tas_frames_run;
    uint64_t novelty_trials, novelty_hits, novelty_archived, novelty_replaced, novelty_frames_run,
             novelty_deaths;
    uint32_t novelty_depth;  // deepest archived cell, frames from power-on
    uint64_t io_writes, vdc_data_writes, copied_bytes;
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
} DpPceResults;

DpPceOptions dp_pce_options(void);
int dp_pce_explore(const uint8_t *data, size_t size, const DpPceOptions *options,
                   DpPceResults *results);
void dp_pce_results_free(DpPceResults *results);
uint8_t *dp_pce_map_build(const DpPceResults *r);
void dp_pce_report(FILE *out, const DpPceResults *results);
// the "bank start end class" text maprender --z80 reads (16 KiB physical banks)
void dp_pce_map(FILE *out, const DpPceResults *results);
#endif
