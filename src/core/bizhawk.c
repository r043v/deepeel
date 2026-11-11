#include "bizhawk.h"
#include "bk2.h"
#include "zip.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void header_value(const char *h, size_t hn, const char *label, char *dst, size_t dn) {
    size_t ll = strlen(label);
    dst[0] = 0;
    for (size_t i = 0; i + ll < hn; i++) {
        if ((i && h[i - 1] != '\n') || strncasecmp(h + i, label, ll) || h[i + ll] != ' ') continue;
        size_t k = 0;
        for (const char *v = h + i + ll + 1; v < h + hn && *v != '\n' && *v != '\r' && k + 1 < dn; v++) dst[k++] = *v;
        dst[k] = 0;
        return;
    }
}

static int add_key(DpBzMovie *m, unsigned *cap, const char *name, unsigned group, int axis) {
    if (m->nkeys == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        DpBzKey *k = realloc(m->keys, *cap * sizeof *k);
        if (!k) return 0;
        m->keys = k;
    }
    DpBzKey *k = &m->keys[m->nkeys++];
    memset(k, 0, sizeof *k);
    snprintf(k->name, sizeof k->name, "%s", name);
    k->group = (uint8_t)(group > 255 ? 255 : group);
    k->axis = (uint8_t)axis;
    k->slot = (uint16_t)(axis ? m->naxes++ : m->nbuttons++);
    return 1;
}

/* LogKey:#Reset|Power|#P1 Up|P1 Down|...  ('#' opens a controller group; names may contain spaces).
 * An analog column is recognised by name ("Axis", "Stick", "Trackball", "Mouse X/Y", "Wheel", "Throttle"). */
static int is_axis_name(const char *s) {
    static const char *const w[] = { "axis", "stick", "trackball", "mouse x", "mouse y", "wheel", "throttle", "paddle", "pointer", "lightgun x", "lightgun y", "x pos", "y pos", "lightpen", "cycle", "counter", "frame count", "lag" };
    char l[DP_BZ_NAME]; size_t i = 0;
    for (; s[i] && i + 1 < sizeof l; i++) l[i] = (char)tolower((unsigned char)s[i]);
    l[i] = 0;
    for (size_t k = 0; k < sizeof w / sizeof *w; k++) if (strstr(l, w[k])) return 1;
    return 0;
}

