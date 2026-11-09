#include "fbwatch.h"
#include "../explore/md_ref_feed.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t buf[DP_FB_MAX_W * DP_FB_MAX_H];
static int bw, bh;
static uint64_t bframe;
/* caption state (the GB watch tile information, mutualized): a fixed part set
 * by the CLI and a progress line the explorer updates per published frame;
 * read back by the SDL window thread through dp_fb_caption_read */
static char cap1[160], cap2[160];
static int cap_done;

void dp_fbwatch_caption(const char *base) {
    pthread_mutex_lock(&mu);
    snprintf(cap1, sizeof cap1, "%s", base ? base : "");
    pthread_mutex_unlock(&mu);
}

void dp_fbwatch_progress(uint32_t f, uint32_t total, int done) {
    pthread_mutex_lock(&mu);
    cap_done = done;
    if (total) snprintf(cap2, sizeof cap2, "%u/%u%s", (unsigned)f, (unsigned)total, done ? "  done" : "");
    else snprintf(cap2, sizeof cap2, "%u frames%s", (unsigned)f, done ? "  done" : "");
    pthread_mutex_unlock(&mu);
}

int dp_fb_caption_read(char *c1, size_t n1, char *c2, size_t n2) {
    int done = 0;
    pthread_mutex_lock(&mu);
    snprintf(c1, n1, "%s", cap1);
    snprintf(c2, n2, "%s", cap2);
    done = cap_done;
    pthread_mutex_unlock(&mu);
    return done;
}

static DpMdRefFeed *ref_feed;
static int ref_feed_tried;

int dp_fb_ref_active(void) {
    if (!ref_feed_tried) {
        ref_feed_tried = 1;
        ref_feed = dp_md_ref_feed_open(getenv("DEEPEEL_MD_REF_FEED"), 0);
    }
    return ref_feed && ref_feed->magic == DP_MDRF_MAGIC;
}

int dp_fb_ref_read(uint32_t *out, int *w, int *h, uint32_t *vframe, uint32_t *total, int *done) {
    if (!dp_fb_ref_active()) return 0;
    return dp_md_ref_read(ref_feed, out, w, h, vframe, total, done);
}

void dp_fb_publish(const uint32_t *argb, int w, int h, uint64_t frame) {
    if (w <= 0 || h <= 0 || w > DP_FB_MAX_W || h > DP_FB_MAX_H) return;
    pthread_mutex_lock(&mu);
    memcpy(buf, argb, (size_t)w * h * sizeof *buf);
    bw = w; bh = h; bframe = frame;
    pthread_mutex_unlock(&mu);
}

int dp_fb_read(uint32_t *out, unsigned cap, int *w, int *h, uint64_t *frame) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    if (bw && (unsigned)(bw * bh) <= cap) {
        memcpy(out, buf, (size_t)bw * bh * sizeof *buf);
        *w = bw; *h = bh; *frame = bframe;
        ok = 1;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

int dp_fb_dump_ppm(const char *path) {
    static uint32_t tmp[DP_FB_MAX_W * DP_FB_MAX_H];
    int w, h;
    uint64_t f;
    if (!dp_fb_read(tmp, DP_FB_MAX_W * DP_FB_MAX_H, &w, &h, &f)) return 1;
    FILE *o = fopen(path, "wb");
    if (!o) return 1;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t px[3] = { (uint8_t)(tmp[i] >> 16), (uint8_t)(tmp[i] >> 8), (uint8_t)tmp[i] };
        fwrite(px, 1, 3, o);
    }
    return fclose(o) ? 1 : 0;
}
