#ifndef DP_CORE_HASHSET_H
#define DP_CORE_HASHSET_H
#include <stdint.h>
#include <stdlib.h>

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

static inline void dp_set_free(DpHashSet *s) {
    free(s->keys);
    free(s->used);
    s->keys = NULL;
    s->used = NULL;
    s->cap = s->cnt = s->cap_abs = 0;
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

#endif
