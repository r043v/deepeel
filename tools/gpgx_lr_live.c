// gpgx_lr_live: replay a Mega Drive movie through the real Genesis Plus GX libretro core and publish
// each frame into a deepeel watch feed (src/explore/md_ref_feed.h), like md_ref_live / bizhawk_md_live.
// usage: gpgx_lr_live CORE.so ROM masks.bin feedfile   (masks: one GMV-order byte per frame)
#include "mach/libretro.h"
#include "explore/md_ref_feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
// movie bits (0 U 1 D 2 L 3 R 4 B 5 C 6 A 7 Start) to libretro joypad ids (B0 Y1 Start3 Up4 Down5 Left6 Right7 A8)
static uint16_t to_retro(uint8_t m) {
    static const int id[8] = { 4, 5, 6, 7, 0, 8, 1, 3 };
    uint16_t r = 0;
    for (int i = 0; i < 8; i++) if (m & (1u << i)) r |= (uint16_t)(1u << id[i]);
    return r;
}
int main(int c, char **v) {
    if (c < 5) { fprintf(stderr, "usage: gpgx_lr_live CORE.so ROM masks feed\n"); return 2; }
    char err[256] = "";
    DpLibretro *lr = dp_lr_open(v[1], err, sizeof err);
    if (!lr) { fprintf(stderr, "open: %s\n", err); return 1; }
    if (!dp_lr_load_rom(lr, v[2], err, sizeof err)) { fprintf(stderr, "rom: %s\n", err); return 1; }
    FILE *f = fopen(v[3], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *masks = malloc((size_t)n + 1); if (fread(masks, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    DpMdRefFeed *feed = dp_md_ref_feed_open(v[4], 0); if (!feed) return 1;
    static uint32_t px[DP_MDRF_MAX_W * DP_MDRF_MAX_H];
    double t0 = now_s(), waited = 0, lastmove = t0; uint32_t lastleader = 0;
    for (long fr = 0; fr < n; fr++) {
        for (;;) {   // replay sync: within two frames of the deepeel core; a stopped leader releases after 2 s
            double t = now_s(); uint32_t ld = feed->leader;
            if (ld != lastleader) { lastleader = ld; lastmove = t; }
            if ((uint32_t)fr <= ld + 2 || (lastleader != 0 && t - lastmove > 2.0) || (lastleader == 0 && t - t0 > 90.0)) break;
            struct timespec s = { 0, 1000000 }; nanosleep(&s, NULL); waited += 0.001;
        }
        dp_lr_set_input(lr, 0, to_retro(masks[fr]));
        dp_lr_advance(lr, 1);
        int w, h, pitch, fmt; const uint8_t *fb = dp_lr_framebuffer(lr, &w, &h, &pitch, &fmt);
        if (!fb || w <= 0 || h <= 0 || w > DP_MDRF_MAX_W || h > DP_MDRF_MAX_H) continue;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t p;
                if (fmt == 2) { uint16_t q = ((const uint16_t *)(fb + (size_t)y * pitch))[x];   // RGB565
                    p = 0xff000000u | (uint32_t)((q >> 11) * 255 / 31) << 16 | (uint32_t)(((q >> 5) & 63) * 255 / 63) << 8 | (uint32_t)((q & 31) * 255 / 31); }
                else p = 0xff000000u | ((const uint32_t *)(fb + (size_t)y * pitch))[x];
                px[y * w + x] = p;
            }
        feed->seq++;
        memcpy((void *)feed->px, px, (size_t)w * h * 4);
        feed->w = (uint32_t)w; feed->h = (uint32_t)h; feed->vframe = (uint32_t)(fr + 1); feed->total = (uint32_t)n;
        feed->done = fr + 1 == n;
        double busy = now_s() - t0 - waited;
        if (fr > 0 && fr % 30 == 0 && busy > 0.01) feed->unsync_x10 = (uint32_t)(10.0 * fr / busy);
        feed->seq++;
    }
    dp_lr_close(lr);
    return 0;
}
