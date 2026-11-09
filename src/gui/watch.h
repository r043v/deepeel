/* watch.h: generic replay watch for the fidelity benches
 * (the *_compare_reference tools).  One tile per core, at the platform's
 * own native buffer ratio — letterboxed, never stretched — with two caption
 * lines under each tile (a too-wide name is elided in the middle).  The
 * window is sized from the buffers the caller passes (SMS 256x192, GG
 * 160x144, NES 256x240, ...); small screens are displayed at x2.  The same
 * window serves every platform: nothing in it knows a resolution.
 * Without SDL2 every entry point is a no-op and the bench stays headless. */
#ifndef DP_WATCH_H
#define DP_WATCH_H
#include <stdint.h>

typedef struct DpWatch DpWatch;

/* ntiles (1 or 2) tiles of native buffer size w[i] x h[i] (RGB565 frames);
 * title is the window title (the movie file name belongs there) */
DpWatch *dp_watch_open(const char *title, int ntiles, const int *w, const int *h);

/* one 5:6:5 frame; stride is the buffer's own width; the frame is copied, the
 * caller may reuse its buffer immediately */
void dp_watch_tile(DpWatch *w, int which, const uint16_t *fb565, int stride);

void dp_watch_caption(DpWatch *w, int which, const char *line1, const char *line2);

/* events (window close or escape sets *stop), present, ~60 f/s pacing */
void dp_watch_frame(DpWatch *w, int *stop);

void dp_watch_close(DpWatch *w);

#endif
