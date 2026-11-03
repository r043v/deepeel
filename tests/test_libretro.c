// test_libretro.c: J11 — level-A libretro backend on a mock core built by the
// tests: load, run, input, serialize round trip, TAS replay determinism,
// framebuffer, and the honest degradation report.
#include "../src/mach/libretro.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    CHECK(argc == 3); // core path, a scratch ROM path
    if (argc != 3) return 2;
    FILE *f = fopen(argv[2], "wb");
    fwrite("ABCDEFGH", 1, 8, f);
    fclose(f);

    char err[256] = "";
    CHECK(dp_lr_open("/nonexistent/core.so", err, sizeof err) == NULL && err[0]);
    DpLibretro *lr = dp_lr_open(argv[1], err, sizeof err);
    CHECK_MSG(lr != NULL, err);
    if (!lr) return 1;
    CHECK(!strcmp(dp_lr_core_name(lr), "dpmock"));
    CHECK(!dp_lr_load_rom(lr, "/nonexistent.bin", err, sizeof err));
    CHECK_MSG(dp_lr_load_rom(lr, argv[2], err, sizeof err), err);

    // the degradations are reported, never silent
    const char *d = dp_lr_degradations(lr);
    CHECK(strstr(d, "no instruction step") && strstr(d, "no bus observers"));

    size_t rn = 0;
    const uint8_t *ram = dp_lr_memory(lr, DP_LR_MEM_SYSTEM_RAM, &rn);
    CHECK(ram && rn == 64 && ram[8] == 'A');
    CHECK(dp_lr_memory(lr, DP_LR_MEM_VIDEO_RAM, &rn) == NULL && rn == 0);

    // input: 3 frames of mask 5 -> ram[0] = 15, ram[1] = 3
    dp_lr_set_input(lr, 0, 5);
    dp_lr_advance(lr, 3);
    CHECK(ram[0] == 15 && ram[1] == 3 && dp_lr_frames(lr) == 3);

    // framebuffer: 8x8 XRGB8888 of ram[0]
    int w, h, pitch, fmt;
    const uint8_t *fb = dp_lr_framebuffer(lr, &w, &h, &pitch, &fmt);
    CHECK(fb && w == 8 && h == 8 && pitch == 32 && fmt == DP_LR_FMT_XRGB8888);
    CHECK(fb && fb[0] == 15);

    // serialize round trip: state is restored exactly
    size_t sn = dp_lr_state_size(lr);
    CHECK(sn == 64);
    uint8_t *snap = malloc(sn);
    CHECK(dp_lr_save(lr, snap, sn));
    uint64_t h0 = dp_lr_state_hash(lr);
    dp_lr_advance(lr, 4);
    CHECK(dp_lr_state_hash(lr) != h0);
    CHECK(dp_lr_load(lr, snap, sn));
    CHECK(dp_lr_state_hash(lr) == h0);

    // TAS replay from the snapshot is deterministic
    uint16_t tas[6] = { 1, 1, 0, 8, 2, 4 };
    uint64_t ha[6], hb[6];
    CHECK(dp_lr_replay(lr, tas, 6, ha) == 6);
    uint8_t expect = (uint8_t)(15 + 1 + 1 + 0 + 8 + 2 + 4);
    CHECK(ram[0] == expect);
    CHECK(dp_lr_load(lr, snap, sn));
    CHECK(dp_lr_replay(lr, tas, 6, hb) == 6);
    CHECK(memcmp(ha, hb, sizeof ha) == 0);
    CHECK(ha[0] != ha[1]); // the hash follows the run

    // two independent instances from private copies of the same core (§8.3)
    {
        DpLibretro *a = dp_lr_open_copy(argv[1], err, sizeof err);
        DpLibretro *b = dp_lr_open_copy(argv[1], err, sizeof err);
        CHECK_MSG(a && b, err);
        if (a && b) {
            CHECK_MSG(dp_lr_load_rom(a, argv[2], err, sizeof err), err);
            CHECK_MSG(dp_lr_load_rom(b, argv[2], err, sizeof err), err);
            dp_lr_set_input(a, 0, 7);
            dp_lr_advance(a, 2);
            dp_lr_advance(b, 5);
            size_t na, nb;
            const uint8_t *ra = dp_lr_memory(a, DP_LR_MEM_SYSTEM_RAM, &na);
            const uint8_t *rb = dp_lr_memory(b, DP_LR_MEM_SYSTEM_RAM, &nb);
            CHECK(ra && rb && ra != rb);
            CHECK(ra[0] == 14 && ra[1] == 2); // a saw its own input and frames
            CHECK(rb[0] == 0 && rb[1] == 5);  // b is untouched by a's input
        }
        dp_lr_close(a);
        dp_lr_close(b);
    }

    free(snap);
    dp_lr_close(lr);
    remove(argv[2]);
    TEST_DONE("test_libretro");
}
