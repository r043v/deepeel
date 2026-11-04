#include <stdlib.h>
#include <string.h>
#include "ramarch.h"

static uint64_t key_of(uint16_t pc, const uint8_t *b, int len) {
    uint64_t h = 1469598103934665603ull ^ pc;
    for (int i = 0; i < len; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h | 1;
}
static int grow(DpRamArch *a) {
    uint32_t nc = a->cap ? a->cap * 2 : 1024;
    DpRamEntry *nt = calloc(nc, sizeof *nt);
    if (!nt) return 0;
    for (uint32_t i = 0; i < a->cap; i++) {
        if (!a->e[i].key) continue;
        uint32_t j = (uint32_t)(a->e[i].key >> 7) & (nc - 1);
        while (nt[j].key) j = (j + 1) & (nc - 1);
        nt[j] = a->e[i];
    }
    free(a->e); a->e = nt; a->cap = nc;
    return 1;
}
static DpRamEntry *slot(DpRamArch *a, uint64_t key) {
    uint32_t j = (uint32_t)(key >> 7) & (a->cap - 1);
    while (a->e[j].key && a->e[j].key != key) j = (j + 1) & (a->cap - 1);
    return &a->e[j];
}
static void add_hits(DpRamArch *a, uint16_t pc, const uint8_t *b, int len, int32_t src, int32_t prod, uint32_t hits) {
    if (len < 1) return;
    if (len > 10) len = 10;
    if (a->n * 2 >= a->cap && !grow(a)) return;
    uint64_t key = key_of(pc, b, len);
    DpRamEntry *e = slot(a, key);
    if (!e->key) {
        e->key = key; e->pc = pc; e->len = (uint8_t)len; memcpy(e->b, b, (size_t)len); e->src = src; e->prod = prod;
        a->n++;
        if (src >= 0) a->attached++;
        else if (prod >= 0) a->derived++;
        // same pc under other bytes: rewritten code (linear scan of the pc's variants is avoided
        // by checking the neighbours that share the pc through a second probe key)
        uint64_t pk = key_of(pc, NULL, 0);
        DpRamEntry *p = slot(a, pk);
        if (!p->key) { p->key = pk; p->pc = pc; p->len = 0; p->hits = 1; a->n++; }
        else { p->hits++; a->poly++; }
    }
    e->hits = e->hits + hits < e->hits ? 0xFFFFFFFFu : e->hits + hits;
}
void dp_ramarch_add(DpRamArch *a, uint16_t pc, const uint8_t *b, int len, int32_t src) { add_hits(a, pc, b, len, src, -1, 1); }
void dp_ramarch_add_p(DpRamArch *a, uint16_t pc, const uint8_t *b, int len, int32_t src, int32_t prod) { add_hits(a, pc, b, len, src, prod, 1); }
void dp_ramarch_merge(DpRamArch *d, const DpRamArch *s) {
    for (uint32_t i = 0; i < s->cap; i++)
        if (s->e[i].key && s->e[i].len) add_hits(d, s->e[i].pc, s->e[i].b, s->e[i].len, s->e[i].src, s->e[i].prod, s->e[i].hits);
}
static int cmp(const void *x, const void *y) {
    const DpRamEntry *a = x, *b = y;
    if (a->pc != b->pc) return a->pc < b->pc ? -1 : 1;
    return memcmp(a->b, b->b, 10);
}
void dp_ramarch_write(const DpRamArch *a, FILE *out, const char *platform) {
    DpRamEntry *o = malloc(sizeof *o * (a->n ? a->n : 1));
    if (!o) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->cap; i++) if (a->e[i].key && a->e[i].len) o[k++] = a->e[i];
    qsort(o, k, sizeof *o, cmp);
    fprintf(out, "# deepeel rtcode v1 (%s): pc bytes hits rom_index(-1=unattached) producer(-1=none)\n", platform);
    for (uint32_t i = 0; i < k; i++) {
        fprintf(out, "%04x ", o[i].pc);
        for (int j = 0; j < o[i].len; j++) fprintf(out, "%02x", o[i].b[j]);
        fprintf(out, " %u %d %d\n", o[i].hits, o[i].src, o[i].prod);
    }
    free(o);
}
void dp_ramarch_free(DpRamArch *a) { free(a->e); memset(a, 0, sizeof *a); }
