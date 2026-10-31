// test_exec.c: the retained SM83 decoder — lengths, conditions, disassembly.
// Execution itself is m0boy's interpreter (flag exactness lives in m0boy's
// own suite and the run_hash rules); what memscan keeps is the static decode.
#include "../src/cpu/sm83/sm83.h"
#include "test_util.h"
#include <string.h>

int main(void) {
    // lengths: representative opcodes of every size class
    CHECK(sm83_len(0x00) == 1); // nop
    CHECK(sm83_len(0x76) == 1); // halt
    CHECK(sm83_len(0x04) == 1); // inc b
    CHECK(sm83_len(0xCB) == 2); // CB prefix
    CHECK(sm83_len(0x3E) == 2); // ld a,n
    CHECK(sm83_len(0x20) == 2); // jr nz
    CHECK(sm83_len(0xE0) == 2); // ldh (n),a
    CHECK(sm83_len(0xC3) == 3); // jp nn
    CHECK(sm83_len(0xCD) == 3); // call nn
    CHECK(sm83_len(0xEA) == 3); // ld (nn),a
    // conditions: JP/JR/CALL cc decode to the flag they test
    CHECK(sm83_cond_of(0x20) == 0); // jr nz -> Z
    CHECK(sm83_cond_of(0x28) == 1); // jr z  -> Z
    CHECK(sm83_cond_of(0x30) == 2); // jr nc -> C
    CHECK(sm83_cond_of(0x38) == 3); // jr c  -> C
    CHECK(sm83_cond_of(0xC2) == 0); // jp nz
    CHECK(sm83_cond_of(0xDA) == 3); // jp c
    CHECK(sm83_cond_of(0xC4) == 0); // call nz
    CHECK(sm83_cond_of(0xDC) == 3); // call c
    CHECK(sm83_cond_of(0xC0) == 0); // ret nz
    CHECK(sm83_cond_of(0xC3) < 0); // jp: unconditional
    CHECK(sm83_cond_of(0x18) < 0); // jr: unconditional
    CHECK(sm83_cond_of(0x3E) < 0); // ld: not a branch
    CHECK(sm83_cond_flag(0) == 0x80); // NZ/Z test the Z flag
    CHECK(sm83_cond_flag(1) == 0x80);
    CHECK(sm83_cond_flag(2) == 0x10); // NC/C test the C flag
    CHECK(sm83_cond_flag(3) == 0x10);
    // disassembly: stable text for the map view and the report
    {
        char buf[64];
        const uint8_t rom[] = {0x3E, 0x12, 0xC3, 0x50, 0x01, 0xFF};
        CHECK(sm83_disasm(rom, sizeof rom, 0, buf, sizeof buf) == 2);
        CHECK(strstr(buf, "a"));
        CHECK(sm83_disasm(rom, sizeof rom, 2, buf, sizeof buf) == 3);
        CHECK(strstr(buf, "0150"));
        CHECK(sm83_disasm(rom, sizeof rom, 5, buf, sizeof buf) == 1);
        CHECK(strstr(buf, "0x38") || strstr(buf, "$38") || strstr(buf, "38"));
        // out-of-bounds operands read 0xFF rather than crashing
        CHECK(sm83_disasm(rom, 1, 2, buf, sizeof buf) == 1); // out-of-range reads 0xFF: rst
    }
    TEST_DONE("test_exec");
}
