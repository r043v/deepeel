// test_ngp.c: the TLCS-900H decoder, the DpIsa contract and the Neo Geo
// Pocket machine behind the generic view (spec §7.2), on the generated
// fixture.  The interpreter's byte order holds everywhere: the (mem) operand
// bytes precede the op byte.
#include <stdlib.h>
#include <string.h>
#include "../src/core/dpmachine.h"
#include "../src/cpu/tlcs900/tlcs900.h"
#include "../src/platform/ngp/ngpmach.h"
#include "test_util.h"

int main(int argc, char **argv) {
    // --- decoder pins -------------------------------------------------------
    DpTlcsInsn r;
    { const uint8_t b[] = {0x00};
      CHECK_MSG(dp_tlcs900_decode(b, 1, 0x200000, &r) == 1 && r.cls == DP_TLCS_OTHER && !strcmp(r.text, "nop"), "nop"); }
    { const uint8_t b[] = {0x05};
      CHECK_MSG(dp_tlcs900_decode(b, 1, 0x200000, &r) == 1 && r.cls == DP_TLCS_HALT, "halt"); }
    { const uint8_t b[] = {0x1B, 0x00, 0x01, 0x20};
      CHECK_MSG(dp_tlcs900_decode(b, 4, 0x200000, &r) == 4 && r.cls == DP_TLCS_JP &&
                r.has_target && r.target == 0x200100, "jp $200100 (LE24)"); }
    { const uint8_t b[] = {0x66, 0x05};
      CHECK_MSG(dp_tlcs900_decode(b, 2, 0x200036, &r) == 2 && r.cls == DP_TLCS_JCC &&
                r.has_target && r.target == 0x20003D && r.cond == 6, "jr eq,+5 (target past the operand)"); }
    { const uint8_t b[] = {0xF1, 0x00, 0x40, 0x41};
      CHECK_MSG(dp_tlcs900_decode(b, 4, 0x200032, &r) == 4 && r.cls == DP_TLCS_OTHER &&
                r.mem_is_abs && r.mem_addr == 0x4000 && r.op_byte == 0x41 &&
                !strcmp(r.text, "ld ($4000),a"), "ld ($4000),a (d16 mode, op byte last)"); }
    { const uint8_t b[] = {0xC9, 0x1C, 0xF8};
      CHECK_MSG(dp_tlcs900_decode(b, 3, 0x200030, &r) == 3 && r.cls == DP_TLCS_DJNZ &&
                r.has_target && r.target == 0x20002B, "djnz (nibble form), target past the operand"); }
    { const uint8_t b[] = {0xB3, 0xD6};
      CHECK_MSG(dp_tlcs900_decode(b, 2, 0x200000, &r) == 2 && r.cls == DP_TLCS_JPMEM &&
                !r.has_target && r.pair == 3, "jp eq,(xhl) is register-based"); }
    { const uint8_t b[] = {0x0E};
      CHECK_MSG(dp_tlcs900_decode(b, 1, 0, &r) == 1 && r.cls == DP_TLCS_RET, "ret"); }
    { const uint8_t b[] = {0xC6};
      CHECK_MSG(dp_tlcs900_decode(b, 1, 0, &r) == 1 && r.cls == DP_TLCS_ILLEGAL, "illegal C6"); }
    // length bounds from the first byte (the closure's safe step)
    CHECK_MSG(dp_tlcs900_length(0x00) == 1 && dp_tlcs900_length(0x66) == 2 &&
              dp_tlcs900_length(0x1B) == 4 && dp_tlcs900_length(0x44) == 5 &&
              dp_tlcs900_length(0x45) == 5, "first-byte lengths are bounds");

    // --- DpIsa contract -------------------------------------------------------
    const DpIsa *isa = dp_isa_tlcs900();
    CHECK_MSG(isa && !strcmp(isa->name, "tlcs900h"), "isa named");
    CHECK_MSG(isa->ptr_width == 3 && !isa->big_endian, "24-bit little-endian pointers");
    CHECK_MSG(isa->table_entry_width == 4 && isa->code_min == 0x4000, "wide LE tables, RAM code floor");
    CHECK_MSG(isa->sp_slot == DP_TLCSR_SP, "stack register named");
    DpFlowInfo fi;
    { const uint8_t b[] = {0x66, 0x05};
      CHECK_MSG(isa->flow(b, 0x200036, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.fall &&
                fi.target == 0x20003D && fi.cond == 6, "jr eq forks"); }
    { const uint8_t b[] = {0x1B, 0x00, 0x01, 0x20};
      CHECK_MSG(isa->flow(b, 0x200000, &fi) == 4 && fi.kind == DP_FLOW_JUMP && !fi.fall &&
                fi.target == 0x200100, "jp is a jump"); }
    { const uint8_t b[] = {0x0E};
      CHECK_MSG(isa->flow(b, 0, &fi) == 1 && fi.kind == DP_FLOW_RET && !fi.fall, "ret returns"); }
    { const uint8_t b[] = {0xB2, 0xFB};
      CHECK_MSG(isa->flow(b, 0, &fi) == 2 && fi.kind == DP_FLOW_COND && fi.fall && fi.target < 0, "ret cc falls, no target"); }
    { const uint8_t b[] = {0xC9, 0x1C, 0xF8};
      CHECK_MSG(isa->flow(b, 0x200030, &fi) == 3 && fi.kind == DP_FLOW_COND &&
                fi.cond == TLCS_DJNZ_COND, "djnz carries its own condition"); }
    { const uint8_t b[] = {0xF1, 0x00, 0x40, 0xD1};
      CHECK_MSG(isa->flow(b, 0x200000, &fi) == 4 && fi.kind == DP_FLOW_COND &&
                fi.target == 0x4000, "jp eq,(abs) is a conditional with the abs target"); }
    { const uint8_t b[] = {0xB3, 0xD6};
      uint32_t param = 99;
      CHECK_MSG(isa->flow(b, 0, &fi) == 2 && fi.kind == DP_FLOW_INDIRECT && !fi.fall, "jp cc,(reg) is indirect");
      CHECK_MSG(isa->table_dispatch(b, &param) == 1 && param == 3, "jp cc,(xhl) names its pair"); }
    { const uint8_t b[] = {0x31, 0x34, 0x12};
      uint32_t imm = 0;
      CHECK_MSG(isa->imm_ptr && isa->imm_ptr(b, &imm) && imm == 0x1234, "ld bc,$1234 loads a pointer"); }
    { const uint8_t b[] = {0x44, 0x00, 0x30, 0x20, 0x00};
      uint32_t imm = 0;
      CHECK_MSG(isa->imm_ptr(b, &imm) && imm == 0x203000, "ld xix,$203000 loads a wide pointer"); }
    { unsigned w = 0, bias = 0, off = 0;
      const uint8_t ret[] = {0x0E};
      CHECK_MSG(isa->sp_ret(ret, &w, &bias, &off) && w == 4 && off == 0, "ret pops 4 bytes"); }
    { const uint8_t ldir[] = {0x84, 0x11}, cpir[] = {0xC1, 0x00, 0x30, 0x15}, ldi[] = {0x84, 0x10};
      CHECK_MSG(isa->flow(ldir, 0x200100, &fi) == 2 && fi.kind == DP_FLOW_COND &&
                fi.target == 0x200100 && fi.cond == 4, "ldir repeats on V (self-loop conditional)");
      CHECK_MSG(isa->flow(cpir, 0x200200, &fi) == 4 && fi.kind == DP_FLOW_COND &&
                fi.target == 0x200200 && fi.cond == 4, "cpir repeats on V");
      CHECK_MSG(isa->flow(ldi, 0x200100, &fi) == 2 && fi.kind == DP_FLOW_SEQ, "ldi falls through"); }
    char txt[96];
    { const uint8_t b[] = {0x1B, 0x00, 0x01, 0x20};
      CHECK_MSG(isa->disasm(b, sizeof b, 0, txt, sizeof txt) == 4 && strstr(txt, "200100"), "disasm jp (the absolute target)"); }

    // --- the machine behind the generic view ---------------------------------
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/fix_ngp_branch.ngp", argv[1]);
        FILE *f = fopen(path, "rb");
        CHECK_MSG(f, "ngp fixture opens");
        static uint8_t rom[0x1000];
        size_t rn = fread(rom, 1, sizeof rom, f);
        fclose(f);
        DpMachine *m = dp_machine_ngp();
        CHECK_MSG(m && !strcmp(m->name, "ngp") && m->isa == dp_isa_tlcs900(), "ngp machine wired");
        CHECK_MSG(m->plat && !strcmp(m->plat->name, "ngp") && m->plat->find, "ngp registry wired (wide find)");
        dp_machine_ngp_source(m, rom, rn);
        CHECK_MSG(m->attach && m->attach(m), "ngp machine attaches (BIOS installed, cart reset)");
        CHECK_MSG(m->pc(m) == 0x200030, "reset pc is the header's entry");
        { const DpCtxComp *xc; int xn = m->ctx_components ? m->ctx_components(m, &xc) : 0;
          CHECK_MSG(xn >= 6 && xc[0].kind == DP_CC_REG && xc[xn - 1].kind == DP_CC_PAGES, "ctx components declared"); }
        CHECK_MSG(m->fetch(m, 0x200030) == rom[0x30], "fetch sees the cartridge");
        CHECK_MSG(m->rom_index(m, 0x200030) == 0x30 && m->rom_index(m, 0x4000) < 0, "rom_index is the image offset");
        CHECK_MSG(m->ram_index(m, 0x4000) == 0 && m->ram_index(m, 0x7FFF) == 0x3FFF && m->ram_index(m, 0x8000) < 0, "ram_index");
        CHECK_MSG(m->cut_pc(m, 0x200030) == 0 && m->cut_pc(m, 0x4000) == 0 && m->cut_pc(m, 0x0000) != 0, "cut_pc keeps code windows");
        // save/load reaches a fixed point (the second save, as on the other runtimes)
        uint32_t n = m->state_bytes(m);
        uint8_t *a = malloc(n), *b2 = malloc(n);
        m->save(m, a); m->load(m, a); m->save(m, b2); m->load(m, b2); m->save(m, a);
        CHECK_MSG(!memcmp(a, b2, n), "ngp save/load/save reaches a fixed point");
        // two natural steps land on the jr; the store is in work RAM
        unsigned ran = 0;
        while (ran < 2 && m->run(m)) ran++;
        CHECK_MSG(ran == 2 && m->pc(m) == 0x200036, "two steps to the jr");
        CHECK_MSG(m->ram_view(m)[0] == 0x7F, "the store landed in work RAM");
        m->save(m, a);
        CHECK_MSG(m->run(m) && m->pc(m) == 0x200038, "the jr falls through (Z clear at reset)");
        // the fork: EQ is forced true by the inversion, the jr takes the jump side
        m->load(m, a);   // rewind to the jr
        CHECK_MSG(m->invert_condition(m, 6) == 1, "EQ inverts");
        CHECK_MSG(m->run(m) && m->pc(m) == 0x20003D, "the inverted jr takes the jump side");
        // F, T and the DJNZ counter cannot be inverted through SR
        CHECK_MSG(!m->invert_condition(m, 0) && !m->invert_condition(m, 8) &&
                  !m->invert_condition(m, TLCS_DJNZ_COND), "F/T/DJNZ refuse the inversion");
        m->load(m, b2);
        free(a); free(b2);
        m->close(m);
    }
    TEST_DONE("ngp");
}
