// Shared growth policy of the explorers' pending-state arrays (live queue up to --queue-max, deferred
// holding area unbounded): the capacity doubles from `first`, clamped to `limit` (0: no limit); the new
// tail is zeroed.  Returns 1 when the array grew, 0 when it is at its limit or memory is out (the array
// is then untouched).
#ifndef DP_CORE_GROWVEC_H
#define DP_CORE_GROWVEC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int dp_vec_grow(void **buf, uint32_t *cap, size_t isz, uint32_t first, uint32_t limit) {
    uint32_t nc = *cap ? *cap * 2 : first;
    if (limit && (nc > limit || *cap > limit / 2)) nc = limit;
    if (nc <= *cap) return 0;
    void *g = realloc(*buf, (size_t)nc * isz);
    if (!g) return 0;
    memset((char *)g + (size_t)*cap * isz, 0, (size_t)(nc - *cap) * isz);
    *buf = g; *cap = nc;
    return 1;
}
#endif
