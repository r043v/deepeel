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
