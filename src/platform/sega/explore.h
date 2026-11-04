#ifndef DP_SEGA_EXPLORE_H
#define DP_SEGA_EXPLORE_H
#include "../../core/coverage.h"
#include "sega.h"
#include "closure.h"
#include "../../../src/core/hashset.h"
#include <stddef.h>
#include <stdint.h>
enum { DP_SEGA_NATURAL=1, DP_SEGA_FORCED=2, DP_SEGA_DATA=4, DP_SEGA_COPIED=8, DP_SEGA_STATIC=16, DP_SEGA_STATIC_DATA=32,
    DP_SEGA_TABLE=64, DP_SEGA_PAD=128 };
// bit of static_flags (own byte, not the flags above): inferred sibling of a copy source
enum { DP_SEGA_SIBLING=1 }; // static scan: pointer table, padding run
// map classes mirror the GB names so maprender renders z80 maps unchanged;
// inferred-* classes are deduced from neighbours, never observed
enum {
    DP_SMAP_UNKNOWN, DP_SMAP_CODE_NAT, DP_SMAP_CODE_FORCED, DP_SMAP_CODE_STATIC,
    DP_SMAP_DATA_COPIED, DP_SMAP_DATA_READ, DP_SMAP_DATA_STATIC, DP_SMAP_TABLE,
    DP_SMAP_PAD, DP_SMAP_INF_DATA, DP_SMAP_INF_CODE, DP_SMAP_INF_PAD, DP_SMAP_COUNT
};
static const char *const dp_smap_names[DP_SMAP_COUNT]={"unknown","code-natural","code-forced",
    "code-static","data-copied","data-read","data-static","table","padding",
    "inferred-data","inferred-code","inferred-padding"};

