// closure.c: static flow closure.  The fork explorer proves code by running
// it; its budgets (path steps, global wall, fork caps, queue) leave direct
// control flow unvisited.  This pass follows that flow without a machine:
// from every executed instruction it walks fall-through, JR/JP/CALL/RST
// targets and both sides of every conditional, and marks what it reaches
// MS_XF_STATIC.  Nothing executed is touched; padding, pointer tables and
// proven data stop the walk.  The visited maps are ROM-sized, so it always
// terminates; what it cannot resolve (bank-0 edge into the $4000 window with
// no bank evidence, JP HL) is counted, never guessed.
#include "explorer.h"
#include "../cpu/sm83/sm83.h"

#include <stdlib.h>
#include <string.h>

#define NO_HINT 0xFFFFu

typedef struct { uint32_t key; uint16_t hint; } Item;
typedef struct { Item *v; size_t n, cap; } Stack;

static void push(Stack *s, uint32_t key, uint16_t hint) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4096;
        s->v = realloc(s->v, s->cap * sizeof(Item));
        if (!s->v) abort();
    }
    s->v[s->n].key = key;
    s->v[s->n].hint = hint;
    s->n++;
}

static int illegal(uint8_t op) {
    switch (op) {
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4:
    case 0xEB: case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
        return 1;
    default:
        return 0;
    }
}

// the bank the $4000 window holds right after reset
static uint16_t boot_bank(const MsResults *res) {
    if (res->cart->mbc == MS_MBC5) return 0;
    return res->banks > 1 ? 1 : 0;
}

static void entry_add(MsResults *res, uint32_t key) {
    if (res->clo_entry_n == res->clo_entry_cap) {
        res->clo_entry_cap = res->clo_entry_cap ? res->clo_entry_cap * 2 : 1024;
        res->clo_entries = realloc(res->clo_entries, (size_t)res->clo_entry_cap * sizeof(uint32_t));
        if (!res->clo_entries) abort();
    }
    res->clo_entries[res->clo_entry_n++] = key;
}

