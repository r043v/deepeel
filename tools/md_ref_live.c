/* Live reference tile for the MD watch window: replays one movie on the
 * ClownMDEmu library (the reference core of tools/md_compare_reference) and
 * publishes each frame into the shared feed (DEEPEEL_MD_REF_FEED,
 * src/explore/md_ref_feed.h), the GB VBA harness pattern.  Built outside the
 * repository build by tools/md_ref_live.sh; no reference implementation is
 * copied.  The three-button replay is the only pad modelled (as in the
 * explorer); the two cores run at their own pace, the watch window shows
 * each tile's speed separately. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clownmdemu.h"
#include <time.h>
#include <unistd.h>
#include "explore/md_ref_feed.h"
#include "platform/md/movie.h"

/* DpMdMovie pad byte (GMV bit order) -> ClownMDEmu button */
static int pad_bit(ClownMDEmu_Button b) {
    switch (b) {
    case CLOWNMDEMU_BUTTON_UP: return 0;
    case CLOWNMDEMU_BUTTON_DOWN: return 1;
    case CLOWNMDEMU_BUTTON_LEFT: return 2;
    case CLOWNMDEMU_BUTTON_RIGHT: return 3;
    case CLOWNMDEMU_BUTTON_A: return 6;   /* GMV bit 6 */
    case CLOWNMDEMU_BUTTON_B: return 4;
    case CLOWNMDEMU_BUTTON_C: return 5;
    case CLOWNMDEMU_BUTTON_START: return 7;
    default: return -1;   /* X/Y/Z/Mode: not modelled by the three-button replay */
    }
}

static const DpMdMovie *g_movie;
static uint32_t g_frame;

static cc_bool input(void *u, cc_u8f p, ClownMDEmu_Button b) {
    (void)u;
    if (p != 0 || !g_movie || g_frame >= g_movie->frames) return 0;
    int bit = pad_bit(b);
    return bit >= 0 && (g_movie->pads[g_frame] >> bit) & 1;
}

static uint32_t palette[256];
static uint32_t screen[DP_MDRF_MAX_H][DP_MDRF_MAX_W];
static int scr_w, scr_h;

