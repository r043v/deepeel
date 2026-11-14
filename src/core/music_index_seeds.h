/* music_index_seeds.h: platform-aware seed extraction from a DPMDX002
 * music-rip index (see music_index.h, built by tools/music_index.py).
 *
 * Identity tiers over one ROM dump, strongest first:
 * 1. CRC32 of the raw dump file (the ripper databases record dump CRCs);
 * 2. normalized cartridge title (ASCII sources only — the Japanese BAB titles
 *    normalize to nothing; only GB/SMS/GG even have a header title field);
 * 3. game identity: the game_key siblings of every hit, i.e. all dumps,
 *    regions and formats of the same game (weaker evidence, named per seed).
 *
 * Every entry found contributes its precomputed init_rom/play_rom (the
 * dp_rom_index convention bank * 0x4000 + (addr & 0x3FFF)) as entry-point
 * seeds with the rip's calling convention: the init call carries the song
 * number in A and starts on the rip's stack; play needs no register.  The
 * pc/bank conversion follows the 16 KiB window convention (bank 0 maps
 * $0000-$3FFF directly, other banks the $4000 window) shared by GB and the
 * Z80 platforms; other platforms convert from `rom` themselves.  The caller
 * validates each seed against its own mapper view — a projection from another
 * dump's banking is refused there, counted, never silent. */
#ifndef DP_MUSIC_INDEX_SEEDS_H
#define DP_MUSIC_INDEX_SEEDS_H

#include <stddef.h>
#include <stdint.h>
#include "music_index.h"

typedef struct DpMusicSeed {
    uint32_t rom;      // dp_rom_index of the entry point
    uint16_t pc, bank; // cpu view, 16 KiB window convention (see header comment)
    uint16_t sp;       // the rip's stack pointer (0xFFFF: keep the start state's)
    uint8_t a;         // song number for init calls (0xFF: none, i.e. play)
    uint8_t kind;      // 0 play, 1 init
    uint8_t tier;      // 1 crc, 2 title, 3 game identity
    uint32_t entry;    // entry position in the index
} DpMusicSeed;

typedef struct DpMusicSeedStats {
    uint32_t crc;
    char title[32];
    int tier;               // highest tier that matched (0: nothing)
    int entries;            // index entries found for this dump
    int seeds, init_seeds;  // entry points queued (deduped, in range)
    int other_platform;     // hits on other platforms than the requested ones
    int no_entry_point;     // entries without any projected init_rom/play_rom
    int out_of_range;       // entries whose only points land outside this dump
} DpMusicSeedStats;

/* Collects the seeds for one dump; `plats` are the platform names to accept
 * ("GB", "GG", "MK3", "SCSG", "MSX", "NES", "PCE").  `norm_title` may be NULL
 * or empty (tier 2 then never matches).  Returns a malloc'd array the caller
 * frees (NULL when nothing matched; *st is always filled), 0 on success,
 * -1 on allocation failure. */
int dp_music_seeds(const DpMusicIndex *ix, const char *const *plats, size_t nplat,
                   uint32_t crc, const char *norm_title, size_t rom_size,
                   DpMusicSeed **out, size_t *nout, DpMusicSeedStats *st);

/* CLI glue shared by every platform command that takes --music-index (the
 * per-platform code above it is the extractor, this is the one loading +
 * reporting path they all run): resolves the default index path, loads it
 * when present, hashes the raw dump file exactly as the databases record it,
 * runs the tier walk and prints the shared report line.  Arguments:
 * `midx_file` is the --music-index path (NULL when the option is absent —
 * the default path is then optional, "" asks for the default path
 * explicitly); `raw`/`raw_bytes` are the dump file bytes the CRC is taken
 * over; `rom_size` is the ROM image size the seeds must fit; `norm_title`
 * may be NULL (tier 2 off); `keep_ix` optionally receives the loaded index
 * (caller calls dp_midx_free, e.g. to print per-seed provenance).  Returns
 * the seed count with *out malloc'd (caller frees), or: */
#define DP_MUSIC_SEEDS_NO_FILE  (-1)  /* option absent and the default index missing: skip silently */
#define DP_MUSIC_SEEDS_LOAD_ERR (-2)  /* the index failed to load: printed; hard when the path was explicit */
#define DP_MUSIC_SEEDS_ALLOC    (-3)  /* allocation failure: printed; always hard */

/* name of an identity tier, for the per-platform reports (0: nothing matched) */
const char *dp_music_seeds_tier_name(int tier);
int dp_music_seeds_cli(const char *midx_file, const char *const *plats, size_t nplat,
                       const uint8_t *raw, size_t raw_bytes, size_t rom_size,
                       const char *norm_title, DpMusicSeed **out, size_t *nout,
                       DpMusicSeedStats *st, DpMusicIndex *keep_ix);

#endif
