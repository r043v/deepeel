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
#include "../core/dpmachine.h"

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
    dp_cov_mark(&res->first, res->src_flags, k, DP_SRC_PROVENANCE, res->steps_total, 0);
    if (!(res->data_flags[k] & DP_DF_STATIC)) {
        res->data_flags[k] |= DP_DF_STATIC;
        res->prov_data++;
    }
}

typedef struct { DpResults *res; int unused; } ProvRead;

// a ROM byte at a provable address: the proof is recorded as static data
static int prov_rom_read(void *ctx, uint16_t addr, uint8_t *v) {
    ProvRead *pr = ctx;
    uint32_t k = prov_addr_key(pr->res, addr);
    if (k == ~0u) return 0;
    *v = pr->res->cart->rom[k];
    prov_data_mark(pr->res, addr);
    return 1;
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
        const DpIsa *isa = dp_isa_sm83();
        int len = isa->length(c->rom[s.k]);
        if ((uint32_t)s.k + (uint32_t)len > c->rom_size) continue;
        res->prov_steps++;
        steps++;

        DpFlowInfo fi;
        isa->flow(&c->rom[s.k], pc, &fi);
        DpProvRegs r = { s.hl, s.hlmask, s.a, s.aknown };
        ProvRead pr = { res, 0 };
        DpProvRom rom = { &pr, prov_rom_read };
        isa->regs_effect(&c->rom[s.k], &r, &rom);
        s.hl = r.p; s.hlmask = r.pmask; s.a = r.a; s.aknown = r.aknown;

        int fall = fi.fall;
        if (fi.halt) fall = 0; // halt/stop: the run ends here
        if (fi.kind == DP_FLOW_INDIRECT) { // jp hl
            if (s.hlmask != 3) {
                res->prov_unresolved++;
            } else if (s.hl < 0x4000 && s.hl < c->rom_size) {
                uint32_t tk = s.hl;
                if (!(res->exec_flags[tk] & (DP_XF_NATURAL | DP_XF_FORCED))) {
                    uint8_t *f = &res->exec_flags[tk];
                    if (!(*f & DP_XF_STATIC)) marked++;
                    *f |= DP_XF_STATIC | DP_XF_START | DP_XF_ENTRY;
                    dp_cov_mark(&res->first, res->src_flags, tk, DP_SRC_PROVENANCE, res->steps_total, 0);
                }
                res->prov_jp_hl++;
                ProvState ns = s;
                ns.k = tk;
                ppush(&st, ns);
            } else {
                res->prov_unresolved++; // $4000 window: no static bank evidence
            }
        } else if (fi.target >= 0 && fi.rst) { // restart vector: bank 0, no forgetting continuation
            ProvState ns = s;
            ns.k = (uint32_t)fi.target;
            ppush(&st, ns);
        } else if (fi.target >= 0) { // direct jump / call / conditional
            ProvState ns = s;
            ns.k = prov_target(res, bank, (uint32_t)fi.target);
            if (ns.k != ~0u) ppush(&st, ns);
            else res->prov_unresolved++;
            if (fi.call) {
                // the callee may clobber everything: the continuation forgets
                ProvState rs = { 0, 0, 0, 0, 0 };
                rs.k = prov_target(res, bank, (uint32_t)pc + 3);
                if (rs.k != ~0u) ppush(&st, rs);
            }
        }
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
