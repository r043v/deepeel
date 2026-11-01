#ifndef DP_SEGA_EXPLORE_H
#define DP_SEGA_EXPLORE_H
#include "sega.h"
#include "closure.h"
#include "../../../src/core/hashset.h"
#include <stddef.h>
#include <stdint.h>
enum { DP_SEGA_NATURAL=1, DP_SEGA_FORCED=2, DP_SEGA_DATA=4, DP_SEGA_COPIED=8, DP_SEGA_STATIC=16, DP_SEGA_STATIC_DATA=32 };
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
    uint32_t max_steps,max_paths,queue_cap,loop_cap,fork_cap,stagnation,copy_cap,input_masks,input_sites;
    int target_closure,block_closure;
    int seed_vectors;
    uint32_t seed_cap;     // max pointer-scan seeds (0 disables the scan)
    int seed_banks;        // seed every switchable window/bank pair
    int no_pointer_scan;   // disable pointer-table seeding
    int no_context_evict;  // disable same-context eviction (GB parity)
    struct { uint32_t bank; uint16_t addr; } entry[32]; // extra --entry seeds
    unsigned entry_count;
    int sega_mapper; // 0 auto; otherwise DP_SEGA_MAPPER_* (SMS/GG only)
    int msx_mapper; // -1 auto; otherwise DP_MSX_* mapper selection
    DpSegaClosureOptions closure;
} DpSegaOptions;
// Executed-range section with per-context bookkeeping (GB DpSection parity).
// A section is a maximal contiguous run of executed physical bytes inside one
// 16 KiB bank; every access an instruction makes is attributed to the section
// its pc belongs to, which is where the necessity statistics live.
enum { DP_SEGA_VERDICT_NONE=0, DP_SEGA_VERDICT_REQUIRED, DP_SEGA_VERDICT_FORCED, DP_SEGA_VERDICT_PLAYED };
typedef struct {
    uint32_t id;
    uint8_t bank;              // physical 16 KiB bank
    uint16_t lo,hi;            // bank offsets, inclusive
    uint64_t hits,natural_hits,forced_hits,boot_hits,entries;
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
typedef struct {
    uint8_t *flags;
    size_t size;
    uint64_t steps,paths,forks,irq_forks,exec_outside,data_reads,vdp_copies,ram_copies;
    uint64_t step_cuts,loop_cuts,fork_cuts,queue_cuts,global_cuts,path_cuts,faults,decode_cuts,stagnation_cuts;
    // Seeds (GB parity): generated specs by kind, admissions, bounded refusals.
    uint64_t seeds_pointer,seeds_bank,seeds_entry,seeds_admitted,seeds_skipped_padding;
    uint64_t seeds_refused_view,seeds_refused_queue,seeds_refused_cap,seeds_pending;
    // sections and same-context eviction (GB parity)
    DpSegaSection *sections;
    uint32_t section_count,section_capacity;
    uint32_t *section_of;      // physical byte -> section id + 1 (0: none)
    uint64_t evicted_ctx,ctx_grew,ctx_full,section_cuts;
    size_t loop_context_peak,loop_context_capacity;
    uint64_t loop_context_growths;
    uint64_t io_read[256],io_write[256],bank_changes,bank_log_overflow;
    uint64_t vram_copies,cram_copies,sram_copies,copy_transformed,copy_log_overflow,provenance_mismatches;
    DpSegaCopy *copies;
    unsigned copy_count,copy_capacity;
    uint64_t input_forks,input_cap_cuts,input_drops,input_site_drops,input_log_overflow;
    uint64_t input_queue_cuts,input_path_cuts;
    uint64_t input_retry_skips;
    uint64_t phase_steps[2],phase_paths[2],phase_pending[2],phase_limits[2],phase_known[2];
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
void dp_sega_report(FILE *out,const DpSegaSystem *system,const DpSegaResults *results);
void dp_sega_map(FILE *out,const DpSegaResults *results);
// Lossless per-byte evidence flags, including overlapping code/data proofs.
void dp_sega_evidence(FILE *out,const DpSegaResults *results);
int dp_sega_explore_command(int argc,char **argv);
#endif
