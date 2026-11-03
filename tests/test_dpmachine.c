// test_dpmachine.c: the DpIsa / DpMachine contract on the GB (spec §7.2-7.3).
#include <stdlib.h>
#include <string.h>
#include "../src/core/cart.h"
#include "../src/core/dpmachine.h"
#include "../src/explore/frontier.h"
#include "../src/cpu/sm83/sm83.h"
#include "../src/mach/m0boy.h"
#include "test_util.h"

int main(int argc, char **argv) {
    const DpIsa *isa = dp_isa_sm83();
    CHECK_MSG(isa && !strcmp(isa->name, "sm83"), "isa named");
    // length: the DpIsa agrees with the decoder helper and the disassembler
    int checked = 0;
    for (int op = 0; op < 256; op++) {
        uint8_t rom[8] = { (uint8_t)op, 0x34, 0x12, 0, 0, 0, 0, 0 };
        if (op == 0xCB) rom[1] = 0x37;
        int dl = sm83_disasm(rom, sizeof rom, 0, (char[64]){0}, 64);
        int il = isa->length((uint8_t)op);
        CHECK_MSG(il == sm83_len((uint8_t)op), "length matches sm83_len");
        if (dl) CHECK_MSG(il == dl, "length matches the disassembler");
        DpFlowInfo f;
        isa->flow(rom, 0x0100, &f);
        CHECK_MSG(f.length == il, "flow length");
        checked++;
    }
    CHECK_MSG(checked == 256, "all opcodes covered");
    // flow classification on the closure's cases
    DpFlowInfo f;
    uint8_t jr[2] = { 0x20, 0xFE };           // JR NZ,-2 at $0100 -> $0100
    isa->flow(jr, 0x0100, &f);
    CHECK_MSG(f.kind == DP_FLOW_COND && f.target == 0x0100 && f.fall && f.cond == 0, "JR NZ");
    uint8_t jp[3] = { 0xC3, 0x50, 0x01 };
    isa->flow(jp, 0x0000, &f);
    CHECK_MSG(f.kind == DP_FLOW_JUMP && f.target == 0x0150 && !f.fall, "JP nn");
    uint8_t call[3] = { 0xCD, 0x00, 0x40 };
    isa->flow(call, 0x0000, &f);
    CHECK_MSG(f.kind == DP_FLOW_CALL && f.call && f.target == 0x4000 && f.fall, "CALL nn");
    uint8_t rst[1] = { 0xEF };
    isa->flow(rst, 0x0000, &f);
    CHECK_MSG(f.kind == DP_FLOW_CALL && f.target == 0x28, "RST $28");
    uint8_t ret[1] = { 0xC9 };
    isa->flow(ret, 0, &f);
    CHECK_MSG(f.kind == DP_FLOW_RET && !f.fall && f.target < 0, "RET");
    uint8_t retc[1] = { 0xC8 };
    isa->flow(retc, 0, &f);
    CHECK_MSG(f.kind == DP_FLOW_COND && f.fall && f.target < 0 && f.cond == 1, "RET Z");
    uint8_t jphl[1] = { 0xE9 };
    isa->flow(jphl, 0, &f);
    CHECK_MSG(f.kind == DP_FLOW_INDIRECT && !f.fall, "JP HL");
    uint8_t bad[1] = { 0xDD };
    isa->flow(bad, 0, &f);
    CHECK_MSG(f.kind == DP_FLOW_ILLEGAL, "illegal opcode");

    // regs_effect: ld hl,$1234 ; ld a,(hl) from a proven address ; ld h,a
    {
        uint8_t image[0x2000] = {0}; image[0x1234] = 0x5A;
        struct { uint8_t *img; int reads; } cx = { image, 0 };
        int rd(void *c, uint16_t a, uint8_t *v) { typeof(cx) *x = c; if (a >= 0x2000) return 0; *v = x->img[a]; x->reads++; return 1; }
        DpProvRom rom = { &cx, rd };
        DpProvRegs r = {0};
        uint8_t ldhl[3] = { 0x21, 0x34, 0x12 }, lda[1] = { 0x7E }, ldha[1] = { 0x67 }, alu[1] = { 0x80 };
        isa->regs_effect(ldhl, &r, &rom);
        CHECK_MSG(r.pmask == 3 && r.p == 0x1234, "ld hl,nn proves HL");
        isa->regs_effect(lda, &r, &rom);
        CHECK_MSG(r.aknown && r.a == 0x5A && cx.reads == 1, "ld a,(hl) reads the proven address");
        isa->regs_effect(ldha, &r, &rom);
        CHECK_MSG(r.pmask == 3 && r.p == 0x5A34, "ld h,a moves the proof");
        isa->regs_effect(alu, &r, &rom);
        CHECK_MSG(!r.aknown && r.pmask == 3, "alu kills A only");
    }
    // frontier policy (spec §6): tiers, caps, names
    CHECK_MSG(dp_frontier_room(0, 0, 0, DP_R_STEPS) == DP_ROOM_OK, "room at tier 0");
    CHECK_MSG(dp_frontier_room(DP_TIER_MAX, 0, 0, DP_R_STEPS) == DP_ROOM_TIER_FINAL, "past the last tier");
    CHECK_MSG(dp_frontier_room(0, DP_DEFER_CAP, 0, DP_R_PATHS) == DP_ROOM_FULL, "area full");
    CHECK_MSG(dp_frontier_room(0, 0, DP_DEFER_CAP / 4, DP_R_FORK_CAP) == DP_ROOM_FULL, "noisy reason capped");
    CHECK_MSG(dp_frontier_room(0, 0, DP_DEFER_CAP / 4, DP_R_PATHS) == DP_ROOM_OK, "quiet reason not capped");
    for (int r = 1; r < DP_R_COUNT; r++) CHECK_MSG(dp_reason_from_name(dp_reason_name(r)) == r, "reason names round trip");
    CHECK_MSG(!dp_reason_eligible(DP_R_COVERED, 0) && dp_reason_eligible(DP_R_COVERED, 1), "covered needs tier inf");

    // machine: save/load round trip through the generic view
    if (argc > 1) {
        char path[512], err[256];
        snprintf(path, sizeof path, "%s/fix_branch.gb", argv[1]);
        DpCart *c = dp_cart_load(path, err, sizeof err);
        CHECK_MSG(c, "fixture loads");
        int vi = dp_mach_pick_variant(c->rom);
        CHECK_MSG(vi >= 0, "variant");
        DpMach *raw = dp_mach_open(vi, 0);
        CHECK_MSG(raw, "instance");
        dp_mach_set_rom(raw, c->rom, c->rom_size);
        DpMachine *m = dp_machine_gb(raw);
        CHECK_MSG(m && !strcmp(m->name, "gb") && m->isa == isa && m->plat, "machine wired");
        uint32_t n = m->state_bytes(m);
        CHECK_MSG(n > 0, "state size");
        uint8_t *a = malloc(n), *b = malloc(n);
        m->save(m, a);
        m->load(m, a);
        m->save(m, b);
        m->load(m, b);
        m->save(m, a);   // the first image carries a few non-restored clock bytes: fixed point from the second
        CHECK_MSG(!memcmp(a, b, n), "save/load/save reaches a fixed point");
        CHECK_MSG(m->fetch(m, 0x0100) == c->rom[0x0100], "fetch sees the ROM");
        CHECK_MSG(m->rom_index(m, 0x0100) == 0x0100 && m->rom_index(m, 0x8000) < 0, "rom_index");
        free(a); free(b);
        m->close(m);
        dp_cart_free(c);
    }
    TEST_DONE("dpmachine");
}
