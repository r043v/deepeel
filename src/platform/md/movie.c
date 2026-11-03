#include "movie.h"
#include "../../core/zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dp_md_movie_load(const char *path, DpMdMovie *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open movie"); return 0; }
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0) { fclose(f); snprintf(err, errsz, "cannot size movie"); return 0; }
    long n = ftell(f);
    if (n > 64 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) { fclose(f); snprintf(err, errsz, "movie too large"); return 0; }
    uint8_t *data = malloc((size_t)n + 1);
    if (!data) { fclose(f); snprintf(err, errsz, "out of memory"); return 0; }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); snprintf(err, errsz, "cannot read movie"); return 0; }
    fclose(f);
    size_t size = (size_t)n;
    if (dp_zip_is(data, size)) {
        size_t len = 0;
        uint8_t *raw = dp_zip_read(data, size, ".gmv", &len, NULL);
        free(data); data = raw; size = len;
        if (!data) { snprintf(err, errsz, "ZIP has no readable GMV"); return 0; }
    }
    if (size > 64u * 1024u * 1024u || size < 64 || memcmp(data, "Gens Movie ", 11) || (size - 64) % 3 || size == 64) {
        free(data); snprintf(err, errsz, "invalid GMV header or frame data"); return 0;
    }
    /* A 6-button pad answers the 3-button protocol (two TH phases) with the
     * same eight bits; X/Y/Z/Mode (frame byte 2) are only visible to a game
     * that runs the third TH cycle, which the replayed pad does not model. */
    int six = data[20] == '6' || data[21] == '6';
    if ((data[20] != '3' && data[20] != '6') || (data[21] != '3' && data[21] != '6') ||
        (data[15] >= 'A' && (data[22] & 0xE0))) {
        free(data); snprintf(err, errsz, "GMV requires unsupported controllers, PAL, or savestate"); return 0;
    }
    out->frames = (uint32_t)((size - 64) / 3);
    out->pads = malloc(out->frames);
    if (!out->pads) { free(data); snprintf(err, errsz, "out of memory"); return 0; }
    for (uint32_t i = 0; i < out->frames; i++) {
        if (data[64 + 3 * i + 1] != 0xFF || (!six && data[64 + 3 * i + 2] != 0xFF)) {
            free(data); dp_md_movie_free(out); snprintf(err, errsz, "GMV uses unsupported additional input"); return 0;
        }
        out->pads[i] = (uint8_t)~data[64 + 3 * i];
    }
    free(data);
    return 1;
}
void dp_md_movie_free(DpMdMovie *movie) { free(movie->pads); memset(movie, 0, sizeof *movie); }
