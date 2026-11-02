// test_nes_cpu.c: 6502 semantics the explorer depends on — decode geometry,
// branch tests and forcing, executor behaviour against a flat bus.
#include "../src/cpu/m6502/m6502.h"
#include <stdio.h>
#include <string.h>

static uint8_t ram[0x10000];
static uint8_t rb(void *u, uint16_t a) { (void)u; return ram[a]; }
static void wb(void *u, uint16_t a, uint8_t v) { (void)u; ram[a] = v; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static m6502_t *cpu_new(void) {
    memset(ram, 0xEA, sizeof(ram)); // NOPs everywhere
    static m6502_t c;
    memset(&c, 0, sizeof(c));
    c.user = NULL; c.read = rb; c.write = wb;
    ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x80;
    m6502_reset(&c);
    c.pc = 0x8000;
    return &c;
}

static void test_decode(void) {
    const uint8_t lda_abs[] = {0xAD, 0x34, 0x12};
    DpM6502Instruction in;
    CHECK(dp_m6502_decode(lda_abs, 3, 0x8000, &in) == 1, "decode failed");
    CHECK(in.length == 3 && in.addr_mode == DP_M6502_ABS && in.access == 0, "LDA abs geometry");
    CHECK(in.flow == DP_M6502_NEXT, "LDA abs flow");

    const uint8_t bne[] = {0xD0, 0x05};
    dp_m6502_decode(bne, 2, 0x8000, &in);
    CHECK(in.flow == DP_M6502_BRANCH && in.condition == DP_M6502_CC_Z, "BNE condition");
    CHECK(in.target == 0x8007, "BNE target 0x%04X", in.target);

    const uint8_t jsr[] = {0x20, 0x00, 0x90};
    dp_m6502_decode(jsr, 3, 0x8000, &in);
    CHECK(in.flow == DP_M6502_CALL && in.target == 0x9000, "JSR geometry");

    const uint8_t rts[] = {0x60};
    dp_m6502_decode(rts, 1, 0x8000, &in);
    CHECK(in.flow == DP_M6502_RETURN, "RTS flow");

    const uint8_t jmp_ind[] = {0x6C, 0x00, 0x80};
    dp_m6502_decode(jmp_ind, 3, 0x8000, &in);
    CHECK(in.flow == DP_M6502_INDIRECT, "JMP indirect flagged");

    const uint8_t jam[] = {0x02};
    dp_m6502_decode(jam, 1, 0x8000, &in);
    CHECK(in.flow == DP_M6502_JAM, "undocumented 0x02 is a jam");

    // truncation refuses
    CHECK(dp_m6502_decode(lda_abs, 2, 0x8000, &in) == 0, "truncated 3-byte op refused");

    // BPL backward across zero
    const uint8_t bpl_back[] = {0x10, 0x80}; // -128
    dp_m6502_decode(bpl_back, 2, 0x8000, &in);
    CHECK(in.target == 0x7F82, "backward wrap target 0x%04X", in.target);
}

static void test_condition_force(void) {
    m6502_t *c = cpu_new();
    c->p = 0x00; // Z clear
    CHECK(dp_m6502_condition(DP_M6502_CC_Z, c) == 1, "Z clear -> BEQ not taken");
    CHECK(dp_m6502_condition(DP_M6502_CS_Z, c) == 0, "Z clear -> BNE taken");
    // forcing BNE not taken means clearing Z again (it already is): force the
    // taken side instead and watch only the tested flag move
    int prev = dp_m6502_force(DP_M6502_CS_Z, c, 1); // make BEQ taken: set Z
    CHECK(prev == 0 && (c->p & 0x02), "force sets only the tested flag");
    CHECK((c->p & 0x80) == 0 && (c->p & 0x40) == 0 && (c->p & 0x01) == 0, "other flags untouched");
    CHECK(dp_m6502_condition(DP_M6502_CC_Z, c) == 0, "forced Z set -> BEQ taken");
}

static void test_execute(void) {
    m6502_t *c = cpu_new();
    // LDA #$7F; ADC #$01; carry+overflow set, A=0x80
    ram[0x8000] = 0xA9; ram[0x8001] = 0x7F;
    ram[0x8002] = 0x69; ram[0x8003] = 0x01;
    c->pc = 0x8000;
    m6502_step(c);
    CHECK(c->a == 0x7F && !(c->p & 0x02), "LDA immediate");
    m6502_step(c);
    CHECK(c->a == 0x80, "ADC sums to 0x%02X", c->a);
    CHECK(!(c->p & 0x01), "ADC 0x7F+0x01 carries nothing");
    CHECK(c->p & 0x40, "ADC overflow set (127 + 1)");
    // borrow case: SBC with carry clear borrows
    ram[0x8004] = 0xE9; ram[0x8005] = 0x01;
    c->p &= (uint8_t)~0x01; // no carry in
    m6502_step(c);
    CHECK(c->a == 0x7E && (c->p & 0x01), "SBC with borrow");

    // branch taken and not taken (BNE: Z clear after the SBC result 0x7E)
    ram[0x8010] = 0xD0; ram[0x8011] = 0x10;
    c->pc = 0x8010;
    m6502_step(c);
    CHECK(c->pc == 0x8022, "BNE taken to 0x%04X", c->pc);
    c->p |= 0x02; // Z set: BNE falls through
    c->pc = 0x8010;
    m6502_step(c);
    CHECK(c->pc == 0x8012, "BNE not taken");

    // zero page store + zp,X addressing
    ram[0x0010] = 0x20;
    ram[0x8010] = 0xA2; ram[0x8011] = 0x10; // LDX #16
    ram[0x8012] = 0xA9; ram[0x8013] = 0x77; // LDA #0x77
    ram[0x8014] = 0x95; ram[0x8015] = 0x00; // STA 0,X -> $10
    c->pc = 0x8010;
    m6502_step(c); m6502_step(c); m6502_step(c);
    CHECK(ram[0x10] == 0x77, "STA zp,X lands at $%02X", ram[0x10]);

    // JSR/RTS round trip
    ram[0x8020] = 0x20; ram[0x8021] = 0x30; ram[0x8022] = 0x80;
    ram[0x8030] = 0xEA; ram[0x8031] = 0x60;
    c->s = 0xFD;
    c->pc = 0x8020;
    m6502_step(c);
    CHECK(c->pc == 0x8030, "JSR lands at target");
    m6502_step(c); // NOP
    m6502_step(c);
    CHECK(c->pc == 0x8023, "RTS returns after operand 0x%04X", c->pc);

    // RMW: INC absolute
    ram[0x1234] = 0x0F;
    ram[0x8040] = 0xEE; ram[0x8041] = 0x34; ram[0x8042] = 0x12;
    c->pc = 0x8040;
    m6502_step(c);
    CHECK(ram[0x1234] == 0x10, "INC absolute");

    // (ind),Y addressing through zero page
    ram[0x0040] = 0x00; ram[0x0041] = 0x90; // pointer $9000
    ram[0x9005] = 0x42;
    ram[0x8050] = 0xA0; ram[0x8051] = 0x05; // LDY #5
    ram[0x8052] = 0xB1; ram[0x8053] = 0x40; // LDA (40),Y
    c->pc = 0x8050;
    m6502_step(c); m6502_step(c);
    CHECK(c->a == 0x42, "LDA (zp),Y reads through pointer");

    // JMP indirect page-boundary alias: pointer at $90FF reads high from $9000
    ram[0x90FF] = 0x50; ram[0x9000] = 0x90;
    ram[0x8060] = 0x6C; ram[0x8061] = 0xFF; ram[0x8062] = 0x90;
    c->pc = 0x8060;
    m6502_step(c);
    CHECK(c->pc == (uint16_t)(0x50 | (0x90 << 8)), "JMP (ind) alias honoured: 0x%04X", c->pc);

    // jam detection
    ram[0x8070] = 0x02;
    c->pc = 0x8070;
    m6502_step(c);
    CHECK(c->jammed, "undocumented opcode jams");
}

static void test_interrupts(void) {
    m6502_t *c = cpu_new();
    ram[0xFFFA] = 0x00; ram[0xFFFB] = 0x70; // NMI vector $7000
    ram[0xFFFE] = 0x00; ram[0xFFFF] = 0x71;
    c->pc = 0x8000;
    c->s = 0xFD;
    // no line: poll only
    CHECK(m6502_service(c) == 2, "idle poll costs 2");
    // NMI is edge-latched: level asserted once dispatches once
    c->nmi = 1;
    unsigned cyc0 = c->cyc;
    CHECK(m6502_service(c) == 7, "NMI dispatch costs 7");
    CHECK(c->pc == 0x7000, "NMI vector taken");
    CHECK(c->p & 0x04, "I set by dispatch");
    CHECK(c->cyc - cyc0 == 7, "cycles accounted");
    m6502_service(c);
    c->nmi = 0;
    // the edge that fired is consumed; a fresh edge needed after deassert
    c->pc = 0x8000;
    CHECK(m6502_service(c) == 2, "consumed NMI does not refire");
    c->nmi = 1;
    CHECK(m6502_service(c) == 7, "fresh edge fires");

    // IRQ is level, masked by I (the NMI line is dropped first)
    c->nmi = 0;
    m6502_reset(c);
    c->pc = 0x8000; c->s = 0xFD;
    c->irq = 1;
    CHECK(m6502_service(c) == 2, "IRQ masked while I set");
    c->p &= (uint8_t)~0x04;
    CHECK(m6502_service(c) == 7, "IRQ dispatches when I clear");
    CHECK(c->pc == 0x7100, "IRQ vector taken");
    // dispatch set I again
    CHECK(m6502_service(c) == 2, "I set again by dispatch");
}

static void test_cycles(void) {
    m6502_t *c = cpu_new();
    ram[0x8000] = 0xE8; // INX, 2 cycles
    c->pc = 0x8000;
    unsigned c0 = c->cyc;
    m6502_step(c);
    CHECK(c->cyc - c0 == 2, "INX costs 2, got %u", c->cyc - c0);
    // page-crossing ABS,Y read: LDA $80FF,Y with Y=1 crosses (3+1)
    ram[0x8010] = 0xB9; ram[0x8011] = 0xFF; ram[0x8012] = 0x80;
    c->y = 1; c->pc = 0x8010;
    c0 = c->cyc;
    m6502_step(c);
    CHECK(c->cyc - c0 == 5, "LDA abs,Y page cross costs 5, got %u", c->cyc - c0);
    // non-crossing: 4
    c->y = 0; c->pc = 0x8010;
    c0 = c->cyc;
    m6502_step(c);
    CHECK(c->cyc - c0 == 4, "LDA abs,Y same page costs 4, got %u", c->cyc - c0);
}

int main(void) {
    test_decode();
    test_condition_force();
    test_execute();
    test_interrupts();
    test_cycles();
    if (fails) printf("%d failures\n", fails);
    else printf("test_nes_cpu: all green\n");
    return fails != 0;
}
