#define _GNU_SOURCE
#include "desync.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { char name[24]; uint32_t len; uint8_t *d; } Blk;
typedef struct { uint32_t frame; unsigned n; Blk *b; } Rec;

int dp_dsy_open(DpDsyWriter *w, const char *path) {
    w->f = fopen(path, "wb");
    if (!w->f) return 0;
    fwrite("DSY1", 1, 4, w->f);
    return 1;
}
int dp_dsy_frame(DpDsyWriter *w, uint32_t frame, unsigned n, const char *const *names, const uint8_t *const *data, const uint32_t *len) {
    if (!w->f) return 0;
    uint16_t nb = (uint16_t)n;
    fwrite(&frame, 4, 1, w->f); fwrite(&nb, 2, 1, w->f);
    for (unsigned i = 0; i < n; i++) {
        uint8_t nl = (uint8_t)strlen(names[i]);
        fwrite(&nl, 1, 1, w->f); fwrite(names[i], 1, nl, w->f);
        fwrite(&len[i], 4, 1, w->f); fwrite(data[i], 1, len[i], w->f);
    }
    return 1;
}
void dp_dsy_close(DpDsyWriter *w) { if (w->f) fclose(w->f); w->f = NULL; }

static void rec_free(Rec *r) { for (unsigned i = 0; i < r->n; i++) free(r->b[i].d); free(r->b); memset(r, 0, sizeof *r); }
static int rec_read(FILE *f, Rec *r) {
    uint16_t nb;
    memset(r, 0, sizeof *r);
    if (fread(&r->frame, 4, 1, f) != 1 || fread(&nb, 2, 1, f) != 1) return 0;
    r->b = calloc(nb ? nb : 1, sizeof *r->b);
    for (unsigned i = 0; i < nb; i++) {
        uint8_t nl;
        Blk *b = &r->b[r->n];
        if (fread(&nl, 1, 1, f) != 1) { rec_free(r); return 0; }
        char nm[256];
        if (fread(nm, 1, nl, f) != nl) { rec_free(r); return 0; }
        snprintf(b->name, sizeof b->name, "%.*s", nl, nm);
        if (fread(&b->len, 4, 1, f) != 1 || b->len > (256u << 20)) { rec_free(r); return 0; }
        b->d = malloc(b->len ? b->len : 1);
        if (!b->d || fread(b->d, 1, b->len, f) != b->len) { free(b->d); rec_free(r); return 0; }
        r->n++;
    }
    return 1;
}
static const Blk *find(const Rec *r, const char *n) { for (unsigned i = 0; i < r->n; i++) if (!strcmp(r->b[i].name, n)) return &r->b[i]; return NULL; }
static int ignored(const DpDsyOptions *o, const char *blk, uint32_t i) {
    for (unsigned k = 0; k < o->nignore; k++) if (!strcmp(o->ignore[k].block, blk) && i >= o->ignore[k].lo && i <= o->ignore[k].hi) return 1;
    return 0;
}
static uint32_t block_diffs(const DpDsyOptions *o, const Blk *a, const Blk *b, uint32_t *first) {
    uint32_t n = 0, m = a->len < b->len ? a->len : b->len;
    for (uint32_t i = 0; i < m; i++) if (a->d[i] != b->d[i] && !ignored(o, a->name, i)) { if (!n) *first = i; n++; }
    return n + (a->len != b->len);
}
static int skip_to(FILE *f, int n, Rec *r) { for (int i = 0; i < n; i++) { if (!rec_read(f, r)) return 0; rec_free(r); } return 1; }
static void ring_dump(const char *dir, char side, const Rec *r) {
    char p[1024];
    mkdir(dir, 0755);
    for (unsigned i = 0; i < r->n; i++) {
        snprintf(p, sizeof p, "%s/%c-f%06u-%s.bin", dir, side, r->frame, r->b[i].name);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(r->b[i].d, 1, r->b[i].len, f); fclose(f); }
    }
}

