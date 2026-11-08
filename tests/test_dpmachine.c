// test_dpmachine.c: the DpIsa / DpMachine contract on the GB (spec §7.2-7.3).
#include <stdlib.h>
#include <string.h>
#include "../src/core/cart.h"
#include "../src/core/dpmachine.h"
#include "../src/explore/frontier.h"
#include "../src/cpu/sm83/sm83.h"
#include "../src/mach/m0boy.h"
#include "../src/platform/sega/backend.h"
#include "../src/mach/nesmach.h"
#include "../src/mach/mdmach.h"
#include "test_util.h"

static unsigned obs_steps, obs_reads;
static DpMach *obs_mach;
static void obs_step(void *u, uint16_t pc, uint8_t op, int entry) {
    (void)u; (void)pc; (void)op; (void)entry;
    if (++obs_steps >= 3000) dp_mach_cut(obs_mach, DP_CUT_QUIT);
}
static void obs_read(void *u, uint16_t a, uint8_t v) { (void)u; (void)a; (void)v; obs_reads++; }
static void obs_write(void *u, uint16_t a, uint8_t v) { (void)u; (void)a; (void)v; obs_reads++; }

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

    // mem_operand
    {
        DpMemOp o;
        uint8_t ld_hl_a[] = { 0x77 }, ldh[] = { 0xE0, 0x40 }, abs_r[] = { 0xFA, 0x34, 0xC2 };
        uint8_t hli[] = { 0x2A }, bit_hl[] = { 0xCB, 0x46 }, set_hl[] = { 0xCB, 0xC6 };
        uint8_t push[] = { 0xC5 }, nop[] = { 0x00 }, ld_ab[] = { 0x78 }, cb_b[] = { 0xCB, 0x40 };
        CHECK_MSG(isa->mem_operand(ld_hl_a, &o) && o.access == DP_MA_WRITE && o.mode == DP_AM_PAIR && o.pair == 2, "ld (hl),a");
        CHECK_MSG(isa->mem_operand(ldh, &o) && o.mode == DP_AM_HIGH_IMM && o.addr == 0xFF40 && o.access == DP_MA_WRITE, "ldh (n),a");
        CHECK_MSG(isa->mem_operand(abs_r, &o) && o.mode == DP_AM_ABS && o.addr == 0xC234 && o.access == DP_MA_READ, "ld a,(nn)");
        CHECK_MSG(isa->mem_operand(hli, &o) && o.post == 1 && o.access == DP_MA_READ, "ld a,(hl+)");
        CHECK_MSG(isa->mem_operand(bit_hl, &o) && o.access == DP_MA_READ, "bit (hl) reads");
        CHECK_MSG(isa->mem_operand(set_hl, &o) && o.access == DP_MA_RW, "set (hl) is rmw");
        CHECK_MSG(isa->mem_operand(push, &o) && o.mode == DP_AM_STACK && o.access == DP_MA_WRITE, "push");
        CHECK_MSG(!isa->mem_operand(nop, &o) && !isa->mem_operand(ld_ab, &o) && !isa->mem_operand(cb_b, &o), "register-only forms");
        // every opcode agrees with the disassembler: an operand mentioning (hl)/(bc)/(de)/(nn)/($ff..)
        // exists exactly when mem_operand says so (cb prefix and stack forms aside)
        int bad = 0;
        for (int op = 0; op < 256; op++) {
            if (isa->illegal((uint8_t)op) || op == 0xCB || op == 0x76) continue; // 0x76 is HALT (the disassembler prints ld (hl),(hl))
            uint8_t rom[4] = { (uint8_t)op, 0x12, 0x34, 0 };
            char txt[64];
            if (!isa->disasm(rom, sizeof rom, 0, txt, sizeof txt)) continue;
            int mem = strchr(txt, '(') != NULL;
            int stk = op == 0xC5 || op == 0xD5 || op == 0xE5 || op == 0xF5 || op == 0xC1 || op == 0xD1 ||
                      op == 0xE1 || op == 0xF1 || op == 0xC9 || op == 0xD9 || (op & 7) == 7 ||
                      ((op & 0xC7) == 0xC4) || op == 0xCD || ((op & 0xE7) == 0xC0) || ((op & 0xE7) == 0xC8 && 0);
            int got = isa->mem_operand(rom, &(DpMemOp){ 0 });
            if (!stk && mem != got) { bad++; fprintf(stderr, "mem_operand %02x '%s' mem=%d got=%d\n", op, txt, mem, got); }
        }
        CHECK_MSG(bad == 0, "mem_operand agrees with the disassembler on all opcodes");
    }
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
        // ctx_components / probes / observe
        const DpCtxComp *cc; const DpProbe *pr;
        int ncc = m->ctx_components(m, &cc), npr = m->probes(m, &pr);
        CHECK_MSG(ncc >= 14 && cc[0].kind == DP_CC_REG && cc[ncc - 1].kind == DP_CC_PAGES, "ctx components");
        CHECK_MSG(npr >= 1 && !strcmp(pr[0].name, "buttons") && pr[0].values == 256, "probe catalog");
        pr[0].apply(m, 0x01);
        DpObservers ob = { obs_step, obs_read, obs_write };
        obs_mach = raw;
        m->observe(m, &ob, NULL);
        m->run(m);
        CHECK_MSG(obs_steps > 0 && obs_steps <= 3000, "observe sees executed instructions");
        CHECK_MSG(obs_reads > 0, "observe sees data accesses");
        m->close(m);
        dp_cart_free(c);
    }
    // machine: the sega backend behind the same generic view (spec §7.2)
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/fix_z80_branch.sms", argv[1]);
        FILE *f = fopen(path, "rb");
        CHECK_MSG(f, "sms fixture opens");
        static uint8_t rom[0x8000];
        size_t rn = fread(rom, 1, sizeof rom, f);
        fclose(f);
        const DpSegaBackend *b = dp_sega_backend_gg();   // the GG backend carries the SMS profile
        CHECK_MSG(b && b->init(rom, rn, NULL, 1) == 1, "sms backend inits");
        DpMachine *m = dp_machine_sega(b);
        CHECK_MSG(m && !strcmp(m->name, "sega") && m->isa == dp_isa_z80(), "sega machine wired");
        uint32_t n = m->state_bytes(m);
        CHECK_MSG(n > 0, "state size");
        uint8_t *a = malloc(n), *bb = malloc(n);
        m->save(m, a);
        m->load(m, a);
        m->save(m, bb);
        m->load(m, bb);
        m->save(m, a);
        CHECK_MSG(!memcmp(a, bb, n), "sega save/load/save reaches a fixed point");
        CHECK_MSG(m->fetch(m, 0x0000) == rom[0x0000], "fetch sees the ROM");
        CHECK_MSG(m->rom_index(m, 0x0000) == 0, "rom_index maps the ROM");
        m->load(m, bb);
        unsigned ran = 0;
        while (ran < 64 && m->run(m)) ran++;
        CHECK_MSG(ran > 0, "run executes instructions");
        free(a); free(bb);
        m->close(m);
    }
    // machine: the NES runtime behind the generic view (spec §7.2)
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/fix_nes_ctx.nes", argv[1]);
        FILE *f = fopen(path, "rb");
        CHECK_MSG(f, "nes fixture opens");
        static uint8_t rom[1 << 17];
        size_t rn = fread(rom, 1, sizeof rom, f);
        fclose(f);
        char nerr[256];
        DpMachine *m = dp_machine_nes(rom, rn, -1, nerr, sizeof nerr);
        CHECK_MSG(m && !strcmp(m->name, "nes"), "nes machine wired");
        uint32_t n = m->state_bytes(m);
        uint8_t *a = malloc(n), *bb = malloc(n);
        m->save(m, a); m->load(m, a); m->save(m, bb); m->load(m, bb); m->save(m, a);
        CHECK_MSG(!memcmp(a, bb, n), "nes save/load/save reaches a fixed point");
        m->load(m, bb);
        unsigned ran = 0;
        while (ran < 64 && m->run(m)) ran++;
        CHECK_MSG(ran > 0, "nes run executes instructions");
        free(a); free(bb);
        m->close(m);   // owns the DpNesMach
        // machine: the Mega Drive runtime behind the generic view
        snprintf(path, sizeof path, "%s/fix_md_pad.md", argv[1]);
        f = fopen(path, "rb");
        CHECK_MSG(f, "md fixture opens");
        fseek(f, 0, SEEK_END); long mn = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *mrom = malloc((size_t)mn);
        if (!mrom || fread(mrom, 1, (size_t)mn, f) != (size_t)mn) mn = 0;
        fclose(f);
        CHECK_MSG(mn, "md fixture reads");
        DpMdMach *md = dp_mdmach_open(mrom, (size_t)mn);
        CHECK_MSG(md, "md machine opens");
        m = dp_machine_md(md);
        CHECK_MSG(m && !strcmp(m->name, "md"), "md machine wired");
        n = m->state_bytes(m);
        a = malloc(n); bb = malloc(n);
        m->save(m, a); m->load(m, a); m->save(m, bb); m->load(m, bb); m->save(m, a);
        CHECK_MSG(!memcmp(a, bb, n), "md save/load/save reaches a fixed point");
        CHECK_MSG(m->fetch(m, 0x0200) == mrom[0x0200], "md fetch sees the ROM");
        CHECK_MSG(m->rom_index(m, 0x0200) == 0x0200, "md rom_index");
        m->load(m, bb);
        ran = 0;
        while (ran < 64 && m->run(m)) ran++;
        CHECK_MSG(ran > 0, "md run executes instructions");
        free(a); free(bb); free(mrom);
        m->close(m);   // owns the DpMdMach
    }
    TEST_DONE("dpmachine");
}
