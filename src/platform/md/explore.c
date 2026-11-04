#include "explore.h"
#include "../../explore/lots.h"
#include "../../explore/novelty.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include <signal.h>
#include "../../explore/stop.h"

// set by the --time-limit alarm: the main loop stops cleanly and the report is still written

#include "../../gui/fbwatch.h"
#include "../../core/statefile.h"
#include "../../cpu/m68k/m68k.h"
#include "../../mach/mdmach.h"
#include "md_registry.h"
#include "../../core/hashset.h"
#include "../../explore/tree.h"


typedef struct {
    void *state;
    uint32_t choice;        // choice-tree node (0: none)
    int forced;
    int in_handler;         // forked inside an exception handler (not persisted in disk lots)
    uint32_t origin;
    uint32_t an_writer[8];
    uint32_t watch_pc, watch_addr, watch_value;
    uint32_t watch_d0, watch_a1, watch_a2;
    unsigned watch_size;
} Item;

typedef struct {
    const DpMdCart *c;
    const DpMdOptions *o;
    DpMdResults *r;
    DpMdMach *m;
    Item *q;
    uint32_t nq, qcap;
    Item *park; uint32_t npark, parkcap; // deferred holding area (queue full)
    void *spill_state; Item spill_item; // state waiting for its post-edits before going to the lot
    DpLotFile *lot;         // disk lot: park overflow, wall leftovers, --lot resume
    void *last_state;   // blob of the state the last push() produced (queued or parked)
    uint32_t *gen;       /* per even rom address: path generation */
    uint32_t *vis;       /* per even rom address: visits in that generation */
    uint32_t *site;      /* per even rom address: forks made from that site */
    DpHashSet pad_full_seen; /* (pc, mask) probes of --pad-full */
    uint16_t *pad_sites; /* input probes already queued at each instruction */
    uint32_t cur_gen;
    int forced, halted;
    uint32_t pc;         /* instruction being run */
    uint32_t push_site;  /* branch site (ROM address) recorded in the tree for the next push, 0 none */
    uint8_t ram_exec[0x10000]; /* work-RAM bytes executed since their last write (SMC tracking) */
    uint32_t origin;     /* site that created the current path */
    uint32_t trace[8];
    unsigned trace_pos, trace_len;
    uint32_t an_before[8], an_writer[8];
    uint32_t dbcc_dn_before;
    uint16_t sr_before;
    uint32_t watch_pc, watch_addr, watch_value;
    uint32_t watch_d0, watch_a1, watch_a2;
    unsigned watch_size;
    int irq6_done, irq4_done;
    int pad_read;
    int movie_active;
    int play_active;                 /* random played session running */
    int play_idle;                   /* the played session presses nothing (demo / attract mode) */
    uint32_t play_left, play_rng, play_hold, play_f;
    uint8_t *pool[64]; unsigned pool_n;   // J1 shared checkpoint pool
    uint8_t play_held;
    uint16_t op;
    uint32_t move_src;
    int move_src_valid;
    uint32_t previous_move_src;
    uint8_t previous_move_reg;
    int previous_move_valid, register_move_valid;
    /* stream-consumer detection (natural/movie path only) */
    struct { uint32_t entry, sp, an[7]; int open; } frames[64];
    unsigned nframes;
    struct { uint32_t entry; unsigned reg, nstarts; uint32_t starts[DP_MD_DECODER_STARTS];
             void *snap; uint32_t snap_sp; } dec[DP_MD_DECODER_CAP];
    unsigned ndec;
    int replay;          /* running a stream replay */
    uint32_t replay_sp;  /* A7 at the replayed entry: above it, the routine returned */
    /* Z80 sound CPU. zsrc: ROM address of the byte a 68000 store put in each
     * Z80 RAM cell (-1: none). It is global to the exploration, not forked
     * with the machine, so it is evidence from the last store seen. */
    int in_z80;
    int32_t zsrc[8192];
    uint32_t lr[4]; unsigned nlr;       /* ROM bytes of the last 68000 data read */
    uint32_t lr_pc;                     /* pc of the instruction that made that read */
    int32_t ins_rd;                     /* ROM cell (or RAM producer) the current instruction read (-1 none) */
    int32_t dtaint[8], ataint[8];       /* register -> a ROM cell its value derives from (-1 none) */
    unsigned nwr; uint32_t wr_a[16]; uint8_t wr_n[16];   /* RAM stores of the current instruction */
    int32_t ram_prod[0x10000];          /* work-RAM byte -> a ROM cell its value derives from (taint through registers) */
    int32_t ram_src[0x10000];           /* work-RAM byte -> ROM index of the byte a store copied (-1 none) */
    uint32_t z80_last_read;             /* ROM address of the last Z80 data read */
    int z80_last_valid;
    uint32_t zfetch_lo, zfetch_hi;      /* ROM range of the instruction being fetched */
    uint8_t z80_bank_seen[128];         /* bitmap of 32 KiB banks (4 MiB / 32 KiB) */
    int disp_done;
    uint64_t wframe;     // frames seen (watch sampling)
    void *nov;           // novelty search state (NULL: off); the frame hook drives it
    uint64_t novel;      // ROM bytes executed for the first time (novelty gain)
    DpHashSet exact_seen;   // --exact-dedup: clock-free machine hashes
    DpHashSet ctx_seen;     // --ctx-cap: admissions per coarse context
    DpTree tree;            // --tree: every pushed state is a choice
    uint32_t cur_choice;
    DpHashSet ind_sites, ind_pairs;   // observed indirect jmp/jsr (D12)
    int64_t exp_pc;          // address the next instruction should start at (-1: unknown)
    int in_handler;          // between an exception vector entry and its RTE
} X;

static size_t ssz;

static void next_generation(X *x) {
    if (++x->cur_gen == 0) {
        memset(x->gen, 0, (x->c->size / 2 + 1) * sizeof *x->gen);
        x->cur_gen = 1;
    }
}

/* Per-frame loop detection permits legitimate polling across V-blank;
 * path_steps and max_total still bound paths that run forever. */
static void novelty_frame(X *x);
static void s4_table(X *x, uint32_t pc, uint32_t obs);
static void s4_ptr_table(X *x, uint32_t pc, unsigned an, uint32_t obs);
static uint8_t play_pad(X *x);
static uint32_t md_rng(uint32_t *s);
static void on_frame(void *u) {
    X *x = u;
    next_generation(x);
    if (x->o->watch_every && ++x->wframe % x->o->watch_every == 0) {
        static uint32_t px[320 * 224];
        dp_mdmach_render(x->m, px);
        dp_fb_publish(px, 320, 224, x->wframe);
        x->r->watch_frames++;
    }
    if (x->nov) novelty_frame(x);
    if (x->play_active) {
        x->r->play_frames_run++;
        const unsigned pc_ = x->o->play_pool < 64 ? x->o->play_pool : 64;
        if (pc_ && !x->play_idle && ++x->play_f % 256 == 0) {   // keep a checkpoint (random replacement when full)
            unsigned slot = x->pool_n < pc_ ? x->pool_n : md_rng(&x->play_rng) % pc_;
            if (!x->pool[slot]) x->pool[slot] = malloc(dp_mdmach_state_size());
            if (x->pool[slot]) {
                dp_mdmach_save(x->m, x->pool[slot]);
                if (slot == x->pool_n) x->pool_n++;
                x->r->pool_saved++;
            }
        }
        if (--x->play_left == 0) {
            x->play_active = 0;
            dp_mdmach_halt(x->m);
        } else dp_mdmach_set_pad(x->m, play_pad(x));
    }
    if (x->movie_active) {
        x->r->movie_frames++;
        if (x->r->movie_frames >= x->o->movie->frames) {
            x->movie_active = 0;
            dp_mdmach_halt(x->m);
        } else dp_mdmach_set_pad(x->m, x->o->movie->pads[x->r->movie_frames]);
    }
}
static void on_dma_read(void *u, uint32_t addr, unsigned size) {
    X *x = u;
    for (unsigned i = 0; i < size && addr + i < x->r->rom_size; i++)
        x->r->flags[addr + i] |= DP_MDF_DMA_READ;
}

static void mark(X *x, uint32_t pc, unsigned len) {
    uint8_t f = x->forced ? DP_MDF_FORCED : DP_MDF_NATURAL;
    if (!x->forced && x->r->exec_hits && pc < x->r->rom_size && x->r->exec_hits[pc] != UINT32_MAX) x->r->exec_hits[pc]++;   // heat: played instruction starts
    for (unsigned i = 0; i < len && pc + i < x->r->rom_size; i++)
        if (dp_mdmach_rom_visible(x->m, pc + i)) {
            if (!(x->r->flags[pc + i] & (DP_MDF_NATURAL | DP_MDF_FORCED))) x->novel++;
            x->r->flags[pc + i] |= f;
        }
}

// one queued item to the disk lot (the state blob stays owned by the caller)
static int lot_spill(X *x, const Item *it) {
    Item m = *it; m.state = NULL;
    return x->lot && dp_lf_put(x->lot, &m, sizeof m, it->state, (uint32_t)ssz);
}

static void spill_flush(X *x) {
    if (!x->spill_state) return;
    if (!lot_spill(x, &x->spill_item)) x->r->parked_lost++;
    free(x->spill_state);
    x->spill_state = NULL;
}

static int push(X *x, uint32_t pc, int vint, int hint) {
    int full = 0;
    if (dp_stop) return 0;   // stop requested: no new work
    spill_flush(x);
    if (x->o->ctx_cap) {
        // coarse same-context eviction: pc, D0-D7, A0-A7, SR and the interrupt kind; RAM
        // excluded.  The k-th admission of a context is the key hash(ctx, k); past
        // ctx_cap the state is closed and counted
        uint64_t h = 0xcbf29ce484222325ull;
        uint32_t raw[20];
        raw[0] = pc; raw[1] = dp_mdmach_sr(x->m) | (uint32_t)vint << 16 | (uint32_t)hint << 17;
        for (unsigned i = 0; i < 8; i++) { raw[2 + i] = dp_mdmach_dn(x->m, i); raw[10 + i] = dp_mdmach_an(x->m, i); }
        for (unsigned i = 0; i < 18; i++) h = (h ^ raw[i]) * 0x100000001b3ull;
        unsigned k = 0;
        while (k < x->o->ctx_cap && dp_set_insert(&x->ctx_seen, (h ^ (k * 0x9E3779B97F4A7C15ull)) | 1) == 0) k++;
        if (k >= x->o->ctx_cap) { x->r->ctx_evicted++; return 0; }
    }
    if (x->o->exact_dedup) {
        // C3: the machine without clocks/audio/counters, plus the entry pc and interrupt kind
        uint64_t h = dp_mdmach_state_hash(x->m);
        h = (h ^ pc) * 0x100000001b3ull;
        h = (h ^ (uint64_t)(vint | hint << 1)) * 0x100000001b3ull;
        if (dp_set_insert(&x->exact_seen, h | 1) == 0) { x->r->exact_dups++; return 0; }
    }
    if (x->nq >= x->qcap) {
        uint32_t limit = x->o->queue_max > x->qcap ? x->o->queue_max : x->qcap;
        uint32_t ncap = x->qcap > limit / 2 ? limit : x->qcap * 2;
        if (!ncap && limit) ncap = 1;
        Item *q = ncap > x->qcap ? realloc(x->q, (size_t)ncap * sizeof *q) : NULL;
        if (!q) { // saturation counted; the state waits aside instead of dying (§6)
            x->r->sat_queue++;
            if (x->npark >= 4096) { // holding area full: the new state goes straight to the disk lot
                void *ls = x->lot ? malloc(ssz) : NULL;
                if (ls) {
                    dp_mdmach_save(x->m, ls);
                    dp_mdmach_state_set_pc(ls, pc);
                    dp_mdmach_state_set_irq(ls, vint, hint);
                    Item *li = &x->spill_item; memset(li, 0, sizeof *li);
                    li->state = ls; li->forced = 1; li->origin = x->pc;
                    memcpy(li->an_writer, x->an_writer, sizeof li->an_writer);
                    li->watch_pc = x->watch_pc; li->watch_addr = x->watch_addr;
                    li->watch_value = x->watch_value; li->watch_size = x->watch_size;
                    li->watch_d0 = x->watch_d0; li->watch_a1 = x->watch_a1; li->watch_a2 = x->watch_a2;
                    x->spill_state = ls;   // written by spill_flush once the caller's post-edits are done
                    x->last_state = ls;
                    x->r->parked++;
                    if (x->o->tree_path) dp_tree_close(&x->tree, dp_tree_add(&x->tree, x->cur_choice, 4, x->push_site, 0, (int)(pc & 0xFFFF)), DP_ISSUE_SPOOLED);
                    if (pc < x->r->rom_size) x->r->flags[pc] |= DP_MDF_QUEUED;
                    return 1;
                }
                free(ls);
                x->r->parked_lost++; return 0;
            }
            if (x->npark == x->parkcap) {
                uint32_t nc = x->parkcap ? x->parkcap * 2 : 64;
                Item *g = realloc(x->park, (size_t)nc * sizeof *g);
                if (!g) { x->r->parked_lost++; return 0; }
                x->park = g; x->parkcap = nc;
            }
            full = 1;
        } else { x->q = q; x->qcap = ncap; x->r->queue_growths++; }
    }
    void *s = malloc(ssz);
    if (!s) { x->r->sat_queue++; return 0; }
    dp_mdmach_save(x->m, s);
    dp_mdmach_state_set_pc(s, pc);
    dp_mdmach_state_set_irq(s, vint, hint);
    Item *item = full ? &x->park[x->npark++] : &x->q[x->nq++];
    if (full) x->r->parked++;
    x->last_state = s;
    item->state = s; item->forced = 1; item->origin = x->pc; item->in_handler = x->in_handler;
    item->choice = x->o->tree_path ? dp_tree_add(&x->tree, x->cur_choice, 4, x->push_site, 0, (int)(pc & 0xFFFF)) : 0;
    x->push_site = 0;
    memcpy(item->an_writer, x->an_writer, sizeof item->an_writer);
    item->watch_pc = x->watch_pc; item->watch_addr = x->watch_addr;
    item->watch_value = x->watch_value; item->watch_size = x->watch_size;
    item->watch_d0 = x->watch_d0; item->watch_a1 = x->watch_a1; item->watch_a2 = x->watch_a2;
    if (pc < x->r->rom_size) x->r->flags[pc] |= DP_MDF_QUEUED;
    return 1;
}

static int find_decoder(X *x, uint32_t entry, unsigned reg) {
    for (unsigned i = 0; i < x->ndec; i++)
        if (x->dec[i].entry == entry && (reg > 6 || x->dec[i].reg == reg)) return (int)i;
    return -1;
}

/* A subroutine was entered on the natural path: open a frame; a routine
 * already known as a stream consumer gets its entry snapshot (once). */
