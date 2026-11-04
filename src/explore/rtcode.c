// Runtime-code archive: every distinct instruction executed from RAM/VRAM/HRAM
// (pc >= $8000) is kept with its bytes, so code generated at run time is never
// lost even when no ROM source can be attached to it (decompressed or
// immediate-built code). One entry per (pc, bytes) variant: a rewritten
// location shows up as several variants (polymorphic code).
#include <stdlib.h>
#include <string.h>
#include "explorer.h"

static uint64_t rt_key(uint16_t pc, const uint8_t *b, int len) {
    uint64_t h = 1469598103934665603ull ^ pc;
    for (int i = 0; i < len; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h | 1;
}

static int rt_grow(DpResults *res) {
    uint32_t ncap = res->rt_cap ? res->rt_cap * 2 : 4096;
    DpRtCode *nt = calloc(ncap, sizeof *nt);
    if (!nt) return 0;
    for (uint32_t i = 0; i < res->rt_cap; i++) {
        if (!res->rt[i].key) continue;
        uint32_t j = (uint32_t)(res->rt[i].key >> 7) & (ncap - 1);
        while (nt[j].key) j = (j + 1) & (ncap - 1);
        nt[j] = res->rt[i];
    }
    free(res->rt);
    res->rt = nt;
    res->rt_cap = ncap;
    return 1;
}

// b: the instruction bytes (len 1..4); rom_src: ROM index of the copy source or -1
void dp_rtcode_add(DpResults *res, uint16_t pc, const uint8_t *b, int len, int32_t rom_src, int32_t derived,
                   int natural) {
    if (len < 1) return;
    if (len > 4) len = 4;
    uint64_t key = rt_key(pc, b, len);
    if (res->mt) pthread_mutex_lock(&res->rec_mtx);
    if (res->rt_n * 2 >= res->rt_cap && !rt_grow(res)) goto out;
    uint32_t j = (uint32_t)(key >> 7) & (res->rt_cap - 1);
    while (res->rt[j].key && res->rt[j].key != key) j = (j + 1) & (res->rt_cap - 1);
    DpRtCode *e = &res->rt[j];
    if (!e->key) {
        e->key = key;
        e->pc = pc;
        e->len = (uint8_t)len;
        memcpy(e->b, b, (size_t)len);
        e->rom_src = rom_src;
        e->derived = rom_src >= 0 ? -1 : derived;
        if (e->derived >= 0) res->rt_derived++;
        res->rt_n++;
        if (!res->rt_pcseen) res->rt_pcseen = calloc(0x10000, 1);
        if (res->rt_pcseen) {
            if (res->rt_pcseen[pc]) res->rt_poly++; // same pc, other bytes: rewritten code
            if (res->rt_pcseen[pc] < 255) res->rt_pcseen[pc]++;
        }
        if (rom_src >= 0) res->rt_attached++;
    }
    if (natural && !e->natural) {
        e->natural = 1;
        if (e->rom_src < 0) res->rt_natural++;
    }
    if (e->hits != 0xFFFFFFFFu) e->hits++;
out:
    if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
}

static int rt_cmp(const void *a, const void *b) {
    const DpRtCode *x = a, *y = b;
    if (x->pc != y->pc) return x->pc < y->pc ? -1 : 1;
    return memcmp(x->b, y->b, 4);
}

// sorted copy of the live entries (caller frees), count in *n
DpRtCode *dp_rtcode_sorted(const DpResults *res, uint32_t *n) {
    *n = 0;
    if (!res->rt_n) return NULL;
    DpRtCode *o = malloc(sizeof *o * res->rt_n);
    if (!o) return NULL;
    uint32_t k = 0;
    for (uint32_t i = 0; i < res->rt_cap && k < res->rt_n; i++)
        if (res->rt[i].key) o[k++] = res->rt[i];
    qsort(o, k, sizeof *o, rt_cmp);
    *n = k;
    return o;
}

// ---- recompiler export: executed RAM code as contiguous byte variants --------
// m0boy's `gbrecomp --ram-code` compiles each variant and enters it only while
// the RAM holds exactly its bytes.  Instructions are packed into "layers": a
// layer never holds two different bytes at one address and never starts an
// instruction inside another, so rewritten code (same pc, other bytes) lands in
// different layers.  A layer is then cut into runs of known contiguous bytes
// (the bytes between two executed instructions are unknown, so nothing is
// guessed across a gap).  WRAM $C000-$DFFF and HRAM $FF80-$FFFE only; other
// pcs (VRAM, cartridge RAM, echo) are counted, not exported.
#define RV_WRAM 0x2000
#define RV_N (RV_WRAM + 0x7F)
#define RV_LAYER_MAX 1024
typedef struct { uint8_t have[RV_N], val[RV_N]; uint16_t owner[RV_N]; } RvLayer;

static int rv_idx(uint32_t a) {
    if (a >= 0xC000u && a < 0xE000u) return (int)(a - 0xC000u);
    if (a >= 0xFF80u && a < 0xFFFFu) return RV_WRAM + (int)(a - 0xFF80u);
    return -1;
}
static uint32_t rv_addr(int i) { return i < RV_WRAM ? 0xC000u + (uint32_t)i : 0xFF80u + (uint32_t)(i - RV_WRAM); }

static int rv_fits(const RvLayer *l, const DpRtCode *c) {
    for (int k = 0; k < c->len; k++) {
        int i = rv_idx((uint32_t)c->pc + (uint32_t)k);
        if (i < 0) return 0;
        if (l->have[i] && (l->val[i] != c->b[k] || l->owner[i] != c->pc)) return 0;
    }
    return 1;
}

// writes `path`; fills the counters when non-NULL.  Returns 0 on success, -1 when the file cannot be written.
int dp_rtcode_export_variants(const DpResults *res, const char *path, int forced_too, uint32_t *variants,
                              uint32_t *instr, uint32_t *skipped, uint32_t *dropped, uint32_t *forced) {
    uint32_t nv = 0, ni = 0, ns = 0, nd = 0, nf = 0, rn = 0;
    DpRtCode *rt = dp_rtcode_sorted(res, &rn);
    RvLayer **lay = NULL;
    uint32_t nl = 0;
    for (uint32_t i = 0; i < rn; i++) {
        const DpRtCode *c = &rt[i];
        if (rv_idx(c->pc) < 0 || rv_idx((uint32_t)c->pc + c->len - 1u) < 0) { ns++; continue; }
        // code only a forced fork made reachable (typically a jump into zeroed RAM) is not game code
        if (!c->natural && !forced_too) { nf++; continue; }
        uint32_t L = 0;
        while (L < nl && !rv_fits(lay[L], c)) L++;
        if (L == nl) {
            if (nl >= RV_LAYER_MAX) { nd++; continue; }
            RvLayer **g = realloc(lay, sizeof *lay * (nl + 1));
            RvLayer *n = calloc(1, sizeof *n);
            if (!g || !n) { free(n); if (g) lay = g; nd++; continue; }
            lay = g;
            lay[nl++] = n;
        }
        RvLayer *l = lay[L];
        for (int k = 0; k < c->len; k++) {
            int ix = rv_idx((uint32_t)c->pc + (uint32_t)k);
            l->have[ix] = 1;
            l->val[ix] = c->b[k];
            l->owner[ix] = c->pc;
        }
        ni++;
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# gbl ram code v1: ram ADDR HEXBYTES OFFSETS (hex starts relative to ADDR, e = entered non-sequentially)\n");
        for (uint32_t L = 0; L < nl; L++) {
            const RvLayer *l = lay[L];
            for (int i = 0; i < RV_N;) {
                if (!l->have[i]) { i++; continue; }
                int j = i;
                while (j + 1 < RV_N && l->have[j + 1] && (j + 1 != RV_WRAM)) j++; // a run never spans WRAM into HRAM
                fprintf(f, "ram %04x ", rv_addr(i));
                for (int k = i; k <= j; k++) fprintf(f, "%02x", l->val[k]);
                fputc(' ', f);
                int first = 1;
                for (int k = i; k <= j; k++) {
                    if (l->owner[k] != rv_addr(k)) continue;
                    fprintf(f, "%s%x%s", first ? "" : ",", k - i, first ? "e" : "");
                    first = 0;
                }
                fputc('\n', f);
                nv++;
                i = j + 1;
            }
        }
        fclose(f);
    }
    for (uint32_t L = 0; L < nl; L++) free(lay[L]);
    free(lay);
    free(rt);
    if (variants) *variants = nv;
    if (instr) *instr = ni;
    if (skipped) *skipped = ns;
    if (dropped) *dropped = nd;
    if (forced) *forced = nf;
    return f ? 0 : -1;
}
