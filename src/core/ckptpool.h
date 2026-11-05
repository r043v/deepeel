// Shared checkpoint pool of the played sessions (J1): states kept by earlier sessions, from which
// later sessions restart.  A new slot is used while the pool is below its cap, then a random one is
// replaced (the generator is only drawn on replacement, which keeps older runs reproducible).
#ifndef DP_CORE_CKPTPOOL_H
#define DP_CORE_CKPTPOOL_H
#include <stdlib.h>
#include "rng.h"

#define DP_CKPT_POOL_MAX 64

typedef struct { uint8_t *slot[DP_CKPT_POOL_MAX]; unsigned n; } DpCkptPool;

// the buffer (size bytes) the caller saves a checkpoint into, or NULL (cap 0 / out of memory)
static inline uint8_t *dp_ckpt_claim(DpCkptPool *p, unsigned cap, size_t size, uint32_t *rng) {
    if (cap > DP_CKPT_POOL_MAX) cap = DP_CKPT_POOL_MAX;
    if (!cap) return NULL;
    unsigned s = p->n < cap ? p->n : dp_xorshift32(rng) % cap;
    if (!p->slot[s]) p->slot[s] = malloc(size);
    if (!p->slot[s]) return NULL;
    if (s == p->n) p->n++;
    return p->slot[s];
}

// a random kept checkpoint (the pool must not be empty)
static inline const uint8_t *dp_ckpt_pick(const DpCkptPool *p, uint32_t *rng) {
    return p->slot[dp_xorshift32(rng) % p->n];
}

static inline void dp_ckpt_free(DpCkptPool *p) {
    for (unsigned i = 0; i < DP_CKPT_POOL_MAX; i++) { free(p->slot[i]); p->slot[i] = NULL; }
    p->n = 0;
}
#endif
