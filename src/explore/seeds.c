// seeds.c: GB entry seeds (reset, RST/IRQ vectors, pointer tables) and the
// indirect-jump cascade.
#include "explorer.h"
#include "explorer_priv.h"
#include "../core/dpmachine.h"
#include "tree.h"
#include <string.h>
#include <stdlib.h>

int is_padding_at(const DpCart *c, uint32_t bank, uint16_t addr) {
    uint32_t base = bank * 0x4000ull + (addr & 0x3FFF);
    uint8_t b0 = base < c->rom_size ? c->rom[base] : 0xFF;
    if (b0 != 0x00 && b0 != 0xFF) return 0;
    for (int i = 1; i < 8; i++) {
        uint32_t a = base + (uint32_t)i;
        uint8_t b = a < c->rom_size ? c->rom[a] : 0xFF;
        if (b != b0) return 0;
    }
    return 1;
}

int is_illegal_op(uint8_t op) {
    switch (op) {
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4:
    case 0xEB: case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
        return 1;
    default:
        return 0;
    }
}

void dp_seed_gb(DpExplorer *ex) {
    DpResults *res = ex->res;
    const DpCart *c = res->cart;
    const DpPlatform *plat = res->plat;
    // reset entry
    {
        uint8_t *img = xrealloc(NULL, ex->sb);
        memcpy(img, seed_base(ex), ex->sb);
        dp_explorer_seed(ex, img, DP_SEED_RESET, 0);
    }
    // RST and IRQ vectors when they hold something that is not padding.  Both
    // are entered cold, from a synthetic state no natural flow produced: the
    // code they reach is forced until the reset flow (or a session) gets
    // there for real
    for (int i = 0; i < plat->entry_count; i++) {
        const DpEntry *e = &plat->entries[i];
        if (e->kind == DP_SEED_RESET) continue;
        if (is_padding_at(c, 0, e->addr)) continue;
        dp_explorer_seed_pc(ex, 0, e->addr, e->kind, 1);
    }
    // pointer-table scan: every ROM word pointing at plausible code; targets
    // inside padding or pointer tables are evicted (static view — the
    // dynamic data proof only exists once coverage ran, see --sweep).
    // A word >= $4000 carries no bank evidence: every switchable bank's view
    // of the target is seeded (bounded by the seed cap, dedup by rom key).
    if (!ex->cfg.no_pointer_scan) {
        int seeded = 0;
        DpHashSet seen;
        dp_set_init(&seen, 65536);
        for (int bank = 0; bank < res->banks; bank++) {
            for (int off = 0; off + 1 < 0x4000; off++) {
                uint32_t a = (uint32_t)bank * 0x4000 + (uint32_t)off;
                if (a + 1 >= c->rom_size) break;
                uint16_t v = (uint16_t)(c->rom[a] | (c->rom[a + 1] << 8));
                if (v < 0x0100 || v >= 0x8000) continue;
                // a word flanked by two other $4000-window words is a
                // dispatch-table entry: its true bank is unknown, so every
                // switchable bank's view is seeded.  An isolated word is a
                // plain pointer: bank 1, as before.
                uint16_t pv = a >= 2 ? (uint16_t)(c->rom[a - 2] | (c->rom[a - 1] << 8)) : 0;
                uint16_t nv = a + 3 < c->rom_size
                                  ? (uint16_t)(c->rom[a + 2] | (c->rom[a + 3] << 8))
                                  : 0;
                int tab = pv >= 0x4000 && pv < 0x8000 && nv >= 0x4000 && nv < 0x8000;
                int nb = v < 0x4000 ? 1 : (tab ? res->banks : 2);
                for (int tbank = v < 0x4000 ? 0 : 1; tbank < nb; tbank++) {
                    if (is_padding_at(c, (uint32_t)tbank, v)) continue;
                    uint32_t tk = dp_rom_index(tbank, v);
                    if (res->static_flags[tk] & (DP_SF_PAD | DP_SF_POINTER)) {
                        res->seeds_skipped_data++;
                        continue;
                    }
                    if (dp_set_insert(&seen, tk) == 0) continue;
                    if (seeded >= (int)ex->cfg.seed_cap) { // the cap holds the seed back, never drops it
                        pseed_add(ex, tbank, v, DP_SEED_POINTER, 1);
                        continue;
                    }
                    dp_explorer_seed_pc(ex, tbank, v, DP_SEED_POINTER, 1);
                    seeded++;
                }
            }
        }
        dp_set_free(&seen);
    }
    // bank sweep seeding: every switchable bank entered at $4000
    if (ex->cfg.seed_banks) {
        for (int bank = 1; bank < res->banks; bank++) {
            if (is_padding_at(c, (uint32_t)bank, 0x4000)) continue;
            dp_explorer_seed_pc(ex, bank, 0x4000, DP_SEED_POINTER, 1);
        }
    }
}

// the JP HL sites recorded the targets the dispatch actually took; some of
// those continuations were dropped by a budget wall, so seed every target
// whose rom key carries no execution evidence yet (bounded: sites x 8, seed cap)
int dp_seed_indirect(DpExplorer *ex, uint32_t cap) {
    DpResults *res = ex->res;
    int seeded = 0;
    for (size_t i = 0; i < res->indirect.cap; i++) {
        if (!res->indirect.used[i]) continue;
        const DpIndirect *ind = (const DpIndirect *)(res->indirect.vals + i * res->indirect.stride);
        for (int t = 0; t < ind->n; t++) {
            uint32_t k = dp_rom_index(ind->tbank[t], ind->target[t]);
            if (res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED)) continue;
            if (res->static_flags[k] & (DP_SF_PAD | DP_SF_POINTER)) continue;
            if (seeded >= (int)cap) { // the phase's solde is spent: counted, held for later
                res->seeds_refused_budget++;
                pseed_add(ex, ind->tbank[t], ind->target[t], DP_SEED_POINTER, 1);
                continue;
            }
            if (!dp_explorer_seed_pc_pri(ex, ind->tbank[t], ind->target[t], DP_SEED_POINTER, 1, 1))
                continue;
            seeded++;
            res->indirect_seeds++;
        }
    }
    return seeded;
}
