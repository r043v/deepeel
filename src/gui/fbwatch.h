// fbwatch.h: platform-neutral framebuffer watch (--watch for NES/PCE/MD) and PPM export.
// The explorer thread publishes ARGB frames (never slower than the sampling asks for);
// a window thread (SDL2 builds) or a file dump shows them.  No machine state is shared.
#ifndef DP_FBWATCH_H
#define DP_FBWATCH_H
#include <stddef.h>
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
// caption under the image (the GB watch tile information, mutualized): a
// fixed part set by the CLI (movie index and file name) and a progress line
// the explorer updates per published frame; done dims the progress line
void dp_fbwatch_caption(const char *base);
void dp_fbwatch_progress(uint32_t frame, uint32_t total, int done);
// the window thread reads the caption back; returns the done flag
int dp_fb_caption_read(char *c1, size_t n1, char *c2, size_t n2);
// reference tile (DEEPEEL_MD_REF_FEED, src/explore/md_ref_feed.h): the
// external harness's latest frame; returns 0 without a feed or frame yet
int dp_fb_ref_active(void);
int dp_fb_ref_read(uint32_t *out, int *w, int *h, uint32_t *vframe, uint32_t *total, int *done);
#endif
