// Reference-emulator feed for the MD watch window (the GB DpRefFeed pattern,
// src/explore/ref_feed.h): an external harness (tools/md_ref_live, ClownMDEmu)
// writes its screen there, and the watch window shows the m0boy replay and the
// reference side by side.  Kept header-only so the harness (built outside the
// repository build, tools/md_ref_live.sh) and the window share one definition.
// No reference implementation is copied.
#pragma once
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DP_MDRF_MAGIC 0x52464D44u /* 'MDRF' */
enum { DP_MDRF_MAX_W = 426, DP_MDRF_MAX_H = 262 }; /* widest/tallest VDP screen */
typedef struct {
    volatile uint32_t magic, seq;      /* seqlock: even = the frame is complete */
    volatile uint32_t vframe, total;
    volatile uint32_t w, h, done;
    volatile uint32_t leader;          /* frame the deepeel core reached: the harness waits while it is ahead (replay sync) */
    volatile uint32_t unsync_x10;      /* harness speed without the sync waits, in tenths of frames/s */
    uint32_t px[DP_MDRF_MAX_W * DP_MDRF_MAX_H];
} DpMdRefFeed;

static inline DpMdRefFeed *dp_md_ref_feed_open(const char *path, int create) {
    if (!path || !*path) return NULL;
    int fd = create ? open(path, O_RDWR | O_CREAT, 0644) : open(path, O_RDWR);
    if (fd < 0 || ftruncate(fd, sizeof(DpMdRefFeed)) != 0) {
        if (fd >= 0) close(fd);
        return NULL;
    }
    void *m = mmap(NULL, sizeof(DpMdRefFeed), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    DpMdRefFeed *f = m == MAP_FAILED ? NULL : (DpMdRefFeed *)m;
    if (f && create) { memset((void *)f, 0, sizeof *f); f->magic = DP_MDRF_MAGIC; }
    return f;
}

// writer side (the harness): one frame under the seqlock
static inline void dp_md_ref_publish(DpMdRefFeed *f, const uint32_t *px, int w, int h,
                                     uint32_t vframe, uint32_t total, int done) {
    if (!f || w <= 0 || h <= 0 || w > DP_MDRF_MAX_W || h > DP_MDRF_MAX_H) return;
    f->seq++;   /* odd: write in progress */
    memcpy((void *)f->px, px, (size_t)w * h * sizeof *px);
    f->w = (uint32_t)w; f->h = (uint32_t)h;
    f->vframe = vframe; f->total = total; f->done = (uint32_t)(done != 0);
    f->seq++;   /* even: stable */
}

// reader side (the window thread): copies the latest complete frame out; 0 when none yet
static inline int dp_md_ref_read(DpMdRefFeed *f, uint32_t *out, int *w, int *h,
                                 uint32_t *vframe, uint32_t *total, int *done) {
    if (!f || f->magic != DP_MDRF_MAGIC) return 0;
    for (int tries = 0; tries < 100; tries++) {
        uint32_t s0 = f->seq;
        if (s0 & 1u) continue;
        __sync_synchronize();
        static uint32_t stage[DP_MDRF_MAX_W * DP_MDRF_MAX_H];   // one reader thread: a torn copy never reaches the caller
        memcpy(stage, (const void *)f->px, sizeof stage);
        int rw = (int)f->w, rh = (int)f->h, rv = (int)f->vframe, rt = (int)f->total, rd = (int)f->done;
        __sync_synchronize();
        if (f->seq == s0) {
            if (rw <= 0 || rh <= 0 || rw > DP_MDRF_MAX_W || rh > DP_MDRF_MAX_H) return 0;
            memcpy(out, stage, (size_t)rw * rh * sizeof *out);
            *w = rw; *h = rh; *vframe = (uint32_t)rv; *total = (uint32_t)rt; *done = rd;
            return 1;
        }
    }
    return 0;
}
