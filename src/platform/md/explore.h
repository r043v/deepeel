/* Forking explorer for the Mega Drive (milestone 2).
 * Both outcomes of every conditional branch are followed (the second one
 * from a whole-machine snapshot); every bound has a counter that is reported.
 * Numbers are observations of this tool on a concrete machine (RAM at zero,
 * static pads), not hardware measurements. */
#ifndef DP_MD_EXPLORE_H
#define DP_MD_EXPLORE_H
#include <stdint.h>
#include <stdio.h>
#include "../../core/cart_md.h"
#include "movie.h"
#include "../../explore/ramarch.h"

enum { DP_MDF_NATURAL = 1, DP_MDF_FORCED = 2, DP_MDF_DATA = 4, DP_MDF_QUEUED = 8,
       DP_MDF_DATA_NATURAL = 16, DP_MDF_DMA_SOURCE = 32, DP_MDF_DMA_READ = 64,
       DP_MDF_CPU_VRAM = 128 };
/* Z80 sound CPU evidence, one byte per ROM byte (DpMdResults.z80_flags). */
enum { DP_MDZ_COPIED = 1,    /* 68000 copied it into Z80 RAM */
       DP_MDZ_CODE_RAM = 2,  /* ... and the Z80 executed it there (code-ramcopy) */
       DP_MDZ_EXEC_ROM = 4,  /* executed by the Z80 from the banked window */
       DP_MDZ_DATA = 8,      /* read by the Z80 through the bank window (not fetch) */
       DP_MDZ_SAMPLE = 16,   /* ... and written to the YM2612 DAC (reg $2A) */
       DP_MDZ_MUSIC = 32 };  /* ... and written to another YM2612 register or the PSG */
#define DP_MD_COPY_CAP 4096
#define DP_MD_READER_CAP 64
#define DP_MD_FAULT_SITE_CAP 64
#define DP_MD_DECODER_CAP 64
#define DP_MD_DECODER_STARTS 256

struct DpMdResults_;
typedef struct {
    uint64_t max_total;   /* instructions over all paths */
    uint64_t path_steps;  /* instructions per path */
    uint32_t queue_cap;   /* pending forks (each holds one machine snapshot) */
    uint32_t queue_max;   /* adaptive queue ceiling; 0 keeps queue_cap */
    uint32_t fork_cap;    /* forks created per branch site */
    uint32_t visit_cap;   /* visits of one instruction per path (loop bound) */
    uint32_t watch_write; /* 24-bit address, UINT32_MAX disables */
    uint32_t exclude_read_pc; /* exclude one CPU read site from the separate union */
    const DpMdMovie *movie; /* optional Gens movie, replayed on the natural path */
    uint32_t entries[16]; /* explicit 68000 entry points (ROM addresses, even) */
    unsigned entry_count;
    const char *tree_path;  // write the choice tree (DpTree file) here, NULL: none
    int exact_dedup;        // C3: drop a fork whose clock-free machine hash was already queued
    unsigned ctx_cap;       // admissions per coarse context (0: off)
    int bank_dispatch;      // seed the targets of longword dispatch tables (G2)
    int second_pass;        // seed the unexecuted static successors of executed code (G4)
    int pad_cut;           // cut paths sliding through 16+ zero bytes (default off)
    /* --jobs 2 partition (set by dp_md_explore_jobs): the secondary instance skips the
     * power-on path and the finalization; the primary calls pre_finalize(results, bank_seen,
     * user) once its drain is done, before the sibling pass and the byte counts */
    uint64_t novelty_steps;    /* novelty search (Go-Explore): instruction budget (0: off) */
    uint32_t novelty_frames;   /* frames per trial */
    uint32_t novelty_archive;  /* archive capacity */
    int novelty_cells;         /* key the archive on the screen fingerprint and lives */
    uint32_t play_seed;
    uint32_t play_frames;      /* random played sessions: frames per session (0: off) */
    uint32_t play_sessions;    /* number of sessions, each from power-on with its own seed */
    const char *export_state;  /* write the deepest novelty cell here (DPST file, needs --novelty) */
    unsigned watch_every;      /* publish a frame to the watch buffer every N frames (0: off) */
    int skip_root;
    int no_finalize;
    unsigned entry_from, entry_step;  /* this instance takes entries[i] with i % step == from (0: all) */
    void (*pre_finalize)(struct DpMdResults_ *r, uint8_t *bank_seen, void *user);
    void *pre_user;
    int lot_drain; /* with a lot backlog: run only the saved states (no root) */
    const char *lot_path; /* disk lot of deferred states (NULL: memory only); an existing file is resumed */
} DpMdOptions;

