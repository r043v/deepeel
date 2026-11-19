#include "dpmovie.h"
#include "dpmachine.h"
#include "../platform/nes/tas.h"
#include "../platform/pce/tas.h"
#include "../platform/md/movie.h"
#include <stdlib.h>
#include <string.h>

uint32_t dp_movie_nes_pad(uint8_t b) {   // NES pad: A B Select Start U D L R = bits 0..7
    uint32_t k = 0;
    if (b & 0x01) k |= DP_IN_B1; if (b & 0x02) k |= DP_IN_B2; if (b & 0x08) k |= DP_IN_START; if (b & 0x04) k |= DP_IN_SELECT;
    if (b & 0x10) k |= DP_IN_UP; if (b & 0x20) k |= DP_IN_DOWN; if (b & 0x40) k |= DP_IN_LEFT; if (b & 0x80) k |= DP_IN_RIGHT;
    return k;
}
uint32_t dp_movie_pce_pad(uint8_t b) {   // PCE pad: I II Select Run U R D L
    uint32_t k = 0;
    if (b & 0x01) k |= DP_IN_B1; if (b & 0x02) k |= DP_IN_B2; if (b & 0x08) k |= DP_IN_START; if (b & 0x04) k |= DP_IN_SELECT;
    if (b & 0x10) k |= DP_IN_UP; if (b & 0x40) k |= DP_IN_DOWN; if (b & 0x80) k |= DP_IN_LEFT; if (b & 0x20) k |= DP_IN_RIGHT;
    return k;
}
uint32_t dp_movie_md_pad(uint8_t b) {   // GMV / BK2: 0-3 directions, 16 A, 32 B, 64 C, 128 Start
    uint32_t k = b & 0x0F;
    if (b & 0x10) k |= DP_IN_B1;   // C12: A and C stay distinct buttons
    if (b & 0x20) k |= DP_IN_B2;
    if (b & 0x40) k |= DP_IN_C;
    if (b & 0x80) k |= DP_IN_START;
    return k;
}

static int convert(const uint8_t *src, uint32_t n, uint32_t (*fn)(uint8_t), DpMovie *out) {
    out->masks = malloc((n ? n : 1) * sizeof *out->masks);
    if (!out->masks) return 0;
    for (uint32_t f = 0; f < n; f++) out->masks[f] = fn(src[f]);
    out->frames = n;
    return 1;
}

int dp_movie_load(const char *plat, const char *path, DpMovie *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);
    int ok = 0;
    if (!strcmp(plat, "nes")) {
        DpNesTas t; memset(&t, 0, sizeof t);
        if (dp_nes_tas_load(path, &t, err, errsz)) ok = convert(t.masks, t.frames, dp_movie_nes_pad, out);
        dp_nes_tas_free(&t);
    } else if (!strcmp(plat, "pce")) {
        DpPceTas t; memset(&t, 0, sizeof t);
        if (dp_pce_tas_load(path, &t, err, errsz)) ok = convert(t.pads, t.frames, dp_movie_pce_pad, out);
        dp_pce_tas_free(&t);
    } else if (!strcmp(plat, "md")) {
        DpMdMovie t; memset(&t, 0, sizeof t);
        if (dp_md_movie_load(path, &t, err, errsz)) {
            ok = convert(t.pads, t.frames, dp_movie_md_pad, out);
            // two-player movie: pad 2 rides in bits 16-23 of the mask, bit 31 = second pad plugged in
            if (ok && t.pads2) for (uint32_t f = 0; f < t.frames; f++) out->masks[f] |= (dp_movie_md_pad(t.pads2[f]) << DP_IN_PORT2) | 0x80000000u;
        }
        dp_md_movie_free(&t);
    } else if (err && errsz) { strncpy(err, "unknown platform", errsz - 1); err[errsz - 1] = 0; }
    return ok;
}

void dp_movie_free(DpMovie *m) { free(m->masks); memset(m, 0, sizeof *m); }
