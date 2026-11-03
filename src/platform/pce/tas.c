// tas.c: PC Engine movie loading (see tas.h).
#include "tas.h"
#include "../../core/zip.h"
#include "../../core/sha1.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int grow(DpPceTas *t, uint32_t *cap) {
    if (t->frames < *cap) return 1;
    *cap = *cap ? *cap * 2 : 8192;
    uint8_t *p = realloc(t->pads, *cap), *r;
    if (!p) return 0;
    t->pads = p;
    r = realloc(t->resets, *cap);
    if (!r) return 0;
    t->resets = r;
    return 1;
}

static int key_bit(const char *k, int *reset) {
    *reset = 0;
    if (!strcmp(k, "Reset") || !strcmp(k, "Power")) { *reset = 1; return -1; }
    if (strncmp(k, "P1 ", 3)) return -1;
    k += 3;
    if (!strcmp(k, "B1") || !strcmp(k, "I")) return 0;
    if (!strcmp(k, "B2") || !strcmp(k, "II")) return 1;
    if (!strcmp(k, "Select")) return 2;
    if (!strcmp(k, "Run")) return 3;
    if (!strcmp(k, "Up")) return 4;
    if (!strcmp(k, "Right")) return 5;
    if (!strcmp(k, "Down")) return 6;
    if (!strcmp(k, "Left")) return 7;
    return -1;
}

static int load_bk2_log(char *text, DpPceTas *t, char *err, size_t errsz) {
    int bits[64], rst[64], nf = 0;
    char *lk = strstr(text, "LogKey:");
    if (!lk) { snprintf(err, errsz, "bk2: no LogKey line"); return 0; }
    lk += 7;
    char *kend = strchr(lk, '\n');
    if (!kend) kend = lk + strlen(lk);
    while (lk < kend && nf < 64) {
        while (lk < kend && (*lk == '#' || *lk == '|')) lk++;
        if (lk >= kend) break;
        char *e = lk;
        while (e < kend && *e != '|' && *e != '\r') e++;
        char key[32];
        size_t len = (size_t)(e - lk);
        if (len >= sizeof key) len = sizeof key - 1;
        memcpy(key, lk, len);
        key[len] = 0;
        bits[nf] = key_bit(key, &rst[nf]);
        nf++;
        lk = e;
        if (lk < kend && *lk == '\r') break;
    }
    uint32_t cap = 0;
    for (char *s = text; (s = strchr(s, '\n'));) {
        s++;
        if (*s != '|') continue;
        if (!grow(t, &cap)) { snprintf(err, errsz, "bk2: out of memory"); return 0; }
        uint8_t pad = 0, reset = 0;
        char *q = s + 1;
        for (int fi = 0; fi < nf && *q && *q != '\n' && *q != '\r'; fi++) {
            if (*q == '|') q++;
            char c = *q++;
            int on = c != '.' && c != ' ';
            if (on && bits[fi] >= 0) pad |= (uint8_t)(1u << bits[fi]);
            if (on && rst[fi]) reset = 1;
        }
        t->pads[t->frames] = pad;
        t->resets[t->frames] = reset;
        t->frames++;
    }
    if (!t->frames) { snprintf(err, errsz, "bk2: no input frames"); return 0; }
    return 1;
}

int dp_pce_tas_load(const char *path, DpPceTas *t, char *err, size_t errsz) {
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = sz > 0 ? malloc((size_t)sz + 1) : NULL;
    if (!blob || fread(blob, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(blob);
        snprintf(err, errsz, "cannot read %s", path);
        return 0;
    }
    fclose(f);
    blob[sz] = 0;
    int ok = 0;
    if (sz > 4 && blob[0] == 'P' && blob[1] == 'K') {
        size_t logn = 0, hdrn = 0;
        char name[128];
        uint8_t *log = dp_zip_read(blob, (size_t)sz, "input log", &logn, name);
        uint8_t *hdr = dp_zip_read(blob, (size_t)sz, "header.txt", &hdrn, name);
        if (!log) snprintf(err, errsz, "bk2: no Input Log.txt inside");
        else {
            char *text = malloc(logn + 1);
            if (text) {
                memcpy(text, log, logn);
                text[logn] = 0;
                ok = load_bk2_log(text, t, err, errsz);
                free(text);
            }
        }
        if (ok && hdr) {
            char *ht = malloc(hdrn + 1);
            if (ht) {
                memcpy(ht, hdr, hdrn);
                ht[hdrn] = 0;
                char *g = strstr(ht, "GameName ");
                if (g) snprintf(t->game, sizeof t->game, "%.*s", (int)strcspn(g + 9, "\r\n"), g + 9);
                char *h = strstr(ht, "SHA1 ");
                if (h) snprintf(t->sha1, sizeof t->sha1, "%.40s", h + 5);
                free(ht);
            }
        }
        free(log);
        free(hdr);
    } else {
        // plain text: a hex pad byte per line ('#' comments, 'R' prefix: reset)
        uint32_t cap = 0;
        ok = 1;
        for (char *s = (char *)blob; *s;) {
            char *e = strchr(s, '\n');
            if (!e) e = s + strlen(s);
            while (s < e && isspace((unsigned char)*s)) s++;
            if (s < e && *s != '#') {
                if (!grow(t, &cap)) { ok = 0; snprintf(err, errsz, "out of memory"); break; }
                int reset = (*s == 'R' || *s == 'r');
                if (reset) s++;
                char *end;
                unsigned long v = strtoul(s, &end, 16);
                if (end == s && !reset) { ok = 0; snprintf(err, errsz, "unknown movie format"); break; }
                t->pads[t->frames] = (uint8_t)v;
                t->resets[t->frames] = (uint8_t)reset;
                t->frames++;
            }
            s = *e ? e + 1 : e;
        }
        if (ok && !t->frames) { ok = 0; snprintf(err, errsz, "no input frames"); }
    }
    free(blob);
    if (!ok) dp_pce_tas_free(t);
    return ok;
}

void dp_pce_tas_free(DpPceTas *t) {
    if (!t) return;
    free(t->pads);
    free(t->resets);
    t->pads = t->resets = NULL;
}

static void pce_sha1_hex(const uint8_t *d, size_t n, char out[41]) {
    uint8_t h[20];
    dp_sha1(d, n, h);
    for (int i = 0; i < 20; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

int dp_pce_tas_identity(const DpPceTas *t, const uint8_t *rom, size_t size) {
    if (!t || !t->sha1[0]) return 3;
    char want[41], a[41], b[41];
    size_t n = 0;
    for (; t->sha1[n] && n < 40; n++) want[n] = (char)tolower((unsigned char)t->sha1[n]);
    want[n] = 0;
    pce_sha1_hex(rom, size, a);
    if (!strcmp(want, a)) return 1;
    if (size > 512 && size % 1024 == 512) { pce_sha1_hex(rom + 512, size - 512, b); if (!strcmp(want, b)) return 1; }
    return 2;
}
