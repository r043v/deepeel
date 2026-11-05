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
    uint16_t de, bc; // whole-pair values (S3: BC/DE beside HL)
    uint8_t pm2;     // bit0 = DE known, bit1 = BC known
    uint16_t tbase;  // last `ld hl,nn` base: the candidate pointer table of an indexed dispatch
    uint8_t tflags;  // bit0 = tbase valid, bit1 = a `ld h,(hl)` pointer load followed it
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

// S3: BC/DE tracked as whole pairs beside HL.  Only instructions known not to touch B/C/D/E keep
// them; anything else forgets both pairs (never a stale value).  Runs after the ISA effect, which
// already treated the opcode as an unknown write to HL/A where it applies.
static void prov_pairs(DpResults *res, ProvRead *pr, ProvState *s, uint8_t op,
                       uint16_t old_hl, uint8_t old_mask, DpProvRegs *r) {
    uint8_t v;
    switch (op) {
    case 0x11: s->de = (uint16_t)(res->cart->rom[s->k + 1] | (res->cart->rom[s->k + 2] << 8)); s->pm2 |= 1; return;
    case 0x01: s->bc = (uint16_t)(res->cart->rom[s->k + 1] | (res->cart->rom[s->k + 2] << 8)); s->pm2 |= 2; return;
    case 0x13: if (s->pm2 & 1) s->de++; return;
    case 0x1B: if (s->pm2 & 1) s->de--; return;
    case 0x03: if (s->pm2 & 2) s->bc++; return;
    case 0x0B: if (s->pm2 & 2) s->bc--; return;
    case 0x1A: case 0x0A: { // ld a,(de) / ld a,(bc)
        int bit = op == 0x1A ? 1 : 2;
        uint16_t addr = op == 0x1A ? s->de : s->bc;
        if ((s->pm2 & bit) && prov_rom_read(pr, addr, &v)) { r->a = v; r->aknown = 1; }
        else r->aknown = 0;
        return;
    }
    case 0x7A: case 0x7B: case 0x78: case 0x79: { // ld a,d|e|b|c
        int bit = (op == 0x7A || op == 0x7B) ? 1 : 2;
        uint16_t pair = bit == 1 ? s->de : s->bc;
        if (s->pm2 & bit) { r->a = (op == 0x7A || op == 0x78) ? (uint8_t)(pair >> 8) : (uint8_t)pair; r->aknown = 1; }
        else r->aknown = 0;
        return;
    }
    case 0x62: case 0x60: { // ld h,d / ld h,b
        int bit = op == 0x62 ? 1 : 2;
        uint16_t pair = bit == 1 ? s->de : s->bc;
        if (s->pm2 & bit) { r->p = (uint16_t)((old_hl & 0x00FF) | (pair & 0xFF00)); r->pmask = old_mask | 2; }
        return;
    }
    case 0x6B: case 0x69: { // ld l,e / ld l,c
        int bit = op == 0x6B ? 1 : 2;
        uint16_t pair = bit == 1 ? s->de : s->bc;
        if (s->pm2 & bit) { r->p = (uint16_t)((old_hl & 0xFF00) | (pair & 0x00FF)); r->pmask = old_mask | 1; }
        return;
    }
    // instructions that provably leave B/C/D/E alone
    case 0x00: case 0x3E: case 0x21: case 0x23: case 0x2B: case 0x2A: case 0x7E: case 0x66: case 0x6E:
    case 0x67: case 0x6F: case 0xFA: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE9:
        return;
    default:
        s->pm2 = 0;
        return;
    }
}


// An indexed `jp hl` (the index is not provable) whose HL came from a pointer table at a known
// base: walk the table's words and prove every plausible one a code entry (DP_XF_STATIC, the
// closure then seeds them) and its bytes data.  Plausible = contiguous, inside the ROM view the
// table belongs to, first byte a legal opcode that is not filler, location never executed.
// Tool inference, not an observation: a table needs two plausible entries, at most 128.
static int prov_legal_op(uint8_t op) {
    switch (op) { case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4: case 0xEB:
                  case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD: return 0;
                  default: return 1; }
}
static uint32_t prov_table_targets(DpResults *res, uint32_t bank, uint16_t base) {
    const DpCart *c = res->cart;
    uint32_t tkey;
    if (base < 0x4000) tkey = base;
    else if (bank) tkey = bank * 0x4000 + (base - 0x4000);
    else return 0; // bank 0 code, table in the window: no static bank evidence
    uint32_t tbank = tkey / 0x4000, got = 0, tg[128];
    uint32_t marked = 0;
    for (uint32_t i = 0; i < 128; i++) {
        uint32_t w = tkey + 2 * i;
        if (w + 1 >= c->rom_size || (w + 1) / 0x4000 != tbank) break;
        if ((res->exec_flags[w] | res->exec_flags[w + 1]) & DP_XF_NATURAL) break; // run by the natural flow: code, not a table (speculative forks may run table bytes)
        uint16_t t = (uint16_t)(c->rom[w] | (c->rom[w + 1] << 8));
        uint32_t tk = t < 0x8000 ? prov_target(res, tbank, t) : ~0u;
        if (tk == ~0u || tk >= c->rom_size) break;
        uint8_t op = c->rom[tk];
        if (op == 0x00 || op == 0xFF || !prov_legal_op(op)) break;
        tg[got++] = tk;
    }
    if (got < 2) return 0;
    for (uint32_t i = 0; i < got; i++) {
        uint32_t w = tkey + 2 * i;
        for (uint32_t b = 0; b < 2; b++) {
            dp_cov_mark(&res->first, res->src_flags, w + b, DP_SRC_PROVENANCE, res->steps_total, 0);
            if (!(res->data_flags[w + b] & DP_DF_STATIC)) { res->data_flags[w + b] |= DP_DF_STATIC; res->prov_data++; }
        }
        uint8_t *f = &res->exec_flags[tg[i]];
        if (!(*f & (DP_XF_NATURAL | DP_XF_FORCED))) {
            if (!(*f & DP_XF_STATIC)) marked++;
            *f |= DP_XF_STATIC | DP_XF_START | DP_XF_ENTRY;
            dp_cov_mark(&res->first, res->src_flags, tg[i], DP_SRC_PROVENANCE, res->steps_total, 0);
        }
    }
    res->prov_table_targets += got;
    return marked;
}

