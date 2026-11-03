#include "tas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../../../src/core/zip.h"
#include "../../../src/core/sha1.h"

/* fm2 controller characters, in the movie's column order, to nes.input
 * bits (A=1 B=2 Select=4 Start=8 Up=16 Down=32 Left=64 Right=128):
 * R L D U S T B A with S = Start, T = Select */
static const struct { char ch; uint8_t bit; } fm2_cols[8] = {
    { 'R', 0x80 }, { 'L', 0x40 }, { 'D', 0x20 }, { 'U', 0x10 },
    { 'S', 0x08 }, { 'T', 0x04 }, { 'B', 0x02 }, { 'A', 0x01 },
};

static int fm2_mask(const char *field) {
    uint8_t mask = 0;
    for (int i = 0; i < 8 && field[i]; i++) {
        if (field[i] == '.') continue;
        for (int j = 0; j < 8; j++)
            if (toupper((unsigned char)field[i]) == fm2_cols[j].ch) mask |= fm2_cols[j].bit;
    }
    return mask;
}

static int tas_load_fm2(FILE *f, DpNesTas *t, char *err, size_t errsz) {
    char line[1024];
    uint32_t cap = 0;
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '|') break;
        /* header: keep the recorded ROM's name when present */
        char *v = strstr(line, "romFilename ");
        if (v) {
            v += 12;
            size_t n = strcspn(v, "\r\n");
            if (n >= sizeof t->rom_name) n = sizeof t->rom_name - 1;
            memcpy(t->rom_name, v, n);
            t->rom_name[n] = 0;
        }
    }
    while (line[0] == '|') {
        /* |cmd|p1|p2|...|: split on '|' so empty controller fields parse */
        char *tok[4] = { 0 };
        int ntok = 0;
        for (char *p = line + 1; ntok < 4; ntok++) {
            tok[ntok] = p;
            char *e = strchr(p, '|');
            if (!e) break;
            *e = 0;
            p = e + 1;
        }
        if (ntok < 3) { snprintf(err, errsz, "fm2: malformed frame line"); return 0; }
        unsigned cmd = (unsigned)strtoul(tok[0], NULL, 16);
        if (t->frames == cap) {
            cap = cap ? cap * 2 : 8192;
            uint8_t *nm = realloc(t->masks, cap);
            uint8_t *nc = realloc(t->cmds, cap);
            if (!nm || !nc) { free(nm); free(nc); snprintf(err, errsz, "fm2: out of memory"); return 0; }
            t->masks = nm; t->cmds = nc;
        }
        uint8_t mask = (uint8_t)fm2_mask(tok[1]);
        if (ntok >= 4) mask |= (uint8_t)(fm2_mask(tok[2]) >> 4); /* pad 2 parked high */
        t->masks[t->frames] = mask;
        t->cmds[t->frames] = (uint8_t)cmd;
        t->frames++;
        if (cmd & 2) t->resets++;
        if (!fgets(line, sizeof line, f)) break;
    }
    if (!t->frames) { snprintf(err, errsz, "fm2: no input frames"); return 0; }
    return 1;
}

/* bk2: LogKey names the fields in order, one character per field per frame
 * line; controller sections are separated by '|', so a section's fields are
 * adjacent characters with no separator between them. */
static int tas_load_bk2(const uint8_t *log, size_t logn, DpNesTas *t,
                        char *err, size_t errsz) {
    char key[40][24];
    int nf = 0;
    char *text = malloc(logn + 1);
    if (!text) return 0;
    memcpy(text, log, logn);
    text[logn] = 0;
    char *lk = strstr(text, "LogKey:");
    if (!lk) { free(text); snprintf(err, errsz, "bk2: no LogKey line"); return 0; }
    lk += 7;
    char *kend = strchr(lk, '\n');
    if (!kend) kend = lk + strlen(lk);
    while (lk < kend && nf < 40) {
        if (*lk == '#' || *lk == '|') lk++;
        char *e = memchr(lk, '|', (size_t)(kend - lk));
        size_t len = e ? (size_t)(e - lk) : (size_t)(kend - lk);
        if (len == 0) break;
        if (len >= sizeof key[0]) len = sizeof key[0] - 1;
        memcpy(key[nf], lk, len);
        key[nf][len] = 0;
        nf++;
        lk = e ? e + 1 : kend;
    }
    /* frame lines: fields consume one character each, a '|' between fields
     * is skipped when present */
    uint32_t cap = 0;
    char *s = text;
    while ((s = strchr(s, '\n'))) {
        s++;
        if (*s != '|') continue;
        if (t->frames == cap) {
            cap = cap ? cap * 2 : 8192;
            uint8_t *nm = realloc(t->masks, cap);
            uint8_t *nc = realloc(t->cmds, cap);
            if (!nm || !nc) { free(nm); free(nc); free(text); snprintf(err, errsz, "bk2: out of memory"); return 0; }
            t->masks = nm; t->cmds = nc;
        }
        uint8_t mask = 0;
        char *q = s + 1;
        for (int fi = 0; fi < nf && *q && *q != '\n'; fi++) {
            char c = *q++;
            if (!strncmp(key[fi], "P1 ", 3)) {
                const char *n = key[fi] + 3;
                int bit = !strcmp(n, "A") ? 0 : !strcmp(n, "B") ? 1 :
                          !strcmp(n, "Select") ? 2 : !strcmp(n, "Start") ? 3 :
                          !strcmp(n, "Up") ? 4 : !strcmp(n, "Down") ? 5 :
                          !strcmp(n, "Left") ? 6 : 7;
                if (c != '.' && c != ' ') mask |= (uint8_t)(1u << bit);
            }
            if (*q == '|') q++;
        }
        t->masks[t->frames] = mask;
        t->cmds[t->frames] = 0;
        t->frames++;
    }
    free(text);
    if (!t->frames) { snprintf(err, errsz, "bk2: no input frames"); return 0; }
    return 1;
}

