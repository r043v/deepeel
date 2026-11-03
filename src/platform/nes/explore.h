// explore.h: the bounded NES explorer contract, mirroring the GB/Z80
// explorer records (sections with necessity verdicts, copy records, bank
// timeline, guard saturations reported, never silent).  The machine is
// m0boy's NES runtime (src/mach/nesmach.h); this header carries only the
// explorer's own records.
#ifndef DP_NES_EXPLORE_H
#define DP_NES_EXPLORE_H
#include "../../core/hashset.h"
#include <stdint.h>
#include <stdio.h>

enum { DP_NES_NATURAL = 1, DP_NES_FORCED = 2, DP_NES_DATA = 4, DP_NES_COPIED = 8,
       DP_NES_IRQ = 16, DP_NES_PAD = 32,
       DP_NES_SAMPLE = 64, // read by the DMC channel (delta-modulation sample DMA)
       DP_NES_STATIC = 128 }; // reached by the static flow closure, never executed

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

typedef struct {
    uint32_t id;
    uint8_t bank;          // physical 16 KiB bank
    uint16_t lo, hi;       // bank offsets, inclusive
    uint64_t hits, natural_hits, forced_hits, boot_hits, entries;
    uint64_t copies;
    uint8_t kind_bits;     // DP_NES_NATURAL | DP_NES_FORCED | DP_NES_IRQ
    uint8_t verdict;       // DP_NES_VERDICT_*
} DpNesSection;

typedef struct {
    uint64_t max_total;
    uint32_t max_steps, max_paths, queue_cap, loop_cap, fork_cap;
    int seed_irq;          // fork the NMI/IRQ entries when enabled (default on)
    // play sessions: random-input frames before the fork exploration.  The
    // visited bytes land in the same physical map as the exploration.
    uint32_t play_frames;  // frames per session (0: no play pass)
    uint32_t play_sessions; // restart count
    uint32_t play_seed;    // RNG seed
    // TAS movies (fm2/bk2), played from power-on before the fork drain;
    // each movie is one session, its end state seeds the fork queue
    char tas_paths[8][512];
    int tas_count;
    uint8_t entry_bank[32]; uint16_t entry_addr[32]; int entry_count; // --entry bank:addr seeds
    int no_pad_probe;      // disable the strobe-site pad probes
    int seed_banks;        // also seed every switchable bank at $8000 (UxROM)
    int lot_drain;         // with a lot backlog: run only the saved states (no root, no seeds)
    const char *lot_path;  // disk lot of deferred states (NULL: memory only); an existing file is resumed
} DpNesOptions;

enum { DP_NES_CHR_BG = 1, DP_NES_CHR_SPR = 2, DP_NES_CHR_READ = 4 };

typedef struct {
    uint8_t *inferred;         // per PRG cell: DP_NMAP_INF_* for unclassified gaps, 0 otherwise
    uint8_t *chr_flags;        // one byte per CHR ROM cell (DP_NES_CHR_*), NULL on CHR-RAM
    size_t chr_size;
    uint64_t chr_tiles_bg, chr_tiles_spr, chr_reads; // distinct 16-byte tiles / $2007 reads
    uint8_t *flags;            // one byte per physical PRG cell
    size_t prg_size;
    uint64_t steps, paths, forks, irq_forks, exec_outside, data_reads, oam_copies;
    uint64_t dmc_starts, dmc_bytes; // $4015 bit 4 starts (copy channel 2) / distinct sample bytes
    uint64_t forced_paths;
    uint64_t step_cuts, loop_cuts, fork_cuts, queue_cuts, global_cuts, path_cuts,
             faults, decode_cuts;
    uint64_t tas_same, tas_different, tas_unrecorded, pad_cuts, table_bytes, table_count, static_bytes, static_roots, static_cap_hits, padding_bytes, inferred_code, inferred_data, inferred_pad, cpu_copy_bytes, indirect_sites, indirect_targets, indirect_overflow, pad_probes, pad_probe_sites, pad_probe_refused, entries_seeded, entries_refused, seeds_reset, seeds_irq, seeds_bank, seeds_refused_queue;
    uint64_t play_frames_run, play_sessions_run, play_steps_accounted, seeds_play;
    uint64_t evicted;          // exact-state dedup admissions refused
    uint64_t lot_written, lot_read, lot_pending; // disk lot (--lot): spilled / read back / left on disk
    uint64_t parked, parked_resumed, parked_lost; // queue-full states held aside (§6), resumed, lost at the holding cap
    uint64_t pending_dropped;  // queued states left undrained at a wall
    uint64_t ram_writes, io_writes, io_reads;
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
    DpNesOptions options;
} DpNesResults;

DpNesOptions dp_nes_options(void);
int dp_nes_explore(const uint8_t *data, size_t size, int mapper,
                   const DpNesOptions *options, DpNesResults *results);
void dp_nes_results_free(DpNesResults *results);
uint8_t *dp_nes_map_build(const DpNesResults *r);
void dp_nes_report(FILE *out, const DpNesResults *results);
void dp_nes_map(FILE *out, const DpNesResults *results);

// the hardware registry (src/platform/nes/registry.c)
struct DpPlatform;
const struct DpPlatform *dp_nes_platform(void);
void dp_nes_regs(FILE *out);
#endif
