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
    uint8_t pm2;     // bit0 = E known, bit1 = D known, bit2 = C known, bit3 = B known (S3 byte-granular)
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
    // byte reads of the tracked pairs: B=(bc>>8) C=bc&0xFF D=(de>>8) E=de&0xFF
    #define PPAIR_BYTE(pair, hi) (uint8_t)((hi) ? (pair) >> 8 : (pair) & 0xFF)
    switch (op) {
    case 0x11: s->de = (uint16_t)(res->cart->rom[s->k + 1] | (res->cart->rom[s->k + 2] << 8)); s->pm2 |= 3; return;  // ld de,nn
    case 0x01: s->bc = (uint16_t)(res->cart->rom[s->k + 1] | (res->cart->rom[s->k + 2] << 8)); s->pm2 |= 12; return; // ld bc,nn
    case 0x16: s->de = (uint16_t)((s->de & 0x00FF) | ((uint16_t)res->cart->rom[s->k + 1] << 8)); s->pm2 |= 2; return; // ld d,n
    case 0x1E: s->de = (uint16_t)((s->de & 0xFF00) | res->cart->rom[s->k + 1]); s->pm2 |= 1; return;                 // ld e,n
    case 0x06: s->bc = (uint16_t)((s->bc & 0x00FF) | ((uint16_t)res->cart->rom[s->k + 1] << 8)); s->pm2 |= 8; return; // ld b,n
    case 0x0E: s->bc = (uint16_t)((s->bc & 0xFF00) | res->cart->rom[s->k + 1]); s->pm2 |= 4; return;                 // ld c,n
    case 0x13: if ((s->pm2 & 3) == 3) s->de++; return;   // inc de
    case 0x1B: if ((s->pm2 & 3) == 3) s->de--; return;   // dec de
    case 0x03: if ((s->pm2 & 12) == 12) s->bc++; return; // inc bc
    case 0x0B: if ((s->pm2 & 12) == 12) s->bc--; return; // dec bc
    case 0x14: if (s->pm2 & 2) s->de = (uint16_t)((s->de & 0x00FF) | ((s->de + 0x0100) & 0xFF00)); return;   // inc d
    case 0x1C: if (s->pm2 & 1) s->de = (uint16_t)((s->de & 0xFF00) | ((s->de + 1) & 0x00FF)); return;        // inc e
    case 0x15: if (s->pm2 & 2) s->de = (uint16_t)((s->de & 0x00FF) | ((s->de - 0x0100) & 0xFF00)); return;   // dec d
    case 0x1D: if (s->pm2 & 1) s->de = (uint16_t)((s->de & 0xFF00) | ((s->de - 1) & 0x00FF)); return;        // dec e
    case 0x04: if (s->pm2 & 8) s->bc = (uint16_t)((s->bc & 0x00FF) | ((s->bc + 0x0100) & 0xFF00)); return;   // inc b
    case 0x0C: if (s->pm2 & 4) s->bc = (uint16_t)((s->bc & 0xFF00) | ((s->bc + 1) & 0x00FF)); return;        // inc c
    case 0x05: if (s->pm2 & 8) s->bc = (uint16_t)((s->bc & 0x00FF) | ((s->bc - 0x0100) & 0xFF00)); return;   // dec b
    case 0x0D: if (s->pm2 & 4) s->bc = (uint16_t)((s->bc & 0xFF00) | ((s->bc - 1) & 0x00FF)); return;        // dec c
    case 0x1A: case 0x0A: { // ld a,(de) / ld a,(bc)
        int bit = op == 0x1A ? 3 : 12;
        uint16_t addr = op == 0x1A ? s->de : s->bc;
        if ((s->pm2 & bit) == bit && prov_rom_read(pr, addr, &v)) { r->a = v; r->aknown = 1; }
        else r->aknown = 0;
        return;
    }
    case 0x7A: case 0x7B: case 0x78: case 0x79: { // ld a,d|e|b|c
        int bit = (op == 0x7A || op == 0x7B) ? 3 : 12;
        uint16_t pair = (op == 0x7A || op == 0x7B) ? s->de : s->bc;
        if ((s->pm2 & bit) == bit) { r->a = PPAIR_BYTE(pair, op == 0x7A || op == 0x78); r->aknown = 1; }
        else r->aknown = 0;
        return;
    }
    case 0x47: case 0x4F: case 0x57: case 0x5F: { // ld b|c|d|e,a
        if (!r->aknown) { s->pm2 &= (uint8_t)~(op == 0x47 ? 8 : op == 0x4F ? 4 : op == 0x57 ? 2 : 1); return; }
        uint8_t byte = r->a;
        if (op == 0x47) s->bc = (uint16_t)((s->bc & 0x00FF) | ((uint16_t)byte << 8)), s->pm2 |= 8;
        else if (op == 0x4F) s->bc = (uint16_t)((s->bc & 0xFF00) | byte), s->pm2 |= 4;
        else if (op == 0x57) s->de = (uint16_t)((s->de & 0x00FF) | ((uint16_t)byte << 8)), s->pm2 |= 2;
        else s->de = (uint16_t)((s->de & 0xFF00) | byte), s->pm2 |= 1;
        return;
    }
    // ld b|c|d|e, another tracked byte (0x40-0x6F, both registers tracked)
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: // ld b,r
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: // ld c,r
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: // ld d,r
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: // ld e,r
    case 0x61: case 0x63: case 0x64: case 0x65:                       // ld h,r (ld h,h keeps)
    case 0x6A: case 0x6C: case 0x6D: {                                // ld l,r (ld l,l keeps)
        unsigned dst = (op >> 3) & 7, src = op & 7;
        if (src == dst) return;   // ld r,r of the same byte: knowledge unchanged
        // the source byte's knowledge and value; (hl) sources fall through to the kill
        int sknown = 0; uint8_t sbyte = 0;
        if (src == 7 && r->aknown) { sknown = 1; sbyte = r->a; }
        else if (src == 4 && (r->pmask & 2)) { sknown = 1; sbyte = (uint8_t)(r->p >> 8); }
        else if (src == 5 && (r->pmask & 1)) { sknown = 1; sbyte = (uint8_t)r->p; }
        else if (src == 0 && (s->pm2 & 8)) { sknown = 1; sbyte = (uint8_t)(s->bc >> 8); }
        else if (src == 1 && (s->pm2 & 4)) { sknown = 1; sbyte = (uint8_t)s->bc; }
        else if (src == 2 && (s->pm2 & 2)) { sknown = 1; sbyte = (uint8_t)(s->de >> 8); }
        else if (src == 3 && (s->pm2 & 1)) { sknown = 1; sbyte = (uint8_t)s->de; }
        else if (src == dst) { sknown = 1; }   // ld b,b: keeps whatever B was
        if (dst == 0 || dst == 1) {            // ld b,src / ld c,src
            uint16_t bit = dst ? 4 : 8;
            if (!sknown) s->pm2 &= (uint8_t)~bit;
            else {
                if (dst) s->bc = (uint16_t)((s->bc & 0xFF00) | sbyte), s->pm2 |= 4;
                else s->bc = (uint16_t)((s->bc & 0x00FF) | ((uint16_t)sbyte << 8)), s->pm2 |= 8;
            }
        } else if (dst == 2 || dst == 3) {     // ld d,src / ld e,src
            uint16_t bit = dst == 2 ? 2 : 1;
            if (!sknown) s->pm2 &= (uint8_t)~bit;
            else {
                if (dst == 2) s->de = (uint16_t)((s->de & 0x00FF) | ((uint16_t)sbyte << 8)), s->pm2 |= 2;
                else s->de = (uint16_t)((s->de & 0xFF00) | sbyte), s->pm2 |= 1;
            }
        } else if ((dst == 4 || dst == 5) && sknown) { // ld h|l, a tracked byte (the ISA effect
            // handled h,a and l,a; h,d/b and l,e/c arrive here with sknown)
            if (dst == 4) { r->p = (uint16_t)((r->p & 0x00FF) | ((uint16_t)sbyte << 8)); r->pmask |= 2; }
            else { r->p = (uint16_t)((r->p & 0xFF00) | sbyte); r->pmask |= 1; }
        }
        return;
    }
    case 0x09: { // add hl,bc
        if (old_mask == 3 && (s->pm2 & 12) == 12) { r->p = (uint16_t)(old_hl + s->bc); r->pmask = 3; }
        return;
    }
    case 0x19: { // add hl,de
        if (old_mask == 3 && (s->pm2 & 3) == 3) { r->p = (uint16_t)(old_hl + s->de); r->pmask = 3; }
        return;
    }
    case 0x62: case 0x60: { // ld h,d / ld h,b
        int bit = op == 0x62 ? 2 : 8;
        uint16_t pair = op == 0x62 ? s->de : s->bc;
        if ((s->pm2 & bit) == bit) { r->p = (uint16_t)((old_hl & 0x00FF) | (pair & 0xFF00)); r->pmask = old_mask | 2; }
        return;
    }
    case 0x6B: case 0x69: { // ld l,e / ld l,c
        int bit = op == 0x6B ? 1 : 4;
        uint16_t pair = op == 0x6B ? s->de : s->bc;
        if ((s->pm2 & bit) == bit) { r->p = (uint16_t)((old_hl & 0xFF00) | (pair & 0x00FF)); r->pmask = old_mask | 1; }
        return;
    }
    default: {
        // which of B/C/D/E does this opcode write?  bit3 B, bit2 C, bit1 D,
        // bit0 E — everything else keeps its knowledge (never a stale value)
        unsigned wr = 0;
        if (op == 0xCB) wr = 0xF;                       // the byte written depends on b[1]
        else if (op == 0xC1) wr = 0xC;                  // pop bc
        else if (op == 0xD1) wr = 0x3;                  // pop de
        else if (op >= 0x40 && op <= 0x7F) {            // ld r,r' / ld r,(hl) / ld (hl),r / halt
            unsigned dst = (op >> 3) & 7;
            if (op != 0x76) wr = dst == 0 ? 8 : dst == 1 ? 4 : dst == 2 ? 2 : dst == 3 ? 1 : 0;
        } else if (op >= 0x04 && op <= 0x3D && (op & 7) < 4 && (op & 7) != 0) {
            // inc/dec r on b/c/d/e (0x04-0x1F zone; a/h/l destinations handled above/by the ISA)
            unsigned dst = (op >> 3) & 7;
            if (dst <= 3) wr = dst == 0 ? 8 : dst == 1 ? 4 : dst == 2 ? 2 : 1;
        } else if ((op & 0xC7) == 0x06) {               // ld r,n
            unsigned dst = (op >> 3) & 7;
            wr = dst == 0 ? 8 : dst == 1 ? 4 : dst == 2 ? 2 : dst == 3 ? 1 : 0;
        }
        s->pm2 &= (uint8_t)~wr;
        return;
    }
    }
    #undef PPAIR_BYTE
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