typedef struct { uint32_t pc, src, len, code; } DpMdCopyRec;
typedef struct { uint32_t pc, lo, hi; uint64_t accesses, natural_accesses; } DpMdReader;
typedef struct {
    uint32_t pc, fault_pc, origin;
    int code;
    uint64_t count, natural;
    uint32_t trace[8];
    unsigned trace_len;
    uint32_t an[8], an_writer[8];
    uint32_t watch_pc, watch_addr, watch_value;
    uint32_t watch_d0, watch_a1, watch_a2;
    unsigned watch_size;
} DpMdFaultSite;

typedef struct DpMdResults_ {
    uint8_t *flags;       /* per ROM byte, DP_MDF_* */
    uint32_t *exec_hits;  /* per ROM byte: natural-path instruction starts (heat) */
    uint8_t *stat;        /* S1/S2 static layer per ROM byte: 3 code (flow closure), 7 pointer table, 8 padding; 0 none */
    uint64_t stat_pad, stat_table, stat_code, stat_code_roots, stat_conf_lost;
    uint8_t *inferred;    /* S7 map inference per ROM byte: 1 pad, 2 code, 3 data, 0 none (never an observation) */
    uint64_t inf_pad, inf_code, inf_data, pad_runs_bytes;
    uint8_t *z80_flags;   /* per ROM byte, DP_MDZ_* */
    uint64_t z80_steps, z80_exec_ram, z80_exec_bank, z80_exec_other, z80_rom_reads;
    uint64_t z80_ram_copied;   /* 68000 stores into Z80 RAM whose byte came from the ROM */
    uint64_t z80_ym_writes, z80_psg_writes, z80_typed_sample, z80_typed_music;
    uint32_t z80_copied_bytes, z80_code_bytes, z80_exec_rom_bytes, z80_data_bytes, z80_sample_bytes,
             z80_music_bytes, z80_banks_read; /* distinct 32 KiB banks */
    uint8_t *nonexcluded_reads; /* per-byte CPU evidence from other read sites */
    size_t rom_size;
    uint64_t steps, paths, forks, irq_forks, pad_forks;
    uint32_t movie_frames;
    uint64_t sat_total;        /* paths dropped/cut by the global budget */
    uint64_t sat_path;         /* paths cut by the per-path budget */
    uint64_t sat_visit;        /* paths cut by the loop bound */
    uint64_t sat_queue;        /* forks dropped, queue full */
    uint64_t queue_growths;
    uint64_t sat_site;         /* forks dropped, per-site cap */
    uint64_t fork_seen;        /* alternative already executed/queued (not a loss) */
    uint64_t fork_off_rom;     /* alternative target outside the ROM */
    uint64_t lot_written, lot_read, lot_pending; /* disk lot (--lot) */
    uint64_t entries_seeded, entries_refused; /* explicit entries queued / refused (odd or outside the ROM) */
    uint64_t parked, parked_resumed, parked_lost; /* queue-full forks held aside (§6) */
    uint64_t dropped_pending;  /* forks left unrun when the total budget ended */
    uint64_t faults;
    DpMdFaultSite fault_sites[DP_MD_FAULT_SITE_CAP];
    unsigned nfault_sites;
    uint64_t fault_sites_overflow;
    DpMdFaultSite fault_origins[DP_MD_FAULT_SITE_CAP];
    unsigned nfault_origins;
    uint64_t fault_origins_overflow;
    uint64_t exec_outside, ram_exec, sram_exec, sram_reads;
    uint64_t smc_writes; uint32_t smc_first_pc; // stores into work-RAM bytes that already ran
    uint64_t decode_length_mismatches;
    uint64_t reg_r[64], reg_w[64], unmapped_r, unmapped_w;
    DpMdCopyRec *copies;
    int ncopy;
    uint64_t copy_overflow;
    uint64_t cpu_vram_words;
    DpMdReader readers[DP_MD_READER_CAP];
    unsigned nreaders;
    uint64_t reader_overflow;
    uint64_t s4_tables, s4_seeds, s4_refused, disp_tables, disp_seeds, disp_refused_view, pass2_seeds, pass2_refused, pass2_capped, ctx_evicted, exact_dups, tree_nodes, sib_tables, sib_entries, sib_bytes, sib_caps, pad_cuts, indirect_sites, indirect_targets, indirect_overflow;
    uint32_t sec_n, sec_req;  /* C1: executed runs, and those with a natural-path byte */
    uint32_t exec_bytes, forced_only_bytes, dread_bytes, natural_data_bytes, dma_source_bytes, dma_read_bytes,
             cpu_vram_bytes, observed_bytes, code_or_vram_bytes, data_only_bytes, filtered_observed_bytes;
    /* stream replay: a called routine whose first ROM read is at the address
     * one of its An registers held on entry is a stream consumer; it is run
     * again (from its own entry snapshot) on the other entries of the ROM
     * pointer tables that hold its observed starts */
    unsigned decoders, decoders_without_snapshot;
    uint64_t decoder_overflow, start_overflow, frame_overflow;
    uint64_t replay_tables, replay_candidates, replay_runs, replay_steps;
    uint64_t sat_replay_steps, sat_replay_total, replay_faults;
    uint32_t replay_only_bytes; /* bytes read only by replays */
    uint64_t novelty_trials, novelty_hits, novelty_archived, novelty_replaced, novelty_frames_run, novelty_deaths, novelty_seeded, novelty_stalled, watch_frames, play_sessions_run, play_frames_run;
    uint32_t state_exported_depth; int state_exported;
    uint32_t novelty_depth;
    unsigned jobs_folded;   /* --jobs 2: instances folded into this report (secondary) */
    uint8_t z80_bank_seen_out[128]; /* secondary instance only: banks the Z80 read (merged by the primary) */
    DpRamArch ra;          // code executed from RAM (archive, low 16 bits of the pc)
} DpMdResults;

void dp_md_options_default(DpMdOptions *o);
int dp_md_explore(const DpMdCart *c, const DpMdOptions *o, DpMdResults *r);
void dp_md_results_free(DpMdResults *r);
/* --jobs 2: entries split between two instances (own machine each); see jobs.c */
int dp_md_explore_jobs(const DpMdCart *c, const DpMdOptions *o, DpMdResults *r, int jobs);
void dp_md_explore_rtcode(const DpMdResults *r, const char *path);
void dp_md_explore_report(const DpMdCart *c, const DpMdOptions *o, const DpMdResults *r, FILE *out);
/* One byte per ROM byte: the DP_MDF_* flags. */
int dp_md_explore_map(const DpMdResults *r, const char *path);
/* compact JSON profile of the run (class counts, search and saturation counters) */
int dp_md_explore_heat(const DpMdResults *r, const char *path);   /* per-zone played usage */
int dp_md_explore_summary(const DpMdResults *r, const char *path);
uint8_t *dp_md_map_build(const DpMdResults *r);   /* DP_NMAP_* digits, caller frees */
/* Text: executed ranges and copies. */
int dp_md_explore_evidence(const DpMdResults *r, const char *path);
#endif
