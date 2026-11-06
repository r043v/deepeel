// forks.c: the fork sites of the exploration (conditional branches, IRQ,
// key probes, P1 reads): both outcomes are captured as state images.
#include "explorer.h"
#include "explorer_priv.h"
#include "../core/dpmachine.h"
#include "tree.h"
#include <string.h>
#include <stdlib.h>

// --- forking -------------------------------------------------------------------------
// The variant is captured by mutating the live machine through the public
// api (the flag bit, the IF byte), saving, then restoring the natural state:
// three state images per fork, no knowledge of the image layout needed.
// true when both successors of the conditional at pc were already executed:
// the fork can then only add state diversity, never a new byte of code
int both_sides_covered(DpExplorer *ex, uint16_t pc, uint8_t op) {
    DpResults *res = ex->res;
    uint16_t bank = cur_bank(ex, pc);
    uint16_t taken;
    switch (op) {
    case 0x20: case 0x28: case 0x30: case 0x38:
        taken = (uint16_t)(pc + 2 + (int8_t)dp_mach_op(ex->mach, (uint16_t)(pc + 1)));
        break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        taken = (uint16_t)(dp_mach_op(ex->mach, (uint16_t)(pc + 1)) |
                           (dp_mach_op(ex->mach, (uint16_t)(pc + 2)) << 8));
        break;
    default:
        return 0; // RET cc: the taken side depends on the stack
    }
    uint16_t fall = (uint16_t)(pc + dp_isa_sm83()->length(op));
    uint16_t sides[2] = { taken, fall };
    for (int i = 0; i < 2; i++) {
        uint16_t a = sides[i];
        if (a >= 0x8000) return 0;
        uint32_t k = dp_rom_index(a < 0x4000 ? *ex->mach->api->rom0_bank : (a >= 0x4000 && pc < 0x4000 ? ex->mach->api->gbr->rom_bank
                                                                              : bank), a);
        if (k >= (uint32_t)res->banks * 0x4000) return 0;
        uint8_t f = res->exec_flags[k];
        if (!(f & DP_XF_START) || !(f & (DP_XF_NATURAL | DP_XF_FORCED))) return 0;
    }
    return 1;
}

/* per-site fork counter; a full counter table must not lift the cap (the
 * NULL slot used to skip it): the fork is refused and the saturation counted */
static uint32_t *fork_slot(DpExplorer *ex, uint32_t key) {
    uint32_t *cnt = dp_map_get_or_add(&ex->res->fork_counts, key);
    if (!cnt) {
        ex->res->forks_table_full++;
        ex->res->forks_capped++;
    }
    return cnt;
}

void fork_branch(DpExplorer *ex, int cond, uint32_t site, uint16_t pc, uint8_t op) {
    // per-site cap: loop bodies reach their conditional every iteration and
    // the extra variants are data drift, not new map information
    uint32_t *cnt = fork_slot(ex, site);
    if (!cnt) return;
    int defer = 0; // the variant is built anyway and parked under its reason
    if (*cnt >= ex->cfg.fork_site_cap) {
        ex->res->forks_capped++;
        defer = DP_R_FORK_CAP;
    } else if (!ex->cfg.no_covered_prune && *cnt >= ex->cfg.covered_fork_keep &&
               both_sides_covered(ex, pc, op)) {
        ex->res->forks_covered_pruned++;
        defer = DP_R_COVERED;
    }
    // both sides already executed: the variant can only add state diversity, never a byte of code. Under
    // --unbounded the cap is infinite, so parking it would send every such fork to the disk spool (the
    // unbounded growth): it is dropped, counted as final, no snapshot paid
    if (defer == DP_R_COVERED && ex->cfg.unbounded) { ex->res->defer_final[DP_R_COVERED]++; return; }
    // a deferred variant spends the site's counter too: each site parks at most
    // as many variants as its cap again (a loop body cannot flood the frontier)
    if (defer && (*cnt >= 2 * ex->cfg.fork_site_cap || !defer_room(ex, defer))) {
        if (*cnt >= 2 * ex->cfg.fork_site_cap) ex->res->defer_final[defer]++;
        return; // counted final, no snapshot paid
    }
    (*cnt)++;
    DpMach *m = ex->mach;
    uint8_t flag = dp_isa_sm83()->cond_flag(cond); // 0x80 = Z, 0x10 = C
    uint8_t *img = xrealloc(NULL, ex->sb);
    ex_save(ex, m, img);
    if (flag == 0x80) m->api->gbr->zr = m->api->gbr->zr ? 0 : 1; // Z set <=> zr == 0
    else m->api->gbr->cf ^= 1;
    uint8_t *fimg = xrealloc(NULL, ex->sb);
    ex_save(ex, m, fimg);
    ex_load(ex, m, img); // the natural side keeps running
    free(img);
    // the fork keeps the reset lineage of its parent path
    uint16_t tpc = ex->mach->api->gbr->pc;
    if (defer) { fork_deferred(ex, fimg, defer, site, tpc); return; }
    queue_push(ex, fimg, 1, DP_PATH_FORK,
               ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
               choice_new(ex, ex->cur_choice, DP_PATH_FORK, site, (int)cur_bank(ex, tpc), tpc));
    ex->res->forks_branch++;
}

