/* music_index.c: DPMDX002 reader (see music_index.h).  The file is read once and every
 * lookup is a pointer walk over its sorted indices: CRC32 numeric, strcmp over the
 * string pool for titles, integer for game keys.  Bounds checks on load make a
 * truncated or hostile file a load error, never a later crash. */
#include "music_index.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ru32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t ru16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* the pool holds the strings; an offset must point inside it (each string is
 * NUL-terminated, so reading it as a C string stays in bounds) */
static int pool_ok(const DpMusicIndex *ix, uint32_t off) {
    return off < ix->pool_bytes;
}

int dp_midx_load(const char *path, DpMusicIndex *ix, char *err, size_t errsz) {
    memset(ix, 0, sizeof *ix);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 40) { snprintf(err, errsz, "%s: too small (%ld bytes)", path, n); fclose(f); return -1; }
    ix->buf = malloc((size_t)n);
    if (!ix->buf || fread(ix->buf, 1, (size_t)n, f) != (size_t)n) {
        snprintf(err, errsz, "%s: read failed", path);
        free(ix->buf); ix->buf = NULL; fclose(f);
        return -1;
    }
    fclose(f);
    ix->size = (size_t)n;
    if (memcmp(ix->buf, "DPMDX002", 8)) {
        snprintf(err, errsz, "%s: not a DPMDX002 index (regenerate with music_index.py export --bin)", path);
        free(ix->buf); ix->buf = NULL;
        return -1;
    }
    const uint8_t *h = ix->buf + 8;
    ix->n_crc = ru32(h); ix->n_entry = ru32(h + 4); ix->n_chunk = ru32(h + 8);
    ix->n_str = ru32(h + 12); ix->pool_bytes = ru32(h + 16);
    ix->n_title = ru32(h + 20); ix->n_game = ru32(h + 24);
    const uint8_t *p = h + 28;
    /* layout walk with cumulative bounds: every section must fit in the file */
    size_t need = (size_t)ix->n_crc * 12 + (size_t)ix->n_entry * 60 + (size_t)ix->n_chunk * 12
                + (size_t)ix->n_title * 12 + (size_t)ix->n_game * 12 + ix->pool_bytes;
    if (need > ix->size - 36) { snprintf(err, errsz, "%s: truncated index", path); free(ix->buf); ix->buf = NULL; return -1; }
    ix->crc_idx = p; p += (size_t)ix->n_crc * 12;
    ix->entries = p; p += (size_t)ix->n_entry * 60;
    ix->chunks = p; p += (size_t)ix->n_chunk * 12;
    ix->title_idx = p; p += (size_t)ix->n_title * 12;
    uint32_t trefs = 0;
    for (uint32_t i = 0; i < ix->n_title; i++) trefs += ru32(ix->title_idx + (size_t)i * 12 + 8);
    ix->title_refs = p; p += (size_t)trefs * 4;
    if ((size_t)trefs * 4 > ix->size - (size_t)(p - ix->buf)) {
        snprintf(err, errsz, "%s: truncated title refs", path); free(ix->buf); ix->buf = NULL; return -1;
    }
    ix->game_idx = p; p += (size_t)ix->n_game * 12;
    uint32_t grefs = 0;
    for (uint32_t i = 0; i < ix->n_game; i++) grefs += ru32(ix->game_idx + (size_t)i * 12 + 8);
    ix->game_refs = p; p += (size_t)grefs * 4;
    if ((size_t)grefs * 4 > ix->size - (size_t)(p - ix->buf)) {
        snprintf(err, errsz, "%s: truncated game refs", path); free(ix->buf); ix->buf = NULL; return -1;
    }
    ix->pool = (const char *)p;
    return 0;
}

void dp_midx_free(DpMusicIndex *ix) {
    free(ix->buf);
    memset(ix, 0, sizeof *ix);
}

int dp_midx_by_crc(const DpMusicIndex *ix, uint32_t crc, uint32_t *first, uint32_t *count) {
    uint32_t lo = 0, hi = ix->n_crc;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2, c = ru32(ix->crc_idx + (size_t)m * 12);
        if (c < crc) lo = m + 1; else hi = m;
    }
    if (lo == ix->n_crc || ru32(ix->crc_idx + (size_t)lo * 12) != crc) return -1;
    *first = ru32(ix->crc_idx + (size_t)lo * 12 + 4);
    *count = ru32(ix->crc_idx + (size_t)lo * 12 + 8);
    return 0;
}

int dp_midx_by_title(const DpMusicIndex *ix, const char *norm, uint32_t *first, uint32_t *count) {
    uint32_t lo = 0, hi = ix->n_title;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2;
        const char *s = ix->pool + ru32(ix->title_idx + (size_t)m * 12);
        if (strcmp(s, norm) < 0) lo = m + 1; else hi = m;
    }
    if (lo == ix->n_title) return -1;
    const uint8_t *e = ix->title_idx + (size_t)lo * 12;
    if (strcmp(ix->pool + ru32(e), norm)) return -1;
    *first = ru32(e + 4);
    *count = ru32(e + 8);
    return 0;
}

int dp_midx_by_game(const DpMusicIndex *ix, uint32_t game_key, uint32_t *first, uint32_t *count) {
    uint32_t lo = 0, hi = ix->n_game;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2, k = ru32(ix->game_idx + (size_t)m * 12);
        if (k < game_key) lo = m + 1; else hi = m;
    }
    if (lo == ix->n_game || ru32(ix->game_idx + (size_t)lo * 12) != game_key) return -1;
    *first = ru32(ix->game_idx + (size_t)lo * 12 + 4);
    *count = ru32(ix->game_idx + (size_t)lo * 12 + 8);
    return 0;
}

void dp_midx_rec(const DpMusicIndex *ix, uint32_t i, DpMusicRec *r) {
    const uint8_t *e = ix->entries + (size_t)i * 60;
    r->game_key = ru32(e);
    r->source = pool_ok(ix, ru32(e + 4)) ? ix->pool + ru32(e + 4) : "";
    r->platform = pool_ok(ix, ru32(e + 8)) ? ix->pool + ru32(e + 8) : "";
    r->format = pool_ok(ix, ru32(e + 12)) ? ix->pool + ru32(e + 12) : "";
    r->load = ru16(e + 16); r->init = ru16(e + 18); r->play = ru16(e + 20); r->stack = ru16(e + 22);
    r->tma = e[24]; r->tac = e[25];
    r->songs = ru16(e + 26);
    r->init_rom = ru32(e + 28); r->play_rom = ru32(e + 32); r->s_addr = ru32(e + 36);
    r->chunk_first = ru32(e + 40); r->chunk_count = ru32(e + 44);
    r->raw = pool_ok(ix, ru32(e + 48)) ? ix->pool + ru32(e + 48) : "";
    r->columns = pool_ok(ix, ru32(e + 52)) ? ix->pool + ru32(e + 52) : "";
    r->origin = pool_ok(ix, ru32(e + 56)) ? ix->pool + ru32(e + 56) : "";
}

uint32_t dp_midx_title_ref(const DpMusicIndex *ix, uint32_t i) {
    return ru32(ix->title_refs + (size_t)i * 4);
}

uint32_t dp_midx_game_ref(const DpMusicIndex *ix, uint32_t i) {
    return ru32(ix->game_refs + (size_t)i * 4);
}

void dp_midx_norm_title(const uint8_t *header_title, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < 16 && header_title[i] && k + 1 < cap; i++)
        if (isalnum((unsigned char)header_title[i])) out[k++] = (char)tolower((unsigned char)header_title[i]);
    out[k] = 0;
}
