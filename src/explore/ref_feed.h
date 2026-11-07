// Reference emulator feed shared by the watch window and the movie replay (DEEPEEL_WATCH_REF=<file>).
// An external harness (VBA, tools: VBAH_LIVE=<file>) writes its screen there; the replay of the first
// movie holds back so the two stay on the same frame, and the watch window shows both side by side.
#pragma once
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DP_REF_MAGIC 0xDEE9EE17u
typedef struct {
    volatile uint32_t magic, master, vframe, seq; // magic: set by the harness at its first frame;
                                                  // master: live frame of the replay (written by the window)
    uint8_t shades[160 * 144];                    // DMG shades 0 (white) .. 3 (black)
} DpRefFeed;

static inline DpRefFeed *dp_ref_open(void) {
    const char *path = getenv("DEEPEEL_WATCH_REF");
    if (!path || !*path) return NULL;
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || ftruncate(fd, sizeof(DpRefFeed)) != 0) {
        if (fd >= 0) close(fd);
        return NULL;
    }
    void *m = mmap(NULL, sizeof(DpRefFeed), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return m == MAP_FAILED ? NULL : (DpRefFeed *)m;
}

// The replay of the first movie never runs more than one frame ahead of the reference (once the
// reference is alive); bounded wait so a dead harness cannot freeze the replay for long.
static inline void dp_ref_gate(uint32_t frame) {
    static DpRefFeed *f;
    static int init;
    if (!init) { init = 1; f = dp_ref_open(); }
    if (!f || f->magic != DP_REF_MAGIC) return;
    struct timespec ts = { 0, 20000 };
    for (int i = 0; i < 100000 && frame > f->vframe + 1; i++) nanosleep(&ts, NULL);
}
