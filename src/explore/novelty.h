// Shared by every core's novelty search (J4): the game-agnostic lives detector and
// the screen-cell key set.  One implementation, so a fix lands on all platforms.
#ifndef DP_NOVELTY_H
#define DP_NOVELTY_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- lives without game knowledge --------------------------------------------------
// A RAM byte <= 9 whose only decreases are single steps at least 200 frames apart and
// which rarely increases (a new game, a bonus life); a trial in which such a byte drops
// lost a life.  Sized for the largest RAM view (the 68000's 64 KiB).
#define DP_LIVES_MAX 65536
typedef struct {
    size_t n;              // bytes watched (RAM view size)
    uint8_t prev[DP_LIVES_MAX], maxv[DP_LIVES_MAX], bad[DP_LIVES_MAX];
    uint16_t dec[DP_LIVES_MAX], inc[DP_LIVES_MAX];
    uint32_t last_dec[DP_LIVES_MAX];
    uint32_t frame;
    int lives_byte;        // the byte whose drops were last seen as deaths (-1: none)
} DpLives;

static inline DpLives *dp_lives_new(size_t n) {
    DpLives *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->n = n > DP_LIVES_MAX ? DP_LIVES_MAX : n;
    L->lives_byte = -1;
    return L;
}
// one frame of the RAM view w (w[0..L->n)); returns 1 when a learned lives byte dropped
static inline int dp_lives_frame(DpLives *L, const uint8_t *w, int learn) {
    int died = 0;
    for (size_t i = 0; i < L->n; i++) {
        uint8_t a = L->prev[i], b = w[i];
        if (b > L->maxv[i]) L->maxv[i] = b;
        if (a == b) continue;
        if (b + 1 == a) {
            int cand = !L->bad[i] && L->dec[i] >= 2 && L->maxv[i] <= 9 && L->inc[i] <= 1 + L->dec[i] / 4;
            if (cand && !learn) { died = 1; L->lives_byte = (int)i; }
            if (L->frame - L->last_dec[i] < 200) L->bad[i] = 1;
            L->dec[i]++;
            L->last_dec[i] = L->frame;
        } else if (b < a) L->bad[i] = 1;
        else if (b == a + 1 && L->inc[i] < 0xFFFF) L->inc[i]++; // a jump of more than one is a new game
    }
    memcpy(L->prev, w, L->n);
    L->frame++;
    return died;
}
// after restoring an archived state: w becomes the baseline, and the jump is no decrement sequence
static inline void dp_lives_restore(DpLives *L, const uint8_t *w, size_t n) {
    if (n > L->n) n = L->n;
    memcpy(L->prev, w, n);
    L->frame += 200;
}

// --- screen-cell key set -----------------------------------------------------------
// Open addressing over 64-bit keys (0 = empty, bit 0 forced on); the table never grows,
// so it must stay below its load: 2^18 slots for the novelty archives, 2^16 for sessions.
typedef struct { uint64_t *k; uint32_t mask, n; } DpKeys;
static inline int dp_keys_init(DpKeys *s, uint32_t slots) {
    s->k = calloc(slots, sizeof *s->k);
    s->mask = slots - 1;
    s->n = 0;
    return s->k != NULL;
}
static inline void dp_keys_free(DpKeys *s) { free(s->k); s->k = NULL; }
static inline int dp_keys_seen(const DpKeys *s, uint64_t k) {
    k |= 1;
    for (uint32_t i = (uint32_t)(k >> 7) & s->mask;; i = (i + 1) & s->mask) {
        if (!s->k[i]) return 0;
        if (s->k[i] == k) return 1;
    }
}
// 1 when the key is new (and now recorded); 0 when known, or when the table is half full
// (a full table would loop forever: the archive then stops growing, it never hangs)
static inline int dp_keys_add(DpKeys *s, uint64_t k) {
    k |= 1;
    if (s->n >= (s->mask + 1) / 2) return 0;
    for (uint32_t i = (uint32_t)(k >> 7) & s->mask;; i = (i + 1) & s->mask) {
        if (s->k[i] == k) return 0;
        if (!s->k[i]) { s->k[i] = k; s->n++; return 1; }
    }
}

// --- cell archive scoring, shared by every core's Go-Explore archive ---
#include <math.h>
#include <stddef.h>
// yield of a cell (gain per pick): the plain archive's pick score and the replacement score
static inline double dp_cell_yield(uint64_t gain, uint32_t picks) { return (double)gain / (1.0 + picks); }
// pick score of the cell archive: lives-behind cells are damped, deeper and less-picked ones favoured
static inline double dp_cell_score(int behind_on_lives, uint64_t gain, uint32_t depth, uint32_t picks) {
    return (behind_on_lives ? 1e-3 : 1.0) * (1.0 + (double)gain) * (1.0 + depth / 200.0) / sqrt(1.0 + picks);
}
// index of the least productive cell among `n` (array of structs with a uint64 gain and a uint32 picks)
static inline uint32_t dp_cell_victim(const void *cells, size_t stride, size_t gain_off, size_t picks_off, uint32_t n) {
    double worst = 1e300;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *c = (const char *)cells + (size_t)i * stride;
        double sc = dp_cell_yield(*(const uint64_t *)(c + gain_off), *(const uint32_t *)(c + picks_off));
        if (sc < worst) { worst = sc; slot = i; }
    }
    return slot;
}
// compress `n` bytes of state (level 1) through the caller's scratch `ztmp` (capacity zcap) and
// return an exact-size malloc'ed copy; NULL on failure.  Shared by every core's cell archive.
#include <zlib.h>
static inline uint8_t *dp_zdup(Bytef *ztmp, uLongf zcap, const uint8_t *src, uLong n, uLongf *zl) {
    *zl = zcap;
    if (compress2(ztmp, zl, src, n, 1) != Z_OK) return NULL;
    uint8_t *z = malloc(*zl ? *zl : 1);
    if (z) memcpy(z, ztmp, *zl);
    return z;
}
#endif
