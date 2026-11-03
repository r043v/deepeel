// tas.h: PC Engine tool-assisted movies as session inputs.
//  - BK2 (.bk2, BizHawk PCEHawk / Mednafen cores): a zip whose
//    "Input Log.txt" LogKey names the fields (P1 Up/Down/Left/Right/Select/
//    Run/B2/B1, the console's Reset/Power), one character per field;
//  - plain text: one frame per line, the pad byte in hex (m0boy layout).
// A frame's byte is m0boy's pad layout: bit 0 I, 1 II, 2 Select, 3 Run,
// 4 Up, 5 Right, 6 Down, 7 Left.
#ifndef DP_PCE_TAS_H
#define DP_PCE_TAS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *pads;       // one byte per frame
    uint8_t *resets;     // per frame: 1 when the movie presses Reset/Power
    uint32_t frames;
    char game[96];
    char sha1[41];
} DpPceTas;

int dp_pce_tas_load(const char *path, DpPceTas *out, char *err, size_t errsz);
void dp_pce_tas_free(DpPceTas *t);
#endif
