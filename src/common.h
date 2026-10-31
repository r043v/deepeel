// deepeel — generic ROM memory-map explorer.
// common.h: shared small utilities (hashing, growable byte buffers).
#ifndef DP_COMMON_H
#define DP_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

#define DP_ARR_INIT_CAP 16

static inline uint64_t dp_fnv1a(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
static inline uint64_t dp_fnv1a_u64(uint64_t h, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return dp_fnv1a(h, b, 8);
}
static inline uint64_t dp_fnv1a_u32(uint64_t h, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    return dp_fnv1a(h, b, 4);
}

// simple growable byte arena (for output building)
typedef struct {
    uint8_t *p;
    size_t len, cap;
} DpBuf;

static inline void dp_buf_init(DpBuf *b) { b->p = NULL; b->len = b->cap = 0; }
static inline void dp_buf_free(DpBuf *b) { free(b->p); dp_buf_init(b); }
static inline void dp_buf_reserve(DpBuf *b, size_t extra) {
    if (b->len + extra > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + extra) nc *= 2;
        b->p = realloc(b->p, nc);
        b->cap = nc;
    }
}
static inline void dp_buf_push(DpBuf *b, const void *data, size_t len) {
    dp_buf_reserve(b, len);
    memcpy(b->p + b->len, data, len);
    b->len += len;
}
static inline void dp_buf_str(DpBuf *b, const char *s) { dp_buf_push(b, s, strlen(s)); }
static inline void dp_buf_ch(DpBuf *b, char c) { dp_buf_push(b, &c, 1); }
static inline void dp_buf_fmt(DpBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    dp_buf_reserve(b, (size_t)n + 1);
    vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

#endif
