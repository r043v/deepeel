// provenance.c: static register provenance (ported from the z80 branch's
// provenance pass).  A straight-line walk over the ROM — no machine — that
// tracks which of HL's two bytes (and A) hold statically provable values:
// immediate loads, ROM words at absolute addresses, INC/DEC by exact
// computation, and pointer-table walks (ld l,(hl); inc hl; ld h,(hl); jp hl).
// Two uses:
//   - a JP HL with a fully known HL proves its target: DP_XF_STATIC (the
//     flow closure then extends from it like any other static entry);
//   - a ROM read at a fully known address (ld a,(hl), ld a,(hli), ld h/l,(hl),
//     ld a,(nn)) proves the byte is data: DP_DF_STATIC.
// A partial proof (high byte unknown, banked window with no bank evidence)
// proves nothing — the address stays unknown, nothing is invented.  Bounded:
// visited (pc, HL, A) states, total steps and run marks all capped, every
// saturation counted.
#include "explorer.h"
#include "../cpu/sm83/sm83.h"

#include <stdlib.h>
#include <string.h>

#define DP_PROV_STEPS 4u * 1024 * 1024 // total instruction steps
#define DP_PROV_STATES (1u << 20)      // visited (pc, hl, a) states

typedef struct {
    uint32_t k;      // rom index of the pc
    uint16_t hl;
    uint8_t hlmask;  // bit0 = l known, bit1 = h known
    uint8_t a;
    uint8_t aknown;
} ProvState;

typedef struct { ProvState *v; size_t n, cap; } ProvStack;

static void ppush(ProvStack *s, ProvState st) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4096;
        s->v = realloc(s->v, s->cap * sizeof(ProvState));
        if (!s->v) abort();
    }
    s->v[s->n++] = st;
}

// conservative kill tables: anything not proven kept is lost, never guessed
static int hl_writes(uint8_t op) {
    if (op == 0xCB) return 1; // rot/shift/bit ops on h/l: untracked
    if (op >= 0x60 && op <= 0x66) return 1; // ld h,r except ld h,(hl) handled
    if (op >= 0x68 && op <= 0x6E) return 1; // ld l,r except ld l,(hl) handled
    switch (op) {
    case 0x09: case 0x19: case 0x29: case 0x39: // add hl,rr
    case 0x22:                                  // ldi (hl),a
    case 0xE1:                                  // pop hl
    case 0xEB:                                  // ex de,hl
    case 0xF8:                                  // ld hl,sp+r8
        return 1;
    default:
        return 0;
    }
}

static int a_writes(uint8_t op) {
    if (op == 0xCB) return 1;
    if ((op >= 0x80 && op <= 0xBF)) return 1;         // alu (except handled (hl) reads)
    if (op >= 0x04 && op <= 0x3D && op != 0x76) return 1; // single-arg alu/rot/daa/cpl/scf..
    switch (op) {
    case 0x0A: case 0x1A:      // ld a,(bc) / ld a,(de)
    case 0xF0: case 0xF2:      // ldh a,(n) / ld a,(C)
    case 0x07: case 0x0F: case 0x17: case 0x1F: // rotates through a
        return 1;
    default:
        return 0;
    }
}

// proof of a ROM byte at a fully known address: bank 0 directly, the $4000
// window has no static bank evidence — never invented
static uint32_t prov_addr_key(DpResults *res, uint16_t addr) {
    if (addr >= 0x4000) return ~0u;
    if ((uint32_t)addr >= res->cart->rom_size) return ~0u;
    return (uint32_t)addr;
}

// ROM key of a direct branch target seen from `bank`: $0000-$3FFF is bank 0
// whatever the caller's bank, the $4000 window stays the caller's bank, and
// bank 0 reaching into the window has no static bank evidence (a fixed
// 32 KiB cart excepted).  ~0u = no provable key (unbanked window, RAM code).
static uint32_t prov_target(const DpResults *res, uint32_t bank, uint32_t t) {
    uint32_t k;
    if (t < 0x4000) k = t;
    else if (t >= 0x8000) return ~0u;
    else if (bank) k = bank * 0x4000 + (t - 0x4000);
    else if (res->banks == 2) k = t;
    else return ~0u;
    return k < res->cart->rom_size ? k : ~0u;
}

