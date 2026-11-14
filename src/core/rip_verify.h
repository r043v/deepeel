/* rip_verify.h: runtime byte-relocation of a music rip inside a ROM dump — the
 * on-demand half of the .verified sidecars.  Given a rip file (GBS/NSF/NSFE/
 * HES/KSS) and the dump being explored, locate the engine block by fragment
 * vote (16-byte fragments, dominant delta, the same evidence the batch tool
 * and dp_gbs_verify produce) and map the entry points into ROM space.  No
 * preparation: ROM + rip is enough; the sidecar written by the caller makes
 * the next index build cache the mapping. */
#ifndef DP_RIP_VERIFY_H
#define DP_RIP_VERIFY_H
#include <stddef.h>
#include <stdint.h>

#define DP_RIP_NONE 0xFFFFFFFFu   /* an entry point the verifier could not map */

typedef struct {
    uint32_t init_rom, play_rom;   // dp_rom_index (physical offset), 0xFFFFFFFF = unmapped
    uint32_t load;                 // rip load address (0xFFFFFFFF when the format has none)
    size_t data_size;              // rip data block size
} DpRipMap;

/* Relocate `rip` inside `rom`.  Returns 0 with `out` filled (the entry points
 * are only set when their own bytes relocated verbatim — a rip head that is
 * the ripper's stub maps nothing), -1 when the block is not found. */
int dp_rip_verify(const uint8_t *rip, size_t rip_size, const uint8_t *rom, size_t rom_size,
                  DpRipMap *out);

#endif