/* the palette colour is the CRAM word (12-bit 0xBGR) */
static void colour(void *u, cc_u16f i, cc_u16f c) {
    (void)u;
    if (i < 256) {
        uint32_t r = (c & 0xF) * 17, g = ((c >> 4) & 0xF) * 17, b = ((c >> 8) & 0xF) * 17;
        palette[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
}

/* the pixels are palette indices; the boundaries may overdraw past the screen */
static void scanline(void *u, cc_u16f y, const cc_u8l *p, cc_u16f l, cc_u16f r,
                     cc_u16f w, cc_u16f h) {
    (void)u;
    if (y >= DP_MDRF_MAX_H || w > DP_MDRF_MAX_W) return;
    scr_w = w; scr_h = h;
    for (cc_u16f x = l; x < r && x < DP_MDRF_MAX_W; x++)
        screen[y][x] = palette[p[x - l] & 0xFF];
}

static void audio(void *u, ClownMDEmu *m, size_t n, void (*gen)(ClownMDEmu *, cc_s16l *, size_t)) {
    cc_s16l buf[2048]; (void)u;
    while (n) { size_t k = n > 1024 ? 1024 : n; gen(m, buf, k); n -= k; }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: md_ref_live ROM movie\n");
        fprintf(stderr, "DEEPEEL_MD_REF_FEED=<file> names the shared feed\n");
        return 2;
    }
    DpMdRefFeed *feed = dp_md_ref_feed_open(getenv("DEEPEEL_MD_REF_FEED"), 1);
    if (!feed) { fprintf(stderr, "md_ref_live: cannot open the feed (DEEPEEL_MD_REF_FEED)\n"); return 2; }
    DpMdMovie movie;
    char err[256];
    if (!dp_md_movie_load(argv[2], &movie, err, sizeof err)) {
        fprintf(stderr, "md_ref_live: %s: %s\n", argv[2], err);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "md_ref_live: cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n < 512 || n > 0x400000 || (n & 1)) { fprintf(stderr, "md_ref_live: invalid ROM size\n"); return 2; }
    uint8_t *rom = malloc((size_t)n); cc_u16l *words = malloc(((size_t)n / 2) * sizeof *words);
    if (!rom || !words || fread(rom, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    for (long i = 0; i < n / 2; i++) words[i] = (cc_u16l)((rom[2*i] << 8) | rom[2*i+1]);

    ClownMDEmu *ref = calloc(1, sizeof *ref);
    ClownMDEmu_InitialConfiguration config = {0}; ClownMDEmu_Callbacks cb = {0};
    config.general.region = CLOWNMDEMU_REGION_OVERSEAS;
    config.general.tv_standard = CLOWNMDEMU_TV_STANDARD_NTSC;
    cb.input_requested = input; cb.colour_updated = colour; cb.scanline_rendered = scanline;
    cb.fm_audio_to_be_generated = audio; cb.psg_audio_to_be_generated = audio;
    cb.pcm_audio_to_be_generated = audio; cb.cdda_audio_to_be_generated = audio;
    ClownMDEmu_Constant_Initialise(); ClownMDEmu_Initialise(ref, &config, &cb);
    ClownMDEmu_SetCartridge(ref, words, (cc_u32f)(n / 2)); ClownMDEmu_HardReset(ref, 1, 0);

    g_movie = &movie;
    /* MD_REF_DSY=file: per-frame "wram" (64 KiB big-endian) in DSY1 (src/core/desync.h), for `deepeel desync` */
    FILE *dsy = getenv("MD_REF_DSY") ? fopen(getenv("MD_REF_DSY"), "wb") : NULL;
    if (dsy) fwrite("DSY1", 1, 4, dsy);
    static uint32_t packed[DP_MDRF_MAX_W * DP_MDRF_MAX_H];
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    double waited = 0, last_move = 0; uint32_t last_leader = 0;
    for (g_frame = 0; g_frame < movie.frames; g_frame++) {
        /* replay sync: stay within two frames of the deepeel core; a leader that stopped moving for 2 s releases us */
        for (;;) {
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            double now = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
            if (feed->leader != last_leader) { last_leader = feed->leader; last_move = now; }
            if (g_frame <= (uint32_t)feed->leader + 2 || (last_leader != 0 && now - last_move > 2.0) || (last_leader == 0 && now > 90.0)) break;
            usleep(300); waited += 300e-6;
        }
        if (g_frame && g_frame % 30 == 0) {
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            double el = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9 - waited;
            if (el > 0.01) feed->unsync_x10 = (uint32_t)(10.0 * g_frame / el);
        }
        ClownMDEmu_Iterate(ref);
        if (dsy) {
            static uint8_t sw[0x10000];
            for (unsigned i = 0; i < 0x8000; i++) { sw[2*i] = (uint8_t)(ref->state.m68k.ram[i] >> 8); sw[2*i+1] = (uint8_t)ref->state.m68k.ram[i]; }
            uint32_t fn = g_frame + 1, len = 0x10000; uint16_t nb = 1; uint8_t nl = 4;
            fwrite(&fn, 4, 1, dsy); fwrite(&nb, 2, 1, dsy); fwrite(&nl, 1, 1, dsy); fwrite("wram", 1, 4, dsy);
            fwrite(&len, 4, 1, dsy); fwrite(sw, 1, len, dsy);
        }
        if (scr_w && scr_h) {
            for (int y = 0; y < scr_h; y++)   /* the row stride differs from the screen width */
                memcpy(packed + (size_t)y * scr_w, screen[y], (size_t)scr_w * sizeof *packed);
            dp_md_ref_publish(feed, packed, scr_w, scr_h, g_frame + 1,
                              movie.frames, g_frame + 1 == movie.frames);
        }
    }
    if (dsy) fclose(dsy);
    dp_md_movie_free(&movie);
    free(rom); free(words); free(ref);
    return 0;
}
