/* Generic BizHawk movie reader (any core): header, LogKey columns, per-frame buttons and analog axes.
 * One parser for every platform; each loader maps the named columns ("P1 Up", "Reset", "P1 X Axis") onto
 * its own pad bits.  Also the common ground for comparing a movie across emulator cores:
 * dp_bz_diff reports the first frame where two movies differ. */
#ifndef DP_BIZHAWK_H
#define DP_BIZHAWK_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DP_BZ_NAME 48
typedef struct {
    char name[DP_BZ_NAME];   /* column name as written in LogKey ("P1 Up", "Reset", "P1 X Axis") */
    uint8_t group;           /* controller group index (a '#' in LogKey starts a group) */
    uint8_t axis;            /* 1: analog column (numeric cell), else a button */
    uint16_t slot;           /* button: bit index in the frame mask words; axis: index in the frame axes */
} DpBzKey;

typedef struct {
    char platform[32], core[48], game[160];   /* Header.txt Platform / Core / GameName */
    char sha1[41], md5[33];                   /* identity lines as written (core-dependent label) */
    int anchored;                             /* StartsFromSavestate true */
    int pal;                                  /* Header: Pal true (timing region) */
    uint64_t rerecords;
    char *sync_json; size_t sync_n;           /* SyncSettings.json verbatim (core options), may be NULL */
    DpBzKey *keys; unsigned nkeys, nbuttons, naxes, ngroups, words;   /* words: 64-bit mask words per frame */
    uint64_t *bits;                           /* nframes * words */
    int16_t *axes;                            /* nframes * naxes */
    uint32_t nframes;
} DpBzMovie;

/* 1 on success.  Accepts a .bk2 zip or the legacy plain-text .bkm (header + log in one file). */
int dp_bz_load(const uint8_t *d, size_t n, DpBzMovie *m, char *err, size_t errsz);
void dp_bz_free(DpBzMovie *m);
/* column index by exact name (case-insensitive), -1 if absent */
int dp_bz_find(const DpBzMovie *m, const char *name);
/* 1 if the button column `key` is held on `frame` */
int dp_bz_held(const DpBzMovie *m, uint32_t frame, int key);
int dp_bz_axis(const DpBzMovie *m, uint32_t frame, int key);
/* first frame (>= 0) where the two movies' same-named columns differ, -1 if identical over the common length;
 * columns present in only one movie are reported through *only_a / *only_b (counts) */
long dp_bz_diff(const DpBzMovie *a, const DpBzMovie *b, unsigned *only_a, unsigned *only_b);
/* one-line-per-fact description for `deepeel tas info` */
void dp_bz_describe(const DpBzMovie *m, FILE *out);
#endif
