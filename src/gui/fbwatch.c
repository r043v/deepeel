#include "../explore/stop.h"
#include <time.h>
#include <unistd.h>
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

#define DP_NREF 3
static DpMdRefFeed *ref_feed[DP_NREF];
static int ref_feed_tried[DP_NREF];
static const char *const ref_env[DP_NREF] = { "DEEPEEL_MD_REF_FEED", "DEEPEEL_MD_REF2_FEED", "DEEPEEL_MD_REF3_FEED" };
static const char *const ref_name_env[DP_NREF] = { "DEEPEEL_MD_REF_NAME", "DEEPEEL_MD_REF2_NAME", "DEEPEEL_MD_REF3_NAME" };
static const char *const ref_name_def[DP_NREF] = { "ClownMDEmu", "BizHawk GPGX", "Genesis Plus GX" };

int dp_fb_ref_active(int i) {
    if (i < 0 || i >= DP_NREF) return 0;
    if (!ref_feed[i] && ++ref_feed_tried[i] % 30 == 1)   // the harness may start after the window: retry now and then
        ref_feed[i] = dp_md_ref_feed_open(getenv(ref_env[i]), 0);
    return ref_feed[i] && ref_feed[i]->magic == DP_MDRF_MAGIC;
}

const char *dp_fb_ref_name(int i) {
    const char *n = i >= 0 && i < DP_NREF ? getenv(ref_name_env[i]) : NULL;
    return n && *n ? n : i >= 0 && i < DP_NREF ? ref_name_def[i] : "";
}

int dp_fb_ref_read(int i, uint32_t *out, int *w, int *h, uint32_t *vframe, uint32_t *total, int *done) {
    if (!dp_fb_ref_active(i)) return 0;
    return dp_md_ref_read(ref_feed[i], out, w, h, vframe, total, done);
}

static double sync_waited;
static double mono_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
double dp_fb_sync_waited(void) { return sync_waited; }
uint32_t dp_fb_ref_unsync_x10(int i) { return dp_fb_ref_active(i) ? ref_feed[i]->unsync_x10 : 0; }
void dp_fb_sync_gate(uint64_t frame) {
    static double last_prog[2], stale_since[2];
    static uint32_t last_v[2];
    int any = 0;
    for (int k = 0; k < DP_NREF; k++) if (dp_fb_ref_active(k)) { ref_feed[k]->leader = (uint32_t)frame; any = 1; }
    if (!any) return;
    double t0 = mono_now();
    for (;;) {
        int wait = 0;
        double now = mono_now();
        for (int k = 0; k < DP_NREF; k++) {
            if (!dp_fb_ref_active(k) || ref_feed[k]->done) continue;
            uint32_t v = ref_feed[k]->vframe;
            if (v != last_v[k] || !last_prog[k]) { last_v[k] = v; last_prog[k] = now; }
            if (now - last_prog[k] > 2.0 && v == 0) continue;   /* the harness never started: do not stall on it */
            if (now - last_prog[k] > 3.0) continue;               /* stalled harness */
            if (frame > (uint64_t)v + 2) wait = 1;
        }
        if (!wait || dp_stop) break;
        usleep(100);
    }
    sync_waited += mono_now() - t0;
    (void)stale_since;
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
