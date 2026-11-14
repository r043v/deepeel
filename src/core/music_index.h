/* music_index.h: C reader of the DPMDX002 music-rip index (tools/music_index.py `export
 * --bin`, layout documented in its writer docstring).  The index merges the BAB ripper
 * databases and local/joshw rips; each entry carries its rip header projection plus the
 * entry points precomputed into ROM space (init_rom/play_rom, dp_rom_index convention
 * bank * 0x4000 + offset).  Lookups: ROM dump CRC32 (exact dump), normalized title
 * (ASCII, ROM header $0134), and game identity (game_key: one game = all its dumps,
 * formats and regions, the cross-region tier).  Single fread, pointer walks, no
 * dependency; the index is a local oracle produced out of the repository. */
#ifndef DP_MUSIC_INDEX_H
#define DP_MUSIC_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;                    // whole file, malloc'd
    size_t size;
    uint32_t n_crc, n_entry, n_chunk, n_str, pool_bytes, n_title, n_game;
    const uint8_t *crc_idx;          // n_crc * 12: { u32 crc, first, count }
    const uint8_t *entries;          // n_entry * 60
    const uint8_t *chunks;           // n_chunk * 12: { u32 rom, dest, len }
    const uint8_t *title_idx;        // n_title * 12: { u32 str_off, first, count }
    const uint8_t *title_refs;       // sum(title counts) * 4: entry indices
    const uint8_t *game_idx;         // n_game * 12: { u32 key, first, count }
    const uint8_t *game_refs;        // sum(game counts) * 4: entry indices
    const char *pool;
} DpMusicIndex;

/* decoded view of one 60-byte entry record */
typedef struct {
    uint32_t game_key;               // 0xFFFFFFFF = unlinked
    const char *source, *platform, *format;
    uint16_t load, init, play, stack; // 0xFFFF = absent
    uint8_t tma, tac;                // 0xFF = absent
    uint16_t songs;
    uint32_t init_rom, play_rom;     // dp_rom_index, 0xFFFFFFFF = absent
    uint32_t s_addr;                 // verbatim convention, do not interpret
    uint32_t chunk_first, chunk_count;
    const char *raw, *columns, *origin;
} DpMusicRec;

/* Load and validate an index file.  Returns 0 on success, -1 with err set otherwise. */
int dp_midx_load(const char *path, DpMusicIndex *ix, char *err, size_t errsz);
void dp_midx_free(DpMusicIndex *ix);

/* Ranges over the entry array; return 0 and set *first/*count when found, -1 when absent. */
int dp_midx_by_crc(const DpMusicIndex *ix, uint32_t crc, uint32_t *first, uint32_t *count);
int dp_midx_by_title(const DpMusicIndex *ix, const char *norm, uint32_t *first, uint32_t *count);
int dp_midx_by_game(const DpMusicIndex *ix, uint32_t game_key, uint32_t *first, uint32_t *count);

/* Decode the i-th entry (i < n_entry, or any index from a lookup range). */
void dp_midx_rec(const DpMusicIndex *ix, uint32_t i, DpMusicRec *r);

/* i-th entry index of the title/game refs (i < sum of the group counts).  A lookup
 * range (first, count) indexes these arrays, not the entry array directly. */
uint32_t dp_midx_title_ref(const DpMusicIndex *ix, uint32_t i);
uint32_t dp_midx_game_ref(const DpMusicIndex *ix, uint32_t i);

/* ASCII alnum-lowercase norm of the 16-byte GB header title (matches title_norm). */
void dp_midx_norm_title(const uint8_t *header_title, char *out, size_t cap);

#endif
