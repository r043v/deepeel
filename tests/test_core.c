// test_core.c: the m0boy machine backend — variant picking, mapper writes,
// state round-trips and execution determinism.
#include "../src/core/cart.h"
#include "../src/core/ckptpool.h"
#include "../src/core/growvec.h"
#include "../src/explore/explorer.h" // dp_rom_index
#include "../src/mach/m0boy.h"
#include "mach_util.h"
#include "test_util.h"

static DpCart *cart_of(uint8_t mbc, uint8_t ram, uint32_t size) {
    uint8_t *rom = calloc(1, size);
    rom[0x147] = mbc;
    rom[0x149] = ram;
    return dp_cart_from_memory(rom, size, NULL, 0);
}

// a tiny step-counting hook: cuts after `budget` interpreted instructions
static int g_steps, g_budget;
static void count_step(DpMach *m, uint16_t pc, uint8_t op, int entry) {
    (void)pc;
    (void)op;
    (void)entry;
    if (++g_steps > g_budget) dp_mach_cut(m, DP_CUT_STEPS);
}

// run `n` instructions from power-on with the given code at $0150
static void run_code(DpMach *m, const uint8_t *code, int n, int count) {
    dp_mach_reset(m);
    memcpy(m->api->rom + 0x150, code, (size_t)n);
    m->api->gbr->pc = 0x150;
    m->api->gbr->sp = 0xFFFE;
    g_steps = 0;
    g_budget = count;
    dp_mach_attach(m, NULL, count_step, NULL, NULL, NULL);
    dp_mach_run(m);
}

