#define _GNU_SOURCE
#include "movie.h"
#include "../../core/zip.h"
#include "../../core/bk2.h"
#include "../../core/bizhawk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* BK2 Input Log: "|pr|UDLRABCSXYZM|...|" per frame; pad byte as in GMV (bit0 U, 1 D, 2 L,
 * 3 R, 4 B, 5 C, 6 A, 7 Start).  X/Y/Z/Mode are not modelled by the 3-button replay. */
static int bk2_parse_positional(const uint8_t *log, size_t n, const DpBk2 *bk,
                     DpMdMovie *out, char *err, size_t errsz) {
    (void)bk;   /* the identity lines live in DpBk2 (dp_md_movie_rom_match reads the movie header); the MD5 the replay checks comes from the movie's own header below */
    if (bk && bk->has_md5) { memcpy(out->md5, bk->md5, 16); out->has_md5 = 1; }
    uint32_t cap = 1024; out->pads = malloc(cap);
    if (!out->pads) { snprintf(err, errsz, "out of memory"); return 0; }
    for (size_t i = 0; i < n;) {
        size_t e = i; while (e < n && log[e] != '\n') e++;
        if (log[i] == '|' && e - i >= 16) {
            const uint8_t *p = log + i + 4; /* after "|pr|" */
            static const int bit[8] = {0, 1, 2, 3, 6, 4, 5, 7}; /* U D L R A B C Start */
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) if (p[k] != '.') v |= (uint8_t)(1u << bit[k]);
            if (out->frames == cap) {
                cap *= 2; uint8_t *np = realloc(out->pads, cap);
                if (!np) { snprintf(err, errsz, "out of memory"); return 0; }
                out->pads = np;
            }
            out->pads[out->frames++] = v;
        }
        i = e + 1;
    }
    if (!out->frames) { dp_md_movie_free(out); snprintf(err, errsz, "BK2 has no input frames"); return 0; }
    return 1;
}

/* LogKey movies: the shared BizHawk reader resolves the columns by name; logs without them keep the positional grammar */
static int bk2_parse(const uint8_t *log, size_t n, const DpBk2 *bk, DpMdMovie *out, char *err, size_t errsz) {
    DpBzMovie m;
    if (!memmem(log, n, "LogKey:", 7) || !dp_bz_load(log, n, &m, err, errsz))
        return bk2_parse_positional(log, n, bk, out, err, errsz);
    static const char *const nm[8] = { "P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 B", "P1 C", "P1 A", "P1 Start" };
    int col[8], found = 0;
    for (int i = 0; i < 8; i++) { col[i] = dp_bz_find(&m, nm[i]); found += col[i] >= 0; }
    if (!found) { dp_bz_free(&m); return bk2_parse_positional(log, n, bk, out, err, errsz); }
    if (bk && bk->has_md5) { memcpy(out->md5, bk->md5, 16); out->has_md5 = 1; }
    out->pads = malloc(m.nframes ? m.nframes : 1);
    if (!out->pads || !m.nframes) { dp_bz_free(&m); snprintf(err, errsz, "BK2 has no input frames"); return 0; }
    for (uint32_t f = 0; f < m.nframes; f++) {
        uint8_t v = 0;   /* bit0 U, 1 D, 2 L, 3 R, 4 B, 5 C, 6 A, 7 Start */
        for (int i = 0; i < 8; i++) if (dp_bz_held(&m, f, col[i])) v |= (uint8_t)(1u << i);
        out->pads[f] = v;
    }
    out->frames = m.nframes;
    dp_bz_free(&m);
    return 1;
}

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
        DpBk2 b;
        size_t glen = 0;
        uint8_t *gprobe = dp_zip_read(data, size, ".gmv", &glen, NULL);   // a zip holding a Gens movie is not a bk2
        int is_gmv = gprobe != NULL;
        free(gprobe);
        if (is_gmv) { memset(&b, 0, sizeof b); }
        else if (!dp_bk2_open(data, size, &b, err, errsz)) { /* err already set */ }
        else if (b.log && b.log_n) { /* BizHawk BK2: container handled by the shared kernel (src/core/bk2.c) */
            free(data);
            int ok = bk2_parse(b.log, b.log_n, &b, out, err, errsz);
            dp_bk2_free(&b);
            return ok;
        }
        if (!is_gmv) dp_bk2_free(&b);
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
