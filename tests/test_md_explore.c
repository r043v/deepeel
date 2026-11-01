// Mega Drive milestone 2: 68000 decoder + forking explorer.
#include <stdio.h>
#include <string.h>
#include "../src/cpu/m68k/m68k.h"
#include "../src/core/cart_md.h"
#include "../src/platform/md/explore.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *dir;

static int run(const char *name, uint64_t total, DpMdResults *r, DpMdCart *c) {
    char path[512], err[128];
    snprintf(path, sizeof path, "%s/%s.md", dir, name);
    if (dp_md_cart_load(c, path, err, sizeof err)) { fprintf(stderr, "load %s: %s\n", path, err); return -1; }
    DpMdOptions o;
    dp_md_options_default(&o);
    o.max_total = total;
    return dp_md_explore(c, &o, r);
}

static int executed(const DpMdResults *r, uint32_t a) {
    return (r->flags[a] & (DP_MDF_NATURAL | DP_MDF_FORCED)) != 0;
}

static void test_disasm(void) {
    unsigned bad = 0;
    for (unsigned op = 0; op < 0x10000; op++) {
        uint8_t b[12] = {op >> 8, op & 0xFF};
        DpM68kInsn in;
        unsigned n = dp_m68k_decode(b, sizeof b, 0x1000, &in);
        if (n < 2 || (n & 1) || n > 12 || n != in.len || !in.text[0]) bad++;
    }
    CHECK(bad == 0);
    DpM68kInsn in;
    uint8_t bne[] = {0x66, 0x10};
    CHECK(dp_m68k_decode(bne, 2, 0x200, &in) == 2 && in.cls == DP_M68K_BCC && in.has_target && in.target == 0x212);
    uint8_t dbf[] = {0x51, 0xC8, 0xFF, 0xFC};
    CHECK(dp_m68k_decode(dbf, 4, 0x300, &in) == 4 && in.cls == DP_M68K_DBCC && in.target == 0x2FE);
    uint8_t jmp[] = {0x4E, 0xF9, 0x00, 0x00, 0x12, 0x34};
    CHECK(dp_m68k_decode(jmp, 6, 0x200, &in) == 6 && in.cls == DP_M68K_JMP && in.has_target && in.target == 0x1234);
    CHECK(dp_m68k_decode(jmp, 4, 0x200, &in) == 0);   // bytes run out
}

int main(int argc, char **argv) {
    dir = argc > 1 ? argv[1] : ".";
    test_disasm();
    DpMdResults r;
    DpMdCart c;

    // both outcomes of a conditional branch
    CHECK(run("fix_md_fork", 100000, &r, &c) == 0);
    CHECK(executed(&r, 0x204));        // natural side (fall-through)
    CHECK(executed(&r, 0x208));        // forced side
    CHECK(r.flags[0x208] & DP_MDF_FORCED);
    CHECK(r.forks >= 1);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    // dbcc: loop exit and loop body both reached
    CHECK(run("fix_md_dbcc", 100000, &r, &c) == 0);
    CHECK(executed(&r, 0x206));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    // a self loop is bounded and its saturation is reported
    CHECK(run("fix_md_loop", 100000, &r, &c) == 0);
    CHECK(r.sat_visit + r.sat_path + r.sat_total >= 1);
    CHECK(r.steps < 100000);
    dp_md_results_free(&r); dp_md_cart_free(&c);

    // V-int enabled: an interrupt fork reaches the handler
    CHECK(run("fix_md_vint", 100000, &r, &c) == 0);
    CHECK(r.irq_forks >= 1);
    CHECK(executed(&r, 0x300));
    dp_md_results_free(&r); dp_md_cart_free(&c);

    // ROM -> VDP DMA is recorded
    CHECK(run("fix_md_dma", 100000, &r, &c) == 0);
    CHECK(r.ncopy >= 1);
    if (r.ncopy >= 1) { CHECK(r.copies[0].src == 0x400); CHECK(r.copies[0].len == 0x10); }
    dp_md_results_free(&r); dp_md_cart_free(&c);

    puts(fails ? "md_explore: FAILED" : "md_explore: ok");
    return fails != 0;
}
