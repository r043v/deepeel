// test_map_png.c: J10 — adaptive squares PNG and the lossless dpCv class
// vector. A synthetic 32 MiB map (2048 banks) must render under the pixel
// budget, and the dpCv chunk must decode back to the exact class vector.
#include "../src/report/report.h"
#include "../src/core/snapshot_png.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const int banks = 2048; // 32 MiB
    size_t n = (size_t)banks * 0x4000;
    // reference class vector: runs of pseudo-random length and class
    uint8_t *ref = calloc(n, 1);
    uint32_t x = 12345;
    DpBuf txt;
    dp_buf_init(&txt);
    for (size_t i = 0; i < n;) {
        x = x * 1103515245u + 12345u;
        size_t run = 1 + ((x >> 8) % 3000);
        int c = (int)((x >> 4) % 9);
        if (i + run > n) run = n - i;
        memset(ref + i, c, run);
        i += run;
    }
    // codec alone
    {
        DpBuf cv;
        dp_buf_init(&cv);
        dp_map_cv_encode(ref, n, &cv);
        uint8_t *back = calloc(n, 1);
        CHECK(dp_map_cv_decode(cv.p, cv.len, back, n) == 1);
        CHECK(memcmp(back, ref, n) == 0);
        CHECK(dp_map_cv_decode(cv.p, cv.len, back, n - 1) == 0); // wrong size is refused
        free(back);
        dp_buf_free(&cv);
    }
    // .map text of the same vector (CPU-view addresses), then the PNG
    for (int b = 0; b < banks; b++) {
        unsigned win = b ? 0x4000u : 0u;
        const uint8_t *q = ref + (size_t)b * 0x4000;
        for (int i = 0; i < 0x4000;) {
            int j = i;
            while (j < 0x4000 && q[j] == q[i]) j++;
            dp_buf_fmt(&txt, "%d %x %x %s\n", b, win + (unsigned)i, win + (unsigned)j - 1, dp_map_class_name(q[i]));
            i = j;
        }
    }
    const char *path = "test_map_png.png";
    CHECK(dp_map_png_from_maptext((const char *)txt.p, txt.len, banks, "SYNTH", "{}", path, "gb", "synth.gb",
                                  (unsigned)n));
    int w = 0, h = 0;
    uint8_t *rgb = NULL, *pl = NULL;
    size_t pll = 0;
    CHECK(dp_png_load(path, &w, &h, &rgb, &pl, &pll));
    free(pl);
    CHECK((double)w * h <= (double)dp_map_png_budget * 1.01 + 4096.0 * 64); // legend band
    CHECK(w > 0 && h > 0);
    free(rgb);
    size_t cl = 0;
    char *cv = dp_png_text(path, "dpCv", &cl);
    CHECK(cv != NULL);
    if (cv) {
        uint8_t *back = calloc(n, 1);
        CHECK(dp_map_cv_decode((const uint8_t *)cv, cl, back, n) == 1);
        CHECK(memcmp(back, ref, n) == 0); // the round trip is exact
        free(back);
        free(cv);
    }
    // a small ROM keeps the exact 1 px = 1 o render (bpp 1)
    {
        DpBuf t2;
        dp_buf_init(&t2);
        dp_buf_fmt(&t2, "0 0 3fff code-natural\n1 4000 7fff data-read\n");
        CHECK(dp_map_png_from_maptext((const char *)t2.p, t2.len, 2, "SMALL", "{}", "test_map_small.png", "gb",
                                      "s.gb", 0x8000));
        int w2, h2;
        uint8_t *r2 = NULL, *p2 = NULL;
        size_t l2 = 0;
        CHECK(dp_png_load("test_map_small.png", &w2, &h2, &r2, &p2, &l2));
        free(p2);
        CHECK(w2 == 6 + 2 * (128 + 6));
        free(r2);
        dp_buf_free(&t2);
    }
    // A saved evidence chunk survives re-rendering and supplies played heat.
    {
        const size_t bytes = 0x4000;
        size_t chlen = 28 + bytes + 4 * bytes;
        uint8_t *ch = calloc(chlen, 1);
        memcpy(ch, "DPCH", 4);
        ch[4] = 1; ch[13] = 0x40; ch[21] = 0x40;
        ch[24] = (1u << 0) | (1u << 5);
        ch[28] = DP_XF_NATURAL | DP_XF_START;
        ch[28 + bytes] = 42;
        const uint8_t pixel[3] = {0, 0, 0};
        const char methods[] = "apport : tas 1.0%";
        DpPngText chunks[] = {{"dpCh", ch, chlen, 0}, {"dpMt", methods, sizeof(methods)-1, 0}};
        CHECK(dp_png_save_chunks("test_map_saved.png", 1, 1, pixel, chunks, 2));
        const char map[] = "0 0 3fff code-natural\n";
        CHECK(dp_map_png_from_saved(map, sizeof(map)-1, 1, "SAVED", "{}", "test_map_restored.png",
                                    "gb", "s.gb", bytes, "test_map_saved.png"));
        size_t got = 0;
        char *restored = dp_png_text("test_map_restored.png", "dpCh", &got);
        CHECK(restored && got == chlen && !memcmp(restored, ch, chlen));
        free(restored);
        restored = dp_png_text("test_map_restored.png", "dpMt", &got);
        CHECK(restored && got == sizeof(methods)-1 && !memcmp(restored, methods, got));
        free(restored);
        int w3 = 0, h3 = 0; uint8_t *r3 = NULL, *p3 = NULL; size_t l3 = 0;
        CHECK(dp_png_load("test_map_restored.png", &w3, &h3, &r3, &p3, &l3));
        CHECK(h3 > 128 + 64); // map, separate heat grid, and legend
        free(r3); free(p3); free(ch);
        remove("test_map_saved.png"); remove("test_map_restored.png");
    }
    remove(path);
    remove("test_map_small.png");
    free(ref);
    dp_buf_free(&txt);
    TEST_DONE("test_map_png");
}
