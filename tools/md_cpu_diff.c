/* md_cpu_diff: instruction-level differential test of the m0boy 68000 against the external Clown68000 interpreter.
 * Each iteration runs ONE random instruction (opcode + 2 random extension words) from random registers/flags with
 * code and data in work RAM, then compares D0-7, A0-7, SR, PC and the whole 64 KiB RAM. Iterations where the
 * instruction reaches outside RAM or raises an exception on either side are skipped (counted).  Mismatches are
 * grouped by opcode class so the first diverging instruction family is visible.  No reference code is copied.
 * usage: md_cpu_diff ITERATIONS [seed] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "clown68000.h"
#include "md_full.h"
void dp_mdrom_set(const uint8_t *, size_t);
static uint8_t cmem[65536]; static int outside;
static cc_u16f rd(const void *u, cc_u32f a, cc_bool hi, cc_bool lo, cc_u32f cy, cc_bool *t) {
    (void)u; (void)hi; (void)lo; (void)cy; (void)t; uint32_t b = (a * 2) & 0xffffff;
    if (b < 0xff0000u) { outside = 1; return 0; }
    return (cc_u16f)(cmem[b & 0xffff] << 8 | cmem[(b + 1) & 0xffff]);
}
static void wr(const void *u, cc_u32f a, cc_bool hi, cc_bool lo, cc_u32f cy, cc_bool *t, cc_u16f v) {
    (void)u; (void)cy; (void)t; uint32_t b = (a * 2) & 0xffffff;
    if (b < 0xff0000u) { outside = 1; return; }
    if (hi) cmem[b & 0xffff] = (uint8_t)(v >> 8);
    if (lo) cmem[(b + 1) & 0xffff] = (uint8_t)v;
}
static uint64_t s = 88172645463325252ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 100000; if (argc > 2) s ^= (uint64_t)atol(argv[2]) * 0x9E3779B97F4A7C15ull;
    static uint8_t rom[512]; dp_mdrom_set(rom, sizeof rom);
    MdFull *m = calloc(1, sizeof *m);
    Clown68000_ReadWriteCallbacks cb = { rd, wr, NULL, NULL };
    static uint8_t base[65536]; long skipped = 0, bad = 0, ran = 0; static long cls[65536 >> 6];
    for (long it = 0; it < n; it++) {
        for (int i = 0; i < 65536; i++) base[i] = (uint8_t)rnd();
        uint16_t op = (uint16_t)rnd(); uint32_t pc = 0xff0100u;
        base[0x100] = op >> 8; base[0x101] = (uint8_t)op;
        uint32_t d[8], a[8]; for (int i = 0; i < 8; i++) { d[i] = rnd() ^ rnd() << 16; if (rnd() & 1) d[i] &= 0xffff; if (rnd() % 5 == 0) d[i] = 0; a[i] = 0xff4000u + ((rnd() % 0x3000u) & ~1u); }
        uint16_t ccr = (uint16_t)(rnd() & 0x1f);
        md_full_reset(m); memcpy(m->ram, base, 65536); memcpy(cmem, base, 65536);
        memcpy(m->d, d, sizeof d); memcpy(m->a, a, sizeof a); m->pc = pc; m->sr = (uint16_t)(0x2000 | ccr); m->usp = 0xff6000u; m->fault = 0;
        Clown68000_State cs; memset(&cs, 0, sizeof cs);
        memcpy(cs.data_registers, d, sizeof d); memcpy(cs.address_registers, a, sizeof a);
        cs.program_counter = pc; cs.status_register = (cc_u16l)(0x2000 | ccr); cs.user_stack_pointer = 0xff6000u; cs.supervisor_stack_pointer = a[7];
        outside = 0; Clown68000_DoCycles(&cs, &cb, 1);
        md_full_step_interp(m);
        if (outside || m->fault || cs.halted || cs.stopped || (cs.program_counter < 0xff0000u) || (m->pc < 0xff0000u)) { skipped++; continue; }
        ran++;
        int diff = 0; char why[96] = "";
        for (int i = 0; i < 8 && !diff; i++) if (m->d[i] != cs.data_registers[i]) { diff = 1; snprintf(why, sizeof why, "D%d %08x/%08x", i, m->d[i], cs.data_registers[i]); }
        for (int i = 0; i < 8 && !diff; i++) if ((m->a[i] & 0xffffff) != (cs.address_registers[i] & 0xffffff)) { diff = 1; snprintf(why, sizeof why, "A%d %08x/%08x", i, m->a[i], cs.address_registers[i]); }
        if (!diff && (m->sr & 0xff1f) != (cs.status_register & 0xff1f)) { diff = 1; snprintf(why, sizeof why, "SR %04x/%04x", m->sr, cs.status_register); }
        if (!diff && m->pc != cs.program_counter) { diff = 1; snprintf(why, sizeof why, "PC %08x/%08x", m->pc, cs.program_counter); }
        if (!diff && memcmp(m->ram, cmem, 65536)) { diff = 1; snprintf(why, sizeof why, "RAM"); }
        if (diff) { bad++; cls[op >> 6]++; if (bad <= 40) printf("op %04x ext %02x%02x%02x%02x ccr %02x  %s   (D%u:%08x A%u:%08x)\n", op, base[0x102], base[0x103], base[0x104], base[0x105], ccr, why, 0u, d[0], 0u, a[0]); }
    }
    printf("ran %ld skipped %ld mismatches %ld\n", ran, skipped, bad);
    printf("worst opcode classes (op>>6):\n");
    for (int k = 0; k < 10; k++) { long bi = 0; for (long i = 0; i < 1024; i++) if (cls[i] > cls[bi]) bi = i; if (!cls[bi]) break; printf("  %03lx*  %ld\n", bi, cls[bi]); cls[bi] = 0; }
    return 0;
}