// inline-table dispatchers (SML2's `rst $28` and `call` equivalents): the routine pops its return address,
// indexes the table that follows the call site and jumps.  Recognised by shape: a `pop hl` then an
// `add hl,..` before the first `jp hl`, no return or absolute jump in between.
static int prov_is_dispatcher(const DpCart *c, uint32_t key) {
    if (key == ~0u || key + 24 > c->rom_size) return 0;
    int pop = 0, add = 0;
    for (uint32_t i = key; i < key + 24; i++) {
        uint8_t op = c->rom[i];
        if (op == 0xC9 || op == 0xD9 || op == 0xC3) return 0;
        if (op == 0xE1) pop = 1;
        else if (pop && (op == 0x19 || op == 0x29)) add = 1;
        else if (op == 0xE9) return pop && add;
    }
    return 0;
}

// Runtime-built dispatch tables: a verbatim ROM->WRAM copy whose source is a run of code pointers.  The
// bytes land in RAM and are indexed there, so no static `jp hl` idiom sees them.  Proof by co-membership:
// at least 3 consecutive words are plausible code entries and at least one of them is already a known
// executed entry, so the copied block is a code-pointer table and its other entries are code too.
static uint32_t prov_copied_tables(DpResults *res) {
    const DpCart *c = res->cart;
    uint32_t got = 0;
    for (int ci = 0; ci < res->copy_count; ci++) {
        const DpCopyRec *r = &res->copies[ci];
        if (r->src_space != DP_SPACE_ROM || !r->verbatim || r->dst_space != DP_SPACE_WRAM) continue;
        uint32_t len = (uint32_t)r->src_hi - r->src_lo + 1;
        if (r->src_hi < r->src_lo || len < 6 || len > 512) continue;
        uint32_t bank = r->src_bank;
        uint32_t base = r->src_lo;
        uint32_t tkey = base < 0x4000 ? base : bank * 0x4000 + (base - 0x4000);
        if (base >= 0x8000 || tkey + len > c->rom_size) continue;
        for (int par = 0; par < 2; par++) { // the table may start on the second byte of the block
            uint32_t tk[256];
            int nt = 0, anchored = 0;
            for (uint32_t o = (uint32_t)par; o + 1 < len && nt < 256; o += 2) {
                uint32_t t = c->rom[tkey + o] | (c->rom[tkey + o + 1] << 8);
                uint32_t k = t < 0x8000 ? prov_target(res, bank, t) : ~0u;
                if (k == ~0u || !prov_legal_op(c->rom[k]) || c->rom[k] == 0x00 || c->rom[k] == 0xFF) break;
                tk[nt++] = k;
                if (res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED) && res->exec_flags[k] & DP_XF_START) anchored = 1;
            }
            if (nt < 3 || !anchored) continue;
            for (int i = 0; i < nt; i++) {
                uint8_t *f = &res->exec_flags[tk[i]];
                if (!(*f & DP_XF_NATURAL)) {
                    if (!(*f & DP_XF_STATIC)) got++;
                    *f |= DP_XF_STATIC | DP_XF_START | DP_XF_ENTRY;
                    dp_cov_mark(&res->first, res->src_flags, tk[i], DP_SRC_PROVENANCE, res->steps_total, 0);
                }
            }
            for (uint32_t o = (uint32_t)par; o < (uint32_t)par + 2u * (uint32_t)nt; o++) {
                res->data_flags[tkey + o] |= DP_DF_STATIC;
                dp_cov_mark(&res->first, res->src_flags, tkey + o, DP_SRC_PROVENANCE, res->steps_total, 0);
            }
            res->prov_data += 2u * (uint32_t)nt;
            break;
        }
    }
    res->prov_table_targets += got;
    return got;
}

