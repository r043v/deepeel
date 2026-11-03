#include "fbwatch.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t buf[DP_FB_MAX_W * DP_FB_MAX_H];
static int bw, bh;
static uint64_t bframe;

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
