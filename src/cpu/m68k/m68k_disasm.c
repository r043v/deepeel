#include "m68k.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t avail;
    uint32_t pc;
    unsigned pos;  /* bytes consumed so far (opcode included) */
    int err;
    int has_target;
    uint32_t target;
} Ctx;

static unsigned rd16(Ctx *c) {
    if (c->pos + 2 > c->avail) { c->err = 1; c->pos += 2; return 0; }
    unsigned v = (unsigned)c->p[c->pos] << 8 | c->p[c->pos + 1];
    c->pos += 2;
    return v;
}
static uint32_t rd32(Ctx *c) {
    uint32_t h = rd16(c);
    return h << 16 | rd16(c);
}

static const char *const CC[16] = {"t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
                                   "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"};
static const char SZ[4] = {'b', 'w', 'l', '?'};

/* Appends an effective address; `size` is 0=b 1=w 2=l (immediate width).
 * `ctl` marks a control EA whose static address is recorded as the target. */
static void ea(Ctx *c, char *o, size_t n, unsigned mode, unsigned reg, unsigned size, int ctl) {
    switch (mode) {
    case 0: snprintf(o, n, "d%u", reg); break;
    case 1: snprintf(o, n, "a%u", reg); break;
    case 2: snprintf(o, n, "(a%u)", reg); break;
    case 3: snprintf(o, n, "(a%u)+", reg); break;
    case 4: snprintf(o, n, "-(a%u)", reg); break;
    case 5: { int16_t d = (int16_t)rd16(c); snprintf(o, n, "%d(a%u)", d, reg); break; }
    case 6: {
        unsigned x = rd16(c);
        snprintf(o, n, "%d(a%u,%c%u.%c)", (int8_t)(x & 0xFF), reg, x & 0x8000 ? 'a' : 'd',
                 (x >> 12) & 7, x & 0x800 ? 'l' : 'w');
        break;
    }
    case 7:
        switch (reg) {
        case 0: { int16_t a = (int16_t)rd16(c); uint32_t v = (uint32_t)(int32_t)a & 0xFFFFFF;
            snprintf(o, n, "$%X.w", v); if (ctl) { c->has_target = 1; c->target = v; } break; }
        case 1: { uint32_t v = rd32(c) & 0xFFFFFF; snprintf(o, n, "$%X", v);
            if (ctl) { c->has_target = 1; c->target = v; } break; }
        case 2: { uint32_t base = c->pc + c->pos; int16_t d = (int16_t)rd16(c);
            uint32_t v = (base + (uint32_t)(int32_t)d) & 0xFFFFFF;
            snprintf(o, n, "$%X(pc)", v); if (ctl) { c->has_target = 1; c->target = v; } break; }
        case 3: { unsigned x = rd16(c);
            snprintf(o, n, "%d(pc,%c%u.%c)", (int8_t)(x & 0xFF), x & 0x8000 ? 'a' : 'd',
                     (x >> 12) & 7, x & 0x800 ? 'l' : 'w'); break; }
        case 4:
            if (size == 2) snprintf(o, n, "#$%X", rd32(c));
            else snprintf(o, n, "#$%X", rd16(c) & (size ? 0xFFFFu : 0xFFu));
            break;
        default: snprintf(o, n, "?"); c->err = 1; break;
        }
        break;
    }
}

static void reglist(char *o, size_t n, unsigned mask, int predec) {
    /* mask bit i = d0..d7,a0..a7 (normal); reversed for predecrement */
    char t[64] = "";
    size_t l = 0;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned bit = predec ? 15 - i : i;
        if (!(mask >> bit & 1)) continue;
        l += (size_t)snprintf(t + l, sizeof t - l, "%s%c%u", l ? "/" : "", i < 8 ? 'd' : 'a', i & 7);
    }
    snprintf(o, n, "%s", t[0] ? t : "-");
}