static void call_entered(X *x, uint32_t entry) {
    uint32_t sp = dp_mdmach_an(x->m, 7);
    while (x->nframes && sp >= x->frames[x->nframes - 1].sp) x->nframes--;
    if (x->nframes == sizeof x->frames / sizeof x->frames[0]) { x->r->frame_overflow++; return; }
    typeof(x->frames[0]) *f = &x->frames[x->nframes++];
    f->entry = entry; f->sp = sp; f->open = 1;
    for (unsigned i = 0; i < 7; i++) f->an[i] = dp_mdmach_an(x->m, i);
    for (unsigned i = 0; i < x->ndec; i++)
        if (x->dec[i].entry == entry && !x->dec[i].snap && (x->dec[i].snap = malloc(ssz))) {
            dp_mdmach_save(x->m, x->dec[i].snap);
            dp_mdmach_state_set_pc(x->dec[i].snap, entry);
            x->dec[i].snap_sp = sp;
        }
}

/* First ROM read of each open frame: at an address an An held on entry,
 * the routine consumes a stream from that register. */
static void stream_read(X *x, uint32_t a) {
    for (unsigned k = x->nframes; k-- > 0;) {
        typeof(x->frames[0]) *f = &x->frames[k];
        if (!f->open) break; /* outer frames already saw their first read */
        f->open = 0;
        for (unsigned n = 0; n < 7; n++) {
            if ((f->an[n] & 0xFFFFFFu) != a) continue;
            int d = find_decoder(x, f->entry, n);
            if (d < 0) {
                if (x->ndec == DP_MD_DECODER_CAP) { x->r->decoder_overflow++; break; }
                d = (int)x->ndec++;
                memset(&x->dec[d], 0, sizeof x->dec[d]);
                x->dec[d].entry = f->entry; x->dec[d].reg = n;
            }
            unsigned j;
            for (j = 0; j < x->dec[d].nstarts && x->dec[d].starts[j] != a; j++) {}
            if (j == x->dec[d].nstarts) {
                if (j < DP_MD_DECODER_STARTS) x->dec[d].starts[x->dec[d].nstarts++] = a;
                else x->r->start_overflow++;
            }
            break;
        }
    }
}

static void on_step(void *u, uint32_t pc, uint16_t op) {
    X *x = u;
    (void)op;
    x->dbcc_dn_before = dp_mdmach_dn(x->m, op & 7);
    x->sr_before = dp_mdmach_sr(x->m);
    x->pad_read = 0;
    x->ins_rd = -1;
    x->nwr = 0;
    x->op = op;
    x->register_move_valid = x->previous_move_valid && (op & 0xF038) == 0x3000 &&
                             (op & 7) == x->previous_move_reg;
    x->previous_move_valid = 0;
    x->move_src_valid = 0;
    x->pc = pc & 0xFFFFFFu;
    for (unsigned i = 0; i < 8; i++) x->an_before[i] = dp_mdmach_an(x->m, i);
    while (x->nframes && x->an_before[7] > x->frames[x->nframes - 1].sp) x->nframes--;
    x->trace[x->trace_pos++ % 8] = x->pc;
    if (x->trace_len < 8) x->trace_len++;
    if (x->replay && dp_mdmach_an(x->m, 7) > x->replay_sp) {
        x->halted = 1;
        dp_mdmach_halt(x->m);
        return;
    }
    if (x->o->pad_cut && !x->replay && op == 0 && x->pc + 16 <= x->r->rom_size && dp_mdmach_rom_visible(x->m, x->pc)) {
        // D14: ori.b #0,d0 slide through zero padding: cut once 16 bytes are all zero
        unsigned z = 0;
        while (z < 16 && x->c->rom[x->pc + z] == 0) z++;
        if (z == 16) {
            x->r->pad_cuts++;
            x->halted = 1;
            dp_mdmach_halt(x->m);
            return;
        }
    }
    if (!x->movie_active && !x->play_active && !x->replay && x->pc < x->r->rom_size && dp_mdmach_rom_visible(x->m, x->pc)) {
        size_t i = x->pc >> 1;
        if (x->gen[i] != x->cur_gen) { x->gen[i] = x->cur_gen; x->vis[i] = 0; }
        if (x->vis[i] >= x->o->visit_cap) {
            x->r->sat_visit++;
            x->halted = 1;
            dp_mdmach_halt(x->m);
            return;
        }
        x->vis[i]++;
    }
}

static void irq_forks(X *x, uint32_t next) {
    const DpMdCart *c = x->c;
    unsigned mask = (dp_mdmach_sr(x->m) >> 8) & 7;
    if (!x->irq6_done && mask < 6 && (dp_mdmach_vdp_reg(x->m, 1) & 0x20)) {
        uint32_t h = c->vec[30] & 0xFFFFFFu;
        if (h >= x->r->rom_size || (h & 1) || (x->r->flags[h] & (DP_MDF_NATURAL | DP_MDF_FORCED)))
            x->irq6_done = 1;
        else if (push(x, next & 0xFFFFFFu, 1, 0)) { x->irq6_done = 1; x->r->irq_forks++; }
    }
    if (!x->irq4_done && mask < 4 && (dp_mdmach_vdp_reg(x->m, 0) & 0x10)) {
        uint32_t h = c->vec[28] & 0xFFFFFFu;
        if (h >= x->r->rom_size || (h & 1) || (x->r->flags[h] & (DP_MDF_NATURAL | DP_MDF_FORCED)))
            x->irq4_done = 1;
        else if (push(x, next & 0xFFFFFFu, 0, 1)) { x->irq4_done = 1; x->r->irq_forks++; }
    }
}


// Taint through registers: which ROM cell a stored value derives from. Works on the disassembly text of the
// instruction (move/lea/ALU forms); a single representative cell, not the full dependency set.
static int op_reg(const char *o, int *is_a) {
    if ((o[0] == 'd' || o[0] == 'a') && o[1] >= '0' && o[1] <= '7' && !o[2]) { *is_a = o[0] == 'a'; return o[1] - '0'; }
    if (!strcmp(o, "sp")) { *is_a = 1; return 7; }
    return -1;
}
static int32_t op_taint(const X *x, const char *o) {
    int a, r = op_reg(o, &a);
    if (r >= 0) return a ? x->ataint[r] : x->dtaint[r];
    if (o[0] == '#') return -1;
    return x->ins_rd;
}
static int32_t regs_taint(const X *x, const char *o) {
    int32_t t = -1;
    for (const char *c = o; *c; c++)
        if ((*c == 'd' || *c == 'a') && c[1] >= '0' && c[1] <= '7' && !(c[2] >= '0' && c[2] <= '9') && (c == o || !(c[-1] >= 'a' && c[-1] <= 'z'))) {
            int32_t v = *c == 'a' ? x->ataint[c[1] - '0'] : x->dtaint[c[1] - '0'];
            if (v >= 0) t = v;
        }
    return t;
}
static void taint_step(X *x, uint32_t pc, unsigned len) {
    uint8_t code[12];
    for (unsigned k = 0; k < 12; k++)
        code[k] = pc >= 0xE00000 ? dp_mdmach_ram8(x->m, pc + k) : (pc + k < x->r->rom_size ? x->c->rom[pc + k] : 0);
    DpM68kInsn in;
    if (!dp_m68k_decode(code, 12, pc, &in)) { x->nwr = 0; return; }
    char buf[96], *src = NULL, *dst = NULL, *sp;
    snprintf(buf, sizeof buf, "%s", in.text);
    char *ops = strchr(buf, ' ');
    if (ops) { *ops++ = 0; src = ops; }
    if (src) {
        int depth = 0;
        for (char *c = src; *c; c++) {
            if (*c == '(') depth++;
            else if (*c == ')') depth--;
            else if (*c == ',' && !depth) { *c = 0; dst = c + 1; break; }
        }
    }
    (void)sp;
    char mn[96]; snprintf(mn, sizeof mn, "%s", buf);
    char *dot = strchr(mn, '.'); if (dot) *dot = 0;
    int32_t v = -1; int has_v = 0;   // has_v: the destination receives v
    if (!strncmp(mn, "movem", 5)) {
        // register list -> memory: the stored words take the first tainted register of the list;
        // memory -> registers: the registers take what the instruction read.
        int a0; int dr = dst ? op_reg(dst, &a0) : -1;
        if (src && dst && dr < 0 && strchr(src, ',') == NULL && regs_taint(x, src) >= -1 && (src[0] == 'd' || src[0] == 'a')) {
            v = regs_taint(x, src); has_v = 1;
        } else if (src && dst && (dst[0] == 'd' || dst[0] == 'a') && src[0] != 'd' && src[0] != 'a') {
            // memory -> register list: every listed register (range ends included) takes what was read
            for (const char *c = dst; *c; c++)
                if ((*c == 'd' || *c == 'a') && c[1] >= '0' && c[1] <= '7') {
                    if (*c == 'a') x->ataint[c[1] - '0'] = x->ins_rd; else x->dtaint[c[1] - '0'] = x->ins_rd;
                }
        }
    } else if (!strcmp(mn, "exg") && src && dst) {
        int ia, ib, ra = op_reg(src, &ia), rb = op_reg(dst, &ib);
        if (ra >= 0 && rb >= 0) {
            int32_t *pa = ia ? &x->ataint[ra] : &x->dtaint[ra], *pb = ib ? &x->ataint[rb] : &x->dtaint[rb];
            int32_t t = *pa; *pa = *pb; *pb = t;
        }
    }
    else if (!strcmp(mn, "move") || !strcmp(mn, "movea")) { v = src ? op_taint(x, src) : -1; has_v = 1; }
    else if (!strcmp(mn, "moveq") || !strcmp(mn, "clr")) { v = -1; has_v = 1; if (!dst) dst = src; }
    else if (!strcmp(mn, "lea")) { v = src ? regs_taint(x, src) : -1; has_v = 1; }
    else if (dst && (!strcmp(mn, "add") || !strcmp(mn, "adda") || !strcmp(mn, "addi") || !strcmp(mn, "addq") || !strcmp(mn, "sub") ||
                     !strcmp(mn, "suba") || !strcmp(mn, "subi") || !strcmp(mn, "subq") || !strcmp(mn, "and") || !strcmp(mn, "andi") ||
                     !strcmp(mn, "or") || !strcmp(mn, "ori") || !strcmp(mn, "eor") || !strcmp(mn, "eori") || !strcmp(mn, "mulu") ||
                     !strcmp(mn, "muls") || !strcmp(mn, "divu") || !strcmp(mn, "divs") || !strcmp(mn, "lsl") || !strcmp(mn, "lsr") ||
                     !strcmp(mn, "asl") || !strcmp(mn, "asr") || !strcmp(mn, "rol") || !strcmp(mn, "ror") || !strcmp(mn, "addx") ||
                     !strcmp(mn, "subx"))) {
        int32_t sv = op_taint(x, src), dv = op_taint(x, dst);
        v = sv >= 0 ? sv : dv; has_v = 1;
    } else if (src && !dst && (!strcmp(mn, "not") || !strcmp(mn, "neg") || !strcmp(mn, "negx") || !strcmp(mn, "swap") ||
                                !strcmp(mn, "ext") || !strcmp(mn, "tst") || !strcmp(mn, "lsl") || !strcmp(mn, "lsr"))) {
        v = op_taint(x, src); has_v = 1; dst = src;
        if (!strcmp(mn, "tst")) has_v = 0;
    }
    if (has_v && dst) {
        int a, r = op_reg(dst, &a);
        if (r >= 0) { if (a) x->ataint[r] = v; else x->dtaint[r] = v; }
    }
    // the RAM bytes this instruction stored take the value's taint (unknown mnemonic or push: none)
    int32_t sv = has_v && dst && op_reg(dst, &(int){0}) < 0 ? v : -1;
    for (unsigned i = 0; i < x->nwr; i++)
        for (unsigned k = 0; k < x->wr_n[i]; k++) x->ram_prod[(x->wr_a[i] + k) & 0xFFFF] = sv;
    x->nwr = 0;
}

static void on_end(void *u, uint32_t pc, uint32_t next, unsigned len) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    for (unsigned i = 0; i < 8; i++)
        if (dp_mdmach_an(x->m, i) != x->an_before[i]) x->an_writer[i] = pc & 0xFFFFFFu;
    pc &= 0xFFFFFFu;
    taint_step(x, pc, len);
    if (pc >= r->rom_size || !dp_mdmach_rom_visible(x->m, pc)) {
        r->exec_outside++;
        if (pc >= 0xE00000) {
            r->ram_exec++;
            uint8_t ib[12];   // the instruction as the work RAM holds it after its execution
            for (unsigned k = 0; k < 12; k++) ib[k] = dp_mdmach_ram8(x->m, pc + k);
            dp_ramarch_add_p(&r->ra, (uint16_t)pc, ib, len > 10 ? 10 : (int)len, x->ram_src[pc & 0xFFFF], x->ram_prod[pc & 0xFFFF]);
            for (unsigned k = 0; k < (len > 10 ? 10 : len); k++) x->ram_exec[(pc + k) & 0xFFFF] = 1;
        }
        if (!dp_mdmach_rom_visible(x->m, pc)) r->sram_exec++;
        irq_forks(x, next);
        return;
    }
    DpM68kInsn in;
    size_t avail = r->rom_size - pc;
    unsigned decoded = dp_m68k_decode(x->c->rom + pc, avail, pc, &in);
    if (r->exec_kind) {   // D1: ENTRY and IRQ marks on the instruction start
        int entry = x->exp_pc != (int64_t)pc;
        if (entry) {
            {
                for (uint32_t v = 0x60; v <= 0xBC && v + 4 <= r->rom_size; v += 4) {
                    const uint8_t *q = x->c->rom + v;
                    if (((uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]) == pc) x->in_handler = 1;
                }
            }
            r->exec_kind[pc] |= DP_MDX_ENTRY;
        }
        if (x->in_handler) r->exec_kind[pc] |= DP_MDX_IRQ;
        if (x->c->rom[pc] == 0x4E && x->c->rom[pc + 1] == 0x73) x->in_handler = 0;
    }
    x->exp_pc = (int64_t)(pc + len);
    mark(x, pc, len);
    if (x->move_src_valid && (x->op & 0xF1C0) == 0x3000) {
        x->previous_move_src = x->move_src;
        x->previous_move_reg = (uint8_t)((x->op >> 9) & 7);
        x->previous_move_valid = 1;
    }
    if (decoded && decoded != len) r->decode_length_mismatches++;
    if (!x->forced && ((x->op & 0xFFC0) == 0x4E80 || (x->op & 0xFF00) == 0x6100))
        call_entered(x, next & 0xFFFFFFu);
    if (x->replay) return; /* a replay records reads; it does not fork */
    irq_forks(x, next);
    if (x->pad_read) {
        /* Start, A, Right, A+Right, then the other single buttons and two chords */
        static const uint8_t pads[] = {128, 16, 8, 24, 1, 2, 4, 32, 64, 144, 40, 33, 0, 0};
        static const uint8_t exts[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x8F};   /* last two: a six-button pad, none / X Y Z Mode held */
        uint16_t *seen = &x->pad_sites[pc >> 1];
        for (unsigned i = 0; i < sizeof pads; i++) {
            uint16_t bit = (uint16_t)(1u << i);
            if (!(*seen & bit) && push(x, next & 0xFFFFFFu, 0, 0)) {
                dp_mdmach_state_set_pad(x->last_state, pads[i]);
                dp_mdmach_state_set_pad_ext(x->last_state, exts[i]);
                *seen |= bit;
                r->pad_forks++;
            }
        }
        if (x->o->pad_full)
            for (unsigned m = 0; m < 256; m++) {
                int known = 0;
                for (unsigned i = 0; i < 12; i++) known |= pads[i] == m;
                if (known || dp_set_insert(&x->pad_full_seen, ((uint64_t)pc << 8) | m) <= 0) continue;
                if (push(x, next & 0xFFFFFFu, 0, 0)) {
                    dp_mdmach_state_set_pad(x->last_state, (uint8_t)m);
                    dp_mdmach_state_set_pad_ext(x->last_state, 0);
                    r->pad_forks++;
                }
            }
    }
    if (!decoded) return;
    if ((in.cls == DP_M68K_JMP || in.cls == DP_M68K_JSR) && !in.has_target && decoded == len && !x->replay) {
        // observed indirect target: the next pc after a jmp/jsr through An/(d,An)/(An,Xn)
        if (x->ind_pairs.cnt >= 65536) r->indirect_overflow++;
        else {
            if (dp_set_insert(&x->ind_sites, (uint64_t)pc + 1) > 0) r->indirect_sites++;
            if (dp_set_insert(&x->ind_pairs, ((uint64_t)(pc + 1) << 24) | (next & 0xFFFFFFu)) > 0) r->indirect_targets++;
        }
    }
    if ((in.cls == DP_M68K_JMP || in.cls == DP_M68K_JSR) && !in.has_target && decoded == len && !x->replay) s4_table(x, pc, next & 0xFFFFFFu);
    if ((in.cls != DP_M68K_BCC && in.cls != DP_M68K_DBCC) || !in.has_target) return;
    if (decoded != len) return; /* do not invent a fall-through from a disputed length */
    if (in.cls == DP_M68K_DBCC && in.cond == 0) return; /* DBT cannot branch */
    uint32_t fall = (pc + in.len) & 0xFFFFFFu;
    uint32_t alt = ((next & 0xFFFFFFu) == fall) ? in.target : fall;
    if (alt == fall && in.target == fall) return;
    if (alt >= r->rom_size || (alt & 1)) { r->fork_off_rom++; return; }
    size_t si = pc >> 1;
    if (x->site[si] >= x->o->fork_cap) { r->sat_site++; return; }
    x->push_site = pc;
    int pushed_alt = push(x, alt, 0, 0);
    x->push_site = 0;
    if (pushed_alt) {
        x->site[si]++;
        r->forks++;
        if (in.cls == DP_M68K_BCC)
            dp_mdmach_state_force_cc(x->last_state, x->c->rom[pc] & 15, alt != fall);
        if (in.cls == DP_M68K_DBCC)
            dp_mdmach_state_force_dbcc(x->last_state, (unsigned)in.cond,
                                       x->c->rom[pc + 1] & 7, alt != fall,
                                       x->dbcc_dn_before, x->sr_before);
    }
}

