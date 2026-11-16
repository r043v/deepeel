#ifndef DP_SEGA_EXPLORE_H
#define DP_SEGA_EXPLORE_H
#include "sega.h"
#include "closure.h"
#include <stddef.h>
#include <stdint.h>
// per-byte evidence bits shared by the provenance closure (closure.c) and the kernel fold-in (command.c)
enum { DP_SEGA_NATURAL=1, DP_SEGA_FORCED=2, DP_SEGA_DATA=4, DP_SEGA_COPIED=8, DP_SEGA_STATIC=16, DP_SEGA_STATIC_DATA=32 };
// Options of the Sega 8-bit command; the kernel's own options live in DpCoreConfig (dp_core_parse_option).
typedef struct {
    uint64_t max_total;     // 0: the kernel default
    uint32_t max_steps,fork_cap,queue_max;
    int jobs;
    int seed_vectors;       // seed the reset/RST vectors when no other seed is given
    int seed_banks;         // seed every switchable bank window
    int target_closure,block_closure;
    const char *tas_path;   // movie (.bk2 or plain input log, sega/tas.h)
    struct { uint32_t bank; uint16_t addr; } entry[32];   // --entry bank:addr
    unsigned entry_count;
    const void *music_seeds; unsigned music_seed_count;   // DpMusicSeed[] from --music-index (owned by the command)
    int sega_mapper;        // 0 auto; otherwise DP_SEGA_MAPPER_* (SMS/GG only)
    int msx_mapper;         // -1 auto; otherwise DP_MSX_*
    DpSegaClosureOptions closure;
} DpSegaOptions;
DpSegaOptions dp_sega_options(void);
int dp_sega_explore_command(int argc,char **argv);
#endif
