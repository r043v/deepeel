// Mega Drive milestone 1: header, registry, linear run, DMA record, budget cut.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/core/cart_md.h"
#include "../src/mach/mdmach.h"
#include "../src/explore/md_trace.h"
#include "../src/platform/md/md_registry.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { DpMdMach *m; void *first, *last; unsigned count, first_at, last_at; } SnapTest;
static void snapshot_end(void *user, uint32_t pc, uint32_t next, unsigned len) {
    SnapTest *s = user;
    (void)pc; (void)next; (void)len;
    if (++s->count == s->first_at) dp_mdmach_save(s->m, s->first);
    if (s->count == s->last_at) { dp_mdmach_save(s->m, s->last); dp_mdmach_halt(s->m); }
}

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
    snprintf(path, sizeof path, "%s/fix_md_loop.md", argc > 1 ? argv[1] : ".");
    CHECK(dp_md_cart_load(&c, path, err, sizeof err) == 0);
    DpMdMach *m = dp_mdmach_open(c.rom, c.size);
    size_t sz = dp_mdmach_state_size();
    void *first = malloc(sz), *expected = malloc(sz), *actual = malloc(sz);
    CHECK(m && first && expected && actual);
    if (m && first && expected && actual) {
        SnapTest s = {m, first, expected, 0, 100, 20000};
        DpMdHooks h = {0}; h.user = &s; h.on_end = snapshot_end;
        dp_mdmach_set_hooks(m, &h); dp_mdmach_reset(m);
        int cut, fault;
        dp_mdmach_run(m, 30000, &cut, &fault);
        CHECK(s.count == 20000 && !fault);
        dp_mdmach_load(m, first);
        s.count = 0; s.first_at = 0; s.last_at = 19900; s.last = actual;
        dp_mdmach_run(m, 30000, &cut, &fault);
        CHECK(s.count == 19900 && !fault);
        CHECK(memcmp(expected, actual, sz) == 0);
        dp_mdmach_state_force_cc(first, 6, 0); /* BNE false => Z set */
        dp_mdmach_load(m, first);
        CHECK(dp_mdmach_sr(m) & 4);
        dp_mdmach_state_force_cc(first, 6, 1); /* BNE true => Z clear */
        dp_mdmach_load(m, first);
        CHECK(!(dp_mdmach_sr(m) & 4));
        dp_mdmach_state_force_dbcc(first, 1, 0, 1, 0xABCD0000u, 0x2704);
        dp_mdmach_load(m, first);
        CHECK(dp_mdmach_dn(m, 0) == 0xABCD0000u); /* DBF taken, decremented to zero */
        dp_mdmach_state_force_dbcc(first, 1, 0, 0, 0xABCD0004u, 0x2704);
        dp_mdmach_load(m, first);
        CHECK(dp_mdmach_dn(m, 0) == 0xABCDFFFFu); /* DBF exhausted */
        dp_mdmach_state_force_dbcc(first, 6, 0, 0, 0xABCD0004u, 0x2704);
        dp_mdmach_load(m, first);
        CHECK(dp_mdmach_dn(m, 0) == 0xABCD0004u && !(dp_mdmach_sr(m) & 4));
    }
    free(first); free(expected); free(actual); dp_mdmach_close(m); dp_md_cart_free(&c);
    puts(fails ? "md: FAILED" : "md: ok");
    return fails != 0;
}