static void on_read(void *u, uint32_t a, unsigned size) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    a &= 0xFFFFFFu;
    if ((a == 0xA10002 || a == 0xA10003) && x->pc < r->rom_size) x->pad_read = 1;
    if (!dp_mdmach_rom_visible(x->m, a)) { r->sram_reads++; return; }
    if (a >= 0xE00000 && x->ins_rd < 0) x->ins_rd = x->ram_prod[a & 0xFFFF];   // RAM read: its producer
    if (a < r->rom_size) {
        if ((x->op & 0xF000) == 0x3000 && size == 2 && a + 1 < r->rom_size &&
            dp_mdmach_rom_visible(x->m, a) && dp_mdmach_rom_visible(x->m, a + 1)) {
            x->move_src = a;
            x->move_src_valid = 1;
        }
        x->nlr = 0;
        x->lr_pc = x->pc;
        if (x->ins_rd < 0) x->ins_rd = (int32_t)a;
        for (unsigned i = 0; i < size && i < 4 && a + i < r->rom_size; i++) x->lr[x->nlr++] = a + i;
        unsigned k;
        for (k = 0; k < r->nreaders && r->readers[k].pc != x->pc; k++) {}
        if (k == r->nreaders && k < DP_MD_READER_CAP) {
            r->readers[k].pc = x->pc;
            r->readers[k].lo = a;
            r->nreaders++;
        }
        if (k < r->nreaders) {
            DpMdReader *reader = &r->readers[k];
            uint32_t end = a + size - 1;
            if (end >= r->rom_size) end = (uint32_t)r->rom_size - 1;
            if (a < reader->lo) reader->lo = a;
            if (end > reader->hi) reader->hi = end;
            reader->accesses++;
            if (!x->forced) reader->natural_accesses++;
        } else r->reader_overflow++;
        for (unsigned i = 0; i < size && a + i < r->rom_size; i++)
        {
            r->flags[a + i] |= DP_MDF_DATA | (x->forced ? 0 : DP_MDF_DATA_NATURAL);
            if (x->pc != x->o->exclude_read_pc) r->nonexcluded_reads[a + i] |= x->replay ? 2 : 1;
        }
        if (!x->forced && x->pc != x->o->exclude_read_pc) stream_read(x, a);
    } else if (a < 0x400000) {
        r->unmapped_r++;
    } else if (a < 0xE00000) {
        int k = dp_md_reg_find(a);
        if (k < 0) r->unmapped_r++;
        else if (k < 64) r->reg_r[k]++;
    }
}

static void on_write(void *u, uint32_t a, unsigned size, uint32_t v) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    a &= 0xFFFFFFu;
    if (a >= 0xE00000)   // provenance: a store of the bytes the same instruction just read from ROM
        for (unsigned k = 0; k < size && k < 4; k++) {
            int32_t src = -1;
            if (x->nlr == size && x->lr_pc == x->pc && x->lr[k] < r->rom_size &&
                x->c->rom[x->lr[k]] == (uint8_t)(v >> (8 * (size - 1 - k)))) src = (int32_t)x->lr[k];
            x->ram_src[(a + k) & 0xFFFF] = src;
        }
    if (a >= 0xE00000 && x->nwr < 16) { x->wr_a[x->nwr] = a; x->wr_n[x->nwr++] = (uint8_t)size; }
    if (a >= 0xE00000)   // a store into RAM bytes that already ran is self-modifying code
        for (unsigned k = 0; k < size; k++)
            if (x->ram_exec[(a + k) & 0xFFFF]) {
                x->ram_exec[(a + k) & 0xFFFF] = 0;
                if (!r->smc_writes++) r->smc_first_pc = x->pc;
            }
    if (x->register_move_valid && size == 2 && a >= 0xC00000 && a <= 0xC00003 && !(a & 1) &&
        (dp_mdmach_vdp_code(x->m) & 15) == 1 &&
        v == ((uint32_t)x->c->rom[x->previous_move_src] << 8 | x->c->rom[x->previous_move_src + 1])) {
        r->flags[x->previous_move_src] |= DP_MDF_CPU_VRAM;
        r->flags[x->previous_move_src + 1] |= DP_MDF_CPU_VRAM;
        r->cpu_vram_words++;
        x->register_move_valid = 0;
    }
    if (x->move_src_valid && size == 2 && a >= 0xC00000 && a <= 0xC00003 && !(a & 1) &&
        (dp_mdmach_vdp_code(x->m) & 15) == 1 &&
        v == ((uint32_t)x->c->rom[x->move_src] << 8 | x->c->rom[x->move_src + 1])) {
        r->flags[x->move_src] |= DP_MDF_CPU_VRAM;
        r->flags[x->move_src + 1] |= DP_MDF_CPU_VRAM;
        r->cpu_vram_words++;
        x->move_src_valid = 0;
    }
    if (x->o->watch_write <= 0xFFFFFFu && a <= x->o->watch_write &&
        x->o->watch_write - a < size) {
        x->watch_pc = x->pc; x->watch_addr = a; x->watch_value = v; x->watch_size = size;
        x->watch_d0 = dp_mdmach_dn(x->m, 0);
        x->watch_a1 = dp_mdmach_an(x->m, 1); x->watch_a2 = dp_mdmach_an(x->m, 2);
    }
    if (a < 0xA00000 || a >= 0xE00000) return;
    int k = dp_md_reg_find(a);
    if (k < 0) r->unmapped_w++;
    else if (k < 64) r->reg_w[k]++;
    if (a >= 0xC00004 && a <= 0xC00007) {
        /* the second command word of a pair carries CD5 (DMA); a long write
         * delivers both words in one access */
        int second = size == 4 || dp_mdmach_vdp_pending(x->m);
        uint16_t w = (uint16_t)v, first = size == 4 ? (uint16_t)(v >> 16) : dp_mdmach_vdp_first(x->m);
        if (!second || !(w & 0x80) || (size != 4 && (w & 0xC000) == 0x8000)) return;
        if (!(dp_mdmach_vdp_reg(x->m, 1) & 0x10) || (dp_mdmach_vdp_reg(x->m, 23) & 0x80)) return;
        DpMdCopyRec c;
        c.pc = x->pc;
        c.src = ((uint32_t)(dp_mdmach_vdp_reg(x->m, 23) & 0x7F) << 17) |
                ((uint32_t)(dp_mdmach_vdp_reg(x->m, 22) & 0xFF) << 9) |
                ((uint32_t)(dp_mdmach_vdp_reg(x->m, 21) & 0xFF) << 1);
        c.len = ((uint32_t)(dp_mdmach_vdp_reg(x->m, 20) & 0xFF) << 8) | (dp_mdmach_vdp_reg(x->m, 19) & 0xFF);
        if (!c.len) c.len = 0x10000;
        c.code = ((first >> 14) & 3) | ((w >> 2) & 0x3C);
        /* The bus DMA reads bypass the CPU trace hook. Record the commanded
         * ROM source separately: a later bus fault can truncate the transfer. */
        for (uint32_t i = 0; i < c.len * 2; i++) {
            uint32_t a = (c.src + i) & 0xFFFFFFu;
            if (a < r->rom_size && dp_mdmach_rom_visible(x->m, a)) r->flags[a] |= DP_MDF_DMA_SOURCE;
        }
        for (int i = 0; i < r->ncopy; i++)
            if (!memcmp(&r->copies[i], &c, sizeof c)) return;
        if (r->ncopy >= DP_MD_COPY_CAP) { r->copy_overflow++; return; }
        r->copies[r->ncopy++] = c;
    }
}

static void on_z80_step(void *u, uint16_t pc, unsigned len, uint32_t rom_pc) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    x->in_z80 = 1;
    r->z80_steps++;
    x->zfetch_lo = x->zfetch_hi = UINT32_MAX;
    if (pc < 0x4000u) {
        r->z80_exec_ram++;
        for (unsigned i = 0; i < len; i++) {
            int32_t s = x->zsrc[(pc + i) & 0x1FFFu];
            if (s >= 0 && (size_t)s < r->rom_size) r->z80_flags[s] |= DP_MDZ_CODE_RAM;
        }
    } else if (rom_pc != UINT32_MAX) {
        r->z80_exec_bank++;
        x->zfetch_lo = rom_pc;
        x->zfetch_hi = rom_pc + len;
        for (unsigned i = 0; i < len && rom_pc + i < r->rom_size; i++) r->z80_flags[rom_pc + i] |= DP_MDZ_EXEC_ROM;
    } else r->z80_exec_other++;
}

static void on_z80_end(void *u) { ((X *)u)->in_z80 = 0; }

static void on_z80_rom(void *u, uint32_t a) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted || a >= r->rom_size) return;
    r->z80_rom_reads++;
    if (a >= x->zfetch_lo && a < x->zfetch_hi) return;   /* instruction fetch */
    r->z80_flags[a] |= DP_MDZ_DATA;
    x->z80_last_read = a;
    x->z80_last_valid = 1;
    if ((a >> 15) < sizeof x->z80_bank_seen * 8) x->z80_bank_seen[a >> 18] |= (uint8_t)(1u << ((a >> 15) & 7));
}

static void on_sound_store(void *u, uint16_t addr, uint8_t v, int ymreg) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    if (addr < 0x4000u) {
        int32_t src = -1;
        if (!x->in_z80)
            for (unsigned i = x->nlr; i-- > 0;)
                if (x->lr[i] < r->rom_size && x->c->rom[x->lr[i]] == v) { src = (int32_t)x->lr[i]; break; }
        x->zsrc[addr & 0x1FFFu] = src;
        if (src >= 0) { r->z80_flags[src] |= DP_MDZ_COPIED; r->z80_ram_copied++; }
        return;
    }
    if (ymreg == -1 || (ymreg >= 0 && !(addr & 1u))) return;
    if (ymreg == -2) r->z80_psg_writes++; else r->z80_ym_writes++;
    uint32_t src = UINT32_MAX;
    if (x->in_z80) {
        if (x->z80_last_valid && x->c->rom[x->z80_last_read] == v) src = x->z80_last_read;
    } else
        for (unsigned i = x->nlr; i-- > 0;)
            if (x->lr[i] < r->rom_size && x->c->rom[x->lr[i]] == v) { src = x->lr[i]; break; }
    if (src == UINT32_MAX) return;
    if (ymreg == 0x2A) { r->z80_flags[src] |= DP_MDZ_SAMPLE; r->z80_typed_sample++; }
    else { r->z80_flags[src] |= DP_MDZ_MUSIC; r->z80_typed_music++; }
}

void dp_md_options_default(DpMdOptions *o) {
    o->seed_vectors = 0;
    o->max_total = 20000000;
    o->path_steps = 2000000;
    o->queue_cap = 256;
    o->queue_max = 0;
    o->fork_cap = 2;
    o->visit_cap = 64;
    o->watch_write = UINT32_MAX;
    o->exclude_read_pc = UINT32_MAX;
    o->movie = NULL;
    o->lot_path = NULL;
    o->lot_drain = 0;
    o->entry_count = 0;
    o->pad_cut = 1;
    o->second_pass = 0;
    o->novelty_steps = 0; o->novelty_frames = 240; o->novelty_archive = 256; o->novelty_cells = 0; o->play_seed = 1; o->play_frames = 0; o->demo_frames = 0; o->play_sessions = 8; o->play_from = 0; o->play_step = 1;
    o->skip_root = o->no_finalize = 0;
    o->entry_from = 0; o->entry_step = 1;
    o->pre_finalize = NULL; o->pre_user = NULL;
    o->bank_dispatch = 0;
    o->ctx_cap = 0;
    o->tree_path = NULL;
}

/* S1/S2 static layer, no machine.  S1: runs of >= 8 bytes 00/FF are padding; runs of >= 4
 * aligned big-endian longwords, even, in [$200, size), >= 3 distinct, outside padding, are
 * pointer tables.  S2: from every executed run, follow branch/jump/call targets and the
 * fall-through of non-terminal instructions over unexecuted bytes (both sides of Bcc/DBcc);
 * confidence is lost after BSR/JSR (inline data idiom): their fall-through is not followed
 * and is counted.  Every instruction must decode, stay in the ROM, sit on an even address
 * and not cross padding.  Observed bytes are never overwritten by the caller. */
