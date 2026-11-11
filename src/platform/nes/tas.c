#include "tas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../../../src/core/bk2.h"
#include "../../../src/core/bizhawk.h"
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

/* bk2: the shared BizHawk reader (src/core/bizhawk.c) resolves the LogKey columns; here only the NES pad
 * mapping (A B Select Start Up Down Left Right = bits 0..7 of player 1) and the Reset/Power command bit. */
static int tas_load_bk2(const uint8_t *log, size_t logn, DpNesTas *t,
                        char *err, size_t errsz) {
    DpBzMovie m;
    if (!dp_bz_load(log, logn, &m, err, errsz)) return 0;
    static const char *const names[8] = { "P1 A", "P1 B", "P1 Select", "P1 Start", "P1 Up", "P1 Down", "P1 Left", "P1 Right" };
    int col[8], reset = dp_bz_find(&m, "Reset"), power = dp_bz_find(&m, "Power");
    for (int i = 0; i < 8; i++) col[i] = dp_bz_find(&m, names[i]);
    t->masks = malloc(m.nframes ? m.nframes : 1);
    t->cmds = calloc(m.nframes ? m.nframes : 1, 1);
    if (!t->masks || !t->cmds) { dp_bz_free(&m); snprintf(err, errsz, "bk2: out of memory"); return 0; }
    for (uint32_t f = 0; f < m.nframes; f++) {
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++) if (dp_bz_held(&m, f, col[i])) mask |= (uint8_t)(1u << i);
        t->masks[f] = mask;
        if (dp_bz_held(&m, f, reset) || dp_bz_held(&m, f, power)) { t->cmds[f] = 2; t->resets++; }
    }
    t->frames = m.nframes;
    dp_bz_free(&m);
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
            DpBk2 b;   /* container handled by the shared kernel (src/core/bk2.c) */
            if (!dp_bk2_open(blob, (size_t)sz, &b, err, errsz)) {
                /* err already set */
            } else if (!tas_load_bk2(b.log, b.log_n, out, err, errsz)) {
                /* err already set */
            } else {
                ok = 1;
                if (b.has_sha1)
                    for (int k = 0; k < 20; k++) snprintf(out->sha1 + 2 * k, 3, "%02x", b.sha1[k]);
                if (b.header) {
                    /* Header.txt is text but not NUL-terminated: search
                     * with length bounds */
                    size_t tl = b.header_n;
                    char *ht = malloc(tl + 1);
                    if (ht) {
                        memcpy(ht, b.header, tl);
                        ht[tl] = 0;
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
            dp_bk2_free(&b);
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
