// xorshift32: the one input-policy generator of every platform's played sessions (cheap, portable,
// deterministic across machines).  The state must be non-zero.
#ifndef DP_CORE_RNG_H
#define DP_CORE_RNG_H
#include <stdint.h>

static inline uint32_t dp_xorshift32(uint32_t *s) {
    uint32_t v = *s;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return *s = v;
}
#endif