int dp_nes_tas_load(const char *path, DpNesTas *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);
    snprintf(out->source, sizeof out->source, "%s", path);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open %s", path); return 0; }
    uint8_t head[512];
    size_t hn = fread(head, 1, sizeof head, f);
    int ok = 0;
    if (hn > 4 && head[0] == 'P' && head[1] == 'K') { /* a zip: a bk2 */
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *blob = malloc((size_t)sz);
        if (!blob || fread(blob, 1, (size_t)sz, f) != (size_t)sz) {
            free(blob);
            snprintf(err, errsz, "bk2: cannot read %s", path);
        } else {
            size_t logn = 0, hdrn = 0;
            char name[128];
            uint8_t *log = dp_zip_read(blob, (size_t)sz, "input log", &logn, name);
            uint8_t *hdr = dp_zip_read(blob, (size_t)sz, "header.txt", &hdrn, name);
            if (!log) {
                snprintf(err, errsz, "bk2: no Input Log.txt inside");
            } else if (!tas_load_bk2(log, logn, out, err, errsz)) {
                /* err already set */
            } else {
                ok = 1;
                if (hdr) {
                    /* Header.txt is text but not NUL-terminated: search
                     * with length bounds */
                    size_t tl = hdrn;
                    char *ht = malloc(tl + 1);
                    if (ht) {
                        memcpy(ht, hdr, tl);
                        ht[tl] = 0;
                        char *sh = strstr(ht, "SHA1 ");
                        if (sh) {
                            sh += 5;
                            unsigned n = 0;
                            while (sh[n] && n < 40) { out->sha1[n] = sh[n]; n++; }
                            out->sha1[n] = 0;
                        }
                        char *gn = strstr(ht, "GameName ");
                        if (gn) {
                            gn += 9;
                            size_t n = strcspn(gn, "\r\n");
                            if (n >= sizeof out->game) n = sizeof out->game - 1;
                            memcpy(out->game, gn, n);
                            out->game[n] = 0;
                        }
                        char *pl = strstr(ht, "Platform ");
                        if (pl && strncmp(pl + 9, "NES", 3))
                            snprintf(err, errsz, "bk2: platform %.16s, NES expected",
                                     pl + 9);
                        free(ht);
                    }
                }
            }
            free(log);
            free(hdr);
            free(blob);
        }
    } else if (hn > 4 && !memcmp(head, "FCM", 3)) {
        snprintf(err, errsz, "FCM movies (pre-fm2 FCEUX) are not supported");
    } else {
        /* fm2 is text: version line then headers then frames */
        fseek(f, 0, SEEK_SET);
        char first[16] = "";
        if (!fgets(first, sizeof first, f) || strncmp(first, "version", 7)) {
            snprintf(err, errsz, "unknown movie format (want .fm2 or .bk2)");
        } else {
            ok = tas_load_fm2(f, out, err, errsz);
        }
    }
    fclose(f);
    return ok;
}

void dp_nes_tas_free(DpNesTas *t) {
    if (!t) return;
    free(t->masks);
    free(t->cmds);
    t->masks = NULL;
    t->cmds = NULL;
}

static void sha1_hex(const uint8_t *d, size_t n, char out[41]) {
    uint8_t h[20];
    dp_sha1(d, n, h);
    for (int i = 0; i < 20; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

int dp_nes_tas_identity(const DpNesTas *t, const uint8_t *rom, size_t size) {
    if (!t || !t->sha1[0]) return 3;
    char want[41], a[41], b[41];
    size_t n = 0;
    for (; t->sha1[n] && n < 40; n++) want[n] = (char)tolower((unsigned char)t->sha1[n]);
    want[n] = 0;
    sha1_hex(rom, size, a);
    if (!strcmp(want, a)) return 1;
    if (size > 16) { sha1_hex(rom + 16, size - 16, b); if (!strcmp(want, b)) return 1; }
    return 2;
}