// rst-vector dispatchers (SML2's `rst $28`): the vector pops its return address, indexes the table
// that follows the call site and jumps.  Recognised by shape: a `pop hl` then an `add hl,..`
// before the first `jp hl`, no return or jump in between.
static uint8_t prov_rst_dispatchers(const DpCart *c) {
    uint8_t mask = 0;
    for (int v = 0; v < 8; v++) {
        uint32_t a = (uint32_t)v * 8;
        if (a + 24 > c->rom_size) break;
        int pop = 0, add = 0, ok = 0;
        for (uint32_t i = a; i < a + 24; i++) {
            uint8_t op = c->rom[i];
            if (op == 0xC9 || op == 0xD9 || op == 0xC3) break;
            if (op == 0xE1) pop = 1;
            else if (pop && (op == 0x19 || op == 0x29)) add = 1;
            else if (op == 0xE9) { ok = pop && add; break; }
        }
        if (ok) mask |= (uint8_t)(1u << v);
    }
    return mask;
}

uint32_t dp_provenance(DpResults *res) {
    const DpCart *c = res->cart;
    size_t n = (size_t)res->banks * 0x4000;
    DpHashSet vis;
    ProvStack st = {0};
    uint32_t marked = 0;
    dp_set_init(&vis, 1 << 16);
    uint8_t rstd = prov_rst_dispatchers(c);
    for (size_t k = 0; rstd && k < n && k < c->rom_size; k++) { // inline tables after each `rst` call site
        uint8_t op = c->rom[k];
        if ((op & 0xC7) != 0xC7 || !(rstd & (1u << ((op >> 3) & 7)))) continue;
        if (!(res->exec_flags[k] & DP_XF_START) || !(res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED | DP_XF_STATIC))) continue;
        uint32_t bank = (uint32_t)(k / 0x4000);
        uint32_t base = bank ? 0x4000 + (uint32_t)(k % 0x4000) + 1 : (uint32_t)k + 1;
        if (base >= 0x8000 || (bank == 0 && base >= 0x4000)) continue;
        marked += prov_table_targets(res, bank, (uint16_t)base);
    }
    for (size_t k = 0; k < n; k++) {
        if (!(res->exec_flags[k] & DP_XF_START) ||
            !(res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED | DP_XF_STATIC)))
            continue;
        ProvState s0 = { (uint32_t)k, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
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
        // BC/DE join the key through a mix: a collision only prunes a state, never invents one
        if (s.tflags) vk ^= ((uint64_t)s.tbase * 4 + s.tflags) * 0xC2B2AE3D27D4EB4Full;
        vk ^= (((uint64_t)(s.pm2 & 1 ? s.de : 0) << 16 | (s.pm2 & 2 ? s.bc : 0)) * 4 + s.pm2) * 0x9E3779B97F4A7C15ull << 20;
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
        uint16_t old_hl = r.p;
        uint8_t old_mask = r.pmask;
        isa->regs_effect(&c->rom[s.k], &r, &rom);
        prov_pairs(res, &pr, &s, c->rom[s.k], old_hl, old_mask, &r);
        s.hl = r.p; s.hlmask = r.pmask; s.a = r.a; s.aknown = r.aknown;
        { // indexed dispatch idiom: ld hl,TABLE ... add hl,idx ... ld h,(hl) ... jp hl
            uint8_t op0 = c->rom[s.k];
            if (op0 == 0x21) {
                uint16_t base = (uint16_t)(c->rom[s.k + 1] | (c->rom[s.k + 2] << 8));
                if (base < 0x8000) { s.tbase = base; s.tflags = 1; } else s.tflags = 0;
            } else if (op0 == 0x66 && (s.tflags & 1)) s.tflags |= 2;
        }

        int fall = fi.fall;
        if (fi.halt) fall = 0; // halt/stop: the run ends here
        if (fi.kind == DP_FLOW_INDIRECT) { // jp hl
            if (s.hlmask != 3) {
                res->prov_unresolved++;
                if (s.tflags == 3) marked += prov_table_targets(res, bank, s.tbase);
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
                ProvState rs = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
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
