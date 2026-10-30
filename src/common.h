// memscan — generic ROM memory-map explorer.
// common.h: shared small utilities (hashing, growable byte buffers).
#ifndef MS_COMMON_H
#define MS_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

#define MS_ARR_INIT_CAP 16

static inline uint64_t ms_fnv1a(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
static inline uint64_t ms_fnv1a_u64(uint64_t h, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return ms_fnv1a(h, b, 8);
}
static inline uint64_t ms_fnv1a_u32(uint64_t h, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    return ms_fnv1a(h, b, 4);
}

// simple growable byte arena (for output building)
typedef struct {
    uint8_t *p;
    size_t len, cap;
} MsBuf;

static inline void ms_buf_init(MsBuf *b) { b->p = NULL; b->len = b->cap = 0; }
static inline void ms_buf_free(MsBuf *b) { free(b->p); ms_buf_init(b); }
static inline void ms_buf_reserve(MsBuf *b, size_t extra) {
    if (b->len + extra > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + extra) nc *= 2;
        b->p = realloc(b->p, nc);
        b->cap = nc;
    }
}
static inline void ms_buf_push(MsBuf *b, const void *data, size_t len) {
    ms_buf_reserve(b, len);
    memcpy(b->p + b->len, data, len);
    b->len += len;
}
static inline void ms_buf_str(MsBuf *b, const char *s) { ms_buf_push(b, s, strlen(s)); }
static inline void ms_buf_ch(MsBuf *b, char c) { ms_buf_push(b, &c, 1); }
static inline void ms_buf_fmt(MsBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    ms_buf_reserve(b, (size_t)n + 1);
    vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

#endif
