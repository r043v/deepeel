/* tas.h: tool-assisted movie files as session inputs.  Three formats:
 *  - VBM (.vmv, VBA/VBA-M): binary "VBM\x1A" header and the older text
 *    "VBM_MOVIE v1" header, the bulk of the classic GB TASes;
 *  - BK2 (.bk2, BizHawk): zip (sometimes nested) with an "Input Log.txt";
 *  - plain input log (a .txt whose lines start with '|', e.g. an extracted
 *    bk2 log or a .bkm log).
 * The loaded movie is one mask byte per frame in the memscan joypad layout
 * (a=1 b=2 select=4 start=8 right=16 left=32 up=64 down=128). */
#ifndef MS_TAS_H
#define MS_TAS_H

#include <stddef.h>
#include <stdint.h>

typedef struct MsTas {
    uint8_t *masks;   // one byte per frame
    uint32_t frames;
    int anchored;     // 1 = the movie starts from a savestate (replay from
                      // power-on is best-effort, not frame-exact)
    char source[512]; // file path
} MsTas;

// sniff and load any supported format; returns 1 on success, 0 with a human
// message in `err` otherwise
int ms_tas_load(const char *path, MsTas *out, char *err, size_t errsz);
void ms_tas_free(MsTas *t);

#endif