uint32_t ms_static_closure(MsResults *res) {
    const MsCart *c = res->cart;
    size_t n = (size_t)res->banks * 0x4000;
    uint8_t *vis = calloc(n, 1);   // banked code (and bank-0 code with no hint)
    MsHashSet vis0;                // bank-0 code per bank hint
    MsHashSet listed;              // entries already recorded
    Stack st = {0};
    uint32_t marked = 0;
    if (!vis) return 0;
    ms_set_init(&vis0, 1 << 24);
    ms_set_init(&listed, 1 << 22);
    res->closure_rounds++;
    res->clo_entry_n = 0;
    // per-round counters are cumulative over rounds only for the totals the
    // report shows; the stop counters restart so a round never double counts
    res->closure_unresolved_bank = res->closure_stopped_data = res->closure_stopped_illegal = 0;

    for (size_t k = 0; k < n; k++) {
        if (!(res->exec_flags[k] & MS_XF_START)) continue;
        if (k >= 0x4000) { push(&st, (uint32_t)k, NO_HINT); continue; }
        uint32_t sid1 = res->section_of[k];
        if (sid1) {
            const MsSection *s = &res->sections[sid1 - 1];
            for (int i = 0; i < s->banks_seen_n; i++)
                push(&st, (uint32_t)k, (uint16_t)s->banks_seen[i]);
        }
        push(&st, (uint32_t)k, boot_bank(res)); // the window before any switch
    }

    while (st.n) {
        Item it = st.v[--st.n];
        uint32_t k = it.key;
        uint32_t bank = k / 0x4000, off = k % 0x4000;
        if (bank == 0) {
            if (ms_set_insert(&vis0, ((uint64_t)k << 16) | it.hint) <= 0) continue;
        } else {
            if (vis[k]) continue;
            vis[k] = 1;
        }
        uint32_t pc = bank ? 0x4000 + off : off;
        if (k >= c->rom_size) continue;
        uint8_t op = c->rom[k];
        int len = sm83_len(op);
        if (k + (uint32_t)len > c->rom_size) continue;
        int executed = (res->exec_flags[k] & (MS_XF_NATURAL | MS_XF_FORCED)) != 0;
        if (!executed) {
            if (illegal(op)) { res->closure_stopped_illegal++; continue; }
            if ((res->static_flags[k] & (MS_SF_PAD | MS_SF_POINTER)) ||
                ((res->data_flags[k] & MS_DF_VERBATIM) && !(res->exec_flags[k] & MS_XF_NATURAL))) {
                res->closure_stopped_data++;
                continue;
            }
            int cross = 0;
            for (int i = 0; i < len; i++) {
                if (bank == 0 ? off + i >= 0x4000 : pc + i >= 0x8000) { cross = 1; break; }
            }
            if (cross) continue;
            for (int i = 0; i < len; i++) {
                uint8_t *f = &res->exec_flags[k + i];
                if (!(*f & (MS_XF_NATURAL | MS_XF_FORCED | MS_XF_STATIC))) marked++;
                *f |= MS_XF_STATIC;
            }
            res->exec_flags[k] |= MS_XF_START;
        }

        // successors
        int target = -1;          // absolute address of a direct transfer
        int fall = 1;
        switch (op) {
        case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
            target = (int)((pc + 2 + (int8_t)c->rom[k + 1]) & 0xFFFF);
            fall = op != 0x18;
            break;
        case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        case 0xCD: case 0xC4: case 0xCC: case 0xD4: case 0xDC:
            target = c->rom[k + 1] | (c->rom[k + 2] << 8);
            fall = op != 0xC3;
            break;
        case 0xC9: case 0xD9: case 0xE9:
            fall = 0;
            break;
        default:
            if ((op & 0xC7) == 0xC7) target = op & 0x38; // RST
            break;
        }
        if (target >= 0) {
            uint32_t tk = ~0u;
            uint16_t thint = it.hint;
            if (target < 0x4000) {
                tk = (uint32_t)target;
            } else if (target < 0x8000) {
                uint32_t tb = bank ? bank : it.hint;
                if (tb == NO_HINT) {
                    res->closure_unresolved_bank++;
                } else {
                    if (tb >= (uint32_t)res->banks) tb %= (uint32_t)res->banks;
                    tk = tb * 0x4000 + (uint32_t)(target & 0x3FFF);
                    thint = NO_HINT;
                }
            } // RAM targets: not ROM map information
            if (tk != ~0u && tk < n) {
                if (!(res->exec_flags[tk] & (MS_XF_NATURAL | MS_XF_FORCED))) {
                    res->exec_flags[tk] |= MS_XF_ENTRY;
                    if (ms_set_insert(&listed, tk) > 0) entry_add(res, tk);
                }
                push(&st, tk, tk >= 0x4000 ? NO_HINT : thint);
            }
        }
        if (fall) {
            uint32_t npc = pc + (uint32_t)len;
            if (bank == 0 ? off + (uint32_t)len < 0x4000 : npc < 0x8000)
                push(&st, k + (uint32_t)len, it.hint);
            else if (bank == 0)
                res->closure_unresolved_bank++;
        }
    }

    res->closure_bytes += marked;
    res->closure_starts = 0;
    for (size_t k = 0; k < n; k++)
        if ((res->exec_flags[k] & MS_XF_STATIC) && (res->exec_flags[k] & MS_XF_START)) res->closure_starts++;
    free(st.v);
    free(vis);
    ms_set_free(&vis0);
    ms_set_free(&listed);
    return marked;
}

int ms_closure_seed(MsExplorer *ex) {
    MsResults *res = ex->res;
    int seeded = 0;
    for (int i = 0; i < res->clo_entry_n; i++) {
        uint32_t k = res->clo_entries[i];
        if (res->exec_flags[k] & (MS_XF_NATURAL | MS_XF_FORCED)) continue;
        if ((uint32_t)seeded >= ex->cfg.seed_cap) { // saturation is reported, not silent
            res->closure_seeds_dropped++;
            continue;
        }
        if (ms_set_insert(&res->clo_seeded, k) <= 0) continue;

        uint32_t bank = k / 0x4000, off = k % 0x4000;
        ms_explorer_seed_pc(ex, (int)bank, (uint16_t)(bank ? 0x4000 + off : off), MS_SEED_POINTER, 1);
        seeded++;
    }
    res->closure_seeds += (uint32_t)seeded;
    return seeded;
}