static void md_static_scan(const uint8_t *rom, size_t n, DpMdResults *r) {
    const uint8_t live = DP_MDF_NATURAL | DP_MDF_FORCED;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && rom[j] == rom[i]) j++;
        if (j - i >= 8 && (rom[i] == 0 || rom[i] == 0xFF)) {
            memset(r->stat + i, 8, j - i);
            r->stat_pad += j - i;
        }
        i = j > i ? j : i + 1;
    }
    for (size_t i = 0x200 & ~3u; i + 4 <= n;) {
        size_t j = i;
        while (j + 4 <= n && !r->stat[j]) {
            uint32_t v = (uint32_t)rom[j] << 24 | (uint32_t)rom[j + 1] << 16 | (uint32_t)rom[j + 2] << 8 | rom[j + 3];
            if ((v & 1) || v < 0x200 || v >= n) break;
            j += 4;
        }
        unsigned cnt = (unsigned)((j - i) / 4), nd = 0;
        uint32_t vs[8];
        for (size_t e = i; e < j && nd < 3; e += 4) {
            uint32_t v = (uint32_t)rom[e] << 24 | (uint32_t)rom[e + 1] << 16 | (uint32_t)rom[e + 2] << 8 | rom[e + 3];
            unsigned k;
            for (k = 0; k < nd && vs[k] != v; k++) {}
            if (k == nd) vs[nd++] = v;
        }
        if (cnt >= 4 && nd >= 3) {
            for (size_t e = i; e < j; e++) if (!(r->flags[e] & live)) r->stat[e] = 7;
            r->stat_table += j - i;
            i = j;
        } else i += 4;
    }
    uint32_t *wl = malloc(sizeof(uint32_t) * (n / 2 + 1));
    if (!wl) return;
    size_t nw = 0;
    #define MD_PUSH(t) do { uint32_t t_ = (t); if (!(t_ & 1) && t_ >= 0x200 && t_ < n && !(r->flags[t_] & live) && r->stat[t_] != 3 && r->stat[t_] != 8 && r->stat[t_] != 7 && nw < n / 2) wl[nw++] = t_; } while (0)
    for (size_t i = 0; i < n; i += 2) {
        if (!(r->flags[i] & live) || (i && (r->flags[i - 1] & live))) continue;
        size_t a = i;
        while (a < n && (r->flags[a] & live)) {
            DpM68kInsn in;
            unsigned len = dp_m68k_decode(rom + a, n - a, (uint32_t)a, &in);
            if (!len) break;
            int term = in.cls == DP_M68K_BRA || in.cls == DP_M68K_JMP || in.cls == DP_M68K_RTS || in.cls == DP_M68K_RTE ||
                       in.cls == DP_M68K_RTR || in.cls == DP_M68K_STOP;
            if ((in.cls == DP_M68K_BRA || in.cls == DP_M68K_BSR || in.cls == DP_M68K_BCC || in.cls == DP_M68K_DBCC ||
                 in.cls == DP_M68K_JMP || in.cls == DP_M68K_JSR) && in.has_target) MD_PUSH(in.target & 0xFFFFFFu);
            if (!term) { if (in.cls == DP_M68K_BSR || in.cls == DP_M68K_JSR) r->stat_conf_lost++; else MD_PUSH((uint32_t)(a + len)); }
            a += len;
        }
    }
    while (nw) {
        uint32_t a = wl[--nw];
        while (a < n && !(r->flags[a] & live) && r->stat[a] != 3) {
            if (r->stat[a] == 8 || r->stat[a] == 7) break;
            DpM68kInsn in;
            unsigned len = dp_m68k_decode(rom + a, n - a, a, &in);
            if (!len || (a & 1) || a + len > n) break;
            int bad = 0;
            for (unsigned k = 0; k < len; k++) if (r->stat[a + k] == 8 || r->stat[a + k] == 7) bad = 1;
            if (bad) break;
            for (unsigned k = 0; k < len; k++) if (!(r->flags[a + k] & live)) { r->stat[a + k] = 3; r->stat_code++; }
            int term = in.cls == DP_M68K_BRA || in.cls == DP_M68K_JMP || in.cls == DP_M68K_RTS || in.cls == DP_M68K_RTE ||
                       in.cls == DP_M68K_RTR || in.cls == DP_M68K_STOP;
            if ((in.cls == DP_M68K_BRA || in.cls == DP_M68K_BSR || in.cls == DP_M68K_BCC || in.cls == DP_M68K_DBCC ||
                 in.cls == DP_M68K_JMP || in.cls == DP_M68K_JSR) && in.has_target) MD_PUSH(in.target & 0xFFFFFFu);
            if (term) break;
            if (in.cls == DP_M68K_BSR || in.cls == DP_M68K_JSR) { r->stat_conf_lost++; break; }
            a += len;
        }
    }
    #undef MD_PUSH
    free(wl);
}

/* G4 second pass: static successors (branch/jump/call targets, the fall-through at the
 * end of an executed run) of executed code that no path reached are seeded as forced
 * paths from the power-on machine.  Runs of executed bytes are walked instruction by
 * instruction; bounded to MD_PASS2_MAX seeds in all. */
enum { MD_PASS2_MAX = 256 };
static unsigned second_pass_seed(X *x) {
    DpMdResults *r = x->r;
    size_t n = r->rom_size;
    uint8_t *sd = calloc(n, 1);
    if (!sd) return 0;
    unsigned seeded = 0;
    const uint8_t live = DP_MDF_NATURAL | DP_MDF_FORCED;
    for (size_t i = 0; i < n; i += 2) {
        if (!(r->flags[i] & live) || (i && (r->flags[i - 1] & live))) continue; /* run starts only */
        size_t a = i;
        while (a < n && (r->flags[a] & live)) {
            DpM68kInsn in;
            unsigned len = dp_m68k_decode(x->c->rom + a, n - a, (uint32_t)a, &in);
            if (!len) break;
            uint32_t cand[2]; int nc = 0;
            int terminal = in.cls == DP_M68K_BRA || in.cls == DP_M68K_JMP || in.cls == DP_M68K_RTS ||
                           in.cls == DP_M68K_RTE || in.cls == DP_M68K_RTR || in.cls == DP_M68K_STOP;
            if ((in.cls == DP_M68K_BRA || in.cls == DP_M68K_BSR || in.cls == DP_M68K_BCC || in.cls == DP_M68K_DBCC ||
                 in.cls == DP_M68K_JMP || in.cls == DP_M68K_JSR) && in.has_target) cand[nc++] = in.target & 0xFFFFFFu;
            size_t next = a + len;
            if (!terminal && (next >= n || !(r->flags[next] & live))) cand[nc++] = (uint32_t)next;
            for (int k = 0; k < nc; k++) {
                uint32_t t = cand[k];
                if ((t & 1) || t < 0x200 || t >= n || (r->flags[t] & live) || sd[t]) continue;
                sd[t] = 1;
                if (r->pass2_seeds + r->pass2_refused >= MD_PASS2_MAX) { r->pass2_capped++; continue; }
                dp_mdmach_reset(x->m);
                if (push(x, t, 0, 0)) { seeded++; r->pass2_seeds++; } else r->pass2_refused++;
            }
            a = next;
        }
    }
    free(sd);
    return seeded;
}

/* G2 dispatch tables: a run of >= 3 aligned longwords (>= 3 distinct) that are all even
 * ROM addresses >= $200 whose first 6 instructions decode (no run of 8 equal bytes) is a
 * candidate table of code pointers; unexecuted targets are seeded as forced paths
 * (256-seed cap shared with the second pass). */
static int md_target_ok(const X *x, uint32_t t) {
    size_t n = x->r->rom_size;
    uint32_t a = t;
    for (unsigned k = 0; k < 6; k++) {
        if (a >= n) return k > 0;
        DpM68kInsn in;
        unsigned len = dp_m68k_decode(x->c->rom + a, n - a, a, &in);
        if (!len) return 0;
        if (a + 8 <= n) {
            unsigned same = 1;
            while (same < 8 && x->c->rom[a + same] == x->c->rom[a]) same++;
            if (same == 8) return 0;
        }
        if (in.cls == DP_M68K_BRA || in.cls == DP_M68K_JMP || in.cls == DP_M68K_RTS || in.cls == DP_M68K_RTE ||
            in.cls == DP_M68K_RTR || in.cls == DP_M68K_STOP) return 1;
        a += len;
    }
    return 1;
}

// S4 for a bare `jmp (An)` / `jsr (An)`: An was loaded by the instruction at an_writer[n]; when that is
// `movea.l d8(Am,Xn),An` or `movea.l d8(pc,Xn),An` (a table of longword pointers), the table base is
// known (Am read live, or pc + 2).  The table is accepted when the observed target is one of its
// entries; its other valid ROM targets are seeded.
static void s4_ptr_table(X *x, uint32_t pc, unsigned an, uint32_t obs) {
    DpMdResults *r = x->r;
    const uint8_t *rom = x->c->rom;
    size_t n = r->rom_size;
    uint32_t w = x->an_writer[an];
    if (w == UINT32_MAX || (size_t)w + 4 > n || (w & 1)) return;
    unsigned op = (unsigned)rom[w] << 8 | rom[w + 1];
    if ((op & 0xF1FF) != 0x2070 && (op & 0xF1FF) != 0x207B) return;      // movea.l d8(Am|pc,Xn),An
    if (((op >> 9) & 7) != an) return;
    if (rom[w + 2] & 0x07) return;                                       // brief extension only
    if (dp_set_insert(&x->ind_sites, ((uint64_t)pc + 1) | (2ull << 40)) == 0) return;
    uint32_t base = (op & 0x3F) == 0x3B ? w + 2 : dp_mdmach_an(x->m, op & 7) & 0xFFFFFFu;
    base += (uint32_t)(int32_t)(int8_t)rom[w + 3];
    uint32_t tg[128];
    unsigned nt = 0;
    int hit = 0;
    for (uint32_t e = base; nt < 128 && (size_t)e + 4 <= n && !(e & 1); e += 4) {
        uint32_t t = (uint32_t)rom[e] << 24 | (uint32_t)rom[e + 1] << 16 | (uint32_t)rom[e + 2] << 8 | rom[e + 3];
        if ((t & 1) || t < 0x200 || t >= n) break;
        if (t == obs) hit = 1;
        tg[nt++] = t;
    }
    if (nt < 2 || !hit) return;
    r->s4_tables++;
    for (unsigned k = 0; k < nt; k++) {
        uint32_t t = tg[k];
        if (r->flags[t] & (DP_MDF_NATURAL | DP_MDF_FORCED)) continue;
        if (push(x, t, 0, 0)) r->s4_seeds++; else r->s4_refused++;
    }
}

// S4 for the 68000: an observed `jmp d8(pc,Xn)` (4EFB + brief extension) names the table base
// statically (pc + 2 + d8); each entry is a `bra.s`/`bra.w` to a handler.  Every handler no run
// has reached is seeded from the current machine state, like the other side of a conditional.
// Entries stop at the first word that is not a branch; at least two are needed to call it a table.
static void s4_table(X *x, uint32_t pc, uint32_t obs) {
    DpMdResults *r = x->r;
    const uint8_t *rom = x->c->rom;
    size_t n = r->rom_size;
    if ((size_t)pc + 4 > n || rom[pc] != 0x4E) return;
    unsigned o2 = rom[pc + 1];
    int pcrel = o2 == 0xFB || o2 == 0xBB;                 // jmp / jsr d8(pc,Xn)
    int anrel = (o2 & 0xF8) == 0xF0 || (o2 & 0xF8) == 0xB0;   // jmp / jsr d8(An,Xn): An read live
    if ((o2 & 0xF8) == 0xD0 || (o2 & 0xF8) == 0x90) { s4_ptr_table(x, pc, o2 & 7, obs); return; }   // jmp / jsr (An)
    if (!pcrel && !anrel) return;
    if (rom[pc + 2] & 0x07) return;                       // brief extension word only (no full-format bits)
    if (dp_set_insert(&x->ind_sites, ((uint64_t)pc + 1) | (1ull << 40)) == 0) return;
    uint32_t base = pcrel ? pc + 2 : dp_mdmach_an(x->m, o2 & 7) & 0xFFFFFFu;
    base += (uint32_t)(int32_t)(int8_t)rom[pc + 3];
    uint32_t tg[128];
    unsigned nt = 0;
    for (uint32_t e = base; nt < 128 && (size_t)e + 4 <= n && !(e & 1);) {
        uint32_t t;
        if (rom[e] == 0x60 && rom[e + 1] != 0x00 && rom[e + 1] != 0xFF) { t = e + 2 + (uint32_t)(int32_t)(int8_t)rom[e + 1]; e += 2; }
        else if (rom[e] == 0x60 && rom[e + 1] == 0x00) {
            int16_t d = (int16_t)((uint16_t)rom[e + 2] << 8 | rom[e + 3]);
            t = e + 2 + (uint32_t)(int32_t)d; e += 4;
        } else break;
        tg[nt++] = t;
    }
    if (nt < 2) {
        // word-offset table: move.w tbl(pc,d0.w),d1 ; jmp tbl(pc,d1.w).  Entries are int16 offsets
        // from the table base; accepted only when the observed target is one of them, and the
        // table ends where the first target (code) begins.
        nt = 0;
        uint32_t lim = n < 0x1000000 ? (uint32_t)n : 0xFFFFFF, hit = 0;
        for (uint32_t e = base; nt < 128 && e < lim && (size_t)e + 2 <= n && !(e & 1); e += 2) {
            int16_t off = (int16_t)((uint16_t)rom[e] << 8 | rom[e + 1]);
            uint32_t t = base + (uint32_t)(int32_t)off;
            if ((t & 1) || t < 0x200 || t >= n || t <= e) break;
            if (t < lim) lim = t;
            if (t == obs) hit = 1;
            tg[nt++] = t;
        }
        if (nt < 2 || !hit) return;
    }
    r->s4_tables++;
    for (unsigned k = 0; k < nt; k++) {
        uint32_t t = tg[k];
        if ((t & 1) || t < 0x200 || t >= n) { r->s4_refused++; continue; }
        if (r->flags[t] & (DP_MDF_NATURAL | DP_MDF_FORCED)) continue;
        if (push(x, t, 0, 0)) r->s4_seeds++; else r->s4_refused++;
    }
}

