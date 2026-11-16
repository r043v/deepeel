// explore.h: the bounded PC Engine explorer contract, mirroring the NES and
// GB records: per-cell flags on the physical HuCard image, sections with
// necessity verdicts, copy records (ROM -> VDC), MPR timeline, interrupt
// and register logs, every guard saturation counted.
#ifndef DP_PCE_EXPLORE_H
#define DP_PCE_EXPLORE_H
#include "../../explore/ramarch.h"
#include "../../core/coverage.h"
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
    const void *music_seeds;   // DpMusicSeed[] from --music-index (ownership: the CLI), confirmed entry points seeded first
    unsigned music_seed_count;
    unsigned ctx_cap;       // max admitted states per coarse context (0 = off, C2)
    unsigned ctx_live;      // --ctx-live: project the context onto the target section's hardware liveness (C1 -> C2)
    int lot_drain;         // with a lot backlog: run only the saved states (no root, no seeds)
    const char *tree_path; // write the choice tree (DpTree file) here, NULL: none
    const char *lot_path;  // disk lot of deferred states (NULL: memory only); an existing file is resumed
} DpPceOptions;

DpPceOptions dp_pce_options(void);
#endif
