// DpIsa.code_table of the 68000: the table-read idioms of Streets of Rage and Sonic, classified without a machine
// (only fetch is used).  Each case: code bytes at pc 0x1000, expected stride/rel/base_reg.
#include "../src/core/dpmachine.h"
#include <stdio.h>
#include <string.h>

static uint8_t mem[0x2000];
static uint8_t fetch(DpMachine *m, uint32_t a) { (void)m; return mem[a & 0x1FFF]; }

static int fails;
static void check(const char *name, const uint8_t *code, unsigned n, int want, int stride, int rel, int base_reg) {
    memset(mem, 0, sizeof mem); memcpy(mem + 0x1000, code, n);
    DpMachine m; memset(&m, 0, sizeof m); m.fetch = fetch;
    DpCodeTable t; uint32_t lp;
    const DpIsa *isa = dp_isa_68000();
    int got = isa->code_table(&m, 0x1000, mem + 0x1000, &t, &lp);
    int ok = got == want && (!want || (t.stride == stride && t.rel == rel && t.base_reg == base_reg));
    if (!ok) { fails++; printf("FAIL %s: got %d stride %d rel %d reg %d\n", name, got, got ? t.stride : -1, got ? t.rel : -1, got ? t.base_reg : -9); }
}
int main(void) {
    // jmp T(pc,d0.w)
    { uint8_t c[] = {0x4E,0xFB,0x00,0x10}; check("jmp pc", c, 4, 1, 2, 1, -1); }
    // jmp (a1,d0.w)
    { uint8_t c[] = {0x4E,0xF1,0x00,0x00}; check("jmp An,Xn", c, 4, 1, 2, 1, DP_M68KR_A0 + 1); }
    // move.w T(pc,d0.w),d0 ; movea.l d0,a1 ; jmp (a1)  -> abs16
    { uint8_t c[] = {0x30,0x3B,0x00,0x10, 0x22,0x40, 0x4E,0xD1}; check("abs16", c, 8, 1, 2, 0, -1); }
    // SoR sub_B186: move.w (a1,d0.w),d0 ; movea.l d0,a1 ; jmp (a1)  -> abs16 from the register
    { uint8_t c[] = {0x30,0x31,0x00,0x00, 0x22,0x40, 0x4E,0xD1}; check("abs16 An", c, 8, 1, 2, 0, DP_M68KR_A0 + 1); }
    // move.l T(pc,d0.w),d0 ; movea.l d0,a0 ; jsr (a0)  -> abs32
    { uint8_t c[] = {0x20,0x3B,0x00,0x10, 0x20,0x40, 0x4E,0x90}; check("abs32 GameModeArray", c, 8, 1, 4, 0, -1); }
    // movea.l T(pc,d0.w),a0 ; jsr (a0)
    { uint8_t c[] = {0x20,0x7B,0x00,0x10, 0x4E,0x90}; check("movea.l", c, 6, 1, 4, 0, -1); }
    // adda.w (a1,d0.w),a1 ; jmp (a1)  -> rel16 from a1
    { uint8_t c[] = {0xD2,0xF1,0x00,0x00, 0x4E,0xD1}; check("adda.w", c, 6, 1, 2, 1, DP_M68KR_A0 + 1); }
    // move.w (a1,d0.w),d0 ; ext.l d0 ; adda.l d0,a1 ; jmp (a1)  -> rel16 from a1
    { uint8_t c[] = {0x30,0x31,0x00,0x00, 0x48,0xC0, 0xD3,0xC0, 0x4E,0xD1}; check("ext+adda.l", c, 10, 1, 2, 1, DP_M68KR_A0 + 1); }
    // move.w (a1,d0.w),d0 ; jmp (a1,d0.w) -> rel16, the jump is also a table
    { uint8_t c[] = {0x30,0x31,0x00,0x00, 0x4E,0xF1,0x00,0x00}; check("move.w+jmp An,Xn", c, 8, 1, 2, 1, DP_M68KR_A0 + 1); }
    // Sonic 1 Junction: move.w Jun_Index(pc,d0.w),d1 ; jmp Jun_Index(pc,d1.w)  (d8 6 then 2: same base)
    { uint8_t c[] = {0x32,0x3B,0x00,0x06, 0x4E,0xFB,0x10,0x02}; check("obj state table", c, 8, 1, 2, 1, -1); }
    // negatives: movea.l T,a0 not followed by a call: a data pointer table
    { uint8_t c[] = {0x20,0x7B,0x00,0x10, 0x4E,0x71}; check("data ptr", c, 6, 0, 0, 0, 0); }
    // move.w then a plain use: not a dispatch
    { uint8_t c[] = {0x30,0x3B,0x00,0x10, 0x4E,0x71}; check("move.w nop", c, 6, 0, 0, 0, 0); }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails != 0;
}
