#ifndef DP_CORE_HASHSET_H
#define DP_CORE_HASHSET_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Open-addressing u64 set shared by the GB and Z80 explorers (seed dedup,
// visited contexts). Kept header-only: both engines are separate libraries.
typedef struct DpHashSet {
    uint64_t *keys;
    uint8_t *used;
    size_t cap, cnt, cap_abs;
} DpHashSet;

static inline size_t dp_set_probe(const DpHashSet *s, uint64_t key) {
    size_t i = (size_t)(key * 0x9E3779B97F4A7C15ull) & (s->cap - 1);
    while (s->used[i]) {
        if (s->keys[i] == key) return i;
        i = (i + 1) & (s->cap - 1);
    }
    return i;
}

static inline int dp_set_grow(DpHashSet *s) {
    if (s->cap >= s->cap_abs) return 0;
    size_t ncap = s->cap * 2;
    uint64_t *nk = calloc(ncap, sizeof(uint64_t));
    uint8_t *nu = calloc(ncap, 1);
    if (!nk || !nu) { free(nk); free(nu); return 0; }
    for (size_t i = 0; i < s->cap; i++) {
        if (!s->used[i]) continue;
        size_t j = (size_t)(s->keys[i] * 0x9E3779B97F4A7C15ull) & (ncap - 1);
        while (nu[j]) j = (j + 1) & (ncap - 1);
        nk[j] = s->keys[i];
        nu[j] = 1;
    }
    free(s->keys);
    free(s->used);
    s->keys = nk;
    s->used = nu;
    s->cap = ncap;
    return 1;
}

static inline void dp_set_init(DpHashSet *s, size_t cap_abs) {
    s->cap_abs = cap_abs ? cap_abs : 1 << 20;
    s->cap = 1024;
    s->keys = calloc(s->cap, sizeof(uint64_t));
    s->used = calloc(s->cap, 1);
    s->cnt = 0;
}

// semantic reset (the liveness mask grew: previously evicted contexts may now
// differ, the seen set starts over) — cheaper than free+init, keeps the arrays
static inline void dp_set_clear(DpHashSet *s) {
    if (!s->keys) return;
    memset(s->used, 0, s->cap);
    s->cnt = 0;
}

static inline void dp_set_free(DpHashSet *s) {
    free(s->keys);
    free(s->used);
    s->keys = NULL;
    s->used = NULL;
    s->cap = s->cnt = s->cap_abs = 0;
}

// persistence (C3 exact.bin): header "DPXS", tag, count, then the keys
#include <stdio.h>
#include <string.h>
static inline int dp_set_save(const DpHashSet *s, const char *path, uint64_t tag) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint64_t n = s->cnt;
    int ok = fwrite("DPXS", 1, 4, f) == 4 && fwrite(&tag, 8, 1, f) == 1 && fwrite(&n, 8, 1, f) == 1;
    for (size_t i = 0; ok && i < s->cap; i++)
        if (s->used[i]) ok = fwrite(&s->keys[i], 8, 1, f) == 1;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}
// returns 1 if inserted, 0 if already present; returns -1 when full
static inline int dp_set_insert(DpHashSet *s, uint64_t key) {
    if (s->cnt >= s->cap * 3 / 4) {
        if (!dp_set_grow(s)) return -1;
    }
    size_t i = dp_set_probe(s, key);
    if (s->used[i]) return 0;
    s->keys[i] = key;
    s->used[i] = 1;
    s->cnt++;
    return 1;
}

// returns the number of keys loaded, -1 when absent or for another tag
static inline long dp_set_load(DpHashSet *s, const char *path, uint64_t tag) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char m[4]; uint64_t t = 0, n = 0; long got = 0;
    if (fread(m, 1, 4, f) != 4 || memcmp(m, "DPXS", 4) || fread(&t, 8, 1, f) != 1 || t != tag || fread(&n, 8, 1, f) != 1) { fclose(f); return -1; }
    for (uint64_t i = 0; i < n; i++) {
        uint64_t k;
        if (fread(&k, 8, 1, f) != 1) break;
        if (dp_set_insert(s, k) == 1) got++;
    }
    fclose(f);
    return got;
}


#endif