uint32_t dp_provenance(DpResults *res) {
    const DpCart *c = res->cart;
    size_t n = (size_t)res->banks * 0x4000;
    DpHashSet vis;
    ProvStack st = {0};
    uint32_t marked = 0;
    dp_set_init(&vis, 1 << 16);
    marked += prov_copied_tables(res);
    for (size_t k = 0; k < n && k + 3 <= c->rom_size; k++) { // inline tables after each dispatcher call site
        uint8_t op = c->rom[k];
        int isrst = (op & 0xC7) == 0xC7;
        if (op != 0xCD && !isrst) continue;
        if (!(res->exec_flags[k] & DP_XF_START) || !(res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED | DP_XF_STATIC))) continue;
        uint32_t bank = (uint32_t)(k / 0x4000);
        uint32_t t = isrst ? (uint32_t)(op & 0x38) : (uint32_t)(c->rom[k + 1] | (c->rom[k + 2] << 8));
        if (t >= 0x8000 || !prov_is_dispatcher(c, prov_target(res, bank, t))) continue;
        uint32_t base = (bank ? 0x4000 + (uint32_t)(k % 0x4000) : (uint32_t)k) + (isrst ? 1u : 3u);
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
        // byte-granular pairs: the values join the key with their own masks
        // (a collision only prunes a state, never invents one)
        vk ^= (((uint64_t)(s.pm2 & 3 ? s.de : 0) << 16 | (s.pm2 & 12 ? s.bc : 0)) * 4 + s.pm2) * 0x9E3779B97F4A7C15ull << 20;
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
