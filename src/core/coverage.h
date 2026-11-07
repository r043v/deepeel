// coverage.h: the common coverage model (docs/COUVERTURE.fr.md §4).
// Per ROM byte: a nature (what it is), types (how it is used), sources (who
// proved it, OR-ed, never a single "winner").  The map classes are a pure
// projection of nature x sources, computed at render time.
#ifndef DP_COVERAGE_H
#define DP_COVERAGE_H
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

enum { // nature (u16)
    DP_NAT_CODE_NAT = 1 << 0,   // executed by natural flow
    DP_NAT_CODE_FORCED = 1 << 1,// executed through a forked path
    DP_NAT_CODE_STATIC = 1 << 2,// reached by static flow only
    DP_NAT_DATA_COPIED = 1 << 3,// source of a copy / verbatim copy
    DP_NAT_DATA_STATIC = 1 << 4,// read by statically proven code
    DP_NAT_DATA_READ = 1 << 5,  // read through the ROM windows
    DP_NAT_DATA_INF = 1 << 6,   // inferred sibling of a copied table
    DP_NAT_TABLE = 1 << 7,      // plausible pointer table
    DP_NAT_PAD = 1 << 8,        // padding run
};
enum { // types (u32), second projection (kept open for later milestones)
    DP_T_START = 1 << 0,  // opcode byte of an instruction
    DP_T_ENTRY = 1 << 1,  // reached non-sequentially
    DP_T_IRQ = 1 << 2,    // inside an IRQ handler
    DP_T_VERBATIM = 1 << 3,
};
enum { // sources (u32), OR-ed
    DP_SRC_SESSION = 1 << 0,      // played session (TAS, random, human)
    DP_SRC_FORK_NATURAL = 1 << 1, // fork explorer, natural flow
    DP_SRC_FORK_FORCED = 1 << 2,  // fork explorer, forced path
    DP_SRC_CLOSURE = 1 << 3,      // static flow closure
    DP_SRC_PROVENANCE = 1 << 4,   // statically proven code reads
    DP_SRC_STATIC_SCAN = 1 << 5,  // pointer / padding scan
    DP_SRC_COPY = 1 << 6,         // observed copy ROM->VRAM/OAM
    DP_SRC_SIBLING = 1 << 7,      // sibling inference
    DP_SRC_COUNT = 8
};
// source families for certainty: dynamic-natural / dynamic-forced / static
#define DP_SRC_DYN_NATURAL (DP_SRC_SESSION | DP_SRC_FORK_NATURAL | DP_SRC_COPY)
#define DP_SRC_DYN_FORCED (DP_SRC_FORK_FORCED)
#define DP_SRC_STATIC_FAMILY (DP_SRC_CLOSURE | DP_SRC_PROVENANCE | DP_SRC_STATIC_SCAN | DP_SRC_SIBLING)

static inline const char *dp_src_name(int bit) {
    static const char *n[DP_SRC_COUNT] = { "session", "fork-natural", "fork-forced",
        "static-closure", "provenance", "static-scan", "copy", "sibling" };
    return bit >= 0 && bit < DP_SRC_COUNT ? n[bit] : "?";
}

// Sparse table of first discoveries (docs/COUVERTURE.fr.md §4.3): the first
// time a ROM byte gains any source, one row (byte key, source bit, stamp,
// actor).  The stamp is the platform's step counter; the actor is a worker or
// movie id (0 when unknown).  Bounded: rows past `max` are counted, not kept.
typedef struct { uint32_t key, src; uint64_t stamp; uint32_t actor; } DpFirstSeen;
typedef struct {
    DpFirstSeen *e;
    uint32_t n, cap, max;
    uint64_t dropped;
    pthread_mutex_t mtx;
} DpFirstLog;
static inline void dp_first_init(DpFirstLog *l, uint32_t max) {
    l->e = NULL; l->n = l->cap = 0; l->max = max; l->dropped = 0;
    pthread_mutex_init(&l->mtx, NULL);
}
static inline void dp_first_free(DpFirstLog *l) {
    free(l->e); l->e = NULL; l->n = l->cap = 0;
    pthread_mutex_destroy(&l->mtx);
}
// ORs `bits` into flags[k]; the byte's first proof is logged.  Returns the old bits.
static inline uint32_t dp_cov_mark(DpFirstLog *l, uint32_t *flags, uint32_t k, uint32_t bits,
                                   uint64_t stamp, uint32_t actor) {
    uint32_t cur = __atomic_load_n(&flags[k], __ATOMIC_RELAXED);
    if (cur && (cur & bits) == bits) return cur; // already proven: no shared write (cache line stays shared)
    uint32_t old = __atomic_fetch_or(&flags[k], bits, __ATOMIC_RELAXED);
    if (old || !bits || !l->max) return old;
    pthread_mutex_lock(&l->mtx);
    if (l->n < l->max) {
        if (l->n == l->cap) {
            uint32_t nc = l->cap ? l->cap * 2 : 1024;
            DpFirstSeen *g = (DpFirstSeen *)realloc(l->e, sizeof *g * nc);
            if (g) { l->e = g; l->cap = nc; }
        }
        if (l->n < l->cap) l->e[l->n++] = (DpFirstSeen){ k, bits & (~bits + 1), stamp, actor };
        else l->dropped++;
    } else l->dropped++;
    pthread_mutex_unlock(&l->mtx);
    return old;
}
// legacy flags -> types (second projection): start / entry / irq / verbatim
static inline uint32_t dp_types_of(int start, int entry, int irq, int verbatim) {
    return (start ? DP_T_START : 0) | (entry ? DP_T_ENTRY : 0) | (irq ? DP_T_IRQ : 0) |
           (verbatim ? DP_T_VERBATIM : 0);
}

// Map class from nature, with the historical precedence of dp_map_build.
// Returns 0 (unknown) when nothing proves the byte.  Class numbers follow
// the DP_MAP_* enum of report.h (1 code-nat, 2 forced, 3 static, 4 copied,
// 5 read, 6 data-static, 7 table, 8 padding, 9 inferred-data).
static inline int dp_nature_class(uint16_t n) {
    if (n & DP_NAT_CODE_NAT) return 1;
    if (n & DP_NAT_CODE_FORCED) return 2;
    if (n & DP_NAT_CODE_STATIC) return 3;
    if (n & DP_NAT_DATA_COPIED) return 4;
    if (n & DP_NAT_DATA_STATIC) return 6;
    if (n & DP_NAT_DATA_READ) return 5;
    if (n & DP_NAT_DATA_INF) return 9;
    if (n & DP_NAT_TABLE) return 7;
    if (n & DP_NAT_PAD) return 8;
    return 0;
}
#endif
