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
#endif