static unsigned dispatch_seed(X *x) {
    DpMdResults *r = x->r;
    size_t n = r->rom_size;
    uint8_t *sd = calloc(n, 1);
    if (!sd) return 0;
    unsigned seeded = 0;
    for (size_t i = 0x200; i + 4 <= n;) {
        size_t j = i;
        while (j + 4 <= n) {
            uint32_t v = (uint32_t)x->c->rom[j] << 24 | (uint32_t)x->c->rom[j + 1] << 16 | (uint32_t)x->c->rom[j + 2] << 8 | x->c->rom[j + 3];
            if ((v & 1) || v < 0x200 || v >= n) break;
            j += 4;
        }
        unsigned cnt = (unsigned)((j - i) / 4);
        if (cnt < 3) { i += 4; continue; }
        uint32_t vs[64]; unsigned nd = 0;
        for (size_t e = i; e < j && nd < 64; e += 4) {
            uint32_t v = (uint32_t)x->c->rom[e] << 24 | (uint32_t)x->c->rom[e + 1] << 16 | (uint32_t)x->c->rom[e + 2] << 8 | x->c->rom[e + 3];
            unsigned k;
            for (k = 0; k < nd && vs[k] != v; k++) {}
            if (k == nd) vs[nd++] = v;
        }
        if (nd >= 3) {
            r->disp_tables++;
            for (unsigned k = 0; k < nd; k++) {
                uint32_t t = vs[k];
                if (sd[t] || (r->flags[t] & (DP_MDF_NATURAL | DP_MDF_FORCED))) continue;
                if (!md_target_ok(x, t)) { r->disp_refused_view++; continue; }
                sd[t] = 1;
                if (r->pass2_seeds + r->pass2_refused >= MD_PASS2_MAX) { r->pass2_capped++; continue; }
                dp_mdmach_reset(x->m);
                if (push(x, t, 0, 0)) { seeded++; r->pass2_seeds++; r->disp_seeds++; } else r->pass2_refused++;
            }
        }
        i = j;
    }
    free(sd);
    return seeded;
}

// dp_mdmach_run in slices so the --time-limit / SIGINT stop is seen inside unbounded runs
static uint64_t run_sliced(DpMdMach *m, uint64_t budget, int *cut, int *fault) {
    uint64_t n = 0;
    for (;;) {
        uint64_t left = budget - n, slice = left < (1u << 20) ? left : (1u << 20);
        *cut = 0;
        uint64_t k = dp_mdmach_run(m, slice, cut, fault);
        n += k;
        // a full slice reports cut (its own budget ran out): only the caller's budget ends the path
        if (k < slice || n >= budget || dp_stop || *fault) break;
    }
    return n;
}

static void run_path(X *x, Item *it, uint64_t budget, int *cut) {
    DpMdResults *r = x->r;
    int fault = 0;
    x->halted = 0;
    x->previous_move_valid = x->register_move_valid = 0;
    x->forced = it ? it->forced : 0;
    x->exp_pc = -1; x->in_handler = it ? it->in_handler : 0;
    x->cur_choice = it ? it->choice : 0;
    x->origin = it ? it->origin : UINT32_MAX;
    for (unsigned i = 0; i < 8; i++) x->an_writer[i] = it ? it->an_writer[i] : UINT32_MAX;
    x->watch_pc = it ? it->watch_pc : UINT32_MAX;
    x->watch_addr = it ? it->watch_addr : 0;
    x->watch_value = it ? it->watch_value : 0;
    x->watch_size = it ? it->watch_size : 0;
    x->watch_d0 = it ? it->watch_d0 : 0;
    x->watch_a1 = it ? it->watch_a1 : 0; x->watch_a2 = it ? it->watch_a2 : 0;
    x->trace_pos = x->trace_len = 0;
    next_generation(x);
    if (it) dp_mdmach_load(x->m, it->state);
    r->paths++;
    uint64_t pb = (x->movie_active || !x->forced) ? budget / 2 : (x->o->path_steps < budget ? x->o->path_steps : budget);
    uint64_t n = run_sliced(x->m, pb, cut, &fault);
    r->steps += n;
    if (*cut) {
        if (n >= budget) r->sat_total++;
        else r->sat_path++;
    }
    if (fault) {
        r->faults++;
        uint32_t fpc = dp_mdmach_fault_pc(x->m);
        unsigned i;
        for (i = 0; i < r->nfault_sites; i++)
            if (r->fault_sites[i].pc == x->pc && r->fault_sites[i].fault_pc == fpc &&
                r->fault_sites[i].code == fault) break;
        if (i == r->nfault_sites && i < DP_MD_FAULT_SITE_CAP) {
            r->fault_sites[i] = (DpMdFaultSite){.pc = x->pc, .fault_pc = fpc, .origin = x->origin, .code = fault};
            r->fault_sites[i].trace_len = x->trace_len;
            for (unsigned j = 0; j < x->trace_len; j++)
                r->fault_sites[i].trace[j] = x->trace[(x->trace_pos - x->trace_len + j) % 8];
            for (unsigned j = 0; j < 8; j++) {
                r->fault_sites[i].an[j] = dp_mdmach_an(x->m, j);
                r->fault_sites[i].an_writer[j] = x->an_writer[j];
            }
            r->fault_sites[i].watch_pc = x->watch_pc;
            r->fault_sites[i].watch_addr = x->watch_addr;
            r->fault_sites[i].watch_value = x->watch_value;
            r->fault_sites[i].watch_size = x->watch_size;
            r->fault_sites[i].watch_d0 = x->watch_d0;
            r->fault_sites[i].watch_a1 = x->watch_a1; r->fault_sites[i].watch_a2 = x->watch_a2;
            r->nfault_sites++;
        }
        if (i < r->nfault_sites) {
            r->fault_sites[i].count++;
            if (!x->forced) r->fault_sites[i].natural++;
        } else r->fault_sites_overflow++;
        for (i = 0; i < r->nfault_origins; i++)
            if (r->fault_origins[i].pc == x->pc && r->fault_origins[i].fault_pc == fpc &&
                r->fault_origins[i].code == fault && r->fault_origins[i].origin == x->origin) break;
        if (i == r->nfault_origins && i < DP_MD_FAULT_SITE_CAP) {
            r->fault_origins[i] = (DpMdFaultSite){.pc = x->pc, .fault_pc = fpc, .origin = x->origin, .code = fault};
            r->nfault_origins++;
        }
        if (i < r->nfault_origins) {
            r->fault_origins[i].count++;
            if (!x->forced) r->fault_origins[i].natural++;
        } else r->fault_origins_overflow++;
    }
}


/* --- novelty search (Go-Explore) --------------------------------------------------------
 * An archive of compressed machine snapshots; each trial restarts from a cell picked among
 * eight random ones (gain per try, depth, square root of its picks) and plays random pad
 * bytes for N frames as a natural path (the frame hook drives it).  With --novelty-cells a new
 * coarse screen fingerprint, or new ROM bytes executed, archive the cell; lives are found
 * without game knowledge (a RAM byte <= 9 that only drops by 1, >= 200 frames apart) and the
 * cells archived just before a death are doomed.  The freshest cells seed the fork queue. */
typedef struct {
    uint8_t *z; uLongf zlen;
    uint64_t gain; uint32_t picks, depth;
    uint8_t lives[65536];
} MdCell;
typedef struct {
    MdCell *a; uint32_t n;
    DpLives *L;
    DpKeys keys;
    uint8_t *tmp; Bytef *ztmp; uLongf zcap;
    uint32_t rng, left, f, best, hold, frames_since;
    uint8_t held;
    uint64_t before;
    int died, learn, cells;
    int recent[64]; uint32_t recent_f[64]; unsigned nrecent;
} MdNov;

