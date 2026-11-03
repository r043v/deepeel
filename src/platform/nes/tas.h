/* tas.h: NES tool-assisted movies as session inputs.  Two formats:
 *  - FM2 (.fm2, FCEUX): text, one "|cmd|p1|p2|...|" line per frame, the
 *    controller characters in R L D U S T B A order (S = Start, T = Select);
 *  - BK2 (.bk2, BizHawk): a zip with Header.txt (Platform, SHA1, GameName)
 *    and an "Input Log.txt" whose LogKey names the fields, one character
 *    per field, controller sections separated by '|'.
 * A frame's mask is the runtime's nes.input layout: bit 0 A, 1 B, 2 Select,
 * 3 Start, 4 Up, 5 Down, 6 Left, 7 Right.  P2 is kept when the movie
 * records it (the runtime's pad 1 has no read path yet). */
#ifndef DP_NES_TAS_H
#define DP_NES_TAS_H
#include <stddef.h>
#include <stdint.h>

typedef struct DpNesTas {
    uint8_t *masks;      // one byte per frame
    uint8_t *cmds;       // one command byte per frame (bit 1: reset)
    uint32_t frames;
    uint32_t resets;     // frames whose command byte carries a reset
    char game[96];       // the movie's game name, when the format tells it
    char source[512];
    char rom_name[128];  // the recorded ROM's file name, when recorded
    char sha1[41];       // the recorded ROM's SHA-1 (bk2), hex, when given
} DpNesTas;

// sniff and load; returns 1 on success, 0 with a human message in `err`
int dp_nes_tas_load(const char *path, DpNesTas *out, char *err, size_t errsz);
void dp_nes_tas_free(DpNesTas *t);

/* does the movie belong to this ROM?  1 same (its SHA-1 matches the whole
 * file or the file without the 16-byte iNES header), 2 different ROM, 3 the
 * movie records no SHA-1 (fm2, or a bk2 without one) */
int dp_nes_tas_identity(const DpNesTas *t, const uint8_t *rom, size_t size);

#endif
