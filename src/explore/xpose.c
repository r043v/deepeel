// xpose.c: content matching between two versions of a ROM (regions, revisions).  Three passes, strictest first:
// verbatim runs (code, graphics, tables moved as a block); the same instructions with their 16-bit operands masked
// (code whose addresses moved), only from windows observed as code; 16-byte tiles observed as data, found anywhere.
// A window found at several source places is only used where it continues the previous match.
#include "xpose.h"
#include "core.h"
#include <stdlib.h>
#include <string.h>

#define BANK 0x4000u
#define CODE_BITS DP_CORE_NATURAL   // forced execution proves nothing about a byte
#define DATA_BITS (DP_CORE_COPIED | DP_CORE_COMPRESSED | DP_CORE_DATA)
#define WIN_VERBATIM 24
#define WIN_MASKED 20
#define WIN_CODE_MIN 16      // of a masked window's bytes, how many the source must have executed
#define TILE 16
#define NONE 0xffffffffu
#define AMBIGUOUS 0x80000000u

static uint8_t cls(uint8_t f) { return f & CODE_BITS ? DP_XPOSE_CODE : f & DATA_BITS ? DP_XPOSE_DATA : 0; }
static int trivial(const uint8_t *w, size_t n) {   // fewer than 5 distinct values: matches anywhere, proves nothing
    uint8_t v[4]; size_t k = 0;
    for (size_t i = 0; i < n; i++) { size_t j = 0; while (j < k && v[j] != w[i]) j++; if (j == k) { if (k == 4) return 0; v[k++] = w[i]; } }
    return 1;
}
static uint8_t *mask_operands(const uint8_t *rom, size_t n) {   // linear sweep per bank (it resynchronises by itself)
    static const uint8_t three[] = { 0x01, 0x11, 0x21, 0x31, 0x08, 0xC2, 0xC3, 0xC4, 0xCA, 0xCC, 0xCD, 0xD2, 0xD4, 0xDA, 0xDC, 0xEA, 0xFA };
    static const uint8_t two[] = { 0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36, 0x3E, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE, 0xE0, 0xF0, 0xE8, 0xF8, 0xCB };
    uint8_t len[256], *o = malloc(n); if (!o) return NULL;
    memset(len, 1, sizeof len); for (size_t i = 0; i < sizeof three; i++) len[three[i]] = 3; for (size_t i = 0; i < sizeof two; i++) len[two[i]] = 2;
    memcpy(o, rom, n);
    for (size_t base = 0; base < n; base += BANK) {
        size_t end = base + BANK < n ? base + BANK : n;
        for (size_t i = base; i < end; i += len[rom[i]]) if (len[rom[i]] == 3) for (size_t k = i + 1; k < i + 3 && k < end; k++) o[k] = 0;
    }
    return o;
}

typedef struct { uint32_t *slot; size_t mask, win; const uint8_t *src; } Index;   // window content -> source position
static size_t hash(const uint8_t *w, size_t n) { uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; i++) h = (h ^ w[i]) * 0x100000001b3ull; return (size_t)(h ^ (h >> 32)); }
static uint32_t *lookup(const Index *x, const uint8_t *w) {
    for (size_t i = hash(w, x->win);; i++) {
        uint32_t *s = &x->slot[i & x->mask];
        if (*s == NONE || !memcmp(x->src + (*s & ~AMBIGUOUS), w, x->win)) return s;
    }
}
static int index_init(Index *x, const uint8_t *src, size_t n, size_t win) {
    size_t cap = 1024; while (cap < 2 * n) cap <<= 1;
    x->slot = malloc(cap * sizeof *x->slot); if (!x->slot) return 0;
    memset(x->slot, 0xff, cap * sizeof *x->slot); x->mask = cap - 1; x->win = win; x->src = src;
    return 1;
}
static void index_add(Index *x, size_t pos) { uint32_t *s = lookup(x, x->src + pos); if (*s == NONE) *s = (uint32_t)pos; else *s |= AMBIGUOUS; }

// finds the windows of `src` in `dst` and extends each hit while the bytes agree, within the banks
static void match_runs(const uint8_t *src, size_t sn, const uint8_t *dst, size_t dn, size_t win,
                       const uint32_t *code_sum, const uint8_t *flags, uint8_t *done, uint8_t *out) {
    Index x; if (!index_init(&x, src, sn, win)) return;
    for (size_t s = 0; s + win <= sn; s++) {
        if ((s & (BANK - 1)) > BANK - win || trivial(src + s, win)) continue;
        if (code_sum && code_sum[s + win] - code_sum[s] < WIN_CODE_MIN) continue;
        index_add(&x, s);
    }
    ptrdiff_t last = 0; int have = 0;
    for (size_t d = 0; d + win <= dn; ) {
        uint32_t v = (d & (BANK - 1)) > BANK - win ? NONE : *lookup(&x, dst + d);
        size_t s = v;
        if (v != NONE && (v & AMBIGUOUS)) {
            ptrdiff_t c = (ptrdiff_t)d + last;
            if (have && c >= 0 && (size_t)c + win <= sn && !memcmp(src + c, dst + d, win)) s = (size_t)c; else v = NONE;
        }
        if (v == NONE) { d++; continue; }
        size_t a = d, b = d + win, lo = d & ~(size_t)(BANK - 1), hi = lo + BANK < dn ? lo + BANK : dn;
        while (a > lo && s > d - a && dst[a - 1] == src[s - (d - a) - 1]) a--;
        while (b < hi && s + (b - d) < sn && dst[b] == src[s + (b - d)]) b++;
        for (size_t t = a; t < b; t++) if (!done[t]) { done[t] = 1; uint8_t c = cls(flags[s + t - d]); if (c && !out[t]) out[t] = c; }
        last = (ptrdiff_t)s - (ptrdiff_t)d; have = 1; d = b;
    }
    free(x.slot);
}

void dp_xpose_gb(const uint8_t *src, size_t sn, const uint8_t *flags, const uint8_t *dst, size_t dn, uint8_t *out) {
    if (sn < BANK || dn < BANK) return;
    uint8_t *done = calloc(dn, 1), *ms = mask_operands(src, sn), *md = mask_operands(dst, dn);
    uint32_t *sum = malloc((sn + 1) * sizeof *sum);
    if (done && ms && md && sum) {
        match_runs(src, sn, dst, dn, WIN_VERBATIM, NULL, flags, done, out);
        sum[0] = 0; for (size_t i = 0; i < sn; i++) sum[i + 1] = sum[i] + ((flags[i] & CODE_BITS) != 0);
        match_runs(ms, sn, md, dn, WIN_MASKED, sum, flags, done, out);
        Index x;
        if (index_init(&x, src, sn, TILE)) {
            for (size_t s = 0; s + TILE <= sn; s += TILE) if (!trivial(src + s, TILE) && cls(flags[s]) == DP_XPOSE_DATA && *lookup(&x, src + s) == NONE) index_add(&x, s);
            for (size_t d = 0; d + TILE <= dn; d += TILE) {
                uint32_t v = *lookup(&x, dst + d); if (v == NONE) continue;
                size_t s = v & ~AMBIGUOUS;
                for (size_t k = 0; k < TILE; k++) if (!done[d + k] && !out[d + k] && cls(flags[s + k]) == DP_XPOSE_DATA) out[d + k] = DP_XPOSE_DATA;
            }
            free(x.slot);
        }
    }
    free(done); free(ms); free(md); free(sum);
}