static uint32_t md_rng(uint32_t *s) { uint32_t v = *s; v ^= v << 13; v ^= v >> 17; v ^= v << 5; return *s = v; }
static int md_archive_add(X *x, MdNov *N, uint64_t gain) {
    DpMdResults *r = x->r;
    dp_mdmach_save(x->m, N->tmp);
    uLongf zl = N->zcap;
    if (compress2(N->ztmp, &zl, N->tmp, (uLong)ssz, 1) != Z_OK) return -1;
    uint8_t *z = malloc(zl);
    if (!z) return -1;
    memcpy(z, N->ztmp, zl);
    uint32_t slot = N->n;
    if (N->n >= x->o->novelty_archive) {
        double worst = 1e300;
        for (uint32_t i = 0; i < N->n; i++) {
            double sc = (double)N->a[i].gain / (1.0 + N->a[i].picks);
            if (sc < worst) { worst = sc; slot = i; }
        }
        free(N->a[slot].z);
        r->novelty_replaced++;
    } else N->n++;
    N->a[slot].z = z;
    N->a[slot].zlen = zl;
    memcpy(N->a[slot].lives, dp_mdmach_ram(x->m), 65536);
    N->a[slot].gain = gain;
    N->a[slot].picks = 0;
    r->novelty_archived++;
    return (int)slot;
}
// pad held 4..43 frames, no opposing directions (bits: U D L R B C A S): same shape as novelty trials
static uint8_t play_pad(X *x) {
    if (x->play_idle) return 0;
    if (!x->play_hold) {
        uint32_t v = md_rng(&x->play_rng);
        uint8_t h = (uint8_t)(v & 0xFF);
        if ((h & 0x03) == 0x03) h &= (uint8_t)~0x02;
        if ((h & 0x0C) == 0x0C) h &= (uint8_t)~0x04;
        x->play_held = h;
        x->play_hold = 4 + (v >> 16) % 40;
    }
    x->play_hold--;
    return x->play_held;
}
static uint8_t md_random_pad(MdNov *N) {   /* bits: U D L R B C A S; no opposing directions */
    if (!N->hold) {
        uint32_t v = md_rng(&N->rng);
        uint8_t h = (uint8_t)(v & 0xFF);
        if ((h & 0x03) == 0x03) h &= (uint8_t)~0x02;
        if ((h & 0x0C) == 0x0C) h &= (uint8_t)~0x04;
        N->held = h;
        N->hold = 4 + (v >> 16) % 40;
    }
    N->hold--;
    return N->held;
}
// called after every frame of a trial: score it, maybe archive it, pick the next pad, stop at the end
static void novelty_frame(X *x) {
    MdNov *N = x->nov;
    DpMdResults *r = x->r;
    r->novelty_frames_run++;
    if (N->cells && dp_lives_frame(N->L, dp_mdmach_ram(x->m), N->learn)) {
        for (unsigned k = 0; k < N->nrecent; k++)
            if (N->recent[k] >= 0 && N->f - N->recent_f[k] < 120) { N->a[N->recent[k]].picks = 1u << 30; N->a[N->recent[k]].gain = 0; }
        r->novelty_deaths++;
        N->died = 1;
        dp_mdmach_halt(x->m);
        return;
    }
    N->frames_since++;
    if (N->f % 8 == 7) {
        uint64_t k = N->cells ? dp_mdmach_screen_key(x->m) : 0;
        int fresh = N->cells && !dp_keys_seen(&N->keys, k);
        if (fresh || (x->novel > N->before && N->frames_since >= 60)) {
            uint64_t g = x->novel - N->before;
            N->a[N->best].gain += g;
            int slot = md_archive_add(x, N, N->cells ? g + (fresh ? 1 : 0) : g + 1);
            if (N->nrecent < 64) { N->recent[N->nrecent] = slot; N->recent_f[N->nrecent++] = N->f; }
            if (slot >= 0) {
                N->a[slot].depth = N->a[N->best].depth + N->f + 1;
                if (N->a[slot].depth > r->novelty_depth) r->novelty_depth = N->a[slot].depth;
            }
            dp_keys_add(&N->keys, k);
            if (g) r->novelty_hits++;
            N->before = x->novel;
            N->frames_since = 0;
        }
    }
    N->f++;
    if (N->f >= x->o->novelty_frames || r->steps >= x->o->max_total) { dp_mdmach_halt(x->m); return; }
    dp_mdmach_set_pad(x->m, md_random_pad(N));
}
static void novelty_search(X *x) {
    DpMdResults *r = x->r;
    const DpMdOptions *o = x->o;
    if (!o->novelty_steps || !o->novelty_archive) return;
    MdNov *N = calloc(1, sizeof *N);
    if (!N) return;
    N->a = calloc(o->novelty_archive, sizeof *N->a);
    N->L = dp_lives_new(65536);
    dp_keys_init(&N->keys, 1u << 18);
    N->tmp = malloc(ssz);
    uint8_t *tmp2 = malloc(ssz);
    N->zcap = compressBound((uLong)ssz);
    N->ztmp = malloc(N->zcap);
    if (!N->a || !N->L || !N->keys.k || !N->tmp || !tmp2 || !N->ztmp) {
        free(N->a); free(N->L); dp_keys_free(&N->keys); free(N->tmp); free(tmp2); free(N->ztmp); free(N);
        return;
    }
    N->cells = o->novelty_cells;
    N->rng = o->play_seed * 0x2545F491u + 7;
    dp_mdmach_reset(x->m);
    md_archive_add(x, N, 1);
    x->nov = N;
    x->forced = 0;
    uint64_t stop = r->steps + o->novelty_steps;
    unsigned idle = 0;   // consecutive trials that never reached a frame: the ROM has no frame loop here
    while (r->steps < stop && r->steps < o->max_total) {
        if (idle >= 8) { r->novelty_stalled++; break; }
        uint8_t maxl = 0;
        if (N->L->lives_byte >= 0)
            for (uint32_t i = 0; i < N->n; i++)
                if (N->a[i].lives[N->L->lives_byte] > maxl && N->a[i].lives[N->L->lives_byte] <= 9) maxl = N->a[i].lives[N->L->lives_byte];
        uint32_t best = N->n - 1;
        double bs = -1;
        for (int k = 0; k < 8; k++) {
            uint32_t i = k ? md_rng(&N->rng) % N->n : N->n - 1;
            if (!N->cells) {
                double g = (double)N->a[i].gain / (1.0 + N->a[i].picks);
                if (g > bs) { bs = g; best = i; }
                continue;
            }
            double lf = (N->L->lives_byte >= 0 && N->a[i].lives[N->L->lives_byte] < maxl) ? 1e-3 : 1.0;
            double sc = lf * (1.0 + (double)N->a[i].gain) * (1.0 + N->a[i].depth / 200.0) / sqrt(1.0 + N->a[i].picks);
            if (sc > bs) { bs = sc; best = i; }
        }
        uLongf sz = (uLongf)ssz;
        if (uncompress(tmp2, &sz, N->a[best].z, N->a[best].zlen) != Z_OK) break;
        N->a[best].picks++;
        r->novelty_trials++;
        N->best = best; N->f = 0; N->hold = 0; N->held = 0; N->frames_since = 0; N->nrecent = 0;
        N->died = 0; N->before = x->novel;
        N->learn = r->novelty_trials < 64;
        // restore, then the trial runs as one natural path whose frame hook plays and scores
        x->halted = 0;
        x->previous_move_valid = x->register_move_valid = 0;
        x->cur_choice = 0;
        x->origin = UINT32_MAX;
        for (unsigned i = 0; i < 8; i++) x->an_writer[i] = UINT32_MAX;
        x->watch_pc = UINT32_MAX;
        x->trace_pos = x->trace_len = 0;
        next_generation(x);
        dp_mdmach_load(x->m, tmp2);
        dp_lives_restore(N->L, dp_mdmach_ram(x->m), 65536);   // a restore is no decrement sequence
        dp_mdmach_set_pad(x->m, md_random_pad(N));
        int cut = 0, fault = 0;
        r->paths++;
        uint64_t left = stop - r->steps < o->max_total - r->steps ? stop - r->steps : o->max_total - r->steps;
        uint64_t fr0 = r->novelty_frames_run;
        r->steps += run_sliced(x->m, left, &cut, &fault);
        if (fault) r->faults++;
        idle = r->novelty_frames_run == fr0 ? idle + 1 : 0;
    }
    x->nov = NULL;
    dp_mdmach_set_pad(x->m, 0);
    // the archive's freshest cells seed the fork queue (64 at most)
    for (uint32_t i = N->n; i > 0; i--) {
        if (i + 64 < N->n) break;
        uLongf sz = (uLongf)ssz;
        if (uncompress(tmp2, &sz, N->a[i - 1].z, N->a[i - 1].zlen) != Z_OK) continue;
        dp_mdmach_load(x->m, tmp2);
        if (push(x, dp_mdmach_pc(x->m), 0, 0)) r->novelty_seeded++;
    }
    if (o->export_state && N->n) {
        uint32_t d = 0;
        for (uint32_t i = 1; i < N->n; i++) if (N->a[i].depth > N->a[d].depth) d = i;
        if (!dp_state_export(o->export_state, "md", x->c->rom, x->c->size, N->a[d].depth, N->a[d].z, (uint32_t)N->a[d].zlen, (uint32_t)ssz)) {
            r->state_exported = 1;
            r->state_exported_depth = N->a[d].depth;
        }
    }
    for (uint32_t i = 0; i < N->n; i++) free(N->a[i].z);
    free(N->a); free(N->L); dp_keys_free(&N->keys); free(N->tmp); free(tmp2); free(N->ztmp); free(N);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A table entry is a pointer into the ROM past the header. */
static int rom_ptr(const X *x, size_t at, uint32_t *v) {
    if (at + 4 > x->c->size) return 0;
    *v = rd32(x->c->rom + at);
    return *v >= 0x200 && *v < x->c->size;
}

static void replay_one(X *x, unsigned d, uint32_t start, uint64_t *left) {
    DpMdResults *r = x->r;
    uint64_t cap = x->o->path_steps < *left ? x->o->path_steps : *left;
    if (!cap) { r->sat_replay_total++; return; }
    void *s = malloc(ssz);
    if (!s) { r->sat_replay_total++; return; }
    memcpy(s, x->dec[d].snap, ssz);
    dp_mdmach_state_set_an(s, x->dec[d].reg, start);
    dp_mdmach_load(x->m, s);
    free(s);
    x->replay = 1; x->forced = 1; x->halted = 0;
    x->replay_sp = x->dec[d].snap_sp;
    x->previous_move_valid = x->register_move_valid = 0;
    next_generation(x);
    int cut = 0, fault = 0;
    uint64_t steps = run_sliced(x->m, cap, &cut, &fault);
    x->replay = 0;
    r->replay_runs++;
    r->replay_steps += steps;
    r->steps += steps;
    *left -= steps < *left ? steps : *left;
    if (cut) { if (cap < x->o->path_steps) r->sat_replay_total++; else r->sat_replay_steps++; }
    if (fault) r->replay_faults++;
}

/* Stream replay (bounded): the observed starts of each consumer locate ROM
 * pointer tables (two observed starts at a constant stride of 4..16 bytes);
 * the consumer is run again from its entry snapshot on every other entry. */
static void replay_streams(X *x, uint64_t budget) {
    DpMdResults *r = x->r;
    const size_t n = x->c->size;
    uint8_t *tried = malloc(n);
    if (!tried) return;
    uint64_t left = budget;
    for (unsigned d = 0; d < x->ndec; d++) {
        if (!x->dec[d].snap) { r->decoders_without_snapshot++; continue; }
        if (x->dec[d].nstarts < 2) continue;
        memset(tried, 0, n);
        for (unsigned j = 0; j < x->dec[d].nstarts; j++) tried[x->dec[d].starts[j]] = 1;
        /* positions of the observed starts in the ROM */
        size_t pos[256]; unsigned np = 0;
        for (size_t at = 0; at + 4 <= n && np < 256; at += 2) {
            uint32_t v = rd32(x->c->rom + at);
            if (v >= n) continue;
            for (unsigned j = 0; j < x->dec[d].nstarts; j++)
                if (x->dec[d].starts[j] == v) { pos[np++] = at; break; }
        }
        for (unsigned i = 0; i + 1 < np; i++) {
            size_t k = pos[i + 1] - pos[i];
            if (k < 4 || k > 16) continue;
            r->replay_tables++;
            size_t lo = pos[i], hi = pos[i + 1];
            uint32_t v;
            while (lo >= k && rom_ptr(x, lo - k, &v)) lo -= k;
            while (rom_ptr(x, hi + k, &v)) hi += k;
            for (size_t at = lo; at <= hi; at += k) {
                if (!rom_ptr(x, at, &v) || tried[v]) continue;
                tried[v] = 1;
                r->replay_candidates++;
                replay_one(x, d, v, &left);
            }
        }
    }
    free(tried);
}

int dp_md_explore(const DpMdCart *c, const DpMdOptions *o, DpMdResults *r) {
    memset(r, 0, sizeof *r);
    r->rom_size = c->size;
    r->flags = calloc(c->size, 1);
    r->exec_kind = calloc(c->size, 1);
    r->nonexcluded_reads = calloc(c->size, 1);
    r->exec_hits = calloc(c->size, sizeof(uint32_t));
    r->z80_flags = calloc(c->size, 1);
    r->copies = calloc(DP_MD_COPY_CAP, sizeof *r->copies);
    X x = {.c = c, .o = o, .r = r, .qcap = o->queue_cap};
    x.ins_rd = -1;
    for (unsigned i = 0; i < 8; i++) x.dtaint[i] = x.ataint[i] = -1;
    for (unsigned i = 0; i < 0x10000; i++) { x.ram_src[i] = -1; x.ram_prod[i] = -1; }
    x.q = calloc(o->queue_cap ? o->queue_cap : 1, sizeof *x.q);
    if (o->lot_path && !(x.lot = dp_lf_open(o->lot_path, "MD", 1)))
        fprintf(stderr, "deepeel: cannot open the lot %s\n", o->lot_path);
    x.gen = calloc(c->size / 2 + 1, sizeof *x.gen);
    x.vis = calloc(c->size / 2 + 1, sizeof *x.vis);
    dp_tree_init(&x.tree);
    // resumed lot: its items carry ids of the tree the previous run saved
    if (x.lot && dp_lf_pending(x.lot) && o->tree_path && dp_tree_load(&x.tree, o->tree_path)) r->tree_loaded = x.tree.n;
    dp_set_init(&x.ctx_seen, 1 << 10);
    dp_set_init(&x.exact_seen, 1 << 10);
    dp_set_init(&x.ind_sites, 1 << 8);
    dp_set_init(&x.pad_full_seen, 1 << 20);
    dp_set_init(&x.ind_pairs, 1 << 10);
    x.site = calloc(c->size / 2 + 1, sizeof *x.site);
    x.pad_sites = calloc(c->size / 2 + 1, sizeof *x.pad_sites);
    x.m = dp_mdmach_open(c->rom, c->size);
    if (!r->flags || !r->z80_flags || !r->nonexcluded_reads || !r->copies || !x.q || !x.gen || !x.vis || !x.site || !x.pad_sites || !x.m) {
        dp_mdmach_close(x.m);
        free(x.q); free(x.gen); free(x.vis); free(x.site); free(x.pad_sites); dp_set_free(&x.ind_sites); dp_set_free(&x.pad_full_seen); dp_set_free(&x.ind_pairs);
        dp_md_results_free(r);
        return -1;
    }
    ssz = dp_mdmach_state_size();
    for (unsigned i = 0; i < 8192; i++) x.zsrc[i] = -1;
    DpMdHooks h = {&x, on_step, on_end, on_read, on_write, on_frame, on_dma_read,
                   on_z80_step, on_z80_end, on_z80_rom, on_sound_store};
    dp_mdmach_set_hooks(x.m, &h);
    dp_mdmach_reset(x.m);
    if (o->movie && o->movie->frames) {
        x.movie_active = 1;
        dp_mdmach_set_pad(x.m, o->movie->pads[0]);
    }
    int cut = 0;
    int drain = o->lot_drain && dp_lf_pending(x.lot); // saved states only: no reset path, no stream replay
    // demo: the console left alone from power-on, no button pressed (attract mode): the cheapest
    // large natural coverage, so it runs before the forking exploration
    if (!drain && o->demo_frames) {
        uint64_t f0 = r->play_frames_run, e0 = 0, e1 = 0;
        for (uint32_t k = 0; k < r->rom_size; k++) e0 += (r->flags[k] & DP_MDF_NATURAL) != 0;
        dp_mdmach_reset(x.m);
        x.play_active = 1; x.play_idle = 1; x.play_left = o->demo_frames;
        run_path(&x, NULL, o->max_total, &cut);
        x.play_active = 0; x.play_idle = 0;
        for (uint32_t k = 0; k < r->rom_size; k++) e1 += (r->flags[k] & DP_MDF_NATURAL) != 0;
        r->demo_run = r->play_frames_run - f0; r->demo_exec = e1 - e0;
        dp_mdmach_reset(x.m);
    }
    if (!drain && !o->skip_root) run_path(&x, NULL, o->max_total, &cut);
    x.movie_active = 0;
    r->decoders = x.ndec;
    if (!drain && r->steps < o->max_total) replay_streams(&x, (o->max_total - r->steps) / 2);
    for (unsigned d = 0; d < x.ndec; d++) free(x.dec[d].snap);
    // played sessions: random pad from power-on, each a natural path bounded in frames
    for (uint32_t s = 0; !drain && o->play_frames && s < o->play_sessions; s++) {
        if (r->steps >= o->max_total || dp_stop) break;
        if (o->play_step > 1 && s % o->play_step != o->play_from) continue;
        x.play_hold = 0; x.play_f = 0;
        x.play_rng = (o->play_seed + s * 0x2545F491u) * 0x9E3779B1u + 7;
        if (!x.play_rng) x.play_rng = 1;
        if (o->play_pool && x.pool_n) {   // continue from a checkpoint an earlier session kept
            dp_mdmach_load(x.m, x.pool[md_rng(&x.play_rng) % x.pool_n]);
            r->pool_starts++;
        } else dp_mdmach_reset(x.m);
        x.play_active = 1;
        x.play_left = o->play_frames;
        dp_mdmach_set_pad(x.m, play_pad(&x));
        run_path(&x, NULL, o->max_total - r->steps, &cut);
        x.play_active = 0;
        r->play_sessions_run++;
    }
    for (unsigned i = 0; i < 64; i++) free(x.pool[i]);
    if (!drain) novelty_search(&x);
    // explicit entries (G3): each starts a forced path from the power-on machine
    if (!drain)
        for (unsigned e = 0; e < o->entry_count; e++) {
            if (o->entry_step > 1 && e % o->entry_step != o->entry_from) continue;
            uint32_t a = o->entries[e];
            if ((a & 1) || a >= r->rom_size) { r->entries_refused++; continue; }
            dp_mdmach_reset(x.m);
            if (push(&x, a, 0, 0)) r->entries_seeded++; else r->entries_refused++;
        }
    // --seed-vectors (G1): the autovector (level 1-7) and TRAP #0-15 targets of the vector table
    if (!drain && o->seed_vectors) {
        uint32_t seen[24]; unsigned ns = 0;
        for (unsigned v = 0; v < 23; v++) {
            unsigned at = v < 7 ? 0x64u + v * 4u : 0x80u + (v - 7) * 4u;   /* 7 autovectors, then TRAP #0-15 */
            if (at + 4 > c->size) break;
            uint32_t a = (uint32_t)c->rom[at] << 24 | (uint32_t)c->rom[at + 1] << 16 | (uint32_t)c->rom[at + 2] << 8 | c->rom[at + 3];
            unsigned k; for (k = 0; k < ns && seen[k] != a; k++) {}
            if (k < ns || a < 0x200u) continue;
            seen[ns++] = a;
            if ((a & 1) || a >= r->rom_size) { r->entries_refused++; continue; }
            dp_mdmach_reset(x.m);
            if (push(&x, a, 0, 0)) r->vectors_seeded++; else r->entries_refused++;
        }
    }
    for (;;) {
        spill_flush(&x);
        if (r->steps >= o->max_total || dp_stop) break;
        if (!x.nq && x.npark) { x.q[x.nq++] = x.park[--x.npark]; r->parked_resumed++; }
        if (!x.nq && dp_lf_pending(x.lot)) { // the disk backlog comes back
            Item m; uint32_t ml = 0, bl = 0; uint8_t *blob = NULL;
            if (dp_lf_get(x.lot, &m, &ml, &blob, &bl)) {
                if (ml == sizeof m && bl == ssz) { m.state = blob; if (m.choice >= x.tree.n) m.choice = 0; x.q[x.nq++] = m; r->parked_resumed++; }
                else free(blob);
            }
            if (!x.nq) continue;
        }
        if (!x.nq) {
            if (o->bank_dispatch && !x.disp_done && r->steps < o->max_total) { x.disp_done = 1; if (dispatch_seed(&x)) continue; }
            if (o->second_pass && r->steps < o->max_total && second_pass_seed(&x)) continue;
            break;
        }
        Item it = x.q[--x.nq];
        run_path(&x, &it, o->max_total - r->steps, &cut);
        if (it.choice) dp_tree_close(&x.tree, it.choice, DP_ISSUE_DONE);
        x.cur_choice = 0;
        free(it.state);
    }
    // leftovers of a wall go to the lot when there is one (the next run resumes them)
    for (uint32_t i = 0; i < x.npark; i++) if (x.park[i].choice) dp_tree_close(&x.tree, x.park[i].choice, DP_ISSUE_BUDGET);
    for (uint32_t i = 0; i < x.nq; i++) if (x.q[i].choice) dp_tree_close(&x.tree, x.q[i].choice, DP_ISSUE_BUDGET);
    for (uint32_t i = 0; i < x.npark; i++) {
        if (!lot_spill(&x, &x.park[i])) r->dropped_pending++;
        free(x.park[i].state);
    }
    free(x.park);
    if (o->tree_path) {
        r->tree_nodes = x.tree.n;
        dp_tree_save(&x.tree, o->tree_path);
    }
    dp_tree_free(&x.tree);
    dp_set_free(&x.ctx_seen);
    dp_set_free(&x.exact_seen);
    for (uint32_t i = 0; i < x.nq; i++) {
        if (!lot_spill(&x, &x.q[i])) r->dropped_pending++;
        free(x.q[i].state);
    }
    if (x.lot) {
        r->lot_written = x.lot->written; r->lot_read = x.lot->read_back;
        r->lot_pending = dp_lf_pending(x.lot);
        dp_lf_close(x.lot, o->lot_path);
    }
    if (o->no_finalize) {   // secondary instance: the primary folds the raw findings in
        memcpy(r->z80_bank_seen_out, x.z80_bank_seen, sizeof x.z80_bank_seen);
        dp_mdmach_close(x.m);
        free(x.q); free(x.gen); free(x.vis); free(x.site); free(x.pad_sites); dp_set_free(&x.ind_sites); dp_set_free(&x.pad_full_seen); dp_set_free(&x.ind_pairs);
        return 0;
    }
    if (o->pre_finalize) o->pre_finalize(r, x.z80_bank_seen, o->pre_user);
    // S5 copy siblings: a longword table entry (read as data) naming the source of a
    // recorded copy of >= 16 bytes marks a table of streams; neighbouring entries
    // that point at unobserved ROM are sibling streams of the same length.
    // Counted (bitmap local to this pass); at most 64 entries a table.
    {
        uint8_t *claimed = calloc(c->size, 1);
        for (int k = 0; claimed && k < r->ncopy && k < DP_MD_COPY_CAP; k++) {
            uint32_t src = r->copies[k].src, len = r->copies[k].len * 2u;   // words -> bytes
            if (len < 16 || src >= c->size) continue;
            for (size_t t = 0; t + 4 <= c->size; t += 2) {
                if (!(r->flags[t] & DP_MDF_DATA)) continue;
                uint32_t v = (uint32_t)c->rom[t] << 24 | (uint32_t)c->rom[t + 1] << 16 | (uint32_t)c->rom[t + 2] << 8 | c->rom[t + 3];
                if (v != src) continue;
                size_t t0 = t, t1 = t + 4;
                unsigned n = 1;
                for (;;) {   // walk down
                    if (t0 < 4 || n >= 64) break;
                    uint32_t w = (uint32_t)c->rom[t0 - 4] << 24 | (uint32_t)c->rom[t0 - 3] << 16 | (uint32_t)c->rom[t0 - 2] << 8 | c->rom[t0 - 1];
                    if (w < 0x200 || w >= c->size || (w & 1) || r->flags[w]) break;
                    t0 -= 4; n++;
                }
                for (;;) {   // walk up
                    if (t1 + 4 > c->size || n >= 64) break;
                    uint32_t w = (uint32_t)c->rom[t1] << 24 | (uint32_t)c->rom[t1 + 1] << 16 | (uint32_t)c->rom[t1 + 2] << 8 | c->rom[t1 + 3];
                    if (w < 0x200 || w >= c->size || (w & 1) || r->flags[w]) break;
                    t1 += 4; n++;
                }
                if (n >= 64) r->sib_caps++;
                if (n < 2) continue;
                r->sib_tables++;
                for (size_t e = t0; e < t1; e += 4) {
                    uint32_t w = (uint32_t)c->rom[e] << 24 | (uint32_t)c->rom[e + 1] << 16 | (uint32_t)c->rom[e + 2] << 8 | c->rom[e + 3];
                    if (w == src) continue;
                    r->sib_entries++;
                    for (uint32_t q = w; q < w + len && q < c->size; q++)
                        if (!r->flags[q] && !claimed[q]) { claimed[q] = 1; r->sib_bytes++; }
                }
                break;
            }
        }
        free(claimed);
    }
    // S1/S2 static layer (no machine, tagged apart from observations)
    r->stat = calloc(c->size, 1);
    if (r->stat) md_static_scan(c->rom, c->size, r);
    // S7 map inference (render only, tagged apart from observations): unobserved
    // runs of one byte value (00/FF from 8 bytes, others from 32) are padding; a
    // gap of <= 3 bytes between executed bytes is code; a gap of <= 1024 bytes next
    // to observed bytes is data.
    r->inferred = calloc(c->size, 1);
    if (r->inferred) {
        const uint8_t M = DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DATA | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM;
        size_t n = c->size;
        for (size_t i = 0; i < n; i++) if (r->stat && r->stat[i] && !(r->flags[i] & M)) r->inferred[i] = 0xFF; /* placeholder: owned by the static layer */
        for (size_t i = 0; i < n;) {
            size_t j = i;
            while (j < n && c->rom[j] == c->rom[i] && !(r->flags[j] & M) && r->inferred[j] != 0xFF) j++;
            if (j - i >= ((c->rom[i] == 0 || c->rom[i] == 0xFF) ? 8u : 32u)) {
                memset(r->inferred + i, 1, j - i);
                r->inf_pad += j - i;
            }
            i = j > i ? j : i + 1;
        }
        for (size_t i = 0; i < n;) {
            if ((r->flags[i] & M) || r->inferred[i]) { i++; continue; }
            size_t j = i;
            while (j < n && !(r->flags[j] & M) && !r->inferred[j]) j++;
            size_t len = j - i;
            uint8_t lf = i ? r->flags[i - 1] : 0, rf = j < n ? r->flags[j] : 0, cl = 0;
            if (len <= 3 && (lf & (DP_MDF_NATURAL | DP_MDF_FORCED)) && (rf & (DP_MDF_NATURAL | DP_MDF_FORCED))) { cl = 2; r->inf_code += len; }
            else if (len <= 1024 && ((lf & M) || (rf & M))) { cl = 3; r->inf_data += len; }
            if (cl) memset(r->inferred + i, cl, len);
            i = j;
        }
        for (size_t i = 0; i < n; i++) if (r->inferred[i] == 0xFF) r->inferred[i] = 0;
    }
    // C1: sections = maximal executed runs; required when some byte ran on a natural path
    for (size_t i = 0; i < c->size;) {
        const uint8_t L = DP_MDF_NATURAL | DP_MDF_FORCED;
        if (!(r->flags[i] & L)) { i++; continue; }
        int nat = 0;
        while (i < c->size && (r->flags[i] & L)) { nat |= (r->flags[i] & DP_MDF_NATURAL) != 0; i++; }
        r->sec_n++;
        if (nat) r->sec_req++;
    }
    for (size_t i = 0; i < c->size; i++) {
        uint8_t f = r->flags[i];
        if (f & (DP_MDF_NATURAL | DP_MDF_FORCED)) r->exec_bytes++;
        if ((f & DP_MDF_FORCED) && !(f & DP_MDF_NATURAL)) r->forced_only_bytes++;
        if (f & DP_MDF_DATA) r->dread_bytes++;
        if (f & DP_MDF_DATA_NATURAL) r->natural_data_bytes++;
        if (f & DP_MDF_DMA_SOURCE) r->dma_source_bytes++;
        if (f & DP_MDF_DMA_READ) r->dma_read_bytes++;
        if (f & DP_MDF_CPU_VRAM) r->cpu_vram_bytes++;
        if (f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DATA | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM))
            r->observed_bytes++;
        if (f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM))
            r->code_or_vram_bytes++;
        else if (f & DP_MDF_DATA) r->data_only_bytes++;
        if ((f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM)) || r->nonexcluded_reads[i])
            r->filtered_observed_bytes++;
        if (!(f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM)) && r->nonexcluded_reads[i] == 2)
            r->replay_only_bytes++;
    }
    for (size_t i = 0; i < c->size; i++) {
        uint8_t z = r->z80_flags[i];
        if (z & DP_MDZ_COPIED) r->z80_copied_bytes++;
        if (z & DP_MDZ_CODE_RAM) r->z80_code_bytes++;
        if (z & DP_MDZ_EXEC_ROM) r->z80_exec_rom_bytes++;
        if (z & DP_MDZ_DATA) r->z80_data_bytes++;
        if (z & DP_MDZ_SAMPLE) r->z80_sample_bytes++;
        if (z & DP_MDZ_MUSIC) r->z80_music_bytes++;
    }
    for (unsigned b = 0; b < sizeof x.z80_bank_seen * 8; b++)
        if (x.z80_bank_seen[b >> 3] & (1u << (b & 7))) r->z80_banks_read++;
    dp_mdmach_close(x.m);
    free(x.q); free(x.gen); free(x.vis); free(x.site); free(x.pad_sites); dp_set_free(&x.ind_sites); dp_set_free(&x.pad_full_seen); dp_set_free(&x.ind_pairs);
    return 0;
}

