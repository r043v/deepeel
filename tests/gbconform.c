// gbconform: run GB test ROMs headless on the m0boy core as deepeel drives it
// and print one verdict per ROM.  Protocols: Mooneye (ld b,b with the
// Fibonacci registers = pass, 0x42 = fail), Blargg (serial text), otherwise
// "none" (needs a screenshot compare, not decided here).  Observation tool,
// not a ctest.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/cart.h"
#include "../src/mach/m0boy.h"

static char ser[4096];
static int nser, verdict, frames, limit, fl_ff02;
static uint8_t sb;
static DpMach *M;
static int micro;

static long nstep, nwr;
static void step(DpMach *m, uint16_t pc, uint8_t op, int e) {
    nstep++; if (getenv("GBC_TRACE") && (nstep < 4 || (getenv("GBC_FROM") && nstep >= (unsigned long)atol(getenv("GBC_FROM")) && nstep < (unsigned long)atol(getenv("GBC_FROM")) + 40) || (pc>=0xFD00 && pc<0xFF00))) fprintf(stderr, "%04x %02x b=%02x c=%02x d=%02x e=%02x h=%02x l=%02x\n", pc, op, m->api->gbr->b, m->api->gbr->c, m->api->gbr->d, m->api->gbr->e, m->api->gbr->h, m->api->gbr->l);
    (void)pc; (void)e;
    if (getenv("GBC_OP") && op == (uint8_t)strtol(getenv("GBC_OP"), 0, 16)) fprintf(stderr, "op %02x pc=%04x a=%02x b=%02x c=%02x d=%02x e=%02x h=%02x l=%02x\n", op, pc, m->api->gbr->a, m->api->gbr->b, m->api->gbr->c, m->api->gbr->d, m->api->gbr->e, m->api->gbr->h, m->api->gbr->l);
    if (op == 0x40 || op == 0x52) {
        const gbl_cpu_t *g = m->api->gbr;
        // ld b,b (mooneye) / ld d,d (wilbertpol): only the register triple decides
        if (g->b == 3 && g->c == 5 && g->d == 8 && g->e == 13 && g->h == 21 && g->l == 34) { verdict = 1; dp_mach_cut(M, DP_CUT_QUIT); }
        else if (op == 0x40 && g->b == 0x42 && g->c == 0x42 && g->d == 0x42 && g->e == 0x42 && g->h == 0x42 && g->l == 0x42) { verdict = -1; if (getenv("GBC_REGS")) fprintf(stderr, "fail at pc=%04x\n", pc); dp_mach_cut(M, DP_CUT_QUIT); }
    }
}
static void wr(DpMach *m, uint16_t a, uint8_t v) {
    (void)m;
    nwr++;
    if (micro && a == 0xFF82 && (v == 0x01 || v == 0xFF)) { verdict = v == 1 ? 3 : -3; dp_mach_cut(M, DP_CUT_QUIT); }
    if (a == 0xFF01) sb = v;
    else if (a == 0xFF02 && (v & 0x81) == 0x81 && nser < (int)sizeof ser - 1) { ser[nser++] = (char)sb; ser[nser] = 0;
        if (strstr(ser, "Passed")) verdict = 2; else if (strstr(ser, "Failed")) verdict = -2;
        if (verdict) dp_mach_cut(M, DP_CUT_QUIT); }
}
static void rd(DpMach *m, uint16_t a, uint8_t v) { (void)m; (void)a; (void)v; }
static void vb(DpMach *m) {
    if (frames % 30 == 29 && m->api->eram && !verdict) { // Blargg cart-RAM protocol: DE B0 61 at $A001, status at $A000
        const uint8_t *r = m->api->eram;
        if (r[1] == 0xDE && r[2] == 0xB0 && r[3] == 0x61 && r[0] != 0x80) { verdict = r[0] == 0 ? 4 : -4; dp_mach_cut(M, DP_CUT_QUIT); }
    }
    if (++frames >= limit) dp_mach_cut(M, DP_CUT_QUIT); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: gbconform FRAMES rom...\n"); return 2; }
    limit = atoi(argv[1]);
    micro = getenv("GBC_MICRO") != NULL;
    for (int i = 2; i < argc; i++) {
        char err[256];
        DpCart *c = dp_cart_load(argv[i], err, sizeof err);
        if (!c) { printf("%s\tload-error\t0\n", argv[i]); continue; }
        int vi = dp_mach_pick_variant(c->rom);
        if (vi < 0) { printf("%s\tno-variant\t0\n", argv[i]); dp_cart_free(c); continue; }
        M = dp_mach_open(vi, 0);
        nser = 0; ser[0] = 0; verdict = 0; frames = 0; sb = 0; (void)fl_ff02;
        dp_mach_set_rom(M, c->rom, c->rom_size);
        dp_mach_reset(M);
        dp_mach_attach(M, NULL, step, rd, wr, vb);
        dp_mach_run(M);
        if (!verdict) { // halted / quiet end: the register triple left behind still decides
            const gbl_cpu_t *g = M->api->gbr;
            if (g->b == 3 && g->c == 5 && g->d == 8 && g->e == 13 && g->h == 21 && g->l == 34) verdict = 1;
            else if (g->b == 0x42 && g->c == 0x42 && g->d == 0x42 && g->e == 0x42 && g->h == 0x42 && g->l == 0x42) verdict = -1;
        }
        if ((verdict == 4 || verdict == -4) && getenv("GBC_END")) printf("  ram status %02x text: %.60s\n", M->api->eram[0], (const char *)M->api->eram + 4);
        const char *v = verdict == 1 ? "pass-mooneye" : verdict == 2 ? "pass-blargg" : verdict == -1 ? "fail-mooneye" :
                        verdict == -2 ? "fail-blargg" : verdict == 4 ? "pass-blargg-ram" : verdict == -4 ? "fail-blargg-ram" : verdict == 3 ? "pass-microtest" : verdict == -3 ? "fail-microtest" : "undecided";
        printf("%s\t%s\t%d\tsteps=%ld wr=%ld ser=%d\n", argv[i], v, frames, nstep, nwr, nser); nstep = nwr = 0;
        if (!verdict && getenv("GBC_END")) { const gbl_cpu_t *g = M->api->gbr; printf("  end pc=%04x a=%02x bc=%04x de=%04x hl=%04x\n", g->pc, g->a, g->bc, g->de, g->hl); }
        fflush(stdout);
        dp_mach_close(M);
        dp_cart_free(c);
    }
    return 0;
}