static int parse_logkey(DpBzMovie *m, char *line, unsigned *cap) {
    unsigned group = 0; int first = 1;
    for (char *tok = strtok(line, "|"); tok; tok = strtok(NULL, "|")) {
        if (*tok == '#') { if (!first) group++; while (*tok == '#') tok++; }
        first = 0;
        while (*tok == ' ' || *tok == '\t') tok++;
        char *e = tok + strlen(tok);
        while (e > tok && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
        if (!*tok) continue;
        if (!add_key(m, cap, tok, group, is_axis_name(tok))) return 0;
    }
    m->ngroups = group + 1;
    return 1;
}

static int push_frame(DpBzMovie *m, uint32_t *cap, const uint64_t *bits, const int16_t *axes) {
    if (m->nframes == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 4096;
        uint64_t *nb = realloc(m->bits, (size_t)nc * m->words * sizeof *nb);
        if (!nb) return 0;
        m->bits = nb;
        if (m->naxes) {
            int16_t *na = realloc(m->axes, (size_t)nc * m->naxes * sizeof *na);
            if (!na) return 0;
            m->axes = na;
        }
        *cap = nc;
    }
    memcpy(m->bits + (size_t)m->nframes * m->words, bits, m->words * sizeof *bits);
    if (m->naxes) memcpy(m->axes + (size_t)m->nframes * m->naxes, axes, m->naxes * sizeof *axes);
    m->nframes++;
    return 1;
}

static int parse_log(DpBzMovie *m, const char *p, size_t n, char *err, size_t errsz) {
    const char *end = p + n;
    unsigned cap = 0; uint32_t fcap = 0;
    uint64_t *bits = NULL; int16_t *axes = NULL;
    int have_key = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *line = p;
        p += len + (nl ? 1 : 0);
        while (len && line[len - 1] == '\r') len--;
        if (len > 7 && !strncmp(line, "LogKey:", 7)) {
            if (have_key) continue;   /* a second LogKey (multi-section log) is the same columns */
            char *copy = malloc(len - 6);
            if (!copy) goto oom;
            memcpy(copy, line + 7, len - 7); copy[len - 7] = 0;
            int ok = parse_logkey(m, copy, &cap);
            free(copy);
            if (!ok) goto oom;
            m->words = (m->nbuttons + 63) / 64 ? (m->nbuttons + 63) / 64 : 1;
            bits = calloc(m->words, sizeof *bits);
            axes = calloc(m->naxes ? m->naxes : 1, sizeof *axes);
            if (!bits || !axes) goto oom;
            have_key = 1;
            continue;
        }
        if (!len || line[0] != '|' || !have_key) continue;
        memset(bits, 0, m->words * sizeof *bits);
        memset(axes, 0, (m->naxes ? m->naxes : 1) * sizeof *axes);
        unsigned ki = 0;
        const char *c = line + 1, *le = line + len;
        while (c < le && ki < m->nkeys) {
            const char *cell_end = memchr(c, '|', (size_t)(le - c));
            if (!cell_end) cell_end = le;
            const char *q = c;
            /* a cell is either one character per button column or comma-separated numbers per axis column */
            while (q < cell_end && ki < m->nkeys) {
                const DpBzKey *k = &m->keys[ki];
                if (k->axis) {
                    char *e; long v = strtol(q, &e, 10);
                    if (e == q) { while (q < cell_end && *q != ',') q++; }
                    else q = e;
                    axes[k->slot] = (int16_t)v;
                    if (q < cell_end && *q == ',') q++;
                } else {
                    if (*q != '.' && *q != ' ') bits[k->slot / 64] |= 1ull << (k->slot % 64);
                    q++;
                }
                ki++;
            }
            c = cell_end < le ? cell_end + 1 : le;
        }
        if (!push_frame(m, &fcap, bits, axes)) goto oom;
    }
    free(bits); free(axes);
    if (!m->nframes) { snprintf(err, errsz, "bizhawk: no input frames"); return 0; }
    return 1;
oom:
    free(bits); free(axes);
    snprintf(err, errsz, "bizhawk: out of memory or no LogKey");
    return 0;
}

int dp_bz_load(const uint8_t *d, size_t n, DpBzMovie *m, char *err, size_t errsz) {
    memset(m, 0, sizeof *m);
    const char *hdr = NULL; size_t hn = 0, ln = 0; const char *log = NULL;
    DpBk2 b; int zipped = dp_zip_is(d, n);
    if (zipped) {
        if (!dp_bk2_open(d, n, &b, err, errsz)) return 0;
        hdr = (const char *)b.header; hn = b.header_n; log = (const char *)b.log; ln = b.log_n;
        m->sync_json = (char *)dp_zip_read(d, n, "syncsettings", &m->sync_n, NULL);
    } else {
        /* plain-text .bkm: header lines, then the log lines, in one file */
        memset(&b, 0, sizeof b);
        hdr = log = (const char *)d; hn = ln = n;
        if (!memmem(d, n < 4096 ? n : 4096, "LogKey", 6)) { snprintf(err, errsz, "bizhawk: not a BizHawk movie"); return 0; }
    }
    if (hdr) {
        header_value(hdr, hn, "Platform", m->platform, sizeof m->platform);
        header_value(hdr, hn, "Core", m->core, sizeof m->core);
        header_value(hdr, hn, "GameName", m->game, sizeof m->game);
        header_value(hdr, hn, "SHA1", m->sha1, sizeof m->sha1);
        header_value(hdr, hn, "MD5", m->md5, sizeof m->md5);
        char t[32];
        header_value(hdr, hn, "rerecordCount", t, sizeof t); m->rerecords = strtoull(t, NULL, 10);
        header_value(hdr, hn, "Pal", t, sizeof t); m->pal = !strcasecmp(t, "true");
        header_value(hdr, hn, "StartsFromSavestate", t, sizeof t); m->anchored = !strcasecmp(t, "true");
    }
    int ok = parse_log(m, log, ln, err, errsz);
    if (zipped) dp_bk2_free(&b);
    if (!ok) dp_bz_free(m);
    return ok;
}

