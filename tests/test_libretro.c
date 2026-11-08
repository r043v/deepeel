// test_libretro.c: J11 — level-A libretro backend on a mock core built by the
// tests: load, run, input, serialize round trip, TAS replay determinism,
// framebuffer, and the honest degradation report.
#include "../src/core/dpmachine.h"
#include "../src/mach/libretro.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { uint32_t pcs[16]; int npc; uint32_t wr[8]; int nwr; } T;
static void ex(void *u, uint32_t pc) { (void)u; T.pcs[T.npc++] = pc; }
static void wr(void *u, uint32_t a, uint8_t v) { (void)u; (void)v; T.wr[T.nwr++] = a; }
static uint32_t g_bank, g_dma_src, g_dma_len;
static void bk(void *u, uint32_t w, uint32_t b) { (void)u; (void)w; g_bank = b; }
static void dm(void *u, uint32_t s, uint32_t d, uint32_t n) { (void)u; (void)d; g_dma_src = s; g_dma_len = n; }

int main(int argc, char **argv) {
    CHECK(argc == 3); // core path, a scratch ROM path
    if (argc != 3) return 2;
    FILE *f = fopen(argv[2], "wb");
    fwrite("ABCDEFGH", 1, 8, f);
    fclose(f);

    char err[256] = "";
    setenv("DPMOCK_NOTRACE", "1", 1); // the level-A checks below run the core without the extension
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

    CHECK(dp_lr_level(lr) == 0 && dp_lr_step(lr) == 0); // level A: no step
    unsetenv("DPMOCK_NOTRACE");

    // --- level B: the trace extension is negotiated -------------------------------------
    {
        // program: 0: A=5 / 2: jz 8 / 4: [10]=A / 6: halt / 8: [12]=A / 10: halt
        const uint8_t prog[12] = { 1, 5, 4, 8, 2, 0x10, 5, 0, 2, 0x12, 5, 0 };
        FILE *pf = fopen(argv[2], "wb");
        fwrite(prog, 1, sizeof prog, pf);
        fclose(pf);
        DpLrObservers o = { &T, ex, NULL, wr, dm, bk, NULL };
        for (int side = 0; side < 3; side++) { // natural, forced taken, forced not taken
            DpLibretro *b = dp_lr_open_copy(argv[1], err, sizeof err);
            CHECK_MSG(b != NULL, err);
            if (!b) continue;
            CHECK_MSG(dp_lr_load_rom(b, argv[2], err, sizeof err), err);
            CHECK(dp_lr_level(b) == 1 && strstr(dp_lr_degradations(b), "level B"));
            memset(&T, 0, sizeof T);
            dp_lr_observe(b, &o);
            if (side == 1) CHECK(dp_lr_force_branch(b, 1));
            if (side == 2) CHECK(dp_lr_force_branch(b, 0));
            int guard = 0;
            while (dp_lr_step(b) && ++guard < 100) {}
            CHECK(guard < 100);
            CHECK(dp_lr_rom_index(b, 4) == 4 && dp_lr_rom_index(b, 40) < 0);
            if (side == 0) { // A=5, so the jump is not taken: falls through to the store at 4
                CHECK(T.npc == 4 && T.pcs[2] == 4 && T.nwr == 1 && T.wr[0] == 0x10);
            } else if (side == 1) { // forced taken: the other side runs
                CHECK(T.npc == 4 && T.pcs[2] == 8 && T.nwr == 1 && T.wr[0] == 0x12);
            } else { // forced not taken matches the natural path here
                CHECK(T.npc == 4 && T.pcs[2] == 4 && T.wr[0] == 0x10);
            }
            dp_lr_close(b);
        }
    }

    // dma / bank observers: program 06 03 / 07 40 / halt
    {
        const uint8_t prog2[8] = { 6, 3, 7, 0x40, 5, 0, 0, 0 };
        FILE *pf = fopen(argv[2], "wb");
        fwrite(prog2, 1, sizeof prog2, pf);
        fclose(pf);
        DpLrObservers o = { &T, NULL, NULL, NULL, dm, bk, NULL };
        DpLibretro *b = dp_lr_open_copy(argv[1], err, sizeof err);
        CHECK_MSG(b && dp_lr_load_rom(b, argv[2], err, sizeof err), err);
        if (b) {
            dp_lr_observe(b, &o);
            while (dp_lr_step(b)) {}
            CHECK(g_bank == 3 && g_dma_src == 0x40 && g_dma_len == 4);
            dp_lr_close(b);
        }
    }

    // forking exploration: both sides of the conditional of the first program are covered
    {
        const uint8_t prog[12] = { 1, 5, 4, 8, 2, 0x10, 5, 0, 2, 0x12, 5, 0 };
        FILE *pf = fopen(argv[2], "wb");
        fwrite(prog, 1, sizeof prog, pf);
        fclose(pf);
        DpLibretro *b = dp_lr_open_copy(argv[1], err, sizeof err);
        CHECK_MSG(b && dp_lr_load_rom(b, argv[2], err, sizeof err), err);
        if (b) {
            size_t zn = dp_lr_state_size(b);
            uint8_t *init = malloc(zn);
            dp_lr_save(b, init, zn); // every exploration starts from this state
            uint8_t map[12] = { 0 };
            DpLrExploreLimits lim = { 100, 10000, 64, 0 };
            DpLrExploreStats st;
            CHECK(dp_lr_explore(b, map, sizeof map, &lim, &st));
            // opcode starts 0,2,4,6,8,10 all reached: the natural run alone misses 8 and 10
            CHECK(map[0] && map[2] && map[4] && map[6] && map[8] && map[10]);
            CHECK(st.covered == 6 && st.branch_sites == 1 && st.forks == 2);
            CHECK(st.cut_path_steps == 0 && st.cut_total == 0 && st.dropped_items == 0);
            // a tiny queue is reported, never silent
            dp_lr_load(b, init, zn);
            uint8_t map2[12] = { 0 };
            DpLrExploreLimits tiny = { 100, 10000, 1, 0 };
            CHECK(dp_lr_explore(b, map2, sizeof map2, &tiny, &st) && st.dropped_items > 0);
            // a tiny step wall is reported too
            dp_lr_load(b, init, zn);
            uint8_t map3[12] = { 0 };
            DpLrExploreLimits wall = { 100, 2, 64, 0 };
            CHECK(dp_lr_explore(b, map3, sizeof map3, &wall, &st) && st.cut_total > 0);
            free(init);
            dp_lr_close(b);
        }
    }

    free(snap);
    dp_lr_close(lr);
    // the same mock core behind the generic DpMachine view (spec §7.2 level A):
    // degraded (no step/fetch/rom_index/observe) but real state and frames
    {
        DpLibretro *lr2 = dp_lr_open(argv[1], err, sizeof err);
        CHECK_MSG(lr2 && dp_lr_load_rom(lr2, argv[2], err, sizeof err), err);
        DpMachine *m = dp_machine_libretro(lr2);
        CHECK_MSG(m && !strcmp(m->name, "libretro") && !m->isa && !m->fetch && !m->observe,
                  "libretro machine degrades visibly");
        uint32_t n = m->state_bytes(m);
        CHECK_MSG(n > 0, "state size");
        uint8_t *a = malloc(n), *b2 = malloc(n);
        m->save(m, a); m->load(m, a); m->save(m, b2); m->load(m, b2); m->save(m, a);
        CHECK_MSG(!memcmp(a, b2, n), "libretro save/load/save reaches a fixed point");
        CHECK_MSG(m->run(m), "a frame advances");
        free(a); free(b2);
        m->close(m);   // owns the DpLibretro instance
    }
    remove(argv[2]);
    TEST_DONE("test_libretro");
}
