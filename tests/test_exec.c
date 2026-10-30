// test_exec.c: SM83 flag exactness on hand-computed vectors, plus flow ops.
#include "../src/core/cart.h"
#include "../src/core/machine.h"
#include "../src/cpu/sm83/sm83.h"
#include "test_util.h"

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

static MsCart *g_cart;

static MsMachine mk(const uint8_t *code, int n) {
    if (!g_cart) {
        uint8_t *img = calloc(1, 0x8000);
        img[0x147] = 0x00;
        g_cart = ms_cart_from_memory(img, 0x8000, NULL, 0);
    }
    memcpy(g_cart->rom + 0x150, code, n);
    MsMachine m;
    ms_machine_init(&m, g_cart, ms_platform_gb());
    m.pc = 0x150;
    m.sp = 0xFFFE;
    m.f = 0x00; // unit tests control flags explicitly (boot leaves Z+H+C set)
    return m;
}

static void done(void) {
    ms_cart_free(g_cart);
}

int main(void) {
    { // ADD: half-carry, carry, zero
        uint8_t code[] = { 0x3E, 0x7F, 0xC6, 0x01 }; // ld a,$7F; add a,$01
        MsMachine m = mk(code, sizeof code);
        CHECK(sm83_step(&m) == 0);
        CHECK(sm83_step(&m) == 0);
        CHECK(m.a == 0x80 && m.f == FH);
    }
    { // ADD overflow: carry + zero
        uint8_t code[] = { 0x3E, 0xFF, 0xC6, 0x01 };
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0x00 && m.f == (FZ | FH | FC)); // half-carry from $F+$1
    }
    { // ADC chains the carry in
        uint8_t code[] = { 0x3E, 0x01, 0xC6, 0xFF, 0xCE, 0x00 }; // a=1; add FF (=0, C1); adc 0
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0x00 && m.f == (FZ | FH | FC)); // 1+FF: half-carry too
        sm83_step(&m);
        CHECK(m.a == 0x01 && m.f == 0); // 0+0+carry = 1
    }
    { // SUB half-borrow and borrow
        uint8_t code[] = { 0x3E, 0x03, 0xD6, 0x05 }; // ld a,3; sub 5
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0xFE && m.f == (FN | FH | FC));
    }
    { // CP never writes A
        uint8_t code[] = { 0x3E, 0x42, 0xFE, 0x42 }; // ld a,$42; cp $42
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0x42 && m.f == (FZ | FN)); // cp sets N on equality
    }
    { // DAA: BCD 45+38 = 83
        uint8_t code[] = { 0x3E, 0x45, 0xC6, 0x38, 0x27 }; // ld a,$45; add $38; daa
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0x83);
    }
    { // INC half-carry, DEC half-borrow
        uint8_t code[] = { 0x3E, 0x0F, 0x3C, 0x3D }; // ld a,$0F; inc a; dec a
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0x10 && m.f == FH);
        sm83_step(&m);
        CHECK(m.a == 0x0F && m.f == (FN | FH));
    }
    { // CB SWAP + SRA of $81
        uint8_t code[] = { 0x3E, 0x81, 0xCB, 0x37, 0x3E, 0x81, 0xCB, 0x2F }; // swap a / sra a
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0x18 && m.f == 0); // swap
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0xC0 && (m.f & FC)); // sra keeps the sign
    }
    { // BIT sets Z when the bit is clear, H always
        uint8_t code[] = { 0x3E, 0x0F, 0xCB, 0x7F }; // ld a,$0F; bit 7,a
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.f == (FZ | FH));
    }
    { // ADD SP,e flag rules
        uint8_t code[] = { 0xE8, 0x01 }; // add sp,1 with sp=$000F
        MsMachine m = mk(code, sizeof code);
        m.sp = 0x000F;
        sm83_step(&m);
        CHECK(m.sp == 0x0010 && m.f == FH);
        MsMachine m2 = mk(code, sizeof code);
        m2.sp = 0x00FF;
        sm83_step(&m2);
        CHECK(m2.sp == 0x0100 && m2.f == (FH | FC));
    }
    { // ADD HL,rr carries above bit 11/15
        uint8_t code[] = { 0x21, 0xFF, 0x0F, 0x01, 0x01, 0xF0, 0x09 }; // hl=$0FFF; bc=$F001; add hl,bc
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        sm83_step(&m);
        uint16_t hl = (uint16_t)((m.h << 8) | m.l);
        CHECK(hl == 0x0000 && m.f == (FZ * 0 | FC | FH));
    }
    { // JR taken/not, conditional fork parity: jr z with Z set jumps
        uint8_t code[] = { 0x3E, 0x01, 0xB7, 0x28, 0x02, 0x06, 0x09, 0x06, 0x42, 0x76 };
        // ld a,1; or a (Z0); jr z,+2; ld b,9; ld b,$42; halt
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m); // a=1
        sm83_step(&m); // or a: Z=0
        sm83_step(&m); // jr z not taken
        CHECK(m.pc == 0x155);
        sm83_step(&m); // ld b,9
        CHECK(m.b == 0x09);
    }
    { // CALL/RET round trip through the concrete stack
        // call $0160; halt; pad; sub at $0160: ld a,$99; ret
        uint8_t code[] = { 0xCD, 0x60, 0x01, 0x76,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x3E, 0x99, 0xC9 }; // sub at $0160
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m); // call
        CHECK(m.pc == 0x160 && m.sp == 0xFFFC);
        sm83_step(&m); // ld a,$99
        sm83_step(&m); // ret
        CHECK(m.pc == 0x153 && m.sp == 0xFFFE && m.a == 0x99);
    }
    { // PUSH/POP round trip incl. AF low-nibble masking
        uint8_t code[] = { 0xF5, 0xF1, 0x76 }; // push af; pop af
        MsMachine m = mk(code, sizeof code);
        m.a = 0xAB;
        m.f = 0xFF;
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0xAB && m.f == 0xF0); // pop af reads only the upper nibble of F
    }
    { // HALT and STOP end the step with return 1
        uint8_t code[] = { 0x76 };
        MsMachine m = mk(code, sizeof code);
        CHECK(sm83_step(&m) == 1 && m.halted);
        uint8_t code2[] = { 0x10, 0x00 };
        MsMachine m2 = mk(code2, sizeof code2);
        CHECK(sm83_step(&m2) == 1 && m2.halted);
    }
    { // undefined opcodes are reported
        uint8_t code[] = { 0xD3 };
        MsMachine m = mk(code, sizeof code);
        CHECK(sm83_step(&m) == 2);
    }
    { // LD (HL+),A increments HL across the carry
        uint8_t code[] = { 0x21, 0xFF, 0xFF, 0x3E, 0x55, 0x22 }; // ld hl,$FFFF; ld a,$55; ld (hl+),a
        MsMachine m = mk(code, sizeof code);
        sm83_step(&m);
        sm83_step(&m);
        sm83_step(&m);
        CHECK(((m.h << 8) | m.l) == 0x0000);
        CHECK(ms_read(&m, 0xFFFF, MS_ACC_DATA) == 0x55);
    }
    { // LDI/LDD read through the ROM window
        uint8_t code[] = { 0x21, 0x00, 0x03, 0x2A, 0x3A }; // hl=$0300; ld a,(hl+); ld a,(hl-)
        MsMachine m = mk(code, sizeof code);
        m.cart->rom[0x300] = 0x11;
        m.cart->rom[0x301] = 0x22;
        sm83_step(&m);
        sm83_step(&m);
        CHECK(m.a == 0x11);
        sm83_step(&m);
        CHECK(m.a == 0x22);
    }
    done();
    TEST_DONE("test_exec");
}