unsigned dp_m68k_decode(const uint8_t *code, size_t avail, uint32_t pc, DpM68kInsn *r) {
    memset(r, 0, sizeof *r);
    r->cond = -1;
    if (avail < 2) return 0;
    Ctx c = {code, avail, pc, 0, 0, 0, 0};
    unsigned op = rd16(&c);
    unsigned mode = (op >> 3) & 7, reg = op & 7, sz = (op >> 6) & 3;
    unsigned dn = (op >> 9) & 7;
    char a[40] = "", b[40] = "", *t = r->text;
    const size_t T = sizeof r->text;
    DpM68kClass cls = DP_M68K_OTHER;

    switch (op >> 12) {
    case 0:
        if (op & 0x100) {
            static const char *const bn[4] = {"btst", "bchg", "bclr", "bset"};
            if (mode == 1) { /* movep */
                int16_t d = (int16_t)rd16(&c);
                if (op & 0x80) snprintf(t, T, "movep.%c d%u,%d(a%u)", op & 0x40 ? 'l' : 'w', dn, d, reg);
                else snprintf(t, T, "movep.%c %d(a%u),d%u", op & 0x40 ? 'l' : 'w', d, reg, dn);
            } else {
                ea(&c, a, sizeof a, mode, reg, 0, 0);
                snprintf(t, T, "%s d%u,%s", bn[sz], dn, a);
            }
        } else if (dn == 4) {
            static const char *const bn[4] = {"btst", "bchg", "bclr", "bset"};
            unsigned bit = rd16(&c) & 0xFF;
            ea(&c, a, sizeof a, mode, reg, 0, 0);
            snprintf(t, T, "%s #%u,%s", bn[sz], bit, a);
        } else if (dn == 7 || sz == 3) {
            snprintf(t, T, "dc.w $%04X", op); c.err = 1;
        } else {
            static const char *const in[7] = {"ori", "andi", "subi", "addi", "", "eori", "cmpi"};
            if (mode == 7 && reg == 4 && dn <= 5 && (dn == 0 || dn == 1 || dn == 5) && sz < 2) {
                unsigned imm = rd16(&c);
                snprintf(t, T, "%s #$%X,%s", in[dn], imm & (sz ? 0xFFFFu : 0xFFu), sz ? "sr" : "ccr");
            } else {
                unsigned imm_hi = 0, imm_lo;
                if (sz == 2) imm_hi = rd16(&c);
                imm_lo = rd16(&c);
                ea(&c, a, sizeof a, mode, reg, sz, 0);
                uint32_t imm = sz == 2 ? imm_hi << 16 | imm_lo : imm_lo & (sz ? 0xFFFFu : 0xFFu);
                snprintf(t, T, "%s.%c #$%X,%s", in[dn], SZ[sz], imm, a);
            }
        }
        break;
    case 1: case 2: case 3: {
        unsigned s = (op >> 12) == 1 ? 0 : (op >> 12) == 3 ? 1 : 2;
        unsigned dm = (op >> 6) & 7;
        ea(&c, a, sizeof a, mode, reg, s, 0);
        ea(&c, b, sizeof b, dm, dn, s, 0);
        if (dm == 1) snprintf(t, T, "movea.%c %s,%s", SZ[s], a, b);
        else snprintf(t, T, "move.%c %s,%s", SZ[s], a, b);
        break;
    }
    case 4:
        if (op == 0x4E71) snprintf(t, T, "nop");
        else if (op == 0x4E70) snprintf(t, T, "reset");
        else if (op == 0x4E72) { snprintf(t, T, "stop #$%X", rd16(&c)); cls = DP_M68K_STOP; }
        else if (op == 0x4E73) { snprintf(t, T, "rte"); cls = DP_M68K_RTE; }
        else if (op == 0x4E75) { snprintf(t, T, "rts"); cls = DP_M68K_RTS; }
        else if (op == 0x4E76) { snprintf(t, T, "trapv"); cls = DP_M68K_TRAP; }
        else if (op == 0x4E77) { snprintf(t, T, "rtr"); cls = DP_M68K_RTR; }
        else if (op == 0x4AFC) { snprintf(t, T, "illegal"); cls = DP_M68K_ILLEGAL; }
        else if ((op & 0xFFF0) == 0x4E40) { snprintf(t, T, "trap #%u", op & 15); cls = DP_M68K_TRAP; }
        else if ((op & 0xFFF8) == 0x4E50) { int16_t d = (int16_t)rd16(&c); snprintf(t, T, "link a%u,#%d", reg, d); }
        else if ((op & 0xFFF8) == 0x4E58) snprintf(t, T, "unlk a%u", reg);
        else if ((op & 0xFFF8) == 0x4E60) snprintf(t, T, "move a%u,usp", reg);
        else if ((op & 0xFFF8) == 0x4E68) snprintf(t, T, "move usp,a%u", reg);
        else if ((op & 0xFFC0) == 0x4E80 || (op & 0xFFC0) == 0x4EC0) {
            int j = (op & 0xFFC0) == 0x4EC0;
            ea(&c, a, sizeof a, mode, reg, 2, 1);
            snprintf(t, T, "%s %s", j ? "jmp" : "jsr", a);
            cls = j ? DP_M68K_JMP : DP_M68K_JSR;
        } else if ((op & 0xFFC0) == 0x40C0) { ea(&c, a, sizeof a, mode, reg, 1, 0); snprintf(t, T, "move sr,%s", a); }
        else if ((op & 0xFFC0) == 0x44C0) { ea(&c, a, sizeof a, mode, reg, 1, 0); snprintf(t, T, "move %s,ccr", a); }
        else if ((op & 0xFFC0) == 0x46C0) { ea(&c, a, sizeof a, mode, reg, 1, 0); snprintf(t, T, "move %s,sr", a); }
        else if ((op & 0xFFC0) == 0x4800) { ea(&c, a, sizeof a, mode, reg, 0, 0); snprintf(t, T, "nbcd %s", a); }
        else if ((op & 0xFFF8) == 0x4840) snprintf(t, T, "swap d%u", reg);
        else if ((op & 0xFFC0) == 0x4840) { ea(&c, a, sizeof a, mode, reg, 2, 0); snprintf(t, T, "pea %s", a); }
        else if ((op & 0xFFB8) == 0x4880) snprintf(t, T, "ext.%c d%u", op & 0x40 ? 'l' : 'w', reg);
        else if ((op & 0xFB80) == 0x4880) {
            unsigned mask = rd16(&c);
            int tomem = !(op & 0x400);
            reglist(b, sizeof b, mask, tomem && mode == 4);
            ea(&c, a, sizeof a, mode, reg, 2, 0);
            if (tomem) snprintf(t, T, "movem.%c %s,%s", op & 0x40 ? 'l' : 'w', b, a);
            else snprintf(t, T, "movem.%c %s,%s", op & 0x40 ? 'l' : 'w', a, b);
        } else if ((op & 0xFF00) == 0x4A00 && sz == 3) { ea(&c, a, sizeof a, mode, reg, 0, 0); snprintf(t, T, "tas %s", a); }
        else if ((op & 0xFF00) == 0x4A00) { ea(&c, a, sizeof a, mode, reg, sz, 0); snprintf(t, T, "tst.%c %s", SZ[sz], a); }
        else if ((op & 0xF100) == 0x4000 && sz < 3 && (op & 0x0E00) != 0x0800) {
            static const char *const un[4] = {"negx", "clr", "neg", "not"};
            ea(&c, a, sizeof a, mode, reg, sz, 0);
            snprintf(t, T, "%s.%c %s", un[(op >> 9) & 3], SZ[sz], a);
        } else if ((op & 0x1C0) == 0x180) { ea(&c, a, sizeof a, mode, reg, 1, 0); snprintf(t, T, "chk %s,d%u", a, dn); }
        else if ((op & 0x1C0) == 0x1C0) { ea(&c, a, sizeof a, mode, reg, 2, 0); snprintf(t, T, "lea %s,a%u", a, dn); }
        else { snprintf(t, T, "dc.w $%04X", op); c.err = 1; }
        break;
    case 5:
        if (sz == 3) {
            if (mode == 1) {
                uint32_t base = pc + c.pos; int16_t d = (int16_t)rd16(&c);
                r->has_target = 1; r->target = (base + (uint32_t)(int32_t)d) & 0xFFFFFF;
                r->cond = (op >> 8) & 15;
                snprintf(t, T, "db%s d%u,$%X", CC[r->cond], reg, r->target);
                cls = DP_M68K_DBCC;
            } else {
                ea(&c, a, sizeof a, mode, reg, 0, 0);
                snprintf(t, T, "s%s %s", CC[(op >> 8) & 15], a);
            }
        } else {
            unsigned q = dn ? dn : 8;
            ea(&c, a, sizeof a, mode, reg, sz, 0);
            snprintf(t, T, "%s.%c #%u,%s", op & 0x100 ? "subq" : "addq", SZ[sz], q, a);
        }
        break;
    case 6: {
        unsigned cnd = (op >> 8) & 15;
        int32_t d = (int8_t)(op & 0xFF);
        uint32_t base = pc + 2;
        if ((op & 0xFF) == 0) d = (int16_t)rd16(&c);
        r->has_target = 1; r->target = (base + (uint32_t)d) & 0xFFFFFF;
        if (cnd == 0) { cls = DP_M68K_BRA; snprintf(t, T, "bra $%X", r->target); }
        else if (cnd == 1) { cls = DP_M68K_BSR; snprintf(t, T, "bsr $%X", r->target); }
        else { cls = DP_M68K_BCC; r->cond = (int)cnd; snprintf(t, T, "b%s $%X", CC[cnd], r->target); }
        break;
    }
    case 7:
        if (op & 0x100) { snprintf(t, T, "dc.w $%04X", op); c.err = 1; }
        else snprintf(t, T, "moveq #%d,d%u", (int8_t)(op & 0xFF), dn);
        break;
    case 8: case 12: {
        int isor = (op >> 12) == 8;
        if (sz == 3) {
            ea(&c, a, sizeof a, mode, reg, 1, 0);
            const char *m = isor ? (op & 0x100 ? "divs" : "divu") : (op & 0x100 ? "muls" : "mulu");
            snprintf(t, T, "%s %s,d%u", m, a, dn);
        } else if ((op & 0x1F0) == 0x100) {
            const char *m = isor ? "sbcd" : "abcd";
            if (op & 8) snprintf(t, T, "%s -(a%u),-(a%u)", m, reg, dn);
            else snprintf(t, T, "%s d%u,d%u", m, reg, dn);
        } else if (!isor && (op & 0x1F8) == 0x140) snprintf(t, T, "exg d%u,d%u", dn, reg);
        else if (!isor && (op & 0x1F8) == 0x148) snprintf(t, T, "exg a%u,a%u", dn, reg);
        else if (!isor && (op & 0x1F8) == 0x188) snprintf(t, T, "exg d%u,a%u", dn, reg);
        else {
            ea(&c, a, sizeof a, mode, reg, sz, 0);
            if (op & 0x100) snprintf(t, T, "%s.%c d%u,%s", isor ? "or" : "and", SZ[sz], dn, a);
            else snprintf(t, T, "%s.%c %s,d%u", isor ? "or" : "and", SZ[sz], a, dn);
        }
        break;
    }
    case 9: case 13: {
        int add = (op >> 12) == 13;
        if (sz == 3) {
            unsigned s = op & 0x100 ? 2 : 1;
            ea(&c, a, sizeof a, mode, reg, s, 0);
            snprintf(t, T, "%s.%c %s,a%u", add ? "adda" : "suba", SZ[s], a, dn);
        } else if ((op & 0x130) == 0x100) {
            const char *m = add ? "addx" : "subx";
            if (op & 8) snprintf(t, T, "%s.%c -(a%u),-(a%u)", m, SZ[sz], reg, dn);
            else snprintf(t, T, "%s.%c d%u,d%u", m, SZ[sz], reg, dn);
        } else {
            ea(&c, a, sizeof a, mode, reg, sz, 0);
            if (op & 0x100) snprintf(t, T, "%s.%c d%u,%s", add ? "add" : "sub", SZ[sz], dn, a);
            else snprintf(t, T, "%s.%c %s,d%u", add ? "add" : "sub", SZ[sz], a, dn);
        }
        break;
    }
    case 11:
        if (sz == 3) {
            unsigned s = op & 0x100 ? 2 : 1;
            ea(&c, a, sizeof a, mode, reg, s, 0);
            snprintf(t, T, "cmpa.%c %s,a%u", SZ[s], a, dn);
        } else if (!(op & 0x100)) {
            ea(&c, a, sizeof a, mode, reg, sz, 0);
            snprintf(t, T, "cmp.%c %s,d%u", SZ[sz], a, dn);
        } else if (mode == 1) snprintf(t, T, "cmpm.%c (a%u)+,(a%u)+", SZ[sz], reg, dn);
        else {
            ea(&c, a, sizeof a, mode, reg, sz, 0);
            snprintf(t, T, "eor.%c d%u,%s", SZ[sz], dn, a);
        }
        break;
    case 14: {
        static const char *const sn[4] = {"as", "ls", "rox", "ro"};
        char d = op & 0x100 ? 'l' : 'r';
        if (sz == 3) {
            ea(&c, a, sizeof a, mode, reg, 1, 0);
            snprintf(t, T, "%s%c.w %s", sn[(op >> 9) & 3], d, a);
        } else {
            unsigned ty = (op >> 3) & 3;
            if (op & 0x20) snprintf(t, T, "%s%c.%c d%u,d%u", sn[ty], d, SZ[sz], dn, reg);
            else snprintf(t, T, "%s%c.%c #%u,d%u", sn[ty], d, SZ[sz], dn ? dn : 8, reg);
        }
        break;
    }
    default: /* line A / line F */
        snprintf(t, T, "dc.w $%04X", op);
        cls = DP_M68K_ILLEGAL;
        break;
    }
    if (c.err && cls == DP_M68K_OTHER) cls = DP_M68K_ILLEGAL;
    if (c.has_target && !r->has_target) { r->has_target = 1; r->target = c.target; }
    r->cls = cls;
    r->len = c.pos;
    if (c.pos > avail) return 0;
    return c.pos;
}
