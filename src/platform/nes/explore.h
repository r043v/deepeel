// explore.h: the bounded NES explorer contract, mirroring the GB/Z80
// explorer records (sections with necessity verdicts, copy records, bank
// timeline, guard saturations reported, never silent).  The machine is
// m0boy's NES runtime (src/mach/nesmach.h); this header carries only the
// explorer's own records.
#ifndef DP_NES_EXPLORE_H
#define DP_NES_EXPLORE_H
#include "../../core/hashset.h"
#include "../../core/coverage.h"
#include "../../mach/nesmach.h"
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
enum { DP_NES_VERDICT_NONE = 0, DP_NES_VERDICT_REQUIRED, DP_NES_VERDICT_FORCED, DP_NES_VERDICT_SPECULATIVE };
static const char *const dp_nes_verdict_names[] = { "none", "required", "forced", "speculative" };

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
    uint8_t channel;   // 1 = OAM DMA ($4014), 2 = DMC, 3 = CPU loop to PPUDATA ($2007), 4 = CPU loop to WRAM, 5 = to SRAM ($6000-$7FFF), 6 = to APU registers ($4000-$4013)
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
       DP_NES_TREE_IRQ, DP_NES_TREE_PROBE, DP_NES_TREE_PASS2, DP_NES_TREE_MUSIC };
enum { DP_NES_ISSUE_NONE = 0, DP_NES_ISSUE_DONE, DP_NES_ISSUE_EVICTED, DP_NES_ISSUE_SPOOLED, DP_NES_ISSUE_DEFERRED };
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
    uint8_t ram_pages;     // C1 liveness: 256-byte pages of the 2 KiB work RAM the section's code read
    uint8_t io_reads;      // C1 liveness: DP_NES_LIVE_* classes of register read ($2000 PPU, $4016/7 pads, other)
    uint8_t kind_bits;     // DP_NES_NATURAL | DP_NES_FORCED | DP_NES_IRQ
    uint8_t verdict;       // DP_NES_VERDICT_*
} DpNesSection;

struct DpNesResults_;
typedef struct {
    uint64_t max_total;
    uint32_t max_steps, queue_cap, queue_max, loop_cap, fork_cap;   // queue_max: adaptive growth ceiling (0: fixed queue_cap)
    int seed_irq;          // fork the NMI/IRQ entries when enabled (default on)
    // play sessions: random-input frames before the fork exploration.  The
    // visited bytes land in the same physical map as the exploration.
    uint32_t play_frames;  // frames per session (0: no play pass)
    uint32_t demo_frames;  // idle-pad run from power-on before the TAS and play passes (0: none)
    uint32_t play_sessions; // restart count
    uint32_t play_seed;    // RNG seed
    // TAS movies (fm2/bk2), played from power-on before the fork drain;
    // each movie is one session, its end state seeds the fork queue
    char tas_paths[8][512];
    int tas_count;
    uint8_t entry_bank[32]; uint16_t entry_addr[32]; int entry_count; // --entry bank:addr seeds
    const void *music_seeds;   // DpMusicSeed[] from --music-index (ownership: the CLI), confirmed entry points seeded first
    unsigned music_seed_count;
    int no_pad_probe;      // disable the strobe-site pad probes
    int event_probes;      // §3.7: fork on plausible $2002 status values at the poll sites
    int pad_full;          // D10: also probe every one of the 256 port-1 button masks at each strobe site
    unsigned ctx_cap;      // legacy C2: max admitted states per coarse context (0 = off; keeps the old opt-in admission-cap mode)
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
} DpNesOptions;

DpNesOptions dp_nes_options(void);

// the hardware registry (src/platform/nes/registry.c)
struct DpPlatform;
const struct DpPlatform *dp_nes_platform(void);
void dp_nes_regs(FILE *out);
#endif
