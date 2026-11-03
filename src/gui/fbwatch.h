// fbwatch.h: platform-neutral framebuffer watch (--watch for NES/PCE/MD) and PPM export.
// The explorer thread publishes ARGB frames (never slower than the sampling asks for);
// a window thread (SDL2 builds) or a file dump shows them.  No machine state is shared.
#ifndef DP_FBWATCH_H
#define DP_FBWATCH_H
#include <stdint.h>

enum { DP_FB_MAX_W = 512, DP_FB_MAX_H = 512 };
// publish the latest frame (copied); w,h within DP_FB_MAX_*
void dp_fb_publish(const uint32_t *argb, int w, int h, uint64_t frame);
// copy the latest frame out (cap pixels); returns 0 when none yet
int dp_fb_read(uint32_t *out, unsigned cap, int *w, int *h, uint64_t *frame);
// write the latest frame as a binary PPM; returns 0 on success
int dp_fb_dump_ppm(const char *path);
// SDL2 window thread (returns 0 on success; 1 and a message without SDL2)
int dp_fbwatch_available(void);
int dp_fbwatch_start(const char *title);
void dp_fbwatch_stop(void);
#endif