void dp_md_explore_rtcode(const DpMdResults *r, const char *path) {
    FILE *f = fopen(path, "w");
    if (f) { dp_ramarch_write(&r->ra, f, "md"); fclose(f); }
}

void dp_md_results_free(DpMdResults *r) {
    dp_ramarch_free(&r->ra);
    free(r->inferred);
    free(r->stat);
    free(r->flags);
    free(r->exec_kind); r->exec_kind = NULL;
    free(r->nonexcluded_reads);
    free(r->exec_hits);
    free(r->z80_flags);
    free(r->copies);
    memset(r, 0, sizeof *r);
}

void dp_md_explore_report(const DpMdCart *c, const DpMdOptions *o, const DpMdResults *r, FILE *out) {
    fprintf(out, "deepeel megadrive (forking exploration)\n");
    fprintf(out, "rom: %s | %s | %zu bytes | region %s\n", c->console, c->overseas, c->size, c->region);
    fprintf(out, "reset pc: %06X  ssp: %08X\n", c->vec[1], c->vec[0]);
    fprintf(out, "bounds: total %llu steps, path %llu, queue %u, forks/site %u, visits/insn %u\n",
            (unsigned long long)o->max_total, (unsigned long long)o->path_steps, o->queue_cap, o->fork_cap,
            o->visit_cap);
    fprintf(out, "steps: %llu in %llu paths, forks %llu (irq %llu)\n", (unsigned long long)r->steps,
            (unsigned long long)r->paths, (unsigned long long)r->forks, (unsigned long long)r->irq_forks);
    fprintf(out, "pad probes: %llu (fourteen patterns per read site: twelve three-button, two six-button)\n", (unsigned long long)r->pad_forks);
    if (o->movie) fprintf(out, "GMV replay: %u / %u frames%s\n", r->movie_frames, o->movie->frames,
                          r->movie_frames == o->movie->frames ? "" : " (incomplete)");
    fprintf(out, "saturation (a non-zero line means the exploration is incomplete):\n");
    if (r->lot_written || r->lot_read || r->lot_pending)
        fprintf(out, "  lot: %llu written, %llu read back, %llu left on disk\n", (unsigned long long)r->lot_written,
                (unsigned long long)r->lot_read, (unsigned long long)r->lot_pending);
    if (r->vectors_seeded) fprintf(out, "  vector roots: %llu seeded (autovectors, TRAP #n)\n", (unsigned long long)r->vectors_seeded);
    fprintf(out, "  entries: %llu seeded, %llu refused (odd or outside the ROM)\n", (unsigned long long)r->entries_seeded, (unsigned long long)r->entries_refused);
    fprintf(out, "  frontier: parked %llu resumed %llu lost %llu\n", (unsigned long long)r->parked,
            (unsigned long long)r->parked_resumed, (unsigned long long)r->parked_lost);
    fprintf(out, "  total budget: %llu  pending forks unrun: %llu\n", (unsigned long long)r->sat_total,
            (unsigned long long)r->dropped_pending);
    fprintf(out, "  path budget: %llu  loop bound: %llu\n", (unsigned long long)r->sat_path,
            (unsigned long long)r->sat_visit);
    fprintf(out, "  queue full: %llu  per-site cap: %llu\n", (unsigned long long)r->sat_queue,
            (unsigned long long)r->sat_site);
    fprintf(out, "  queue growths: %llu (initial %u, ceiling %u)\n", (unsigned long long)r->queue_growths,
            o->queue_cap, o->queue_max > o->queue_cap ? o->queue_max : o->queue_cap);
    if (r->tree_nodes) fprintf(out, "  choice tree: %llu nodes\n", (unsigned long long)r->tree_nodes);
    if (r->tree_loaded) fprintf(out, "  choice tree resumed: %llu nodes reloaded\n", (unsigned long long)r->tree_loaded);
    if (r->ctx_evicted) fprintf(out, "  context eviction: %llu states closed (coarse pc/regs/SR context past the cap)\n", (unsigned long long)r->ctx_evicted);
    if (r->pass2_seeds || r->pass2_refused || r->pass2_capped) fprintf(out, "  second pass: %llu seeds, %llu refused, %llu capped\n", (unsigned long long)r->pass2_seeds, (unsigned long long)r->pass2_refused, (unsigned long long)r->pass2_capped);
    if (r->s4_tables) fprintf(out, "  jump tables (S4, jmp d(pc,Xn)): %llu tables, %llu handlers seeded, %llu refused\n", (unsigned long long)r->s4_tables, (unsigned long long)r->s4_seeds, (unsigned long long)r->s4_refused);
    if (r->disp_tables) fprintf(out, "  dispatch tables: %llu tables, %llu seeds, %llu refused targets\n", (unsigned long long)r->disp_tables, (unsigned long long)r->disp_seeds, (unsigned long long)r->disp_refused_view);
    if (r->state_exported) fprintf(out, "  state exported: deepest cell, %u frames from power-on\n", r->state_exported_depth);
    if (r->exact_dups) fprintf(out, "  exact dedup: %llu forks dropped (same clock-free machine)\n", (unsigned long long)r->exact_dups);
    if (r->demo_run) fprintf(out, "  demo (no button pressed, from power-on): %llu frames, %llu ROM bytes executed\n", (unsigned long long)r->demo_run, (unsigned long long)r->demo_exec);
    if (r->pool_saved) fprintf(out, "  checkpoint pool: %llu kept, %llu sessions started from one\n", (unsigned long long)r->pool_saved, (unsigned long long)r->pool_starts);
    if (r->play_sessions_run) fprintf(out, "  played sessions: %llu, %llu frames\n", (unsigned long long)r->play_sessions_run, (unsigned long long)r->play_frames_run);
    if (r->watch_frames) fprintf(out, "  watch: %llu frames published\n", (unsigned long long)r->watch_frames);
    if (r->novelty_trials)
        fprintf(out, "  novelty: depth %u frames, %llu trials, %llu frames, %llu hits, %llu deaths, %llu archived (%llu replaced), %llu seeded, %llu stalled\n",
                r->novelty_depth, (unsigned long long)r->novelty_trials, (unsigned long long)r->novelty_frames_run,
                (unsigned long long)r->novelty_hits, (unsigned long long)r->novelty_deaths,
                (unsigned long long)r->novelty_archived, (unsigned long long)r->novelty_replaced, (unsigned long long)r->novelty_seeded, (unsigned long long)r->novelty_stalled);
    if (r->jobs_folded) fprintf(out, "  jobs: %u secondary instance folded (entries split even/odd)\n", r->jobs_folded);
    fprintf(out, "  static layer (S1/S2, no machine): %llu padding, %llu table, %llu code bytes, %llu calls with lost confidence\n  map inference (S7, not observations): %llu padding, %llu code, %llu data bytes\n", (unsigned long long)r->stat_pad, (unsigned long long)r->stat_table, (unsigned long long)r->stat_code, (unsigned long long)r->stat_conf_lost, (unsigned long long)r->inf_pad, (unsigned long long)r->inf_code, (unsigned long long)r->inf_data);
    fprintf(out, "  copy siblings: %llu tables, %llu entries, %llu bytes, %llu table caps\n", (unsigned long long)r->sib_tables, (unsigned long long)r->sib_entries, (unsigned long long)r->sib_bytes, (unsigned long long)r->sib_caps);
    fprintf(out, "  padding cuts: %llu\n", (unsigned long long)r->pad_cuts);
    fprintf(out, "  indirect jumps: %llu sites, %llu targets, overflow %llu\n", (unsigned long long)r->indirect_sites, (unsigned long long)r->indirect_targets, (unsigned long long)r->indirect_overflow);
    fprintf(out, "  copy records dropped: %llu\n", (unsigned long long)r->copy_overflow);
    fprintf(out, "  stream replay: step cap %llu  total budget %llu  consumers dropped %llu  starts dropped %llu  call frames dropped %llu\n",
            (unsigned long long)r->sat_replay_steps, (unsigned long long)r->sat_replay_total,
            (unsigned long long)r->decoder_overflow, (unsigned long long)r->start_overflow,
            (unsigned long long)r->frame_overflow);
    fprintf(out, "not losses: alternative already seen %llu, alternative outside rom %llu\n",
            (unsigned long long)r->fork_seen, (unsigned long long)r->fork_off_rom);
    if (r->faults) {
        fprintf(out, "paths ended by a machine fault: %llu (unrecorded: %llu)\n",
                (unsigned long long)r->faults, (unsigned long long)r->fault_sites_overflow);
        for (unsigned i = 0; i < r->nfault_sites; i++) {
            const DpMdFaultSite *f = &r->fault_sites[i];
            fprintf(out, "  code %d instruction %06X machine-pc %06X count %llu (natural %llu)\n",
                    f->code, f->pc, f->fault_pc, (unsigned long long)f->count, (unsigned long long)f->natural);
            fprintf(out, "    sample trace:");
            for (unsigned j = 0; j < f->trace_len; j++) fprintf(out, " %06X", f->trace[j]);
            fputc('\n', out);
            if (o->watch_write <= 0xFFFFFFu) {
                if (f->watch_pc == UINT32_MAX) fprintf(out, "    watched write %06X: none on this path\n", o->watch_write);
                else fprintf(out, "    watched write %06X: pc %06X addr %06X size %u value %08X\n",
                             o->watch_write, f->watch_pc, f->watch_addr, f->watch_size, f->watch_value);
                if (f->watch_pc != UINT32_MAX)
                    fprintf(out, "    at write: D0=%08X A1=%08X A2=%08X\n", f->watch_d0, f->watch_a1, f->watch_a2);
            }
            fprintf(out, "    sample address registers (value@last-change):");
            for (unsigned j = 0; j < 8; j++) {
                fprintf(out, " A%u=%08X@", j, f->an[j]);
                if (f->an_writer[j] == UINT32_MAX) fprintf(out, "reset");
                else fprintf(out, "%06X", f->an_writer[j]);
            }
            fputc('\n', out);
        }
        fprintf(out, "fault origins (unrecorded: %llu):\n", (unsigned long long)r->fault_origins_overflow);
        for (unsigned i = 0; i < r->nfault_origins; i++) {
            const DpMdFaultSite *f = &r->fault_origins[i];
            fprintf(out, "  code %d instruction %06X machine-pc %06X origin ", f->code, f->pc, f->fault_pc);
            if (f->origin == UINT32_MAX) fprintf(out, "reset");
            else fprintf(out, "%06X", f->origin);
            fprintf(out, " count %llu (natural %llu)\n", (unsigned long long)f->count, (unsigned long long)f->natural);
        }
    }
    fprintf(out, "executed rom bytes: %u (%.1f%%), of which only via forks: %u; data-read: %u\n", r->exec_bytes,
            100.0 * r->exec_bytes / r->rom_size, r->forced_only_bytes, r->dread_bytes);
    fprintf(out, "sections: %u executed runs, %u required (natural path), %u forced only\n", r->sec_n, r->sec_req, r->sec_n - r->sec_req);
    fprintf(out, "data-read on natural path: %u; commanded DMA source bytes: %u (may include an interrupted transfer)\n",
            r->natural_data_bytes, r->dma_source_bytes);
    fprintf(out, "DMA ROM bytes actually read: %u\n", r->dma_read_bytes);
    fprintf(out, "CPU MOVE.W ROM->VRAM (direct or consecutive register move): %llu words, %u distinct source bytes\n",
            (unsigned long long)r->cpu_vram_words, r->cpu_vram_bytes);
    fprintf(out, "observed union (execution, CPU reads, DMA reads, VRAM copies): %u / %zu (%.2f%%)\n",
            r->observed_bytes, r->rom_size, 100.0 * r->observed_bytes / r->rom_size);
    fprintf(out, "  code or observed VRAM source: %u (%.2f%%); CPU-read-only: %u; unobserved: %zu\n",
            r->code_or_vram_bytes, 100.0 * r->code_or_vram_bytes / r->rom_size,
            r->data_only_bytes, r->rom_size - r->observed_bytes);
    fprintf(out, "  CPU reads include integrity scans; observed does not mean played or understood.\n");
    if (o->exclude_read_pc <= 0xFFFFFFu)
        fprintf(out, "observed union excluding CPU reads from pc %06X: %u / %zu (%.2f%%)\n",
                o->exclude_read_pc, r->filtered_observed_bytes, r->rom_size,
                100.0 * r->filtered_observed_bytes / r->rom_size);
    fprintf(out, "stream replay: %u consumers (%u without entry snapshot), %llu tables, %llu candidates, %llu runs, %llu steps\n",
            r->decoders, r->decoders_without_snapshot, (unsigned long long)r->replay_tables,
            (unsigned long long)r->replay_candidates, (unsigned long long)r->replay_runs,
            (unsigned long long)r->replay_steps);
    fprintf(out, "  bytes read only by replays: %u (%.2f%%); faults %llu\n", r->replay_only_bytes,
            100.0 * r->replay_only_bytes / r->rom_size, (unsigned long long)r->replay_faults);
    fprintf(out, "ROM read sites: %u (unrecorded accesses: %llu)\n", r->nreaders,
            (unsigned long long)r->reader_overflow);
    for (unsigned i = 0; i < r->nreaders; i++) {
        const DpMdReader *p = &r->readers[i];
        fprintf(out, "  pc %06X range %06X-%06X accesses %llu (natural %llu)\n", p->pc, p->lo, p->hi,
                (unsigned long long)p->accesses, (unsigned long long)p->natural_accesses);
    }
    fprintf(out, "exec outside rom: %llu (ram: %llu)\n", (unsigned long long)r->exec_outside,
            (unsigned long long)r->ram_exec);
    if (r->ra.n) fprintf(out, "runtime code archive: %u variants, %u attached to a ROM copy source, %u generated with an approximate ROM producer, %u rewrote an earlier variant\n",
            r->ra.n, r->ra.attached, r->ra.derived, r->ra.poly);
    if (r->smc_writes) fprintf(out, "self-modifying RAM code: %llu stores into executed bytes (first at pc %06X)\n",
                               (unsigned long long)r->smc_writes, r->smc_first_pc);
    fprintf(out, "SRAM: %llu instructions, %llu CPU reads\n",
            (unsigned long long)r->sram_exec, (unsigned long long)r->sram_reads);
    fprintf(out, "decoder/execution length mismatches: %llu\n", (unsigned long long)r->decode_length_mismatches);
    fprintf(out, "registers:\n");
    for (int i = 0; i < dp_md_reg_count && i < 64; i++) {
        if (!r->reg_r[i] && !r->reg_w[i]) continue;
        if (!strcmp(dp_md_regs[i].category, "memory")) continue;
        fprintf(out, "  %06X %-12s %-6s r=%llu w=%llu\n", dp_md_regs[i].lo, dp_md_regs[i].name,
                dp_md_regs[i].category, (unsigned long long)r->reg_r[i], (unsigned long long)r->reg_w[i]);
    }
    if (r->unmapped_r || r->unmapped_w)
        fprintf(out, "  unmapped: r=%llu w=%llu\n", (unsigned long long)r->unmapped_r,
                (unsigned long long)r->unmapped_w);
    fprintf(out, "z80 sound cpu: %llu steps (RAM %llu, bank window %llu, other %llu); inside the 68000 budget, not counted in it\n",
            (unsigned long long)r->z80_steps, (unsigned long long)r->z80_exec_ram,
            (unsigned long long)r->z80_exec_bank, (unsigned long long)r->z80_exec_other);
    fprintf(out, "  ROM bytes copied to Z80 RAM by the 68000: %u (%llu stores); executed there (code-ramcopy): %u\n",
            r->z80_copied_bytes, (unsigned long long)r->z80_ram_copied, r->z80_code_bytes);
    fprintf(out, "  executed from the bank window: %u bytes; data read through it: %u bytes in %u bank(s) of 32 KiB (%llu reads)\n",
            r->z80_exec_rom_bytes, r->z80_data_bytes, r->z80_banks_read, (unsigned long long)r->z80_rom_reads);
    fprintf(out, "  YM2612 stores %llu, PSG stores %llu; ROM bytes typed sample (DAC): %u, music (FM/PSG data): %u\n",
            (unsigned long long)r->z80_ym_writes, (unsigned long long)r->z80_psg_writes,
            r->z80_sample_bytes, r->z80_music_bytes);
    fprintf(out, "rom->vdp copies: %d\n", r->ncopy);
    for (int i = 0; i < r->ncopy; i++)
        fprintf(out, "  pc %06X src %06X len %u words code %02X\n", r->copies[i].pc, r->copies[i].src,
                r->copies[i].len, r->copies[i].code);
}