int dp_dsy_compare(const char *pa, const char *pb, const DpDsyOptions *o, FILE *out, uint32_t *compared) {
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    char m[4];
    if (!fa || !fb || fread(m, 1, 4, fa) != 4 || memcmp(m, "DSY1", 4) || fread(m, 1, 4, fb) != 4 || memcmp(m, "DSY1", 4)) {
        if (fa) fclose(fa); if (fb) fclose(fb);
        fprintf(out, "desync: cannot open %s / %s as DSY1\n", pa, pb);
        return -1;
    }
    unsigned W = o->window ? o->window : 8, show = o->show ? o->show : 32;
    Rec tmp;
    skip_to(fa, o->offset_a, &tmp); skip_to(fb, o->offset_b, &tmp);
    Rec *ra = calloc(W, sizeof *ra), *rb = calloc(W, sizeof *rb);
    uint32_t n = 0; int rc = 0;
    for (;; n++) {
        Rec *a = &ra[n % W], *b = &rb[n % W];
        rec_free(a); rec_free(b);
        if (!rec_read(fa, a) || !rec_read(fb, b)) break;
        unsigned bad = 0;
        for (unsigned i = 0; i < a->n; i++) {
            const Blk *y = find(b, a->b[i].name);
            uint32_t first = 0;
            if (y && block_diffs(o, &a->b[i], y, &first)) bad++;
        }
        if (!bad) continue;
        rc = 1;
        fprintf(out, "DESYNC at compared frame %u (A frame %u, B frame %u)\n", n, a->frame, b->frame);
        for (unsigned i = 0; i < a->n; i++) {
            const Blk *y = find(b, a->b[i].name);
            if (!y) { fprintf(out, "  block %-10s only in A\n", a->b[i].name); continue; }
            uint32_t first = 0, d = block_diffs(o, &a->b[i], y, &first);
            if (!d) continue;
            fprintf(out, "  block %-10s %u byte(s) differ (len A %u, B %u), first at +%04x\n", a->b[i].name, d, a->b[i].len, y->len, first);
            unsigned shown = 0;
            for (uint32_t k = 0; k < a->b[i].len && k < y->len && shown < show; k++)
                if (a->b[i].d[k] != y->d[k] && !ignored(o, a->b[i].name, k)) { fprintf(out, "    +%04x  A=%02x  B=%02x\n", k, a->b[i].d[k], y->d[k]); shown++; }
        }
        for (unsigned i = 0; i < b->n; i++) if (!find(a, b->b[i].name)) fprintf(out, "  block %-10s only in B\n", b->b[i].name);
        // history: the window's earlier frames, oldest first, with the per-block difference count (all ignored bytes excluded)
        unsigned have = n + 1 < W ? n + 1 : W;
        fprintf(out, "  window (%u frames kept%s):\n", have, o->ring_dir ? ", dumped" : "");
        for (unsigned k = have; k-- > 0;) {
            Rec *wa = &ra[(n - k) % W], *wb = &rb[(n - k) % W];
            fprintf(out, "    A f%u / B f%u:", wa->frame, wb->frame);
            for (unsigned i = 0; i < wa->n; i++) {
                const Blk *y = find(wb, wa->b[i].name); uint32_t first = 0;
                fprintf(out, " %s=%u", wa->b[i].name, y ? block_diffs(o, &wa->b[i], y, &first) : 0);
            }
            fputc('\n', out);
            if (o->ring_dir) { ring_dump(o->ring_dir, 'A', wa); ring_dump(o->ring_dir, 'B', wb); }
        }
        break;
    }
    if (!rc) fprintf(out, "identical over %u frame(s)\n", n);
    if (compared) *compared = n;
    for (unsigned i = 0; i < W; i++) { rec_free(&ra[i]); rec_free(&rb[i]); }
    free(ra); free(rb); fclose(fa); fclose(fb);
    return rc;
}
