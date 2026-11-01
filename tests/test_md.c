// Mega Drive milestone 1: header, registry, linear run, DMA record, budget cut.
#include <stdio.h>
#include <string.h>
#include "../src/core/cart_md.h"
#include "../src/explore/md_trace.h"
#include "../src/platform/md/md_registry.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv) {
    char path[512], err[128];
    snprintf(path, sizeof path, "%s/fix_md_dma.md", argc > 1 ? argv[1] : ".");
    DpMdCart c;
    CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
    CHECK(c.vec[1] == 0x200);

    int r = dp_md_reg_find(0xC00004);
    CHECK(r >= 0 && !strcmp(dp_md_regs[r].name, "VDP_CTRL"));
    CHECK(dp_md_reg_find(0x200000) >= 0 && !strcmp(dp_md_regs[dp_md_reg_find(0x200000)].name, "SRAM"));

    DpMdTrace t;
    CHECK(dp_md_trace_run(&c, 1000, &t) == 0);
    CHECK(t.cut == 1);                 // the final bra.s * only ends by budget
    CHECK(t.steps == 1000);
    CHECK(t.exec_bytes == 52);         // 5 x move.w #,abs.l (8) + move.l (10) + bra.s (2)
    CHECK(t.exec_outside == 0);
    CHECK(t.ncopy == 1);
    if (t.ncopy == 1) {
        CHECK(t.copies[0].src == 0x400);
        CHECK(t.copies[0].len == 0x10);
    }
    CHECK(r >= 0 && t.reg_w[r] == 7);   // 5 word writes + the long write as 2 bus words
    dp_md_trace_free(&t);
    dp_md_cart_free(&c);
    puts(fails ? "md: FAILED" : "md: ok");
    return fails != 0;
}