void fork_irq(DpExplorer *ex) {
    const DpGblApi *api = ex->mach->api;
    uint8_t pending = (uint8_t)(*api->ie & api->io[0x0F] & 0x1F);
    if (!pending) return;
    DpMach *m = ex->mach;
    uint8_t *img = xrealloc(NULL, ex->sb);
    ex_save(ex, m, img);
    api->io[0x0F] &= (uint8_t)~pending; // the variant: the IRQ never fires
    uint8_t *fimg = xrealloc(NULL, ex->sb);
    ex_save(ex, m, fimg);
    ex_load(ex, m, img);
    free(img);
    uint16_t tpc = m->api->gbr->pc;
    queue_push(ex, fimg, 1, DP_PATH_FORK,
               ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
               choice_new(ex, ex->cur_choice, DP_PATH_FORK, 0, (int)cur_bank(ex, tpc), tpc));
    ex->res->forks_irq++;
}


/* the key-IRQ variant: the console raises the joypad IRQ when a NEW key is
 * pressed; games that poll keys only through that IRQ see the press through
 * the handler, which the natural path here cannot produce without a player.
 * Each pressed variant is one key the machine currently does not hold,
 * capped per site like branch forks. */
void fork_key(DpExplorer *ex, uint32_t site) {
    const DpGblApi *api = ex->mach->api;
    if (api->io[0x0F] & 0x10) return; // already pending: the natural IRQ fires
    uint32_t *cnt = fork_slot(ex, site ^ 0x80000000u);
    if (!cnt) return;
    if (ex->cfg.unbounded && *cnt >= 1) { ex->res->defer_final[DP_R_COVERED]++; return; }   // one round of presses per site
    // once the joypad handler ($60) has run, more key variants at a site only
    // re-run known code: one round of presses per site is enough
    uint32_t kcap = (ex->res->exec_flags[0x60] & DP_XF_START) ? 1 : ex->cfg.fork_site_cap;
    if (ex->cfg.no_covered_prune) kcap = ex->cfg.fork_site_cap;
    int kdefer = 0;
    if (*cnt >= kcap) {
        ex->res->forks_capped++;
        if (*cnt >= 2 * kcap) { ex->res->defer_final[DP_R_FORK_CAP]++; return; }
        if (!defer_room(ex, DP_R_FORK_CAP)) return;
        kdefer = DP_R_FORK_CAP;
    }
    (*cnt)++;
    static const uint8_t keys[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    // the keys the machine already holds, read back from P1 (bits low = pressed)
    DpMach *m = ex->mach;
    uint8_t *img = xrealloc(NULL, ex->sb);
    ex_save(ex, m, img);
    // the fork explorer never holds keys, so every key press is "new":
    // set_buttons raises IF bit 4, which is exactly the console behaviour
    for (int k = 0; k < 8; k++) {
        if (k) { ex_load(ex, m, img); }
        m->api->set_buttons(keys[k]);
        if (api->io[0x0F] & 0x10) { // this key raised the joypad IRQ
            uint8_t *fimg = xrealloc(NULL, ex->sb);
            ex_save(ex, m, fimg);
            uint16_t tpc = m->api->gbr->pc;
            if (kdefer) { fork_deferred(ex, fimg, kdefer, site, tpc); continue; }
            queue_push(ex, fimg, 1, DP_PATH_FORK,
                       ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
                       choice_new(ex, ex->cur_choice, DP_PATH_FORK, site,
                                  (int)cur_bank(ex, tpc), tpc));
            ex->res->forks_key++;
        }
    }
    ex_load(ex, m, img); // the natural side continues, untouched
    m->api->set_buttons(0); // recompute the derived P1 register (not in the image)
    free(img);
}

/* the P1-poll variant: games that read $FF00 directly (polling, no joypad
 * IRQ armed) never see a key pressed on this explorer — the buttons stay
 * released for the whole run.  Each variant holds one key the machine is
 * not pressing; the joypad IRQ the console would raise at the press is
 * cleared in the variant so only the polled VALUE differs (the IRQ path
 * already has its own forks).  Capped per site like the branch forks. */
void fork_p1(DpExplorer *ex, uint32_t site, uint16_t pc, uint8_t op) {
    const DpGblApi *api = ex->mach->api;
    if (op == 0xF0) { // ldh a,(n): only $FF00 is the joypad
        if (dp_mach_op(ex->mach, (uint16_t)(pc + 1)) != 0x00) return;
    } else if (api->gbr->c != 0x00) { // ld a,(C): P1 only when C == $FF00
        return;
    }
    uint32_t *cnt = fork_slot(ex, site ^ 0xC0000000u);
    if (!cnt) return;
    if (ex->cfg.unbounded && *cnt >= 1) { ex->res->defer_final[DP_R_COVERED]++; return; }   // one round of keys per polling site
    int pdefer = 0;
    if (*cnt >= ex->cfg.fork_site_cap) {
        ex->res->forks_capped++;
        if (*cnt >= 2 * ex->cfg.fork_site_cap) { ex->res->defer_final[DP_R_FORK_CAP]++; return; }
        if (!defer_room(ex, DP_R_FORK_CAP)) return;
        pdefer = DP_R_FORK_CAP;
    }
    (*cnt)++;
    static const uint8_t keys[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    DpMach *m = ex->mach;
    uint8_t *img = xrealloc(NULL, ex->sb);
    ex_save(ex, m, img);
    for (int k = 0; k < 8; k++) {
        m->api->set_buttons(keys[k]);
        api->io[0x0F] &= (uint8_t)~0x10; // the value fork: no IRQ side effect
        uint8_t *fimg = xrealloc(NULL, ex->sb);
        ex_save(ex, m, fimg);
        uint16_t tpc = m->api->gbr->pc;
        if (pdefer) { fork_deferred(ex, fimg, pdefer, site, tpc); continue; }
        queue_push(ex, fimg, 1, DP_PATH_FORK,
                   ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
                   choice_new(ex, ex->cur_choice, DP_PATH_FORK, site,
                              (int)cur_bank(ex, tpc), tpc));
        ex->res->forks_p1++;
    }
    ex_load(ex, m, img); // the natural side continues, untouched
    m->api->set_buttons(0); // recompute the derived P1 register (not in the image)
    free(img);
}

/* Event-register read probes (docs/COUVERTURE.fr.md 3.7).  LY, STAT, DIV, TIMA and the serial
 * registers are static for the explorer, so a wait loop on them only leaves through the guards.
 * Before an instruction that reads one of them, each significant value the register is not holding
 * becomes its own forced variant:
 *   LY   : 0, 143, 144, 153, plus the constant the next compare tests (n-1, n, n+1)
 *   STAT : the four PPU modes x the LY=LYC coincidence bit
 *   DIV  : 0x00, 0x40, 0x80, 0xFF, plus the compare constant
 *   TIMA : 0x00, 0x80, 0xFF, plus the compare constant
 *   SC   : transfer done / transfer pending (a serial wait loop with no partner)
 *   SB   : 0x00, 0xFF (what a linked console would have sent)
 * The register is found from the read forms `ldh a,(n)`, `ld a,(C)`, `ld a,(nn)`, `ld r,(hl)`,
 * `ld a,(hl+/-)`, `ld a,(bc/de)`, `cp/and/or/xor (hl)` and `bit n,(hl)` whose address is $FFxx.
 * Capped per site and per register like the P1 forks. */
static int ev_read_reg(const DpGblApi *api, DpMach *m, uint16_t pc, uint8_t op, int *next_len) {
    const gbl_cpu_t *g = api->gbr;
    *next_len = 0;
    switch (op) {
    case 0xF0: *next_len = 2; return dp_mach_op(m, (uint16_t)(pc + 1));      // ldh a,(n)
    case 0xF2: *next_len = 1; return g->c ? g->c : -1;                       // ld a,(C)
    case 0xFA:                                                               // ld a,(nn)
        *next_len = 3;
        return dp_mach_op(m, (uint16_t)(pc + 2)) == 0xFF ? dp_mach_op(m, (uint16_t)(pc + 1)) : -1;
    case 0x7E: case 0x46: case 0x4E: case 0x56: case 0x5E: case 0x6E: case 0x2A: case 0x3A:
    case 0xBE: case 0xA6: case 0xB6: case 0xAE:                              // reads through HL
        *next_len = 1;
        return (g->hl >> 8) == 0xFF ? (int)(g->hl & 0xFF) : -1;
    case 0x0A: *next_len = 1; return (g->bc >> 8) == 0xFF ? (int)(g->bc & 0xFF) : -1; // ld a,(bc)
    case 0x1A: *next_len = 1; return (g->de >> 8) == 0xFF ? (int)(g->de & 0xFF) : -1; // ld a,(de)
    case 0xCB: {                                                             // bit n,(hl)
        uint8_t x = dp_mach_op(m, (uint16_t)(pc + 1));
        if (x >= 0x40 && x < 0x80 && (x & 7) == 6) { *next_len = 2; return (g->hl >> 8) == 0xFF ? (int)(g->hl & 0xFF) : -1; }
        return -1;
    }
    }
    return -1;
}

void fork_event(DpExplorer *ex, uint32_t site, uint16_t pc, uint8_t op) {
    const DpGblApi *api = ex->mach->api;
    int nlen;
    int r = ev_read_reg(api, ex->mach, pc, op, &nlen);
    if (r < 0) return;
    uint8_t reg = (uint8_t)r;
    if (reg != 0x44 && reg != 0x41 && reg != 0x04 && reg != 0x05 && reg != 0x02 && reg != 0x01) return;
    // the constant of the compare that follows the read: the value worth testing is that one +/- 1
    int cmpk = -1;
    {
        uint16_t np = (uint16_t)(pc + nlen);
        uint8_t nop = dp_mach_op(ex->mach, np);
        if (nop == 0xFE || nop == 0xD6) cmpk = dp_mach_op(ex->mach, (uint16_t)(np + 1)); // cp n / sub n
    }
    uint8_t vbuf[12];
    const uint8_t *vals; int n = 0;
    if (reg == 0x44) { static const uint8_t b[] = {0, 143, 144, 153}; memcpy(vbuf, b, 4); n = 4; }
    else if (reg == 0x41) { static const uint8_t b[] = {0, 1, 2, 3, 4, 5, 6, 7}; memcpy(vbuf, b, 8); n = 8; }
    else if (reg == 0x04) { static const uint8_t b[] = {0x00, 0x40, 0x80, 0xFF}; memcpy(vbuf, b, 4); n = 4; }
    else if (reg == 0x05) { static const uint8_t b[] = {0x00, 0x80, 0xFF}; memcpy(vbuf, b, 3); n = 3; }
    else if (reg == 0x02) { vbuf[0] = (uint8_t)(api->io[0x02] & ~0x80); vbuf[1] = (uint8_t)(api->io[0x02] | 0x80); n = 2; }
    else { vbuf[0] = 0x00; vbuf[1] = 0xFF; n = 2; }
    if (cmpk >= 0 && reg != 0x41 && reg != 0x02) {
        int cand[3] = { cmpk - 1, cmpk, cmpk + 1 };
        for (int i = 0; i < 3; i++) {
            if (cand[i] < 0 || cand[i] > 0xFF) continue;
            int dup = 0;
            for (int q = 0; q < n; q++) dup |= vbuf[q] == (uint8_t)cand[i];
            if (!dup && n < 12) vbuf[n++] = (uint8_t)cand[i];
        }
    }
    vals = vbuf;
    // the values worth testing come from constants hard-coded at the site (plus the compare constant that
    // follows): one round per (site, register, constant) is enough, a loop revisiting the read with another
    // context re-runs the same destinations. --unbounded keeps that one round and drops the repeats
    uint32_t *cnt = fork_slot(ex, site ^ 0xE0000000u ^ ((uint32_t)reg << 20) ^ ((uint32_t)(cmpk + 1) * 0x9E3779B1u));
    if (!cnt) return;
    if (ex->cfg.unbounded && *cnt >= 1) { ex->res->defer_final[DP_R_COVERED]++; return; }
    int edefer = 0;
    if (*cnt >= ex->cfg.fork_site_cap) {
        ex->res->forks_capped++;
        if (*cnt >= 2 * ex->cfg.fork_site_cap) { ex->res->defer_final[DP_R_FORK_CAP]++; return; }
        if (!defer_room(ex, DP_R_FORK_CAP)) return;
        edefer = DP_R_FORK_CAP;
    }
    (*cnt)++;
    DpMach *m = ex->mach;
    uint8_t *img = xrealloc(NULL, ex->sb);
    ex_save(ex, m, img);
    uint8_t cur = api->io[reg];
    for (int k = 0; k < n; k++) {
        uint8_t v = reg == 0x41 ? (uint8_t)((cur & ~7) | vals[k]) : vals[k];
        if (v == cur) continue;
        api->io[reg] = v;
        uint8_t *fimg = xrealloc(NULL, ex->sb);
        ex_save(ex, m, fimg);
        api->io[reg] = cur;
        uint16_t tpc = m->api->gbr->pc;
        if (edefer) { fork_deferred(ex, fimg, edefer, site, tpc); continue; }
        queue_push(ex, fimg, 1, DP_PATH_FORK,
                   ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
                   choice_new(ex, ex->cur_choice, DP_PATH_FORK, site,
                              (int)cur_bank(ex, tpc), tpc));
        ex->res->forks_event++;
    }
    free(img);
}