enum { DP_SEGA_COPY_CAP=4096 };
enum { DP_SEGA_INPUT_LOG_CAP=1024 };
enum { DP_SEGA_HARDWARE_LOG_CAP=1024 };
enum { DP_SEGA_IRQ=0x80 }; // kind bit: section runs inside an accepted interrupt
enum { DP_SEGA_SECTION_CAP=65536 };
static const char *const dp_sega_verdict_names[]={"none","required","forced","played"};
typedef struct {
    uint64_t path,step;
    int64_t target_rom;
    uint16_t pc,target,sp;
    uint8_t mode,vector,i,sources,forced;
} DpSegaInterrupt;
typedef struct {
    uint64_t path,step;
    uint16_t pc,port;
    uint8_t reg,value,ignored,forced;
} DpSegaRegisterWrite;
typedef struct {
    uint64_t path,step;
    uint16_t pc,port;
    uint8_t previous,value,effect_mask,forced;
} DpSegaMemoryWrite;
typedef struct {
    uint64_t path,first_step,last_step,occurrences;
    uint32_t source,destination,length;
    uint16_t pc;
    int8_t source_stride,destination_stride;
    uint8_t space,verbatim,forced;
} DpSegaCopy;
typedef struct {
    uint64_t max_total;
    uint64_t input_total;
    uint64_t indirect_total; // second pass seeding observed jp (hl) targets
    uint64_t play_total;     // instrumented play session steps (0: no session)
    uint64_t novelty_total;  // Go-Explore novelty search: instruction budget (0: off)
    uint32_t novelty_frames, novelty_archive, novelty_frame_steps; // frames per trial, archive size, instructions per frame
    unsigned watch_every;    // publish a frame to the watch buffer every N frames (0: off; forces one worker)
    const char *export_state; // write the deepest novelty cell here (DPST file, needs --novelty)
    int novelty_cells;       // key the archive on the screen fingerprint and learned lives
    uint64_t tas_total;      // TAS replay budget, steps (bounded whatever the movie length)
    uint32_t tas_frame_steps; // per-frame step cap of the TAS replay
    const char *tas_path;    // TAS movie (.bk2 or plain input log, sega/tas.h)
    int jobs;                // parallel drain workers (2 max: prefixed instances)
    uint32_t defer_tiers; // 0: budget cuts stay final; N: resume them up to N times with a 4x cap (CLI default 3)
    uint32_t max_steps,max_paths,queue_cap,loop_cap,fork_cap,stagnation,copy_cap,input_masks,input_sites;
    int target_closure,block_closure;
    int seed_vectors;
    uint32_t seed_cap;     // max pointer-scan seeds (0 disables the scan)
    int seed_banks;        // seed every switchable window/bank pair
    int no_pointer_scan;   // disable pointer-table seeding
    int no_context_evict;  // disable same-context eviction (GB parity)
    uint32_t queue_max;    // queue growth ceiling (0: no growth past queue_cap)
    const char *spool_path; // overflow states survive here (--resume re-reads)
    int resume;             // continue from spool_path instead of truncating it
    int lot_drain;          // with a lot backlog: run only the saved states (no root, no seeds)
    const char *lot_path;   // portable frontier lot: imported into the spool at start, the unread spool saved back at the end
    uint32_t spool_cap_mb;  // 0: unbounded
    const char *tree_path;  // choice tree record (--tree FILE)
    struct { uint32_t bank; uint16_t addr; } entry[32]; // extra --entry seeds
    unsigned entry_count;
    int sega_mapper; // 0 auto; otherwise DP_SEGA_MAPPER_* (SMS/GG only)
    int msx_mapper; // -1 auto; otherwise DP_MSX_* mapper selection
    DpSegaClosureOptions closure;
} DpSegaOptions;
// Choice tree (GB tree.c parity): every queued path is a node; issues close
// nodes when their outcome is known. Nodes are written as binary records.
enum {
    DP_SEGA_TREE_RESET=1, DP_SEGA_TREE_VECTOR, DP_SEGA_TREE_POINTER, DP_SEGA_TREE_BANK,
    DP_SEGA_TREE_ENTRY, DP_SEGA_TREE_BRANCH, DP_SEGA_TREE_IRQ, DP_SEGA_TREE_INPUT
};
enum {
    DP_SEGA_ISSUE_NONE=0, DP_SEGA_ISSUE_DONE, DP_SEGA_ISSUE_EVICTED,
    DP_SEGA_ISSUE_SPOOLED, DP_SEGA_ISSUE_BUDGET
};
typedef struct {
    uint32_t parent;   // 0 for roots
    uint32_t site;     // physical byte of the entry pc (size+cpu addr outside)
    uint16_t pc;
    uint8_t kind,issue;
} DpSegaTreeNode;
// Executed-range section with per-context bookkeeping (GB DpSection parity).
// A section is a maximal contiguous run of executed physical bytes inside one
// 16 KiB bank; every access an instruction makes is attributed to the section
// its pc belongs to, which is where the necessity statistics live.
enum { DP_SEGA_VERDICT_NONE=0, DP_SEGA_VERDICT_REQUIRED, DP_SEGA_VERDICT_FORCED, DP_SEGA_VERDICT_PLAYED };
typedef struct {
    uint32_t id;
    uint8_t bank;              // physical 16 KiB bank
    uint16_t lo,hi;            // bank offsets, inclusive
    uint64_t hits,natural_hits,forced_hits,boot_hits,played_hits,entries;
    uint8_t kind_bits;         // DP_SEGA_NATURAL|DP_SEGA_FORCED|DP_SEGA_IRQ
    uint64_t copies;           // copy records attributed to a pc in this section
    uint8_t verdict;           // DP_SEGA_VERDICT_*, computed at finalize
    // liveness learned from the section's own reads; the context projection
    // hashes only these components (contexts seen resets when a mask grows)
    uint64_t page_mask_wram;   // 32 pages x 256 B over the 8 KiB WRAM window
    uint8_t page_mask_cart[16];// 128 pages x 256 B over cartridge RAM
    uint8_t io_read_live[32];  // 256 IN ports
    uint64_t ctx_count;        // distinct incoming contexts admitted
    DpHashSet ctx_seen;
} DpSegaSection;
// legacy Sega flags -> common nature (docs/COUVERTURE.fr.md annexe A)
static inline uint16_t dp_sega_nature(uint8_t f,uint8_t st) {
    uint16_t n=0;
    if(f&DP_SEGA_NATURAL) n|=DP_NAT_CODE_NAT;
    if(f&DP_SEGA_FORCED) n|=DP_NAT_CODE_FORCED;
    if(f&DP_SEGA_STATIC) n|=DP_NAT_CODE_STATIC;
    if(f&DP_SEGA_COPIED) n|=DP_NAT_DATA_COPIED;
    if(f&DP_SEGA_STATIC_DATA) n|=DP_NAT_DATA_STATIC;
    if(f&DP_SEGA_DATA) n|=DP_NAT_DATA_READ;
    if(st&DP_SEGA_SIBLING) n|=DP_NAT_DATA_INF;
    if(st&DP_SEGA_TABLE) n|=DP_NAT_TABLE;
    if(st&DP_SEGA_PAD) n|=DP_NAT_PAD;
    return n;
}
typedef struct {
    uint8_t *flags;
    uint32_t *exec_hits,*data_hits; // per ROM byte, saturating: how often it was executed / read as data
    size_t size;
    uint64_t s6_decoders,s6_tables,s6_candidates,s6_runs,s6_sat; // S6 decoder replay
    uint64_t steps,paths,forks,irq_forks,exec_outside,data_reads,vdp_copies,ram_copies;
    uint64_t defer_spooled, fork_deferred, fork_final, steps_deferred,loop_deferred,defer_resumed,steps_final,loop_final,defer_dropped,defer_pending; // §6
    uint64_t step_cuts,loop_cuts,fork_cuts,queue_cuts,global_cuts,path_cuts,faults,decode_cuts,pad_cuts,stagnation_cuts;
    // Seeds (GB parity): generated specs by kind, admissions, bounded refusals.
    uint64_t seeds_pointer,seeds_bank,seeds_entry,seeds_admitted,seeds_skipped_padding;
    uint64_t seeds_refused_view,seeds_refused_queue,seeds_refused_cap,seeds_pending;
    // sections and same-context eviction (GB parity)
    DpSegaSection *sections;
    uint32_t section_count,section_capacity;
    uint32_t *section_of;      // physical byte -> section id + 1 (0: none)
    uint64_t evicted_ctx,ctx_grew,ctx_full,section_cuts;
    // growing queue, disk spool and choice tree (GB parity)
    uint64_t queue_growths,spool_writes,spool_reads,spool_refused,spool_flushed;
    uint64_t lot_written,lot_read,lot_pending; // disk lot (--lot): imported at start, rewritten with the unread spool at the end
    DpSegaTreeNode *tree_nodes;
    uint32_t tree_count,tree_capacity;
    const uint8_t *rom;        // ROM image copy for static map classes
    uint8_t *static_flags;     // static scan: DP_SEGA_TABLE|DP_SEGA_PAD per byte
    uint32_t *src_flags;       // DP_SRC_* per byte, OR-ed (every proving source)
    DpFirstLog first;          // sparse first-discovery table (bounded)
    // observed indirect jumps and their seeding pass (GB dp_seed_indirect)
    uint64_t indirect_sites,indirect_targets,indirect_site_overflow,indirect_target_overflow;
    uint64_t indirect_seeds,indirect_refused_budget,indirect_pending;
    // play session (GB sessions parity): natural coverage from held inputs
    uint64_t play_steps,play_epochs;
    // novelty search (Go-Explore)
    uint64_t novelty_trials,novelty_hits,novelty_archived,novelty_replaced,novelty_frames_run,novelty_deaths;
    uint64_t watch_frames;
    uint32_t novelty_depth,state_exported_depth; int state_exported;
    // TAS replay (GB tas.c parity): natural coverage from a movie, priority
    uint64_t tas_frames,tas_frames_refused,tas_steps;
    // copy siblings (S5): tables found, ranges marked, ranges refused over executed code, tables cut at the entry cap, bytes newly marked
    uint64_t halt_cuts; // loop cuts at a HALT: the D14 watchdog, bounded by the loop cap
    uint64_t sib_tables,sib_ranges,sib_refused,sib_caps,sib_bytes;
    // jump tables (S4): dispatch tables found around provenance-proven jp (hl) loads
    uint64_t jt_tables,jt_entries,jt_caps,jt_bytes;
    int tas_identity;        // 0 no movie, 1 same ROM, 2 different ROM, 3 movie records none
    // parallel drain (--jobs): seeds routed to another worker, requests capped
    uint64_t seeds_offlane,jobs_capped,merge_conflicts;
    size_t loop_context_peak,loop_context_capacity;
    uint64_t loop_context_growths;
    uint64_t io_read[256],io_write[256],bank_changes,bank_log_overflow;
    uint64_t vram_copies,cram_copies,sram_copies,copy_transformed,copy_log_overflow,provenance_mismatches;
    DpSegaCopy *copies;
    unsigned copy_count,copy_capacity;
    uint64_t input_forks,input_cap_cuts,input_drops,input_site_drops,input_log_overflow;
    uint64_t input_queue_cuts,input_path_cuts;
    uint64_t input_retry_skips;
    uint64_t phase_steps[3],phase_paths[3],phase_pending[3],phase_limits[3],phase_known[3];
    uint32_t input_site_count,input_site_capacity,input_mask_limit;
    struct {
        uint64_t parent_path,step;
        uint16_t pc,port;
        DpSegaInput input;
        uint8_t halted;
    } inputs[DP_SEGA_INPUT_LOG_CAP];
    unsigned input_count;
    struct { uint64_t step,path; uint16_t pc,addr; uint8_t value,kind; } banks[1024];
    unsigned bank_count;
    uint64_t interrupts[4],interrupt_suppressed,interrupt_log_overflow;
    DpSegaInterrupt interrupt_log[DP_SEGA_HARDWARE_LOG_CAP];
    unsigned interrupt_count;
    uint64_t vdp_register_writes[16],vdp_register_ignored,register_log_overflow;
    DpSegaRegisterWrite register_log[DP_SEGA_HARDWARE_LOG_CAP];
    unsigned register_count;
    uint64_t memory_writes,memory_effect_changes,memory_log_overflow;
    DpSegaMemoryWrite memory_log[DP_SEGA_HARDWARE_LOG_CAP];
    unsigned memory_count;
    uint64_t bios_calls[64],ay_writes[16],ay_log_overflow;
    DpSegaRegisterWrite ay_log[DP_SEGA_HARDWARE_LOG_CAP];
    unsigned ay_count;
    uint8_t msx_mapper,sega_mapper,sega_mapper_explicit;
    DpSegaClosureStats closure,target_closure,block_closure;
} DpSegaResults;
DpSegaOptions dp_sega_options(void);
int dp_sega_explore(const DpSegaSystem *system,const uint8_t *rom,size_t size,
                    const DpSegaOptions *options,DpSegaResults *results);
void dp_sega_results_free(DpSegaResults *results);
// one class per byte (GB dp_map_build parity); NULL when out of memory
uint8_t *dp_sega_map_build(const DpSegaResults *r);
void dp_sega_report(FILE *out,const DpSegaSystem *system,const DpSegaResults *results);
// campaign summary (--summary FILE): one JSON object per run for scan_all.py
void dp_sega_summary(FILE *out,const DpSegaSystem *system,const DpSegaResults *results);
void dp_sega_map(FILE *out,const DpSegaResults *results);
// Lossless per-byte evidence flags, including overlapping code/data proofs.
void dp_sega_cov(FILE *out,const DpSegaResults *r);
void dp_sega_romdata(FILE *out,const DpSegaResults *r);
void dp_sega_evidence(FILE *out,const DpSegaResults *results);
int dp_sega_explore_command(int argc,char **argv);
#endif