// One class per ROM byte (same digits as the NES map, DP_NMAP_*): observed
// evidence first, then S7 inferences; nothing inferred overrides an observation.
uint8_t *dp_md_map_build(const DpMdResults *r) {
    if (!r || !r->flags) return NULL;
    uint8_t *m = malloc(r->rom_size ? r->rom_size : 1);
    if (!m) return NULL;
    for (size_t i = 0; i < r->rom_size; i++) {
        uint8_t f = r->flags[i], c;
        if (f & DP_MDF_NATURAL) c = 1;                         // code-natural
        else if (f & DP_MDF_FORCED) c = 2;                     // code-forced
        else if (f & (DP_MDF_DMA_READ | DP_MDF_CPU_VRAM)) c = 4;   // data-copied
        else if (f & DP_MDF_DATA) c = 5;                       // data-read
        else if (r->stat && r->stat[i] == 3) c = 3;            // code-static (S2)
        else if (r->stat && r->stat[i] == 7) c = 7;            // table (S1)
        else if (r->stat && r->stat[i] == 8) c = 8;            // padding (S1)
        else if (r->inferred && r->inferred[i] == 1) c = 11;   // inferred-padding
        else if (r->inferred && r->inferred[i] == 2) c = 10;   // inferred-code
        else if (r->inferred && r->inferred[i] == 3) c = 9;    // inferred-data
        else c = 0;
        m[i] = c;
    }
    return m;
}

int dp_md_explore_map(const DpMdResults *r, const char *path) {
    uint8_t *m = dp_md_map_build(r);
    if (!m) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(m); return -1; }
    fputs("md map v1\n", f);
    for (size_t i = 0; i < r->rom_size; i++) fputc('0' + m[i], f);   // same encoding as the NES map
    fputc('\n', f);
    int rc = 0;
    if (fclose(f)) rc = -1;
    free(m);
    return rc;
}

int dp_md_explore_summary(const DpMdResults *r, const char *path) {
    uint8_t *m = dp_md_map_build(r);
    if (!m) return -1;
    static const char *const nm[12] = { "unknown", "code-natural", "code-forced", "code-static", "data-copied",
        "data-read", "data-static", "table", "padding", "inferred-data", "inferred-code", "inferred-padding" };
    uint64_t cls[12] = { 0 };
    for (size_t i = 0; i < r->rom_size; i++) cls[m[i] < 12 ? m[i] : 0]++;
    free(m);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\"deepeel\":\"%s\",\"platform\":\"md\",\"rom_size\":%zu,\"classes\":{", DEEPEEL_VERSION, r->rom_size);
    for (int k = 0; k < 12; k++) fprintf(f, "%s\"%s\":%llu", k ? "," : "", nm[k], (unsigned long long)cls[k]);
    fprintf(f, "},\"known_pct\":%.3f,\"counters\":{\"steps\":%llu,\"paths\":%llu,\"forks\":%llu,"
               "\"sat_total\":%llu,\"sat_path\":%llu,\"sat_visit\":%llu,\"sat_queue\":%llu,\"sat_site\":%llu,"
               "\"exec_bytes\":%llu,\"forced_only_bytes\":%llu,\"sections\":%u,\"sections_required\":%u}}\n",
            r->rom_size ? 100.0 * (double)(r->rom_size - cls[0]) / (double)r->rom_size : 0.0,
            (unsigned long long)r->steps, (unsigned long long)r->paths, (unsigned long long)r->forks,
            (unsigned long long)r->sat_total, (unsigned long long)r->sat_path, (unsigned long long)r->sat_visit,
            (unsigned long long)r->sat_queue, (unsigned long long)r->sat_site,
            (unsigned long long)r->exec_bytes, (unsigned long long)r->forced_only_bytes, r->sec_n, r->sec_req);
    return fclose(f) ? -1 : 0;
}

// Heat (O1): played usage per 256-byte zone, natural paths only (forks excluded).
int dp_md_explore_heat(const DpMdResults *r, const char *path) {
    if (!r->exec_hits) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    uint64_t total = 0;
    for (size_t i = 0; i < r->rom_size; i++) total += r->exec_hits[i];
    fprintf(f, "# deepeel md heat v1: played instruction starts per 256-byte zone (natural paths only)\n# exec %llu instructions\n# columns: start end(inclusive) exec\n", (unsigned long long)total);
    for (size_t z = 0; z < r->rom_size; z += 256) {
        uint64_t n = 0;
        size_t e = z + 256 < r->rom_size ? z + 256 : r->rom_size;
        for (size_t i = z; i < e; i++) n += r->exec_hits[i];
        fprintf(f, "%06zx %06zx %llu\n", z, e - 1, (unsigned long long)n);
    }
    return fclose(f) ? -1 : 0;
}

int dp_md_explore_evidence(const DpMdResults *r, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# executed ranges (N=natural path, F=only via a forced fork)\n");
    size_t i = 0;
    while (i < r->rom_size) {
        uint8_t m = r->flags[i] & (DP_MDF_NATURAL | DP_MDF_FORCED);
        if (!m) { i++; continue; }
        int nat = 0;
        size_t j = i;
        while (j < r->rom_size && (r->flags[j] & (DP_MDF_NATURAL | DP_MDF_FORCED))) {
            if (r->flags[j] & DP_MDF_NATURAL) nat = 1;
            j++;
        }
        fprintf(f, "exec %06zX-%06zX %c\n", i, j - 1, nat ? 'N' : 'F');
        i = j;
    }
    static const struct { uint8_t bit; const char *name; } zk[] = {
        {DP_MDZ_CODE_RAM, "z80-code-ramcopy"}, {DP_MDZ_EXEC_ROM, "z80-code-rom"}, {DP_MDZ_SAMPLE, "z80-sample"},
        {DP_MDZ_MUSIC, "z80-music"}, {DP_MDZ_DATA, "z80-data"}, {DP_MDZ_COPIED, "z80-ram-source"}};
    for (unsigned k = 0; k < sizeof zk / sizeof *zk; k++) {
        size_t a = 0;
        while (a < r->rom_size) {
            if (!(r->z80_flags[a] & zk[k].bit)) { a++; continue; }
            size_t b = a;
            while (b < r->rom_size && (r->z80_flags[b] & zk[k].bit)) b++;
            fprintf(f, "%s %06zX-%06zX\n", zk[k].name, a, b - 1);
            a = b;
        }
    }
    for (int k = 0; k < r->ncopy; k++)
        fprintf(f, "copy pc=%06X src=%06X words=%u code=%02X\n", r->copies[k].pc, r->copies[k].src,
                r->copies[k].len, r->copies[k].code);
    fclose(f);
    return 0;
}