static void prov_data_mark(DpResults *res, uint16_t addr) {
    uint32_t k = prov_addr_key(res, addr);
    if (k == ~0u) return;
    res->src_flags[k] |= DP_SRC_PROVENANCE;
    if (!(res->data_flags[k] & DP_DF_STATIC)) {
        res->data_flags[k] |= DP_DF_STATIC;
        res->prov_data++;
    }
}

uint32_t dp_provenance(DpResults *res) {
    const DpCart *c = res->cart;
    size_t n = (size_t)res->banks * 0x4000;
    DpHashSet vis;
    ProvStack st = {0};
    uint32_t marked = 0;
    dp_set_init(&vis, 1 << 16);
    for (size_t k = 0; k < n; k++) {
        if (!(res->exec_flags[k] & DP_XF_START) ||
            !(res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED | DP_XF_STATIC)))
            continue;
        ProvState s0 = { (uint32_t)k, 0, 0, 0, 0 };
        ppush(&st, s0);
    }
    uint32_t steps = 0;
    while (st.n) {
        // per-call budget: a later call is not starved by the earlier ones
        if (steps >= DP_PROV_STEPS || vis.cnt >= DP_PROV_STATES) {
            res->prov_sat++;
            break;
        }
        ProvState s = st.v[--st.n];
        uint64_t vk = ((uint64_t)s.k << 17) | ((uint64_t)(s.hlmask ? s.hl : 0) << 1) | s.aknown;
        if (dp_set_insert(&vis, vk) <= 0) continue;
        if (s.k >= n || s.k >= c->rom_size) continue;
        uint32_t bank = s.k / 0x4000, off = s.k % 0x4000;
        uint16_t pc = bank ? (uint16_t)(0x4000 + off) : (uint16_t)off;
        uint8_t op = c->rom[s.k];
        int len = sm83_len(op);
        if ((uint32_t)s.k + (uint32_t)len > c->rom_size) continue;
        res->prov_steps++;
        steps++;

        uint32_t rd = ~0u; // a (hl)/(nn) read at a provable address
        int fall = 1;
        switch (op) {
        case 0x21: s.hl = c->rom[s.k + 1] | (c->rom[s.k + 2] << 8); s.hlmask = 3; break;
        case 0x26: s.hl = (uint16_t)((s.hl & 0x00FF) | (c->rom[s.k + 1] << 8)); s.hlmask |= 2; break;
        case 0x2E: s.hl = (uint16_t)((s.hl & 0xFF00) | c->rom[s.k + 1]); s.hlmask |= 1; break;
        case 0x23: if (s.hlmask == 3) s.hl++; break;
        case 0x2B: if (s.hlmask == 3) s.hl--; break;
        case 0x2A: // ld a,(hli)
            if (s.hlmask == 3) {
                rd = prov_addr_key(res, s.hl);
                if (rd != ~0u) { s.a = c->rom[rd]; s.aknown = 1; } else s.aknown = 0;
                s.hl++;
            } else s.aknown = 0;
            break;
        case 0x7E: // ld a,(hl)
            if (s.hlmask == 3) {
                rd = prov_addr_key(res, s.hl);
                if (rd != ~0u) { s.a = c->rom[rd]; s.aknown = 1; } else s.aknown = 0;
            } else s.aknown = 0;
            break;
        case 0x66: // ld h,(hl)
            if (s.hlmask == 3 && (rd = prov_addr_key(res, s.hl)) != ~0u) {
                s.hl = (uint16_t)((s.hl & 0x00FF) | (c->rom[rd] << 8));
                s.hlmask |= 2;
            } else s.hlmask &= (uint8_t)~2;
            break;
        case 0x6E: // ld l,(hl)
            if (s.hlmask == 3 && (rd = prov_addr_key(res, s.hl)) != ~0u) {
                s.hl = (uint16_t)((s.hl & 0xFF00) | c->rom[rd]);
                s.hlmask |= 1;
            } else s.hlmask &= (uint8_t)~1;
            break;
        case 0x3E: s.a = c->rom[s.k + 1]; s.aknown = 1; break; // ld a,n
        case 0x67: // ld h,a
            if (s.aknown) { s.hl = (uint16_t)((s.hl & 0x00FF) | (s.a << 8)); s.hlmask |= 2; }
            else s.hlmask &= (uint8_t)~2;
            break;
        case 0x6F: // ld l,a
            if (s.aknown) { s.hl = (uint16_t)((s.hl & 0xFF00) | s.a); s.hlmask |= 1; }
            else s.hlmask &= (uint8_t)~1;
            break;
        case 0xFA: // ld a,(nn): a word read at an absolute address
            rd = prov_addr_key(res, (uint16_t)(c->rom[s.k + 1] | (c->rom[s.k + 2] << 8)));
            if (rd != ~0u) { s.a = c->rom[rd]; s.aknown = 1; } else s.aknown = 0;
            break;
        case 0xE9: // jp hl
            fall = 0;
            if (s.hlmask != 3) {
                res->prov_unresolved++;
                break;
            }
            if (s.hl < 0x4000 && s.hl < c->rom_size) {
                uint32_t tk = s.hl;
                if (!(res->exec_flags[tk] & (DP_XF_NATURAL | DP_XF_FORCED))) {
                    uint8_t *f = &res->exec_flags[tk];
                    if (!(*f & DP_XF_STATIC)) marked++;
                    *f |= DP_XF_STATIC | DP_XF_START | DP_XF_ENTRY;
                    res->src_flags[tk] |= DP_SRC_PROVENANCE;
                }
                res->prov_jp_hl++;
                ProvState ns = s;
                ns.k = tk;
                ppush(&st, ns);
            } else {
                res->prov_unresolved++; // $4000 window: no static bank evidence
            }
            break;
        case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
            uint16_t t = (uint16_t)(pc + 2 + (int8_t)c->rom[s.k + 1]);
            fall = op != 0x18;
            ProvState ns = s;
            ns.k = prov_target(res, bank, t);
            if (ns.k != ~0u) ppush(&st, ns);
            else res->prov_unresolved++;
            break;
        }
        case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        case 0xCD: case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
            uint16_t t = (uint16_t)(c->rom[s.k + 1] | (c->rom[s.k + 2] << 8));
            fall = op != 0xC3;
            ProvState ns = s;
            ns.k = prov_target(res, bank, t);
            if (ns.k != ~0u) ppush(&st, ns);
            else res->prov_unresolved++;
            if (op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC) {
                // the callee may clobber everything: the continuation forgets
                ProvState rs = { 0, 0, 0, 0, 0 };
                rs.k = prov_target(res, bank, (uint32_t)pc + 3);
                if (rs.k != ~0u) ppush(&st, rs);
            }
            break;
        }
        case 0xC9: case 0xD9: case 0x76: case 0x10:
            fall = 0; // ret/reti/halt/stop: the run ends here
            break;
        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF: { // RST
            ProvState ns = s;
            ns.k = (uint32_t)(op & 0x38);
            ppush(&st, ns);
            break;
        }
        default:
            if (hl_writes(op)) { s.hlmask = 0; }
            if (a_writes(op)) s.aknown = 0;
            break;
        }
        if (rd != ~0u) prov_data_mark(res, rd);
        if (fall) {
            uint32_t nk = s.k + (uint32_t)len;
            uint16_t npc = pc + (uint32_t)len;
            int cross = bank == 0 ? npc >= 0x4000 : npc >= 0x8000;
            if (!cross && nk < n) {
                ProvState ns = s;
                ns.k = nk;
                ppush(&st, ns);
            }
        }
    }
    free(st.v);
    dp_set_free(&vis);
    return marked;
}
