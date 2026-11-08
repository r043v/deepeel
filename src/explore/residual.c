// residual.c: targeted re-seeding of the residual unknown (plan de finition,
// étape C).  The residual anatomy names what blocks each unknown range; this
// module acts on the dispatch-shaped ones:
//   - predecessor: the nearest executed `jp hl` before the range — the
//     dispatch site responsible for the code the closure never resolved.  Its
//     operand table is re-proven with the strengthened acceptance (stride 2/3,
//     target must decode to a transfer); a validated table marks its entries
//     and proves its bytes data, exactly like the closure's S4.
//   - forced entry: when the range starts right after an unconditional
//     transfer (ret/jp/jr — the byte before is executed), its start is a
//     plausible entry point: a forced cold seed enters it, the machine
//     disposes.  A range whose start a known pointer word already aims at is
//     refused (the pointer scan seeds it — counting it again would be noise).
// Every range is tried once (the tried set survives rounds); the caps bound
// the work, every refusal is counted — never silent.
#include "explorer.h"
#include "../report/report.h"
#include "../cpu/sm83/sm83.h"
#include "../core/dpmachine.h"

#include <stdlib.h>
#include <string.h>

#define RESIDUAL_LOOKBACK 2048 // predecessor search window behind a range
#define RESIDUAL_TABLE_LOOKBACK 24 // ld hl,nn within this many bytes of jp hl (S4 idiom)

static int illegal(uint8_t op) { return dp_isa_sm83()->illegal(op); }

// an unconditional transfer (ret/reti/jr/jp) whose instruction ENDS exactly at
// tk — the byte before a range may be the last operand of a 2/3-byte transfer
static int ends_with_transfer(const DpCart *c, uint32_t tk) {
    if (tk >= 1) { uint8_t op = c->rom[tk - 1]; if (op == 0xC9 || op == 0xD9 || op == 0xE9) return 1; }
    if (tk >= 2) { if (c->rom[tk - 2] == 0x18) return 1; }
    if (tk >= 3) { if (c->rom[tk - 3] == 0xC3) return 1; }
    return 0;
}

// stride-2/3 operand-table proof of one dispatch site (the closure's S4,
// re-run for a range whose entries stayed unknown): entries keep legal starts,
// an unproven target must decode to a transfer; at least two entries validate
// a table (one entry proves nothing about a dispatch).  Marks the validated
// entries and proves the table bytes data.  Returns entries marked.
static uint32_t prove_table(DpResults *res, uint32_t site, uint32_t steps_total) {
    const DpCart *c = res->cart;
    size_t n = c->rom_size;
    // the site's own bank view (site key is compact bank*0x4000 + off)
    uint32_t bank = site / 0x4000;
    // look back for the executed `ld hl,nn` (or `ld de/bc,nn` + add hl)
    uint32_t base = ~0u;
    int found = 0;
    for (uint32_t b = site - 1; b + RESIDUAL_TABLE_LOOKBACK > site && b < site; b--) {
        if (b / 0x4000 != bank) break;
        uint8_t lop = c->rom[b];
        int base_ok = lop == 0x21;
        if (lop == 0x11 || lop == 0x01) {
            uint8_t addop = lop == 0x11 ? 0x19 : 0x09;
            for (uint32_t q = b + 3; q < site; q++)
                if (c->rom[q] == addop && (res->exec_flags[q] & DP_XF_START)) { base_ok = 1; break; }
        }
        if (!base_ok || !(res->exec_flags[b] & DP_XF_START)) { if (b == 0) break; continue; }
        base = (uint32_t)(c->rom[b + 1] | c->rom[b + 2] << 8);
        found = 1;
        break;
    }
    if (!found) return 0;
    uint32_t tb = bank;
    uint32_t bk = ~0u;
    if (base < 0x4000) bk = base;
    else if (base < 0x8000 && tb < (uint32_t)res->banks) bk = tb * 0x4000 + (base & 0x3FFF);
    if (bk == ~0u || bk + 6 > n) return 0;
    uint32_t marked = 0;
    for (int stride = 2; stride <= 3; stride++) {
        int cnt = 0;
        for (int e = 0; e < 64 && bk + (uint32_t)e * (uint32_t)stride + (stride > 2 ? 2 : 1) < n; e++) {
            uint16_t t = (uint16_t)(c->rom[bk + e * (uint32_t)stride] | c->rom[bk + e * (uint32_t)stride + 1] << 8);
            uint32_t ebk = tb;
            if (stride == 3) {
                uint8_t eb = c->rom[bk + e * 3 + 2];
                if (eb >= (uint32_t)res->banks) break;
                ebk = eb;
            }
            if (t < 0x150 || t >= 0x8000) break;
            uint32_t tk = ~0u;
            if (stride == 3) {
                if (ebk < (uint32_t)res->banks) tk = ebk * 0x4000 + (t & 0x3FFF);
            } else if (t < 0x4000) tk = t;
            else if (ebk < (uint32_t)res->banks) tk = ebk * 0x4000 + (t & 0x3FFF);
            if (tk == ~0u || tk >= n || (res->static_flags[tk] & DP_SF_PAD)) break;
            if (illegal(c->rom[tk])) break;
            if (!(res->exec_flags[tk] & (DP_XF_NATURAL | DP_XF_FORCED)) &&
                !dp_closure_decodes_transfer(c, tk, res->banks)) break;
            cnt++;
            if (!(res->exec_flags[tk] & (DP_XF_NATURAL | DP_XF_FORCED | DP_XF_STATIC))) {
                res->exec_flags[tk] |= DP_XF_ENTRY;
                dp_cov_mark(&res->first, res->src_flags, tk, DP_SRC_CLOSURE, steps_total, 0);
                marked++;
            }
        }
        if (cnt >= 2) { // a real table: entries code, its bytes proven data
            res->closure_jp_tables++;
            res->residual_static++;
            uint32_t tbytes = bk + (uint32_t)cnt * (uint32_t)stride;
            if (tbytes > n) tbytes = (uint32_t)n;
            for (uint32_t wb = bk; wb < tbytes; wb++)
                if (!(res->data_flags[wb] & DP_DF_STATIC)) {
                    res->data_flags[wb] |= DP_DF_STATIC;
                    res->prov_data++;
                }
            return marked;
        }
    }
    return 0;
}