int main(void) {
    char err[256];
    // --- shared pending-array growth (J1) ---------------------------------------
    {
        uint32_t *v = NULL, cap = 0;
        CHECK(dp_vec_grow((void **)&v, &cap, sizeof *v, 1, 6) && cap == 1);
        CHECK(dp_vec_grow((void **)&v, &cap, sizeof *v, 1, 6) && cap == 2);
        CHECK(dp_vec_grow((void **)&v, &cap, sizeof *v, 1, 6) && cap == 4);
        CHECK(dp_vec_grow((void **)&v, &cap, sizeof *v, 1, 6) && cap == 6);   // clamped to the limit
        CHECK(!dp_vec_grow((void **)&v, &cap, sizeof *v, 1, 6) && cap == 6);  // at the limit: untouched
        CHECK(v[5] == 0);                                                      // new tail is zeroed
        free(v);
        uint32_t *w = NULL, wc = 0;
        CHECK(dp_vec_grow((void **)&w, &wc, sizeof *w, 64, 0) && wc == 64);    // unbounded holding area
        CHECK(dp_vec_grow((void **)&w, &wc, sizeof *w, 64, 0) && wc == 128);
        free(w);
    }
    // --- shared checkpoint pool (J1) ------------------------------------------
    {
        DpCkptPool p = {{0}, 0};
        uint32_t rng = 12345;
        CHECK(dp_ckpt_claim(&p, 0, 16, &rng) == NULL);          // cap 0: no pool
        for (unsigned i = 0; i < 3; i++) { uint8_t *b = dp_ckpt_claim(&p, 3, 16, &rng); CHECK(b != NULL); if (b) b[0] = (uint8_t)(i + 1); }
        CHECK(p.n == 3);
        uint32_t before = rng;
        uint8_t *rep = dp_ckpt_claim(&p, 3, 16, &rng);          // full: random replacement, pool size unchanged
        CHECK(rep != NULL && p.n == 3 && rng != before);
        const uint8_t *pk = dp_ckpt_pick(&p, &rng);
        CHECK(pk != NULL);
        dp_ckpt_free(&p);
        CHECK(p.n == 0 && p.slot[0] == NULL);
    }
    // --- variant picking -----------------------------------------------------
    {
        DpCart *c = cart_of(0x19, 0x03, 0x800000); // MBC5+32K RAM
        CHECK(dp_mach_pick_variant(c->rom) >= 0);
        DpMach *m = mach_for(c);
        CHECK(m != NULL);
        CHECK(m->api->rom_max == 0x800000);
        CHECK(m->api->eram != NULL);
        CHECK(m->api->state_bytes() > 0x4000); // ERAM rides in the state
        dp_mach_close(m);
        dp_cart_free(c);
    }
    {
        DpCart *c = cart_of(0x00, 0x00, 0x8000); // ROM only
        DpMach *m = mach_for(c);
        CHECK(m != NULL);
        CHECK(m->api->eram == NULL);
        CHECK(dp_mach_pick_variant(c->rom) == dp_mach_pick_variant(((DpCart *)c)->rom));
        dp_mach_close(m);
        dp_cart_free(c);
    }
    // --- mapper writes: MBC1 bank register -------------------------------------
    {
        DpCart *c = cart_of(0x01, 0x00, 0x40000); // MBC1, 256 KiB
        DpMach *m = mach_for(c);
        CHECK(m != NULL);
        dp_mach_reset(m);
        CHECK(m->api->gbr->rom_bank == 1); // boot leaves bank 1
        m->api->wr(0x2000, 5);             // ROMB0 = 5
        CHECK(m->api->gbr->rom_bank == 5);
        m->api->wr(0x2000, 0); // MBC1 folds bank 0 to 1
        CHECK(m->api->gbr->rom_bank == 1);
        m->api->wr(0x2000, 2);
        // rmap follows: $4000 now reads bank 2 through the fast page
        m->api->rom[2 * 0x4000 + 0x10] = 0xAB;
        CHECK(m->api->rmap[0x4] && m->api->rmap[0x4][0x4010] == 0xAB);
        dp_mach_close(m);
        dp_cart_free(c);
    }
    // --- joypad buttons ----------------------------------------------------------
    {
        DpCart *c = cart_of(0x00, 0x00, 0x8000);
        DpMach *m = mach_for(c);
        dp_mach_reset(m);
        m->api->io[0x00] = 0x20; // P1 selects the action buttons
        m->api->set_buttons(0xFF);
        CHECK((*m->api->joyp & 0x0F) == 0x00); // P1 low nibble: pressed = 0
        m->api->set_buttons(0x00);
        CHECK((*m->api->joyp & 0x0F) == 0x0F);
        dp_mach_close(m);
        dp_cart_free(c);
    }
    // --- ADD flag vectors on the real machine (the old executor's evidence) -------
    {
        DpCart *c = cart_of(0x00, 0x00, 0x8000);
        DpMach *m = mach_for(c);
        uint8_t add[] = {0x3E, 0x7F, 0xC6, 0x01, 0x76}; // ld a,$7F; add $01; halt
        run_code(m, add, 5, 2);
        CHECK(m->api->gbr->a == 0x80);
        CHECK(m->api->gbr->hf == 1 && m->api->gbr->cf == 0 && m->api->gbr->zr != 0);
        uint8_t add2[] = {0x3E, 0xFF, 0xC6, 0x01, 0x76}; // $FF + 1: Z H C
        run_code(m, add2, 5, 2);
        CHECK(m->api->gbr->a == 0x00);
        CHECK(m->api->gbr->hf == 1 && m->api->gbr->cf == 1 && m->api->gbr->zr == 0);
        uint8_t sub[] = {0x3E, 0x03, 0xD6, 0x05, 0x76}; // 3 - 5: borrow, N
        run_code(m, sub, 5, 2);
        CHECK(m->api->gbr->a == 0xFE);
        CHECK(m->api->gbr->cf == 1 && m->api->gbr->nf == 1);
        // CALL/RET round the stack
        uint8_t call[] = {0xCD, 0x58, 0x01, 0x76, 0x00, 0x00,
                          0x00, 0x00, 0xC9}; // call $158; halt; ...: ret
        run_code(m, call, 9, 3);
        CHECK(m->api->gbr->pc == 0x154); // halt after the return ran
        dp_mach_close(m);
        dp_cart_free(c);
    }
    // --- state round-trip: save/load continues deterministically -------------------
    {
        DpCart *c = cart_of(0x01, 0x00, 0x40000);
        DpMach *m = mach_for(c);
        uint8_t code[] = {0x3E, 0x01, 0x3C, 0x3C, 0x3C, 0x76}; // a=1; inc a x3; halt
        run_code(m, code, 6, 2);                               // a=1 -> 2
        uint32_t sb = dp_mach_state_bytes(m);
        uint8_t *img = malloc(sb), *img2 = malloc(sb);
        dp_mach_save(m, img);
        memcpy(img2, img, sb);
        // two saves agree
        dp_mach_save(m, img2);
        CHECK(memcmp(img, img2, sb) == 0);
        // a fresh machine restored from the image continues identically
        run_code(m, code, 6, 2); // back to the same state by replay
        dp_mach_load(m, img);
        dp_mach_attach(m, NULL, count_step, NULL, NULL, NULL);
        g_steps = 0;
        g_budget = 2;
        dp_mach_run(m);
        uint8_t *after_a = malloc(sb);
        dp_mach_save(m, after_a);
        dp_mach_load(m, img);
        dp_mach_attach(m, NULL, count_step, NULL, NULL, NULL);
        g_steps = 0;
        g_budget = 2;
        dp_mach_run(m);
        uint8_t *after_b = malloc(sb);
        dp_mach_save(m, after_b);
        CHECK(memcmp(after_a, after_b, sb) == 0); // deterministic continuation
        CHECK(m->api->gbr->a == 4); // two more inc from the saved a=2
        free(after_a);
        free(after_b);
        free(img);
        free(img2);
        dp_mach_close(m);
        dp_cart_free(c);
    }
    // --- a fork by flag flip explores the other side -------------------------------
    {
        DpCart *c = cart_of(0x00, 0x00, 0x8000);
        DpMach *m = mach_for(c);
        // ld a,$FF; or a (Z clear); jr z +4; ld b,$11; jr +4; ld b,$22; halt...
        uint8_t code[] = {0x3E, 0xFF, 0xB7, 0x28, 0x04, 0x06, 0x11, 0x18, 0x04,
                          0x06, 0x22, 0x76, 0x76, 0x76};
        run_code(m, code, 14, 4); // or a; jr z (not taken); ld b,$11
        CHECK(m->api->gbr->b == 0x11 && m->api->gbr->zr != 0);
        // flip Z and stand before the conditional: the machine takes it
        m->api->gbr->zr = 0;
        m->api->gbr->pc = 0x153;
        dp_mach_attach(m, NULL, count_step, NULL, NULL, NULL);
        g_steps = 0;
        g_budget = 2;
        dp_mach_run(m);
        CHECK(m->api->gbr->b == 0x22);
        dp_mach_close(m);
        dp_cart_free(c);
    }
    TEST_DONE("test_core");
}