void dp_bz_free(DpBzMovie *m) {
    if (!m) return;
    free(m->keys); free(m->bits); free(m->axes); free(m->sync_json);
    memset(m, 0, sizeof *m);
}

int dp_bz_find(const DpBzMovie *m, const char *name) {
    for (unsigned i = 0; i < m->nkeys; i++) if (!strcasecmp(m->keys[i].name, name)) return (int)i;
    return -1;
}

int dp_bz_held(const DpBzMovie *m, uint32_t frame, int key) {
    if (key < 0 || (unsigned)key >= m->nkeys || frame >= m->nframes || m->keys[key].axis) return 0;
    unsigned s = m->keys[key].slot;
    return (int)((m->bits[(size_t)frame * m->words + s / 64] >> (s % 64)) & 1);
}

int dp_bz_axis(const DpBzMovie *m, uint32_t frame, int key) {
    if (key < 0 || (unsigned)key >= m->nkeys || frame >= m->nframes || !m->keys[key].axis) return 0;
    return m->axes[(size_t)frame * m->naxes + m->keys[key].slot];
}

long dp_bz_diff(const DpBzMovie *a, const DpBzMovie *b, unsigned *only_a, unsigned *only_b) {
    unsigned oa = 0, ob = 0;
    int *map = malloc((a->nkeys ? a->nkeys : 1) * sizeof *map);
    if (!map) return -2;
    for (unsigned i = 0; i < a->nkeys; i++) { map[i] = dp_bz_find(b, a->keys[i].name); if (map[i] < 0) oa++; }
    for (unsigned j = 0; j < b->nkeys; j++) if (dp_bz_find(a, b->keys[j].name) < 0) ob++;
    if (only_a) *only_a = oa;
    if (only_b) *only_b = ob;
    uint32_t nf = a->nframes < b->nframes ? a->nframes : b->nframes;
    long first = -1;
    for (uint32_t f = 0; f < nf && first < 0; f++)
        for (unsigned i = 0; i < a->nkeys; i++) {
            if (map[i] < 0) continue;
            int differs = a->keys[i].axis ? dp_bz_axis(a, f, (int)i) != dp_bz_axis(b, f, map[i])
                                          : dp_bz_held(a, f, (int)i) != dp_bz_held(b, f, map[i]);
            if (differs) { first = (long)f; break; }
        }
    free(map);
    return first;
}

void dp_bz_describe(const DpBzMovie *m, FILE *out) {
    fprintf(out, "platform %s, core %s, game %s\n", m->platform[0] ? m->platform : "?", m->core[0] ? m->core : "?", m->game[0] ? m->game : "?");
    fprintf(out, "identity sha1 %s md5 %s, rerecords %llu, pal %d, anchored %d\n", m->sha1[0] ? m->sha1 : "-", m->md5[0] ? m->md5 : "-", (unsigned long long)m->rerecords, m->pal, m->anchored);
    fprintf(out, "frames %u, columns %u (%u buttons, %u axes), groups %u, sync settings %s\n", m->nframes, m->nkeys, m->nbuttons, m->naxes, m->ngroups, m->sync_json ? "yes" : "no");
    fprintf(out, "columns:");
    for (unsigned i = 0; i < m->nkeys; i++) fprintf(out, "%s%s%s", i ? ", " : " ", m->keys[i].name, m->keys[i].axis ? "(axis)" : "");
    fputc('\n', out);
    uint32_t used = 0;
    for (unsigned i = 0; i < m->nkeys && !used; i++) for (uint32_t f = 0; f < m->nframes; f++) if (m->keys[i].axis ? dp_bz_axis(m, f, (int)i) : dp_bz_held(m, f, (int)i)) { used = 1; break; }
    fprintf(out, "any input: %s\n", used ? "yes" : "no");
}