uint32_t dp_residual_target(DpExplorer *ex, uint32_t cap) {
    DpResults *res = ex->res;
    const DpCart *c = res->cart;
    DpMapSummary sum;
    uint8_t *m = dp_map_build(res, &sum);
    if (!m) return 0;
    DpResidualRange rr[128];
    int nr = dp_map_residual_list(res, m, rr, 128); // returns the TOTAL range count
    if (nr > 128) nr = 128; // only cap entries were actually filled
    uint32_t seeded = 0;
    for (int i = 0; i < nr; i++) {
        DpResidualRange *r = &rr[i];
        const char *mk = dp_residual_mechanism(m, r);
        int fringe_code = !strcmp(mk, "padding-fringe") && r->lo > 0 &&
                          dp_map_is_code(m[(size_t)r->bank * 0x4000 + r->lo - 1]);
        if (strcmp(mk, "code-code") && strcmp(mk, "edge-code") && !fringe_code) continue;
        if (res->residual_targets >= cap) {
            res->residual_refused++;
            break; // budget spent: the remaining ranges stay untried, reported
        }
        uint32_t tk = (uint32_t)r->bank * 0x4000 + r->lo;
        res->residual_targets++;
        // the range is marked tried up front: a verdict (seeded or refused,
        // even a transient drop) is final — no reloop (plan: échec marqué)
        if (!dp_set_insert(&res->residual_tried, tk)) continue;
        // predecessor: the nearest executed dispatch site behind the range
        uint32_t site = ~0u;
        uint32_t from = (uint32_t)r->bank * 0x4000 + r->lo;
        for (uint32_t k = from; k-- > 0 && from - k <= RESIDUAL_LOOKBACK;) {
            if (k % 0x4000 == 0 && k != from) break; // bank edge: the range's own bank only
            if ((res->exec_flags[k] & (DP_XF_START | DP_XF_NATURAL | DP_XF_FORCED)) ==
                (DP_XF_START | DP_XF_NATURAL) && c->rom[k] == 0xE9) { site = k; break; }
        }
        // predecessor dispatch re-proof: its entries get marked, never seeded here
        if (site != ~0u) prove_table(res, site, res->steps_total);
        // forced entry: plausible when the byte before the range is an
        // executed unconditional transfer (ret/reti/jp/jr) — the range then
        // starts an unreached sibling routine
        uint8_t prev = tk && (tk % 0x4000 || r->bank) ? c->rom[tk - 1] : 0;
        int plausible = (prev == 0xC9 || prev == 0xD9 || prev == 0xC3 || prev == 0x18 ||
                         (tk % 0x4000 == 0 && r->bank)); // a bank's first byte is always an entry
        if (prev == 0xE9 && (res->exec_flags[tk - 1] & DP_XF_START)) {
            // the range follows an executed, unproven `jp hl`: presumed its
            // operand table (data), never seeded as code
            res->residual_refused++;
            continue;
        }
        uint8_t op = c->rom[tk];
        if (!plausible && ends_with_transfer(c, tk)) plausible = 1;
        if (!plausible || (res->static_flags[tk] & DP_SF_PAD) || op == 0xFF || illegal(op)) {
            res->residual_refused++; // no plausible entry: marked tried, never retried
            continue;
        }
        if (dp_explorer_seed_pc_pri(ex, r->bank, r->lo, DP_SEED_POINTER, 1, 1) > 0) {
            res->residual_seeded++;
            seeded++;
        } else {
            // dropped (full queue or dedup set): the refusal is counted, the
            // range stays marked tried — no reloop (plan: échec marqué)
            res->residual_refused++;
        }
    }
    free(m);
    return seeded;
}
