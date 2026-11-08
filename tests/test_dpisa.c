/* test_dpisa.c: the DpIsa kernel interface (spec §7.3) pinned per CPU —
 * length, flow classes, targets, conditions and the disasm path. */
#include "../src/core/dpmachine.h"
#include "../src/cpu/z80/z80.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void test_z80(void) {
    const DpIsa *isa = dp_isa_z80();
    CHECK(!strcmp(isa->name, "z80"), "name");
    DpFlowInfo fi;
    uint8_t nop[] = { 0x00 };
    CHECK(isa->flow(nop, 0x100, &fi) == 1 && fi.kind == DP_FLOW_SEQ && fi.fall, "nop");
    uint8_t jp[] = { 0xC3, 0x50, 0x01 };
    CHECK(isa->flow(jp, 0x100, &fi) == 3 && fi.kind == DP_FLOW_JUMP && !fi.fall && fi.target == 0x0150, "jp nn");
    uint8_t jrnz[] = { 0x20, 0x05 };
    CHECK(isa->flow(jrnz, 0x100, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.fall && fi.cond == 0, "jr nz");
    uint8_t call[] = { 0xCD, 0x00, 0x02 };
    CHECK(isa->flow(call, 0x100, &fi) == 3 && fi.kind == DP_FLOW_CALL && fi.call && fi.target == 0x0200, "call nn");
    uint8_t ret[] = { 0xC9 };
    CHECK(isa->flow(ret, 0x100, &fi) == 1 && fi.kind == DP_FLOW_RET && !fi.fall, "ret");
    uint8_t retz[] = { 0xC8 };
    CHECK(isa->flow(retz, 0x100, &fi) == 1 && fi.kind == DP_FLOW_COND && fi.fall && fi.cond == 1, "ret z");
    uint8_t jphl[] = { 0xE9 };
    CHECK(isa->flow(jphl, 0x100, &fi) == 1 && fi.kind == DP_FLOW_INDIRECT && !fi.fall, "jp hl");
    uint8_t halt[] = { 0x76 };
    CHECK(isa->flow(halt, 0x100, &fi) == 1 && fi.halt, "halt");
    uint8_t ldir[] = { 0xED, 0xB0 };
    CHECK(isa->flow(ldir, 0x100, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.target == 0x100, "ldir self-loop");
    uint8_t djnz[] = { 0x10, 0xFE };
    CHECK(isa->flow(djnz, 0x100, &fi) == 2 && fi.cond == 8, "djnz");
    CHECK(isa->cond_flag(1) == 0x40 && isa->cond_flag(3) == 0x01 && isa->cond_flag(5) == 0x04 && isa->cond_flag(7) == 0x80, "cond flags");
    CHECK(isa->cond_flag(0) == 0x40 && isa->cond_flag(2) == 0x01, "negated conds share the flag");
    CHECK(isa->length(0x21) == 3 && isa->length(0x36) == 2 && isa->length(0xED) == 2 && isa->length(0xDD) == 3, "lengths from the first byte are bounds");
    char txt[32];
    CHECK(isa->disasm(jp, sizeof jp, 0, txt, sizeof txt) == 3 && strstr(txt, "0150"), "disasm jp (addr is a view offset, like sm83)");
}

static void test_sm83(void) {
    const DpIsa *isa = dp_isa_sm83();
    CHECK(!strcmp(isa->name, "sm83"), "name");
    DpFlowInfo fi;
    uint8_t jrnz[] = { 0x20, 0x05 };
    CHECK(isa->flow(jrnz, 0x100, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.fall && fi.cond == 0, "jr nz");
    uint8_t call[] = { 0xCD, 0x00, 0x02 };
    CHECK(isa->flow(call, 0x100, &fi) == 3 && fi.kind == DP_FLOW_CALL && fi.target == 0x0200, "call nn");
    uint8_t rst[] = { 0xFF };
    CHECK(isa->flow(rst, 0x100, &fi) == 1 && fi.kind == DP_FLOW_CALL && fi.rst && fi.target == 0x38, "rst 38");
    uint8_t ill[] = { 0xD3 };
    CHECK(isa->flow(ill, 0x100, &fi) == 1 && fi.kind == DP_FLOW_ILLEGAL, "illegal d3");
    uint8_t jphl[] = { 0xE9 };
    CHECK(isa->flow(jphl, 0x100, &fi) == 1 && fi.kind == DP_FLOW_INDIRECT, "jp hl");
}

static void test_m6502(void) {
    const DpIsa *isa = dp_isa_m6502();
    CHECK(!strcmp(isa->name, "m6502"), "name");
    DpFlowInfo fi;
    uint8_t nop[] = { 0xEA };
    CHECK(isa->flow(nop, 0x8000, &fi) == 1 && fi.kind == DP_FLOW_SEQ && fi.fall, "nop");
    uint8_t jmp[] = { 0x4C, 0x50, 0x80 };
    CHECK(isa->flow(jmp, 0x8000, &fi) == 3 && fi.kind == DP_FLOW_JUMP && !fi.fall && fi.target == 0x8050, "jmp abs");
    uint8_t jmpi[] = { 0x6C, 0x00, 0x02 };
    CHECK(isa->flow(jmpi, 0x8000, &fi) == 3 && fi.kind == DP_FLOW_INDIRECT, "jmp (ind)");
    uint8_t jsr[] = { 0x20, 0x00, 0x90 };
    CHECK(isa->flow(jsr, 0x8000, &fi) == 3 && fi.kind == DP_FLOW_CALL && fi.call && fi.target == 0x9000, "jsr");
    uint8_t rts[] = { 0x60 };
    CHECK(isa->flow(rts, 0x8000, &fi) == 1 && fi.kind == DP_FLOW_RET && !fi.fall, "rts");
    uint8_t bne[] = { 0xD0, 0x05 };
    CHECK(isa->flow(bne, 0x8000, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.fall && fi.target == 0x8007, "bne");
    CHECK(isa->cond_of(0xD0) >= 0 && isa->cond_of(0xEA) < 0, "cond_of");
    CHECK(isa->cond_flag(6) == 0x01 && isa->cond_flag(2) == 0x01, "carry flag both polarities");
    uint8_t jam[] = { 0x02 };
    CHECK(isa->illegal(0x02) && isa->flow(jam, 0x8000, &fi) == 1 && fi.kind == DP_FLOW_ILLEGAL, "jam 02");
    char txt[32];
    CHECK(isa->disasm(jsr, sizeof jsr, 0, txt, sizeof txt) == 3 && strstr(txt, "9000"), "disasm jsr (addr is a view offset)");
}

static void test_huc6280(void) {
    const DpIsa *isa = dp_isa_huc6280();
    CHECK(!strcmp(isa->name, "huc6280"), "name");
    DpFlowInfo fi;
    uint8_t nop[] = { 0x00 };
    CHECK(isa->flow(nop, 0x8000, &fi) >= 1 && fi.fall, "nop/brk falls");
    uint8_t jmp[] = { 0x4C, 0x00, 0x90 };
    CHECK(isa->flow(jmp, 0x8000, &fi) == 3 && fi.kind == DP_FLOW_JUMP && !fi.fall && fi.target == 0x9000, "jmp abs");
    uint8_t jsr[] = { 0x20, 0x00, 0x90 };
    CHECK(isa->flow(jsr, 0x8000, &fi) == 3 && fi.kind == DP_FLOW_CALL && fi.call, "jsr");
    uint8_t rts[] = { 0x60 };
    CHECK(isa->flow(rts, 0x8000, &fi) == 1 && fi.kind == DP_FLOW_RET && !fi.fall, "rts");
    uint8_t bne[] = { 0xD0, 0x05 };
    CHECK(isa->flow(bne, 0x8000, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.fall && fi.target == 0x8007, "bne");
    uint8_t jmpi[] = { 0x6C, 0x00, 0x20 };
    CHECK(isa->flow(jmpi, 0x8000, &fi) == 3 && fi.kind == DP_FLOW_INDIRECT, "jmp (ind)");
    char txt[40];
    CHECK(isa->disasm(jsr, sizeof jsr, 0, txt, sizeof txt) == 3 && strstr(txt, "9000"), "disasm jsr");
}

static void test_68000(void) {
    const DpIsa *isa = dp_isa_68000();
    CHECK(!strcmp(isa->name, "68000") && isa->big_endian && isa->ptr_width == 3, "name, big-endian, 24-bit pointers");
    DpFlowInfo fi;
    uint8_t nop[] = { 0x4E, 0x71 };
    CHECK(isa->flow(nop, 0x400, &fi) == 2 && fi.kind == DP_FLOW_SEQ && fi.fall, "nop");
    uint8_t bra[] = { 0x60, 0x10 };
    CHECK(isa->flow(bra, 0x400, &fi) == 2 && fi.kind == DP_FLOW_JUMP && !fi.fall && fi.target == 0x412, "bra");
    uint8_t bne[] = { 0x66, 0x08 };
    CHECK(isa->flow(bne, 0x400, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.fall && fi.target == 0x40A && fi.cond == 6, "bne");
    uint8_t jsr[] = { 0x4E, 0xB9, 0x00, 0x10, 0x00, 0x00 };
    CHECK(isa->flow(jsr, 0x400, &fi) == 6 && fi.kind == DP_FLOW_CALL && fi.call && fi.target == 0x100000, "jsr abs");
    uint8_t jsran[] = { 0x4E, 0x90 };
    CHECK(isa->flow(jsran, 0x400, &fi) == 2 && fi.kind == DP_FLOW_INDIRECT && fi.call, "jsr (a3) is an indirect call");
    uint8_t rts[] = { 0x4E, 0x75 };
    CHECK(isa->flow(rts, 0x400, &fi) == 2 && fi.kind == DP_FLOW_RET && !fi.fall, "rts");
    uint8_t jmpd[] = { 0x4E, 0xD0 };   // jmp (a3): register-indirect, computed
    CHECK(isa->flow(jmpd, 0x400, &fi) == 2 && fi.kind == DP_FLOW_INDIRECT, "jmp (an)");
    uint8_t stop[] = { 0x4E, 0x72, 0x27, 0x00 };
    CHECK(isa->flow(stop, 0x400, &fi) == 4 && fi.halt, "stop");
    char txt[96];
    CHECK(isa->disasm(jsr, sizeof jsr, 0, txt, sizeof txt) == 6 && strstr(txt, "100000"), "disasm jsr");
}

int main(void) {
    test_z80();
    test_sm83();
    test_m6502();
    test_huc6280();
    test_68000();
    printf(fails ? "dpisa: %d checks failed\n" : "dpisa: all checks passed\n", fails);
    return fails != 0;
}
