/* gbs.h: Game Boy Sound Format (GBS) rip parsing and verification against a loaded ROM.
 * A GBS holds the code+data block of a game's music engine with three confirmed entry
 * points (load/init/play, 112-byte header).  Per-rip-bank best-match scoring re-locates
 * the block in the original ROM (rips are patched and trimmed) and maps init/play back
 * to {bank, address} seeds the explorer can run. */
#ifndef DP_GBS_H
#define DP_GBS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned version, songs, first;
    uint16_t load, init, play;
    uint8_t sp, tma_tac;
    char title[33], author[33], copyright[33];
    const uint8_t *data;   // into the caller's buffer: everything past the header
    size_t data_size;
} DpGbs;

#define DP_GBS_MAX_BANKS 256   // rip banks the verifier maps (a music rip is a handful)

typedef struct {
    unsigned bank_base;        // virtual bank of rip bank 0 (load >> 14)
    unsigned rip_banks;        // rip banks carrying data (0..rip_banks-1 below)
    struct {
        int matched;           // best score reached the threshold
        unsigned rom_bank, best, second;   // equal-byte scores of the two best ROM banks
        size_t bytes;          // bytes compared for this rip bank
    } bank[DP_GBS_MAX_BANKS];
    int init_bank, play_bank;  // mapped ROM banks, -1 when unmapped
    uint16_t init_addr, play_addr;
    /* fragment relocation (the fallback when no rip bank is byte-identical: rips are
     * often a contiguous extract of the engine from mid-bank, with ripper patches) */
    int frag_scanned;          // the fragment scan ran (no aligned bank matched)
    unsigned frag_total, frag_matched;   // 16-byte fragments searched / found in the ROM
    size_t frag_dom_delta;     // dominant rom-offset - rip-offset delta
    unsigned frag_dom_votes;   // fragments sharing it
    unsigned init_votes, play_votes;     // local agreement for each mapped pc (0 = unmapped)
} DpGbsMatch;

/* parse the 112-byte header; data points into buf; 0 ok, -1 bad (err filled) */
int dp_gbs_parse(const uint8_t *buf, size_t n, DpGbs *g, char *err, size_t errsz);

/* locate the rip in the ROM and map init/play to {bank, address} seeds.
 * Path A: per-rip-bank best match (verbatim rips); min_ratio is the fraction of equal
 *   bytes a rip bank needs on its best ROM bank.
 * Path B (no bank matched): exact 16-byte fragments are searched in the ROM; init/play
 *   are mapped by a local vote of the fragments surrounding their data offset (>= 3
 *   agreeing votes, majority of the nearby matches).
 * Returns the number of aligned-matched rip banks (0 when only path B ran); the seeds
 * are in init_bank/init_addr and play_bank/play_addr either way (-1 = unmapped). */
unsigned dp_gbs_verify(const DpGbs *g, const uint8_t *rom, size_t rom_size,
                       double min_ratio, DpGbsMatch *m);

/* lowercase alphanumeric run of `in` written to out (site names vs ROM titles) */
void dp_gbs_norm_name(const char *in, char *out, size_t cap);

/* decode %XX escapes of an href (Apache index listings) */
void dp_gbs_url_decode(const char *in, size_t n, char *out, size_t cap);

/* pick the *.7z href of an Apache index page whose decoded, normalized name contains
 * the normalized `name` (exact normalized equality wins, then the shortest containment);
 * writes the raw href (still URL-encoded, ready for download) to out.
 * Returns the href length, 0 when nothing matches. */
size_t dp_gbs_listing_pick(const char *html, const char *name, char *out, size_t cap);

#endif
