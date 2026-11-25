// quoting for the commands handed to popen/system: ROM names hold apostrophes ("Link's Awakening")
#ifndef DP_SHQUOTE_H
#define DP_SHQUOTE_H
#include <stddef.h>

// s as one single-quoted shell word (truncated, still closed, when out is too small)
static inline const char *dp_shquote(const char *s, char *out, size_t n) {
    size_t o = 0;
    if (n < 3) { if (n) out[0] = 0; return out; }
    out[o++] = '\'';
    for (; *s && o + 6 < n; s++) {
        if (*s == '\'') { out[o++] = '\''; out[o++] = '\\'; out[o++] = '\''; out[o++] = '\''; }
        else out[o++] = *s;
    }
    out[o++] = '\''; out[o] = 0;
    return out;
}
#endif
