// explore.c: the bounded PC Engine explorer over m0boy's machine (through
// the pcemach bridge).  Same contract as the GB, Z80 and NES explorers:
//
//   - both sides of every conditional are explored (flag branches by
//     flipping the flag, BBR/BBS by flipping the zero-page bit), capped per
//     site (fork_cap);
//   - pad reads ($1000) fork the input catalog, capped per site;
//   - IRQ forks raise the VDC vblank / timer sources at the points where the
//     CPU unmasks them (CLI, PLP, RTI, $1402 writes), capped per site;
//   - play sessions (random held inputs) and TAS movies run first; their
//     checkpoints seed the fork queue;
//   - eviction keeps it bounded: exact-state dedup, per-path step budget,
//     per-path back-branch cap, global step budget, queue cap.  Every
//     saturation lands in a counter.
//
// Everything observed lands on physical HuCard cells (page * $2000 +
// (addr & $1FFF), which is the house bank * $4000 + (addr & $3FFF) index);
// execution outside ROM (WRAM trampolines) counts as exec_outside.
//
// After the dynamic passes, a static closure follows the decoded flow of the
// observed code inside each page's observed window, then padding and gap
// inference classify what is left (reported apart from the observations).
#include "explore.h"
#include "../../explore/stop.h"
#include "../../explore/lots.h"
#include "../../core/hashset.h"
#include "../../explore/tree.h"
#include "pce.h"
#include "tas.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../../gui/fbwatch.h"
#include "../../core/statefile.h"

typedef struct {
    uint8_t *z;            // compressed bridge state
    uLongf zlen;
    uint8_t forced;
    uint32_t choice;       // choice-tree node (0: none)
} PceQueued;

enum { LOOP_SLOTS = 4096 };
typedef struct { uint64_t key; uint32_t count; } PceLoop;

static unsigned g_watch_every;
static DpPceResults *g_watch_r;
static uint64_t g_watch_n;
static void watch_frame_hook(void) {
    if (!g_watch_every || ++g_watch_n % g_watch_every) return;
    static uint32_t px[352 * 256];
    int w, h;
    if (!dp_pce_render(px, 352 * 256, &w, &h)) return;
    dp_fb_publish(px, w, h, g_watch_n);
    g_watch_r->watch_frames++;
}

typedef struct {
    DpPceOptions opt;
    DpPceResults *r;
    const uint8_t *rom;
    size_t rom_size;
    PceQueued *queue;
    uint32_t queue_len;
    PceQueued *park; uint32_t park_len, park_cap; // deferred holding area (queue full)
    DpLotFile *lot;            // disk lot: park overflow, wall leftovers, --lot resume
    uint64_t *seen;        // open-addressed dedup set
    int disp_done;
    DpTree tree;           // --tree: every queued state is a choice
    uint32_t cur_choice;
    DpHashSet ind_sites, ind_pairs;   // observed indirect jumps (D12)
    DpHashSet ctx_seen;    // per-context admission counters (C2)
    size_t seen_cap, seen_len;
    uint8_t *fork_sites, *pad_sites, *irq_sites; // per-cell counters (saturating)
    uint8_t *istart;       // instruction starts (for the static closure)
    uint8_t page_win[128]; // a page's observed CPU window (0xFF: none)
    PceLoop loops[LOOP_SLOTS];
    uint64_t total_steps, path_steps, path_id;
    int mode;              // 0 fork exploration, 1 play/TAS
    int forced;            // current path entered through a flipped side
    uint16_t cur_pc;
    uint8_t cur_len, cur_op;
    uint8_t ram_exec[0x8000];  // WRAM bytes executed since their last write (SMC tracking)
    int32_t ram_src[0x8000];   // last ROM cell whose read fed the store to each WRAM byte (-1 none)
    int64_t *prov_anchor; uint32_t prov_len; // table-word cells that fed an indirect jmp pointer (S3)
    int64_t last_read_cell;
    uint64_t last_read_step;
    int was_outside;
    uint8_t *tmp, *tmp2;   // raw bridge states
    uint8_t *ztmp;
    uLongf zcap;
    size_t state_size;
    uint32_t rng;
    uint64_t novel;        // cells observed for the first time (novelty search)
    int failed;
    struct PStm *stm;      // S6 decoder replay state
} PceRun;

#define OBS_MASK (DP_PCE_NATURAL | DP_PCE_FORCED | DP_PCE_DATA | DP_PCE_COPIED)
static inline void mark(PceRun *x, size_t cell, uint8_t bit) {
    uint8_t *f = &x->r->flags[cell];
    if (!(*f & OBS_MASK)) x->novel++;
    *f |= bit;
}

// --- logs ---------------------------------------------------------------------

static void log_interrupt(PceRun *x, uint8_t kind) {
    DpPceResults *r = x->r;
    r->irq_counts[kind]++;
    if (r->interrupt_count < DP_PCE_IRQ_LOG_CAP) {
        DpPceInterrupt *i = &r->interrupts[r->interrupt_count++];
        i->path = x->path_id;
        i->step = x->total_steps;
        i->pc = x->cur_pc;
        i->kind = kind;
    } else {
        r->interrupt_overflow++;
    }
}

static void log_bank(PceRun *x, uint8_t window, uint8_t old_page, uint8_t new_page) {
    DpPceResults *r = x->r;
    if (r->bank_count < DP_PCE_BANK_LOG_CAP) {
        DpPceBankEvent *b = &r->banks[r->bank_count];
        b->seq = r->bank_count + 1;
        b->path = x->path_id;
        b->step = x->total_steps;
        b->pc = x->cur_pc;
        b->window = window;
        b->old_page = old_page;
        b->new_page = new_page;
        r->bank_count++;
    } else {
        r->bank_overflow++;
    }
}

static void log_copy(PceRun *x, uint32_t cell, uint8_t channel) {
    DpPceResults *r = x->r;
    r->copied_bytes++;
    // coalesce: same pc and channel, the next cell of the previous run
    for (unsigned k = r->copy_count; k > 0 && k + 8 > r->copy_count; k--) {
        DpPceCopy *c = &r->copies[k - 1];
        // one stream: a decompressor alternates store sites and skips its
        // control bytes, so the run continues within a few cells whatever
        // the pc (same channel)
        if (c->channel == channel) {
            uint32_t end = c->source + c->length;
            if (cell >= end && cell <= end + 8) { c->length = cell - c->source + 1; return; }
            if (cell >= c->source && cell < end) { c->occurrences++; return; }
        }
    }
    if (x->r->flags[cell] & DP_PCE_COPIED) return; // a replay of a recorded stream
    if (r->copy_count < DP_PCE_COPY_CAP) {
        DpPceCopy *c = &r->copies[r->copy_count++];
        c->path = x->path_id;
        c->first_step = x->total_steps;
        c->occurrences = 1;
        c->source = cell;
        c->length = 1;
        c->pc = x->cur_pc;
        c->channel = channel;
    } else {
        r->copy_overflow++;
    }
}

// --- bus observer -----------------------------------------------------------------

// abs,X / abs,Y data-read opcodes of the 6502 family (HuC6280 included)
static int indexed_read_op(uint8_t op) {
    switch (op) {
    case 0xBD: case 0xB9: case 0xBE: case 0xBC: // lda,x lda,y ldx,y ldy,x
    case 0x7D: case 0x79: case 0xFD: case 0xF9: // adc sbc
    case 0x3D: case 0x39: case 0x1D: case 0x19: // and ora
    case 0x5D: case 0x59: case 0xDD: case 0xD9: // eor cmp
        return 1;
    }
    return 0;
}

// Records the index of an abs,X/Y read against its operand base. The base is
// proven (it is the operand); the index is bounded by 8 bits.
static void index_note(PceRun *x, uint32_t cell, uint16_t addr) {
    DpPceResults *r = x->r;
    if (!indexed_read_op(x->cur_op)) return;
    int64_t c1 = dp_pce_rom_index((uint16_t)(x->cur_pc + 1)), c2 = dp_pce_rom_index((uint16_t)(x->cur_pc + 2));
    int64_t site = dp_pce_rom_index(x->cur_pc);
    if (c1 < 0 || c2 < 0 || site < 0 || (size_t)c2 >= x->rom_size) return;
    uint16_t base = (uint16_t)(x->rom[c1] | x->rom[c2] << 8);
    uint16_t idx = (uint16_t)(addr - base);
    if (idx > 255) return;
    // a forced fork can run an index loop far past what the game does: the
    // range comes from natural paths only
    if (x->forced) { r->idx_forced_ignored++; return; }
    if ((base >> 13) != (addr >> 13)) { r->idx_cross_page++; return; }
    int32_t cell0 = (int32_t)cell - idx;
    unsigned k;
    for (k = 0; k < r->nidx; k++)
        if (r->idx[k].site == (uint32_t)site && r->idx[k].base == base && r->idx[k].cell0 == cell0) break;
    if (k == r->nidx) {
        if (k >= DP_PCE_IDX_CAP) { r->idx_overflow++; return; }
        r->idx[k] = (DpPceIndexed){ (uint32_t)site, base, cell0, idx, idx, 0 };
        r->nidx++;
    }
    DpPceIndexed *e = &r->idx[k];
    if (idx < e->idx_min) e->idx_min = idx;
    if (idx > e->idx_max) e->idx_max = idx;
    e->n_obs++;
}

// Declares the unobserved elements between the lowest and the highest index
// seen at each site. Never below idx_min (a biased base such as tbl-1 starts
// at index 1; its element 0 lies before the table), never above idx_max
// without a proven bound, never over executed code, never past the 8 KiB page.
static void index_declare(PceRun *x) {
    DpPceResults *r = x->r;
    if (!r->idx_flags) return;
    for (unsigned k = 0; k < r->nidx; k++) {
        const DpPceIndexed *e = &r->idx[k];
        if (e->idx_min >= 1) r->idx_biased++;
        unsigned room = 0x2000u - (e->base & 0x1FFFu);
        for (unsigned i = e->idx_min; i <= e->idx_max && i < room; i++) {
            int64_t c = (int64_t)e->cell0 + i;
            if (c < 0 || (size_t)c >= x->rom_size) continue;
            uint8_t f = r->flags[c];
            if (f & (DP_PCE_DATA | DP_PCE_COPIED)) continue;     // already observed
            if (f & (DP_PCE_NATURAL | DP_PCE_FORCED | DP_PCE_STATIC)) { r->idx_refused_code++; continue; }
            if (!r->idx_flags[c]) { r->idx_flags[c] = 1; r->idx_declared++; }
        }
    }
}

static void pstm_read(PceRun *x, unsigned a);
static void obs_rom_read(void *user, uint32_t cell, uint16_t addr) {
    PceRun *x = user;
    if (addr >= x->cur_pc && (unsigned)addr < (unsigned)x->cur_pc + x->cur_len) return; // fetch
    if (cell >= x->rom_size) return;
    index_note(x, cell, addr);
    x->r->data_reads++;
    mark(x, cell, DP_PCE_DATA);
    x->last_read_cell = cell;
    x->last_read_step = x->total_steps;
    if (x->mode == 0) pstm_read(x, addr);
}

static int is_block(uint8_t op) {
    return op == 0x73 || op == 0xC3 || op == 0xD3 || op == 0xE3 || op == 0xF3;
}

static void obs_io_write(void *user, uint16_t reg, uint8_t value) {
    PceRun *x = user;
    DpPceResults *r = x->r;
    r->io_writes++;
    if (r->register_count < DP_PCE_REG_LOG_CAP) {
        DpPceRegisterWrite *w = &r->register_writes[r->register_count++];
        w->path = x->path_id;
        w->step = x->total_steps;
        w->pc = x->cur_pc;
        w->reg = reg;
        w->value = value;
    } else {
        r->register_overflow++;
    }
    // the VCE colour-data port ($0404-$0405): a ROM byte copied here is
    // palette data (typed apart from the VRAM copies)
    if ((reg & 0x1C00) == 0x0400 && (reg & 7) >= 4 && (reg & 7) <= 5) {
        r->vce_data_writes++;
        if (x->last_read_cell >= 0) {
            int block = is_block(x->cur_op);
            uint64_t age = x->total_steps - x->last_read_step;
            if ((block && age == 0) ||
                (!block && age <= 3 && x->rom[x->last_read_cell] == value)) {
                if (r->pal_flags && !r->pal_flags[x->last_read_cell]) {
                    r->pal_flags[x->last_read_cell] = 1;
                    r->palette_bytes++;
                }
                log_copy(x, (uint32_t)x->last_read_cell, block ? 4 : 3);
                mark(x, (size_t)x->last_read_cell, DP_PCE_COPIED);
                x->last_read_cell = -1;
            }
        }
    }
    // the VDC data port: a ROM byte read just before and written here is a
    // ROM -> VRAM copy (block moves: the read of this very transfer)
    if ((reg & 0x1C00) == 0 && (reg & 3) >= 2) {
        r->vdc_data_writes++;
        if (x->last_read_cell >= 0) {
            int block = is_block(x->cur_op);
            uint64_t age = x->total_steps - x->last_read_step;
            if ((block && age == 0) ||
                (!block && age <= 3 && x->rom[x->last_read_cell] == value)) {
                log_copy(x, (uint32_t)x->last_read_cell, block ? 2 : 1);
                mark(x, (size_t)x->last_read_cell, DP_PCE_COPIED);
                x->last_read_cell = -1;
            }
        }
    }
}

// --- queue / dedup --------------------------------------------------------------

static int seen_insert(PceRun *x, uint64_t h) {
    if (!h) h = 1;
    if (x->seen_len * 2 >= x->seen_cap) {
        size_t nc = x->seen_cap ? x->seen_cap * 2 : 1 << 16;
        uint64_t *n = calloc(nc, sizeof *n);
        if (!n) { x->failed = 1; return 0; }
        for (size_t i = 0; i < x->seen_cap; i++)
            if (x->seen[i]) {
                size_t j = x->seen[i] & (nc - 1);
                while (n[j]) j = (j + 1) & (nc - 1);
                n[j] = x->seen[i];
            }
        free(x->seen);
        x->seen = n;
        x->seen_cap = nc;
    }
    size_t j = h & (x->seen_cap - 1);
    while (x->seen[j]) {
        if (x->seen[j] == h) return 0;
        j = (j + 1) & (x->seen_cap - 1);
    }
    x->seen[j] = h;
    x->seen_len++;
    return 1;
}

// one compressed state to the disk lot (takes z on success)
static int lot_spill(PceRun *x, uint8_t *z, uLongf zlen, uint8_t forced) {
    if (!x->lot) return 0;
    uLongf rl = x->state_size;
    if (uncompress(x->tmp2, &rl, z, zlen) != Z_OK || rl != x->state_size) return 0;
    if (!dp_lf_put(x->lot, &forced, 1, x->tmp2, (uint32_t)rl)) return 0; // raw state: XOR deltas need fixed-size images
    free(z);
    return 1;
}

// queue the live machine state (dedup on its control-state hash)
static int push_live(PceRun *x, int forced) {
    DpPceResults *r = x->r;
    uint32_t node = 0;
    if (x->opt.tree_path) {
        node = dp_tree_add(&x->tree, x->cur_choice, 4 /* fork */, 0, 0, dp_pce_cpu().pc);
    }
    if (!seen_insert(x, dp_pce_state_hash())) { r->evicted++; if (node) dp_tree_close(&x->tree, node, DP_ISSUE_MERGED); return 0; }
    if (x->opt.ctx_cap) {
        // coarse same-context eviction: registers, pc, MPR and interrupt mask; the
        // k-th admission of a context is the key hash(ctx, k), past ctx_cap it is closed
        DpPceCpu c = dp_pce_cpu();
        uint64_t h = 0xcbf29ce484222325ull;
        uint8_t raw[16] = { c.a, c.x, c.y, c.s, c.p, (uint8_t)c.pc, (uint8_t)(c.pc >> 8), c.irq_mask };
        memcpy(raw + 8, c.mpr, 8);
        for (unsigned i = 0; i < sizeof raw; i++) h = (h ^ raw[i]) * 0x100000001b3ull;
        unsigned k = 0;
        while (k < x->opt.ctx_cap && dp_set_insert(&x->ctx_seen, (h ^ (k * 0x9E3779B97F4A7C15ull)) | 1) == 0) k++;
        if (k >= x->opt.ctx_cap) { r->ctx_evicted++; if (node) dp_tree_close(&x->tree, node, DP_ISSUE_CAPPED); return 0; }
    }
    int full = x->queue_len >= x->opt.queue_cap;
    dp_pce_save(x->tmp2);
    uLongf zl = x->zcap;
    if (compress2(x->ztmp, &zl, x->tmp2, x->state_size, 1) != Z_OK) { x->failed = 1; return 0; }
    uint8_t *z = malloc(zl);
    if (!z) { x->failed = 1; return 0; }
    memcpy(z, x->ztmp, zl);
    if (full && x->park_len >= 4096) { // the holding area is full: the state goes to the disk lot
        if (lot_spill(x, z, zl, (uint8_t)forced)) { r->parked++; if (node) dp_tree_close(&x->tree, node, DP_ISSUE_SPOOLED); return 1; }
        free(z); r->queue_cuts++; r->parked_lost++; return 0;
    }
    if (full) { // a full queue is not a loss: the state waits aside (bounded, counted)
        if (x->park_len == x->park_cap) {
            uint32_t nc = x->park_cap ? x->park_cap * 2 : 64;
            PceQueued *g = realloc(x->park, nc * sizeof *g);
            if (!g) { free(z); r->queue_cuts++; r->parked_lost++; return 0; }
            x->park = g; x->park_cap = nc;
        }
        x->park[x->park_len].z = z; x->park[x->park_len].zlen = zl; x->park[x->park_len].forced = (uint8_t)forced;
        x->park[x->park_len].choice = node;
        x->park_len++; r->parked++;
        return 1;
    }
    x->queue[x->queue_len].z = z;
    x->queue[x->queue_len].zlen = zl;
    x->queue[x->queue_len].forced = (uint8_t)forced;
    x->queue[x->queue_len].choice = node;
    x->queue_len++;
    r->state_bytes_peak += zl;
    return 1;
}

// fork: x->tmp holds the pre-mutation state, the live machine carries the
// mutation; queue it, then restore
static int fork_end(PceRun *x, int forced) {
    int ok = push_live(x, forced);
    dp_pce_load(x->tmp);
    return ok;
}

// --- forks ----------------------------------------------------------------------------

static const uint8_t flag_bits[4] = { 0x80, 0x40, 0x01, 0x02 }; // N V C Z

static int branch_taken(const DpHucIns *in, const DpPceCpu *c, int *bit_out, int *wram_off) {
    *wram_off = -1;
    if (in->cond < 8) {
        uint8_t bit = flag_bits[in->cond & 3];
        *bit_out = bit;
        int set = (c->p & bit) != 0;
        return in->cond >= 4 ? set : !set;
    }
    // BBR/BBS: the zero page lives in the window MPR1 maps (WRAM $F8-$FB)
    uint8_t page = c->mpr[1];
    if (page < 0xF8 || page > 0xFB) return -1;
    int bit = in->cond & 7;
    *bit_out = 1 << bit;
    *wram_off = in->operand & 0xFF;
    int set = (dp_pce_wram()[*wram_off] >> bit) & 1;
    return in->cond >= 16 ? set : !set;
}

static void fork_branch(PceRun *x, const DpHucIns *in, int64_t site) {
    DpPceResults *r = x->r;
    if (site < 0) return;
    if (x->fork_sites[site] >= (x->opt.fork_cap > 255 ? 255u : x->opt.fork_cap)) /* uint8 counter: cap saturates at 255 */ { r->fork_cuts++; return; }
    DpPceCpu c = dp_pce_cpu();
    int bit, woff;
    if (branch_taken(in, &c, &bit, &woff) < 0) return;
    x->fork_sites[site]++;
    dp_pce_save(x->tmp);
    if (woff < 0) dp_pce_set_flags((uint8_t)(c.p ^ bit));
    else dp_pce_wram()[woff] ^= (uint8_t)bit;
    if (fork_end(x, 1)) r->forks++;
}

static const uint8_t pad_catalog[] = {
    0x00, 0x08, 0x01, 0x02, 0x04, 0x10, 0x20, 0x40, 0x80,
    0x08 | 0x01, 0x01 | 0x02, 0x08 | 0x04
};

// does this instruction read the pad port ($1000-$13FF of the hardware page)?
static int reads_pad(const DpHucIns *in, const DpPceCpu *c) {
    uint16_t a;
    switch (in->mode) {
    case DP_HUC_M_ABS: case DP_HUC_M_ABSX: case DP_HUC_M_ABSY: a = in->operand; break;
    case DP_HUC_M_TST_ABS: return 0;
    default: return 0;
    }
    if (in->op == 0x8D || in->op == 0x9D || in->op == 0x99 || in->op == 0x8C ||
        in->op == 0x8E || in->op == 0x9C || in->op == 0x9E)
        return 0; // stores
    return c->mpr[a >> 13] == 0xFF && (a & 0x1C00) == 0x1000;
}

static void fork_pad(PceRun *x, int64_t site) {
    DpPceResults *r = x->r;
    if (site < 0) return;
    if (x->pad_sites[site] >= (x->opt.pad_fork_cap > 255 ? 255u : x->opt.pad_fork_cap)) /* uint8 counter: cap saturates at 255 */ { r->pad_fork_cuts++; return; }
    x->pad_sites[site]++;
    uint8_t now = dp_pce_input_get();
    dp_pce_save(x->tmp);
    for (unsigned i = 0; i < sizeof pad_catalog; i++) {
        if (pad_catalog[i] == now) continue;
        dp_pce_input(pad_catalog[i]);
        if (push_live(x, 0)) r->pad_forks++;
        dp_pce_load(x->tmp);
    }
}

// IRQ forks where the CPU unmasks: queue a state with the source raised
static void fork_irq(PceRun *x, const DpHucIns *in, int64_t site) {
    DpPceResults *r = x->r;
    if (!x->opt.seed_irq || site < 0) return;
    int unmask = in->op == 0x58 || in->op == 0x28 || in->op == 0x40;
    if (!unmask && (in->mode == DP_HUC_M_ABS) && (in->operand & 0x1FFF) == 0x1402 &&
        (in->op == 0x8D || in->op == 0x9C))
        unmask = 1;
    if (!unmask) return;
    if (x->irq_sites[site] >= (x->opt.irq_fork_cap > 255 ? 255u : x->opt.irq_fork_cap)) /* uint8 counter: cap saturates at 255 */ { r->irq_fork_cuts++; return; }
    x->irq_sites[site]++;
    dp_pce_save(x->tmp);
    for (int src = 1; src <= 2; src++) {
        dp_pce_raise(src);
        if (push_live(x, 0)) r->irq_forks++;
        dp_pce_load(x->tmp);
    }
}

static uint32_t loop_bump(PceRun *x, uint64_t key) {
    size_t j = (key * 0x9E3779B97F4A7C15ull) >> 52;
    for (unsigned probe = 0; probe < 32; probe++) {
        PceLoop *l = &x->loops[(j + probe) & (LOOP_SLOTS - 1)];
        if (l->key == key) return ++l->count;
        if (!l->key) { l->key = key; l->count = 1; return 1; }
    }
    return 0; // table crowded: no guard from here (the step budget bounds it)
}

// --- one step -------------------------------------------------------------------------

// returns 0 when the path ends here
// --- S6: decoder replay on pointer tables ---------------------------------------------
// A JSR into ROM whose routine's first ROM data read equals a zero-page word (the pointer
// the caller set) is a decoder; two starts at a constant table stride locate a pointer
// table (same 8 KiB cell page, pointers in the same CPU window); every other entry is
// replayed from the pre-call snapshot with that zero-page word set to the pointer.
enum { PS_DEC = 64, PS_START = 16, PS_FR = 16, PS_CAND = 2048 };
typedef struct { uint32_t key; uint8_t zp, nstarts, replayed; uint16_t starts[PS_START]; uint8_t *snap; } PDec;
typedef struct PStm {
    PDec dec[PS_DEC]; unsigned ndec;
    struct { uint32_t key; uint8_t sp, open; uint8_t zp[256]; } fr[PS_FR]; unsigned nfr;
} PStm;
static void pstm_free(PceRun *x) {
    if (!x->stm) return;
    for (unsigned i = 0; i < x->stm->ndec; i++) free(x->stm->dec[i].snap);
    free(x->stm); x->stm = NULL;
}
static void pstm_call(PceRun *x, uint32_t key) {
    if (!x->stm && !(x->stm = calloc(1, sizeof *x->stm))) return;
    PStm *s = x->stm;
    for (unsigned i = 0; i < s->ndec; i++)
        if (s->dec[i].key == key && !s->dec[i].snap) {
            s->dec[i].snap = malloc(x->state_size);
            if (s->dec[i].snap) dp_pce_save(s->dec[i].snap);
        }
    if (s->nfr == PS_FR) return;
    s->fr[s->nfr].key = key; s->fr[s->nfr].sp = (uint8_t)(dp_pce_cpu().s - 2); s->fr[s->nfr].open = 1;
    memcpy(s->fr[s->nfr].zp, dp_pce_wram(), 256);
    s->nfr++;
}
static void pstm_step(PceRun *x) {
    PStm *s = x->stm;
    if (!s) return;
    uint8_t sp = dp_pce_cpu().s;
    while (s->nfr && sp > s->fr[s->nfr - 1].sp) s->nfr--;
}
static void pstm_read(PceRun *x, unsigned a) {
    PStm *s = x->stm;
    if (!s || !s->nfr) return;
    for (unsigned k = s->nfr; k-- > 0;) {
        if (!s->fr[k].open) break;
        s->fr[k].open = 0;
        for (unsigned p = 0; p < 255; p++) {
            if ((unsigned)(s->fr[k].zp[p] | s->fr[k].zp[p + 1] << 8) != a) continue;
            PDec *d = NULL;
            for (unsigned i = 0; i < s->ndec; i++)
                if (s->dec[i].key == s->fr[k].key && s->dec[i].zp == p) d = &s->dec[i];
            if (!d) {
                if (s->ndec == PS_DEC) break;
                d = &s->dec[s->ndec++]; memset(d, 0, sizeof *d);
                d->key = s->fr[k].key; d->zp = (uint8_t)p; x->r->s6_decoders++;
            }
            unsigned j;
            for (j = 0; j < d->nstarts && d->starts[j] != a; j++) {}
            if (j == d->nstarts && j < PS_START && a >= 0x2000) d->starts[d->nstarts++] = (uint16_t)a;
            break;
        }
    }
}
static int pstm_ptr(const PceRun *x, size_t at, size_t lo, size_t hi, unsigned slot, uint16_t *v) {
    if (at < lo || at + 2 > hi) return 0;
    *v = (uint16_t)(x->rom[at] | x->rom[at + 1] << 8);
    return *v >= 0x2000 && (unsigned)(*v >> 13) == slot;
}
static int pstm_replay(PceRun *x) {
    PStm *s = x->stm;
    if (!s) return 0;
    DpPceResults *r = x->r; int pushed = 0;
    for (unsigned di = 0; di < s->ndec; di++) {
        PDec *d = &s->dec[di];
        if (d->nstarts < 2 || d->nstarts <= d->replayed || !d->snap) continue;
        d->replayed = d->nstarts;
        unsigned slot = d->starts[0] >> 13;
        uint8_t *tried = calloc(65536, 1);
        if (!tried) continue;
        for (unsigned j = 0; j < d->nstarts; j++) tried[d->starts[j]] = 1;
        size_t pos[256]; unsigned np = 0;
        for (size_t at = 0; at + 2 <= x->rom_size && np < 256; at++) {
            uint16_t v = (uint16_t)(x->rom[at] | x->rom[at + 1] << 8);
            for (unsigned j = 0; j < d->nstarts; j++) if (d->starts[j] == v) { pos[np++] = at; break; }
        }
        unsigned cands = 0;
        for (unsigned i = 0; i + 1 < np; i++) {
            size_t k = pos[i + 1] - pos[i];
            if (k < 2 || k > 16 || (pos[i] >> 13) != (pos[i + 1] >> 13)) continue;
            r->s6_tables++;
            size_t blo = (pos[i] >> 13) << 13, bhi = blo + 0x2000, lo = pos[i], hi = pos[i + 1];
            uint16_t v;
            while (lo >= k && pstm_ptr(x, lo - k, blo, bhi, slot, &v)) lo -= k;
            while (pstm_ptr(x, hi + k, blo, bhi, slot, &v)) hi += k;
            for (size_t at = lo; at <= hi; at += k) {
                if (!pstm_ptr(x, at, blo, bhi, slot, &v) || tried[v]) continue;
                tried[v] = 1;
                if (cands >= PS_CAND || x->queue_len >= x->opt.queue_cap) { r->s6_sat++; break; }
                cands++; r->s6_candidates++;
                dp_pce_load(d->snap);
                uint8_t *w = dp_pce_wram();
                w[d->zp] = (uint8_t)v; w[d->zp + 1] = (uint8_t)(v >> 8);
                dp_pce_save(x->tmp2);
                uLongf zl = x->zcap;
                if (compress2(x->ztmp, &zl, x->tmp2, x->state_size, 1) != Z_OK) continue;
                uint8_t *z = malloc(zl);
                if (!z) continue;
                memcpy(z, x->ztmp, zl);
                x->queue[x->queue_len].z = z; x->queue[x->queue_len].zlen = zl;
                x->queue[x->queue_len].forced = 1; x->queue[x->queue_len].choice = 0;
                x->queue_len++; r->s6_runs++; pushed++;
            }
        }
        free(tried);
    }
    return pushed;
}

static int step_one(PceRun *x, int *frame_done) {
    DpPceResults *r = x->r;
    if (dp_pce_irq_due()) {
        DpPceCpu c = dp_pce_cpu();
        uint8_t kind = (c.irq_low & 4) ? 2 : (c.irq_low & 2) ? 1 : 0;
        x->cur_pc = c.pc;
        x->cur_len = 0;
        x->cur_op = 0;
        log_interrupt(x, kind);
        int64_t v = dp_pce_rom_index((uint16_t)(0xFFF6 + 2 * (kind == 1 ? 1 : kind == 2 ? 2 : 0)));
        if (v >= 0) {
            r->flags[v] |= DP_PCE_DATA | DP_PCE_IRQ;
            r->flags[v + 1] |= DP_PCE_DATA | DP_PCE_IRQ;
        }
        dp_pce_step(frame_done);
        x->total_steps++;
        x->path_steps++;
        r->steps++;
        return 1;
    }
    DpPceCpu c = dp_pce_cpu();
    uint8_t b[7];
    for (int i = 0; i < 7; i++) b[i] = dp_pce_peek((uint16_t)(c.pc + i));
    DpHucIns in;
    dp_huc_decode(b, 7, c.pc, &in);
    int64_t site = dp_pce_rom_index(c.pc);
    if (x->mode == 0) {
        pstm_step(x);
        if (b[0] == 0x20) { int64_t tk = dp_pce_rom_index(in.target); if (tk >= 0) pstm_call(x, (uint32_t)tk); }
    }
    // padding slide: a natural/forced path running into 8 identical $00/$FF bytes
    // of the ROM has left the code; cut it, counted in pad_cuts
    if (x->mode == 0 && site >= 0 && (b[0] == 0x00 || b[0] == 0xFF) && (size_t)site + 8 <= x->rom_size) {
        unsigned same = 1;
        for (unsigned k = 1; k < 8; k++) if (x->rom[site + k] == b[0]) same++;
        if (same == 8) { r->pad_cuts++; return 0; }
    }
    if (in.flow == DP_HUC_ILLEGAL) {
        // the machine runs it as a NOP; exploration takes it as data run as
        // code and ends the path (play keeps running, unmarked)
        r->faults++;
        if (x->mode == 0) { r->illegal_cuts++; return 0; }
    } else if (site >= 0) {
        for (unsigned i = 0; i < in.length; i++) {
            int64_t cell = dp_pce_rom_index((uint16_t)(c.pc + i));
            if (cell < 0) continue;
            mark(x, (size_t)cell, x->forced ? DP_PCE_FORCED : DP_PCE_NATURAL);
        }
        x->istart[site] = 1;
        uint32_t page = (uint32_t)(site >> 13);
        if (page < 128 && x->page_win[page] == 0xFF) x->page_win[page] = (uint8_t)(c.pc >> 13);
    }
    // S3 provenance: a store right after a ROM data read carries that cell to WRAM
    if (site >= 0 && (b[0] == 0x85 || b[0] == 0x86 || b[0] == 0x84 || b[0] == 0x8D || b[0] == 0x8E || b[0] == 0x8C)) {
        unsigned sa = b[0] < 0x88 ? (0x2000u | b[1]) : (unsigned)(b[1] | b[2] << 8);
        unsigned pg = c.mpr[sa >> 13];
        if (pg >= 0xF8 && pg <= 0xFB && x->ram_exec[(pg - 0xF8) * 0x2000u + (sa & 0x1FFF)]) {
            x->ram_exec[(pg - 0xF8) * 0x2000u + (sa & 0x1FFF)] = 0;   // an executed WRAM byte is rewritten
            if (!r->smc_writes++) r->smc_first_pc = c.pc;
        }
        if (pg >= 0xF8 && pg <= 0xFB)
            x->ram_src[(pg - 0xF8) * 0x2000u + (sa & 0x1FFF)] =
                (x->last_read_cell >= 0 && x->total_steps - x->last_read_step <= 4) ? (int32_t)x->last_read_cell : -1;
    }
    // observed indirect jumps: jmp (abs) / jmp (abs,X) read their target from the
    // live bus; sites and distinct (site, target) pairs are counted, bounded
    if (in.flow == DP_HUC_INDIRECT && in.length == 3 && site >= 0 && (b[0] == 0x6C || b[0] == 0x7C)) {
        uint16_t ptr = (uint16_t)(b[1] | b[2] << 8);
        if (b[0] == 0x7C) ptr = (uint16_t)(ptr + c.x);
        uint16_t tgt = (uint16_t)(dp_pce_peek(ptr) | dp_pce_peek((uint16_t)(ptr + 1)) << 8);
        unsigned ppg = c.mpr[ptr >> 13];
        if (ppg >= 0xF8 && ppg <= 0xFB && (ptr & 0x1FFF) != 0x1FFF) {
            unsigned pi = (ppg - 0xF8) * 0x2000u + (ptr & 0x1FFF);
            if (x->ram_src[pi] >= 0 && x->ram_src[pi + 1] == x->ram_src[pi] + 1 && x->prov_len < 4096) {
                uint32_t a;
                for (a = 0; a < x->prov_len && x->prov_anchor[a] != x->ram_src[pi]; a++) {}
                if (a == x->prov_len) {
                    int64_t *g = realloc(x->prov_anchor, (x->prov_len + 1) * sizeof *g);
                    if (g) { x->prov_anchor = g; x->prov_anchor[x->prov_len++] = x->ram_src[pi]; r->prov_sites++; }
                }
            }
        }
        if (x->ind_pairs.cnt >= 65536) r->indirect_overflow++;
        else {
            if (dp_set_insert(&x->ind_sites, (uint64_t)site + 1) > 0) r->indirect_sites++;
            if (dp_set_insert(&x->ind_pairs, ((uint64_t)(site + 1) << 16) | tgt) > 0) r->indirect_targets++;
        }
    }
    int outside = site < 0;
    if (outside && !x->was_outside) r->exec_outside++;
    x->was_outside = outside;
    if (outside && x->mode == 0 && in.flow != DP_HUC_ILLEGAL) {   // RAM-code archive (WRAM pages only)
        unsigned pg = c.mpr[c.pc >> 13];
        if (pg >= 0xF8 && pg <= 0xFB)
            dp_ramarch_add(&r->ra, c.pc, b, in.length, x->ram_src[(pg - 0xF8) * 0x2000u + (c.pc & 0x1FFF)]);
        if (pg >= 0xF8 && pg <= 0xFB)
            for (int i = 0; i < (int)in.length; i++)
                x->ram_exec[(pg - 0xF8) * 0x2000u + ((c.pc + (unsigned)i) & 0x1FFF)] = 1;
    }

    if (x->mode == 0) {
        if (in.flow == DP_HUC_BRANCH) {
            fork_branch(x, &in, site);
            int bit, woff;
            int taken = branch_taken(&in, &c, &bit, &woff);
            if (taken > 0 && in.target <= c.pc && site >= 0) {
                if (loop_bump(x, ((uint64_t)site << 16) | in.target | 1ull << 63) > x->opt.loop_cap) {
                    r->loop_cuts++;
                    return 0;
                }
            }
        } else if (reads_pad(&in, &c)) {
            fork_pad(x, site);
        }
        fork_irq(x, &in, site);
        if (x->failed) return 0;
    }

    x->cur_pc = c.pc;
    x->cur_len = in.length;
    x->cur_op = in.op;
    dp_pce_step(frame_done);
    x->total_steps++;
    x->path_steps++;
    r->steps++;
    if (in.mapchange) {
        DpPceCpu a = dp_pce_cpu();
        for (int w = 0; w < 8; w++)
            if (a.mpr[w] != c.mpr[w]) log_bank(x, (uint8_t)w, c.mpr[w], a.mpr[w]);
    }
    return 1;
}

// --- fork exploration -------------------------------------------------------------

static void run_path(PceRun *x, PceQueued q) {
    DpPceResults *r = x->r;
    uLongf n = x->state_size;
    if (uncompress(x->tmp, &n, q.z, q.zlen) != Z_OK || n != x->state_size) {
        free(q.z);
        x->failed = 1;
        return;
    }
    free(q.z);
    dp_pce_load(x->tmp);
    memset(x->loops, 0, sizeof x->loops);
    x->path_steps = 0;
    x->was_outside = 0;
    x->forced = q.forced;
    x->cur_choice = q.choice;
    x->path_id = r->paths++;
    r->forced_paths += q.forced != 0;
    x->last_read_cell = -1;
    for (;;) {
        if (x->path_steps >= x->opt.max_steps) { r->step_cuts++; break; }
        if ((x->total_steps >= x->opt.max_total || dp_stop)) { r->global_cuts++; break; }
        int frame;
        if (!step_one(x, &frame)) break;
    }
    if (q.choice) dp_tree_close(&x->tree, q.choice, DP_ISSUE_DONE);
    x->cur_choice = 0;
}

// --- play / TAS sessions --------------------------------------------------------------

static uint32_t rng_next(uint32_t *s) {
    uint32_t v = *s;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    return *s = v;
}

static uint8_t random_input(PceRun *x, uint8_t *held, uint32_t *hold) {
    if (!*hold) {
        uint32_t v = rng_next(&x->rng);
        uint8_t h = (uint8_t)(v & 0xFF);
        if ((h & 0xA0) == 0xA0) h &= (uint8_t)~0x20;   // not left and right together
        if ((h & 0x50) == 0x50) h &= (uint8_t)~0x40;   // not up and down together
        h &= 0xF3;                                     // SEL+RUN is a soft reset,
        if ((v >> 8) % 16 == 0) h |= 0x08;             // RUN alone pauses: rare
        *held = h;
        *hold = 4 + (v >> 16) % 40;
    }
    (*hold)--;
    return *held;
}

// the native loop's per-instruction report (play mode)
static void trace_cb(void *user, uint16_t pc, uint8_t op) {
    PceRun *x = user;
    DpPceResults *r = x->r;
    x->cur_pc = pc;
    x->cur_len = dp_huc_length(op);
    x->cur_op = op;
    x->total_steps++;
    r->steps++;
    int64_t site = dp_pce_rom_index(pc);
    int outside = site < 0;
    if (outside && !x->was_outside) r->exec_outside++;
    x->was_outside = outside;
    if (outside) return;
    if (!dp_huc_defined(op)) { r->faults++; return; }
    if (x->istart[site]) return; // seen: its cells are marked
    for (unsigned i = 0; i < x->cur_len; i++) {
        int64_t cell = dp_pce_rom_index((uint16_t)(pc + i));
        if (cell >= 0) mark(x, (size_t)cell, DP_PCE_NATURAL);
    }
    x->istart[site] = 1;
    uint32_t page = (uint32_t)(site >> 13);
    if (page < 128 && x->page_win[page] == 0xFF) x->page_win[page] = (uint8_t)(pc >> 13);
}

// one frame of play with this input (the native loop, traced)
static int play_frame(PceRun *x, uint8_t input) {
    dp_pce_input(input);
    if (dp_pce_multitap_enabled()) // pads 2-5 replay the played input rotated by their number: distinct, deterministic
        for (unsigned n = 1; n < 5; n++) dp_pce_pad_n(n, (uint8_t)((input << n) | (input >> (8 - n))));
    dp_pce_trace(trace_cb, x);
    dp_pce_run_frame();
    dp_pce_trace(NULL, NULL);
    x->r->play_frames_run++;
    return 1;
}

// one session from power-on; inputs per frame from the movie or random held
// inputs; a checkpoint seeds the fork queue every `seed_every` frames
static void play_session(PceRun *x, uint32_t frames, uint32_t seed, const DpPceTas *tas) {
    DpPceResults *r = x->r;
    x->mode = 1;
    x->forced = 0;
    x->path_id = r->paths;
    x->rng = seed ? seed : 0x9E3779B9u;
    dp_pce_reset();
    uint8_t held = 0;
    uint32_t hold = 0;
    const uint32_t seed_every = 300;
    for (uint32_t f = 0; f < frames; f++) {
        if ((x->total_steps >= x->opt.max_total || dp_stop)) { r->global_cuts++; break; }
        uint8_t in;
        if (tas && f < tas->frames) {
            in = tas->pads[f];
            if (tas->resets && tas->resets[f]) dp_pce_reset();
            r->tas_frames_run++;
        } else {
            in = random_input(x, &held, &hold);
        }
        play_frame(x, in);
        if (x->opt.queue_cap && f % seed_every == seed_every - 1) push_live(x, 0);
    }
    if (x->opt.queue_cap) push_live(x, 0);
    x->mode = 0;
    r->play_sessions_run++;
}

// --- novelty search ---------------------------------------------------------------------

typedef struct {
    uint8_t *z;
    uLongf zlen;
    uint64_t gain;         // cells found by the trial that produced it + its children
    uint32_t picks;
    uint64_t key;          // screen fingerprint
    uint32_t depth;        // frames played from power-on along its chain
    uint8_t lives[8192];   // WRAM at archiving (the lives byte is learned later)
} PceCell;

static int archive_add(PceRun *x, PceCell *a, uint32_t *n, uint64_t gain) {
    DpPceResults *r = x->r;
    dp_pce_save(x->tmp2);
    uLongf zl = x->zcap;
    if (compress2(x->ztmp, &zl, x->tmp2, x->state_size, 1) != Z_OK) return -1;
    uint8_t *z = malloc(zl);
    if (!z) return -1;
    memcpy(z, x->ztmp, zl);
    uint32_t slot = *n;
    if (*n >= x->opt.novelty_archive) {
        // replace the least productive entry (gain per pick)
        double worst = 1e300;
        for (uint32_t i = 0; i < *n; i++) {
            double sc = (double)a[i].gain / (1.0 + a[i].picks);
            if (sc < worst) { worst = sc; slot = i; }
        }
        free(a[slot].z);
        r->novelty_replaced++;
    } else {
        (*n)++;
    }
    a[slot].z = z;
    a[slot].zlen = zl;
    memcpy(a[slot].lives, dp_pce_wram(), 8192);
    a[slot].gain = gain;
    a[slot].picks = 0;
    r->novelty_archived++;
    return (int)slot;
}

// lives detection without game knowledge: a WRAM byte <= 9 whose only
// decreases are single steps at least 200 frames apart and which rarely
// increases (a new game, a bonus life); a trial in which such a byte drops has lost a life
typedef struct {
    uint8_t prev[8192], maxv[8192];
    uint16_t dec[8192], inc[8192];
    uint8_t bad[8192];
    uint32_t last_dec[8192];
    uint32_t frame;
    int lives_byte;        // the byte whose drops were last seen as deaths (-1: none)
} PceLives;

static int lives_frame(PceLives *L, const uint8_t *w, int learn) {
    int died = 0;
    for (int i = 0; i < 8192; i++) {
        uint8_t a = L->prev[i], b = w[i];
        if (b > L->maxv[i]) L->maxv[i] = b;
        if (a == b) continue;
        if (b + 1 == a) {
            int cand = !L->bad[i] && L->dec[i] >= 2 && L->maxv[i] <= 9 &&
                       L->inc[i] <= 1 + L->dec[i] / 4;
            if (cand && !learn) { died = 1; L->lives_byte = i; if (getenv("DP_PCE_LIVES_DEBUG")) fprintf(stderr, "death $%04X %u->%u\n", 0x2000 + i, a, b); }
            if (L->frame - L->last_dec[i] < 200) L->bad[i] = 1;
            L->dec[i]++;
            L->last_dec[i] = L->frame;
        } else if (b < a) {
            L->bad[i] = 1;
        } else if (b == a + 1 && L->inc[i] < 0xFFFF) {
            L->inc[i]++; // a jump of more than one is a new game, not counted
        }
    }
    memcpy(L->prev, w, 8192);
    L->frame++;
    return died;
}

enum { KEY_SET = 1 << 18 };
static int key_seen(const uint64_t *set, uint64_t k) {
    k |= 1;
    for (uint32_t i = (uint32_t)(k >> 7) & (KEY_SET - 1);; i = (i + 1) & (KEY_SET - 1)) {
        if (!set[i]) return 0;
        if (set[i] == k) return 1;
    }
}
static void key_add(uint64_t *set, uint64_t k) {
    k |= 1;
    for (uint32_t i = (uint32_t)(k >> 7) & (KEY_SET - 1);; i = (i + 1) & (KEY_SET - 1)) {
        if (set[i] == k) return;
        if (!set[i]) { set[i] = k; return; }
    }
}

static void novelty_search(PceRun *x) {
    DpPceResults *r = x->r;
    if (!x->opt.novelty_steps || !x->opt.novelty_archive) return;
    PceCell *a = calloc(x->opt.novelty_archive, sizeof *a);
    if (!a) { x->failed = 1; return; }
    uint32_t n = 0;
    PceLives *L = calloc(1, sizeof *L);
    uint64_t *keys = calloc(KEY_SET, sizeof *keys);
    if (!L || !keys) { free(a); free(L); free(keys); x->failed = 1; return; }
    L->lives_byte = -1;
    const int cells = x->opt.novelty_cells;
    x->mode = 1;
    x->forced = 0;
    x->rng = x->opt.play_seed * 0x2545F491u + 7;
    dp_pce_reset();
    archive_add(x, a, &n, 1);
    uint64_t stop = x->total_steps + x->opt.novelty_steps;
    while (!x->failed) {
        if (x->total_steps >= stop) break;
        if ((x->total_steps >= x->opt.max_total || dp_stop)) { r->global_cuts++; break; }
        // pick: the best of eight random cells, scored by gain and the
        // inverse square root of their picks (rarely tried cells first),
        // deeper cells (frames from power-on) favoured
        // once the lives byte is known, cells with fewer lives than the
        // archive's best are all but dropped (a chain that lost lives ends
        // in a game over)
        uint8_t maxl = 0;
        if (L->lives_byte >= 0)
            for (uint32_t i = 0; i < n; i++)
                if (a[i].lives[L->lives_byte] > maxl && a[i].lives[L->lives_byte] <= 9) maxl = a[i].lives[L->lives_byte];
        uint32_t best = n - 1;
        double bs = -1;
        for (int k = 0; k < 8; k++) {
            uint32_t i = k ? rng_next(&x->rng) % n : n - 1;
            if (!cells) {
                double g = (double)a[i].gain / (1.0 + a[i].picks);
                if (g > bs) { bs = g; best = i; }
                continue;
            }
            double lf = (L->lives_byte >= 0 && a[i].lives[L->lives_byte] < maxl) ? 1e-3 : 1.0;
            double sc = lf * (1.0 + (double)a[i].gain) * (1.0 + a[i].depth / 200.0) / sqrt(1.0 + a[i].picks);
            if (sc > bs) { bs = sc; best = i; }
        }
        uLongf sz = x->state_size;
        if (uncompress(x->tmp, &sz, a[best].z, a[best].zlen) != Z_OK) { x->failed = 1; break; }
        dp_pce_load(x->tmp);
        a[best].picks++;
        r->novelty_trials++;
        uint64_t before = x->novel;
        uint8_t held = 0;
        uint32_t hold = 0;
        uint64_t frames_since = 0;
        memcpy(L->prev, dp_pce_wram(), 8192);
        L->frame += 200; // a restore is no decrement sequence
        int learn = r->novelty_trials < 64;
        int died = 0;
        int recent[64];        // slots added by this trial, with their frame
        uint32_t recent_f[64];
        unsigned nrecent = 0;
        for (uint32_t f = 0; f < x->opt.novelty_frames; f++) {
            if (!play_frame(x, random_input(x, &held, &hold))) break;
            r->novelty_frames_run++;
            if (cells && lives_frame(L, dp_pce_wram(), learn)) {
                // the cells archived shortly before a death are doomed
                for (unsigned k = 0; k < nrecent; k++)
                    if (recent[k] >= 0 && f - recent_f[k] < 120) { a[recent[k]].picks = 1u << 30; a[recent[k]].gain = 0; }
                died = 1;
                r->novelty_deaths++;
                break;
            }
            frames_since++;
            // a new screen cell, or new ROM cells, joins the archive
            if (f % 8 == 7) {
                uint64_t k = cells ? dp_pce_screen_key() : 0;
                int fresh = cells && !key_seen(keys, k);
                if (fresh || (x->novel > before && frames_since >= 60)) {
                    uint64_t g = x->novel - before;
                    a[best].gain += g;
                    int slot = archive_add(x, a, &n, cells ? g + (fresh ? 1 : 0) : g + 1);
                    if (nrecent < 64) { recent[nrecent] = slot; recent_f[nrecent++] = f; }
                    if (slot >= 0) { a[slot].key = k; a[slot].depth = a[best].depth + f + 1; if (a[slot].depth > r->novelty_depth) r->novelty_depth = a[slot].depth; }
                    key_add(keys, k);
                    if (g) r->novelty_hits++;
                    before = x->novel;
                    frames_since = 0;
                }
            }
        }
    }
    // the archive's freshest checkpoints seed the fork queue
    for (uint32_t i = n; i > 0 && x->opt.queue_cap; i--) {
        if (i + 64 < n) break;
        uLongf sz = x->state_size;
        if (uncompress(x->tmp, &sz, a[i - 1].z, a[i - 1].zlen) == Z_OK) {
            dp_pce_load(x->tmp);
            push_live(x, 0);
        }
    }
    if (x->opt.export_state && n) {
        uint32_t d = 0;
        for (uint32_t i = 1; i < n; i++) if (a[i].depth > a[d].depth) d = i;
        if (!dp_state_export(x->opt.export_state, "pce", x->rom, x->rom_size, a[d].depth, a[d].z, (uint32_t)a[d].zlen, (uint32_t)x->state_size)) {
            r->state_exported = 1;
            r->state_exported_depth = a[d].depth;
        }
    }
    for (uint32_t i = 0; i < n; i++) free(a[i].z);
    free(a);
    free(L);
    free(keys);
    x->mode = 0;
}

// --- static closure ---------------------------------------------------------------------

static void static_closure(PceRun *x) {
    DpPceResults *r = x->r;
    size_t n = x->rom_size;
    uint32_t *work = malloc(n * sizeof *work);
    if (!work) { x->failed = 1; return; }
    size_t top = 0;
    for (size_t i = 0; i < n; i++) if (x->istart[i]) work[top++] = (uint32_t)i;
    while (top) {
        uint32_t cell = work[--top];
        uint32_t page = cell >> 13;
        if (page >= 128 || x->page_win[page] == 0xFF) continue;
        uint16_t addr = (uint16_t)(x->page_win[page] * 0x2000 + (cell & 0x1FFF));
        size_t avail = 0x2000 - (cell & 0x1FFF);
        DpHucIns in;
        if (!dp_huc_decode(x->rom + cell, avail < 7 ? avail : 7, addr, &in)) continue;
        if (in.flow == DP_HUC_ILLEGAL) continue;
        uint16_t succ[2];
        int ns = 0;
        switch (in.flow) {
        case DP_HUC_NEXT: succ[ns++] = (uint16_t)(addr + in.length); break;
        case DP_HUC_BRANCH: succ[ns++] = (uint16_t)(addr + in.length); succ[ns++] = in.target; break;
        case DP_HUC_JUMP: succ[ns++] = in.target; break;
        case DP_HUC_CALL: succ[ns++] = (uint16_t)(addr + in.length); succ[ns++] = in.target; break;
        default: break;
        }
        if (in.mapchange) ns = 0; // TAM: the windows may move under the next bytes
        for (int k = 0; k < ns; k++) {
            if ((succ[k] >> 13) != x->page_win[page]) continue; // another window: unknown page
            uint32_t t = (page << 13) | (succ[k] & 0x1FFF);
            if (t >= n || x->istart[t]) continue;
            size_t tav = 0x2000 - (t & 0x1FFF);
            DpHucIns ti;
            if (!dp_huc_decode(x->rom + t, tav < 7 ? tav : 7, succ[k], &ti)) continue;
            if (ti.flow == DP_HUC_ILLEGAL) continue;
            int clash = 0;
            for (unsigned j = 0; j < ti.length; j++)
                if (r->flags[t + j] & (DP_PCE_DATA | DP_PCE_COPIED)) clash = 1;
            if (clash) continue;
            x->istart[t] = 2;
            for (unsigned j = 0; j < ti.length; j++)
                if (!(r->flags[t + j] & (DP_PCE_NATURAL | DP_PCE_FORCED | DP_PCE_STATIC))) {
                    r->flags[t + j] |= DP_PCE_STATIC;
                    r->static_bytes++;
                }
            work[top++] = t;
        }
    }
    free(work);
}

// G4 second pass: the unexecuted static successors of executed instructions (branch,
// jump, call targets and fall-throughs in the same window) are seeded as forced paths
// from the power-on machine with the page mapped under the window.  Bounded.
enum { PCE_PASS2_MAX = 256 };
static unsigned second_pass_seed(PceRun *x) {
    DpPceResults *r = x->r;
    size_t n = x->rom_size;
    uint8_t *sd = calloc(n, 1);
    if (!sd) { x->failed = 1; return 0; }
    unsigned seeded = 0;
    for (size_t cell = 0; cell < n && !x->failed; cell++) {
        if (x->istart[cell] != 1) continue;
        uint32_t page = (uint32_t)(cell >> 13);
        if (page >= 128 || x->page_win[page] == 0xFF) continue;
        uint16_t addr = (uint16_t)(x->page_win[page] * 0x2000 + (cell & 0x1FFF));
        size_t avail = 0x2000 - (cell & 0x1FFF);
        DpHucIns in;
        if (!dp_huc_decode(x->rom + cell, avail < 7 ? avail : 7, addr, &in) || in.flow == DP_HUC_ILLEGAL || in.mapchange) continue;
        uint16_t succ[2];
        int ns = 0;
        switch (in.flow) {
        case DP_HUC_NEXT: succ[ns++] = (uint16_t)(addr + in.length); break;
        case DP_HUC_BRANCH: succ[ns++] = (uint16_t)(addr + in.length); succ[ns++] = in.target; break;
        case DP_HUC_JUMP: succ[ns++] = in.target; break;
        case DP_HUC_CALL: succ[ns++] = (uint16_t)(addr + in.length); succ[ns++] = in.target; break;
        default: break;
        }
        for (int k = 0; k < ns; k++) {
            if ((succ[k] >> 13) != x->page_win[page]) continue;
            uint32_t t = (page << 13) | (succ[k] & 0x1FFF);
            if (t >= n || x->istart[t] || sd[t] || (r->flags[t] & (DP_PCE_DATA | DP_PCE_COPIED))) continue;
            sd[t] = 1;
            if (r->pass2_seeds + r->pass2_refused >= PCE_PASS2_MAX) { r->pass2_capped++; continue; }
            dp_pce_reset();
            if (!dp_pce_enter(page, succ[k])) { r->pass2_refused++; continue; }
            if (push_live(x, 1)) { seeded++; r->pass2_seeds++; }
        }
    }
    free(sd);
    dp_pce_reset();
    return seeded;
}

// G2 bank-view dispatch: a run of >= 3 aligned ROM words (>= 3 distinct) whose top three
// bits name one CPU window is a candidate dispatch table; each target is entered under
// every ROM page where 6 instructions decode without an undefined opcode or an 8-byte
// run of one value.  Seeded as forced paths (shared 256-seed cap with the second pass).
static int view_ok(const PceRun *x, uint32_t t, uint16_t addr) {
    size_t n = x->rom_size;
    unsigned off = 0;
    for (unsigned k = 0; k < 6; k++) {
        if (t + off >= n || (t & 0x1FFF) + off >= 0x2000) return k > 0;
        size_t avail = 0x2000 - ((t & 0x1FFF) + off);
        DpHucIns in;
        if (!dp_huc_decode(x->rom + t + off, avail < 7 ? avail : 7, (uint16_t)(addr + off), &in) || in.flow == DP_HUC_ILLEGAL) return 0;
        if (t + off + 8 <= n) {
            unsigned same = 1;
            while (same < 8 && x->rom[t + off + same] == x->rom[t + off]) same++;
            if (same == 8) return 0;
        }
        if (in.flow == DP_HUC_JUMP || in.flow == DP_HUC_RETURN || in.flow == DP_HUC_INDIRECT || in.mapchange) return 1;
        off += in.length;
    }
    return 1;
}

static unsigned dispatch_seed(PceRun *x) {
    DpPceResults *r = x->r;
    size_t n = x->rom_size;
    unsigned npages = (unsigned)((n + 0x1FFF) >> 13), seeded = 0;
    uint8_t *sd = calloc(n, 1);
    if (!sd) { x->failed = 1; return 0; }
    for (unsigned p = 0; p < npages && !x->failed; p++) {
        size_t base = (size_t)p << 13;
        for (size_t i = 0; i + 1 < 0x2000 && base + i + 1 < n; ) {
            uint16_t v0 = (uint16_t)(x->rom[base + i] | x->rom[base + i + 1] << 8);
            unsigned w = v0 >> 13;
            if (w < 2) { i += 2; continue; }
            size_t j = i;
            while (j + 1 < 0x2000 && base + j + 1 < n && (unsigned)((x->rom[base + j] | x->rom[base + j + 1] << 8) >> 13) == w) j += 2;
            unsigned cnt = (unsigned)((j - i) / 2);
            if (cnt < 3) { i = j > i ? j : i + 2; continue; }
            uint16_t vs[64]; unsigned nd = 0;
            for (size_t e = i; e < j && nd < 64; e += 2) {
                uint16_t v = (uint16_t)(x->rom[base + e] | x->rom[base + e + 1] << 8);
                unsigned k;
                for (k = 0; k < nd && vs[k] != v; k++) {}
                if (k == nd) vs[nd++] = v;
            }
            if (nd >= 3) {
                r->disp_tables++;
                for (unsigned k = 0; k < nd; k++)
                    for (unsigned q = 0; q < npages; q++) {
                        uint32_t t = (q << 13) | (vs[k] & 0x1FFF);
                        if (t >= n || sd[t] || x->istart[t] || (r->flags[t] & (DP_PCE_NATURAL | DP_PCE_FORCED | DP_PCE_STATIC))) continue;
                        if (!view_ok(x, t, vs[k])) { r->disp_refused_view++; continue; }
                        sd[t] = 1;
                        if (r->pass2_seeds + r->pass2_refused >= PCE_PASS2_MAX) { r->pass2_capped++; continue; }
                        dp_pce_reset();
                        if (!dp_pce_enter(q, vs[k])) { r->pass2_refused++; continue; }
                        if (push_live(x, 1)) { seeded++; r->pass2_seeds++; r->disp_seeds++; }
                    }
            }
            i = j;
        }
    }
    free(sd);
    dp_pce_reset();
    return seeded;
}

// jump tables: JMP [abs,X] in an observed window — the words that point into
// observed code of the same window are table entries
static void tables(PceRun *x) {
    DpPceResults *r = x->r;
    for (size_t i = 0; i + 2 < x->rom_size; i++) {
        if (!x->istart[i] || x->rom[i] != 0x7C) continue;
        uint32_t page = (uint32_t)(i >> 13);
        uint8_t w = x->page_win[page];
        uint16_t t = (uint16_t)(x->rom[i + 1] | x->rom[i + 2] << 8);
        if ((t >> 13) != w) continue;
        uint32_t base = (page << 13) | (t & 0x1FFF);
        for (unsigned e = 0; e < 128; e++) {
            uint32_t c = base + 2 * e;
            if (c + 1 >= x->rom_size || (c >> 13) != page) break;
            uint16_t v = (uint16_t)(x->rom[c] | x->rom[c + 1] << 8);
            if ((v >> 13) != w) break;
            uint32_t tc = (page << 13) | (v & 0x1FFF);
            if (!x->istart[tc]) break;
            r->flags[c] |= DP_PCE_TABLE;
            r->flags[c + 1] |= DP_PCE_TABLE;
            r->table_bytes += 2;
        }
    }
}

// --- inference ---------------------------------------------------------------------------

static int cls_code(uint8_t f) { return (f & (DP_PCE_NATURAL | DP_PCE_FORCED | DP_PCE_STATIC)) != 0; }

static void infer(PceRun *x, uint8_t *inf) {
    DpPceResults *r = x->r;
    size_t n = x->rom_size;
    // padding: runs of one byte value ($00/$FF from 8, others from 32)
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && x->rom[j] == x->rom[i] && !r->flags[j]) j++;
        size_t len = j - i;
        uint8_t v = x->rom[i];
        if (len >= ((v == 0 || v == 0xFF) ? 8u : 32u)) {
            for (size_t k = i; k < j; k++) r->flags[k] |= DP_PCE_PAD;
            r->padding_bytes += len;
        }
        i = j > i ? j : i + 1;
    }
    // gaps between classified cells
    for (size_t i = 0; i < n;) {
        if (r->flags[i]) { i++; continue; }
        size_t j = i;
        while (j < n && !r->flags[j]) j++;
        size_t len = j - i;
        uint8_t lf = i ? r->flags[i - 1] : 0, rf = j < n ? r->flags[j] : 0;
        uint8_t c = 0;
        if (len <= 3 && cls_code(lf) && cls_code(rf)) c = DP_PMAP_INF_CODE;
        else if (len <= 1024 && (lf & DP_PCE_PAD) && (!rf || (rf & DP_PCE_PAD))) c = DP_PMAP_INF_PAD;
        else if (len <= 1024 && (lf || rf)) c = DP_PMAP_INF_DATA;
        if (c) {
            for (size_t k = i; k < j; k++) inf[k] = c;
            if (c == DP_PMAP_INF_CODE) r->inferred_code += len;
            else if (c == DP_PMAP_INF_PAD) r->inferred_pad += len;
            else r->inferred_data += len;
        }
        i = j;
    }
}

// S5 copy siblings: a table word that names the source of a recorded VDC block
// copy (>= 16 bytes) marks a table of streams; every neighbouring word of the same
// window that points at unclassified cells of the same page is a sibling stream of
// the same length, typed inferred data.  Same-page tables only (the window is
// known from the observed code); at most 64 entries a table, saturation counted.
enum { PCE_SIB_MAX = 64 };
static int pce_sib_word(const PceRun *x, size_t k, uint32_t page, uint8_t w) {
    if (k + 1 >= x->rom_size || (k >> 13) != page) return 0;
    uint16_t v = (uint16_t)(x->rom[k] | x->rom[k + 1] << 8);
    if ((v >> 13) != w) return 0;
    uint32_t t = (page << 13) | (v & 0x1FFF);
    return t < x->rom_size && !x->r->flags[t] && !x->istart[t];
}
static void siblings(PceRun *x, uint8_t *inf) {
    DpPceResults *r = x->r;
    for (unsigned c = 0; c < r->copy_count && c < DP_PCE_COPY_CAP; c++) {
        const DpPceCopy *rec = &r->copies[c];
        if (rec->channel != 2 || rec->length < 16 || rec->source >= x->rom_size) continue;
        uint32_t page = rec->source >> 13;
        if (page >= 128 || x->page_win[page] == 0xFF) continue;
        uint8_t w = x->page_win[page];
        for (size_t k = (size_t)page << 13; k + 1 < ((size_t)(page + 1) << 13) && k + 1 < x->rom_size; k++) {
            if (!(r->flags[k] & DP_PCE_DATA)) continue;
            uint16_t v = (uint16_t)(x->rom[k] | x->rom[k + 1] << 8);
            if ((v >> 13) != w || ((page << 13) | (v & 0x1FFF)) != rec->source) continue;
            size_t t0 = k, t1 = k + 2;
            unsigned entries = 1;
            while (t0 >= 2 && entries < PCE_SIB_MAX && ((t0 - 2) >> 13) == page && pce_sib_word(x, t0 - 2, page, w)) { t0 -= 2; entries++; }
            while (entries < PCE_SIB_MAX && pce_sib_word(x, t1, page, w)) { t1 += 2; entries++; }
            if (entries >= PCE_SIB_MAX) r->sib_caps++;
            if (entries < 2) continue;
            r->sib_tables++;
            for (size_t e = t0; e < t1; e += 2) {
                uint16_t sv = (uint16_t)(x->rom[e] | x->rom[e + 1] << 8);
                uint32_t b = (page << 13) | (sv & 0x1FFF);
                if (b == rec->source) continue;
                r->sib_entries++;
                for (uint32_t q = b; q < b + rec->length && (q >> 13) == page && q < x->rom_size; q++) {
                    if (r->flags[q] & ~DP_PCE_PAD) break;
                    if (inf[q] == DP_PMAP_INF_PAD) r->inferred_pad--;
                    else if (inf[q] == DP_PMAP_INF_DATA) { r->inferred_data--; }
                    else if (inf[q]) continue;
                    inf[q] = DP_PMAP_INF_DATA; r->inferred_data++; r->sib_bytes++;
                }
            }
            k = t1;
        }
    }
}

// --- sections ---------------------------------------------------------------------------

static void sections_build(PceRun *x) {
    DpPceResults *r = x->r;
    uint32_t cap = 64;
    r->sections = calloc(cap, sizeof *r->sections);
    if (!r->sections) { x->failed = 1; return; }
    const uint8_t code = DP_PCE_NATURAL | DP_PCE_FORCED;
    for (size_t i = 0; i < x->rom_size;) {
        if (!(r->flags[i] & code)) { i++; continue; }
        size_t j = i;
        while (j < x->rom_size && (r->flags[j] & code) && (j >> 13) == (i >> 13)) j++;
        if (r->section_count == cap) {
            cap *= 2;
            DpPceSection *ns = realloc(r->sections, cap * sizeof *ns);
            if (!ns) { x->failed = 1; return; }
            r->sections = ns;
        }
        DpPceSection *s = &r->sections[r->section_count];
        memset(s, 0, sizeof *s);
        s->id = ++r->section_count;
        s->lo = (uint32_t)i;
        s->hi = (uint32_t)(j - 1);
        for (size_t k = i; k < j; k++) {
            if (r->flags[k] & DP_PCE_NATURAL) s->natural_hits++;
            else s->forced_hits++;
        }
        s->verdict = s->natural_hits ? DP_PCE_VERDICT_REQUIRED : DP_PCE_VERDICT_FORCED;
        i = j;
    }
}

// --- driver --------------------------------------------------------------------------------

DpPceOptions dp_pce_options(void) {
    DpPceOptions o;
    memset(&o, 0, sizeof o);
    o.max_total = 60000000;
    o.max_steps = 400000;
    o.queue_cap = 8192;
    o.loop_cap = 2048;
    o.fork_cap = 4;
    o.pad_fork_cap = 2;
    o.irq_fork_cap = 2;
    o.seed_irq = 1;
    o.static_closure = 1;
    o.infer = 1;
    o.play_frames = 0;
    o.play_sessions = 8;
    o.play_seed = 1;
    o.novelty_steps = 0;
    o.novelty_frames = 240;
    o.novelty_archive = 4096;
    return o;
}

void dp_pce_results_free(DpPceResults *r) {
    if (!r) return;
    dp_ramarch_free(&r->ra);
    free(r->flags);
    free(r->pal_flags);
    free(r->idx_flags);
    r->idx_flags = NULL;
    free(r->sections);
    r->pal_flags = NULL;
    r->flags = NULL;
    r->sections = NULL;
}

static uint8_t *g_inferred; // the last run's inference (map building)
static size_t g_inferred_size;

int dp_pce_explore(const uint8_t *data, size_t size, const DpPceOptions *options,
                   DpPceResults *results) {
    if (!data || !results) return 0;
    g_watch_every = options ? options->watch_every : 0;
    g_watch_r = results;
    g_watch_n = 0;
    if (!dp_pce_init(data, size)) {
        fprintf(stderr, "deepeel: not a HuCard image (size %zu)\n", size);
        return 0;
    }
    PceRun *x = calloc(1, sizeof *x);
    if (!x) return 0;
    DpPceResults *r = results;
    dp_set_init(&x->ctx_seen, 1 << 10);
    for (unsigned i = 0; i < 0x8000; i++) x->ram_src[i] = -1;
    dp_tree_init(&x->tree);
    dp_set_init(&x->ind_sites, 1 << 8);
    dp_set_init(&x->ind_pairs, 1 << 10);
    memset(r, 0, sizeof *r);
    x->opt = options ? *options : dp_pce_options();
    r->options = x->opt;
    dp_pce_set_frame_hook(watch_frame_hook);
    dp_pce_watch_enable(g_watch_every != 0);
    if (x->opt.lot_path && !(x->lot = dp_lf_open(x->opt.lot_path, "PCE", 1)))
        fprintf(stderr, "deepeel: cannot open the lot %s\n", x->opt.lot_path);
    x->r = r;
    x->rom = dp_pce_rom();
    x->rom_size = r->rom_size = dp_pce_rom_size();
    x->last_read_cell = -1;
    memset(x->page_win, 0xFF, sizeof x->page_win);
    r->flags = calloc(x->rom_size, 1);
    r->pal_flags = calloc(x->rom_size, 1);
    r->idx_flags = calloc(x->rom_size, 1);
    x->fork_sites = calloc(x->rom_size, 1);
    x->pad_sites = calloc(x->rom_size, 1);
    x->irq_sites = calloc(x->rom_size, 1);
    x->istart = calloc(x->rom_size, 1);
    x->state_size = dp_pce_state_size();
    x->tmp = malloc(x->state_size);
    x->tmp2 = malloc(x->state_size);
    x->zcap = compressBound(x->state_size);
    x->ztmp = malloc(x->zcap);
    x->queue = calloc(x->opt.queue_cap ? x->opt.queue_cap : 1, sizeof *x->queue);
    if (!r->flags || !x->fork_sites || !x->pad_sites || !x->irq_sites || !x->istart ||
        !x->tmp || !x->tmp2 || !x->ztmp || !x->queue)
        x->failed = 1;
    DpPceObserver o = { x, obs_rom_read, obs_io_write, NULL };
    dp_pce_observe(&o);
    // the reset vector cells are data the machine reads
    {
        int64_t v = dp_pce_rom_index(0xFFFE);
        if (v >= 0) { r->flags[v] |= DP_PCE_DATA; r->flags[v + 1] |= DP_PCE_DATA; }
    }

    // the power-on state first: the root of the fork exploration
    if (!x->failed && x->opt.queue_cap && !x->opt.skip_root &&
        !(x->opt.lot_drain && dp_lf_pending(x->lot))) push_live(x, 0);

    // explicit entries (G3): each starts a forced path from the power-on mapping;
    // an address outside the ROM mapping is refused and counted
    for (unsigned e = 0; e < x->opt.entry_count && !x->failed; e++) {
        dp_pce_reset();
        if (dp_pce_rom_index(x->opt.entries[e]) < 0) { r->entries_refused++; continue; }
        dp_pce_set_pc(x->opt.entries[e]);
        if (push_live(x, 1)) r->entries_seeded++;
    }
    if (x->opt.entry_count) dp_pce_reset();

    // TAS movies, then the play sessions; their checkpoints join the queue
    for (int i = 0; i < x->opt.tas_count && !x->failed; i++) {
        DpPceTas t;
        char err[256];
        if (!dp_pce_tas_load(x->opt.tas_paths[i], &t, err, sizeof err)) {
            fprintf(stderr, "deepeel: TAS %s: %s\n", x->opt.tas_paths[i], err);
            continue;
        }
        fprintf(stderr, "deepeel: TAS %s: %u frames\n", x->opt.tas_paths[i], t.frames);
        int id = dp_pce_tas_identity(&t, x->rom, x->rom_size);
        if (id == 1) x->r->tas_same++; else if (id == 2) x->r->tas_different++; else x->r->tas_unrecorded++;
        if (id == 2) fprintf(stderr, "deepeel: TAS %s: recorded for a different ROM\n", x->opt.tas_paths[i]);
        play_session(x, x->opt.play_frames > t.frames ? x->opt.play_frames : t.frames, 0, &t);
        dp_pce_tas_free(&t);
    }
    if (x->opt.play_frames)
        for (uint32_t i = 0; i < x->opt.play_sessions && !x->failed; i++) {
            if ((x->total_steps >= x->opt.max_total || dp_stop)) break;
            play_session(x, x->opt.play_frames, x->opt.play_seed + i * 0x2545F491u, NULL);
        }

    novelty_search(x);

    // depth-first drain of the fork queue
drain:
    for (;;) {
        if (x->failed) break;
        if (!x->queue_len && x->park_len) { x->queue[x->queue_len++] = x->park[--x->park_len]; r->parked_resumed++; }
        if (!x->queue_len && dp_lf_pending(x->lot)) { // the disk backlog comes back
            uint8_t forced = 0, *blob = NULL; uint32_t ml = 0, bl = 0;
            if (dp_lf_get(x->lot, &forced, &ml, &blob, &bl) && ml == 1) {
                uLongf zl = x->zcap;
                uint8_t *z = malloc(zl);
                if (z && compress2(z, &zl, blob, bl, 1) == Z_OK) {
                    x->queue[x->queue_len].z = z; x->queue[x->queue_len].zlen = zl;
                    x->queue[x->queue_len].forced = forced; x->queue[x->queue_len].choice = 0; x->queue_len++;
                } else free(z);
                free(blob); blob = NULL;
            } else free(blob);
            if (!x->queue_len) continue;
        }
        if (!x->queue_len && (x->total_steps < x->opt.max_total && !dp_stop) && pstm_replay(x)) continue;
        if (!x->queue_len) break;
        if ((x->total_steps >= x->opt.max_total || dp_stop)) { r->global_cuts++; break; }
        PceQueued q = x->queue[--x->queue_len];
        run_path(x, q);
    }

    if (x->opt.bank_dispatch && !x->disp_done && !x->failed && (x->total_steps < x->opt.max_total && !dp_stop)) { x->disp_done = 1; if (dispatch_seed(x)) goto drain; }
    if (x->opt.second_pass && !x->failed && (x->total_steps < x->opt.max_total && !dp_stop) && second_pass_seed(x)) goto drain;

    if (x->opt.pre_finalize) x->opt.pre_finalize(r, x->opt.pre_user);
    if (!x->opt.no_finalize) {
        if (x->opt.static_closure) static_closure(x);
        index_declare(x);
        tables(x);
        sections_build(x);
        free(g_inferred);
        g_inferred = calloc(x->rom_size, 1);
        g_inferred_size = x->rom_size;
        if (g_inferred && x->opt.infer) { infer(x, g_inferred); siblings(x, g_inferred); }
    }

    pstm_free(x);
    dp_pce_observe(NULL);
    // states still queued when the global wall stopped the drain: counted
    // (saved in the lot when there is one: the next run resumes them)
    for (uint32_t i = 0; i < x->park_len; i++) if (x->park[i].choice) dp_tree_close(&x->tree, x->park[i].choice, DP_ISSUE_BUDGET);
    for (uint32_t i = 0; i < x->queue_len; i++) if (x->queue[i].choice) dp_tree_close(&x->tree, x->queue[i].choice, DP_ISSUE_BUDGET);
    for (uint32_t i = 0; i < x->park_len; i++)
        if (!lot_spill(x, x->park[i].z, x->park[i].zlen, x->park[i].forced)) { r->pending_dropped++; free(x->park[i].z); }
    free(x->park);
    for (uint32_t i = 0; i < x->queue_len; i++)
        if (!lot_spill(x, x->queue[i].z, x->queue[i].zlen, x->queue[i].forced)) { r->pending_dropped++; free(x->queue[i].z); }
    if (x->lot) {
        r->lot_written = x->lot->written; r->lot_read = x->lot->read_back;
        r->lot_pending = dp_lf_pending(x->lot);
        dp_lf_close(x->lot, x->opt.lot_path);
    }
    if (x->opt.tree_path) {
        r->tree_nodes = x->tree.n;
        if (!dp_tree_save(&x->tree, x->opt.tree_path)) x->failed = 1;
    }
    dp_tree_free(&x->tree);
    free(x->queue);
    dp_set_free(&x->ctx_seen);
    dp_set_free(&x->ind_sites);
    dp_set_free(&x->ind_pairs);
    free(x->prov_anchor);
    free(x->seen);
    free(x->fork_sites);
    free(x->pad_sites);
    free(x->irq_sites);
    free(x->istart);
    free(x->tmp);
    free(x->tmp2);
    free(x->ztmp);
    int ok = !x->failed;
    free(x);
    dp_pce_close();
    return ok;
}

// --- outputs ---------------------------------------------------------------------------------

uint8_t *dp_pce_map_build(const DpPceResults *r) {
    if (!r || !r->flags) return NULL;
    uint8_t *map = malloc(r->rom_size);
    if (!map) return NULL;
    for (size_t i = 0; i < r->rom_size; i++) {
        uint8_t f = r->flags[i], cls;
        if (f & DP_PCE_COPIED) cls = DP_PMAP_DATA_COPIED;
        else if (f & DP_PCE_TABLE) cls = DP_PMAP_TABLE;
        else if (f & DP_PCE_NATURAL) cls = DP_PMAP_CODE_NAT;
        else if (f & DP_PCE_FORCED) cls = DP_PMAP_CODE_FORCED;
        else if (f & DP_PCE_DATA) cls = DP_PMAP_DATA_READ;
        else if (f & DP_PCE_STATIC) cls = DP_PMAP_CODE_STATIC;
        else if (f & DP_PCE_PAD) cls = DP_PMAP_PAD;
        else if (g_inferred && i < g_inferred_size && g_inferred[i]) cls = g_inferred[i];
        else cls = DP_PMAP_UNKNOWN;
        map[i] = cls;
    }
    return map;
}

void dp_pce_rtcode(FILE *out, const DpPceResults *r) { dp_ramarch_write(&r->ra, out, "pce"); }

void dp_pce_map(FILE *out, const DpPceResults *r) {
    uint8_t *map = dp_pce_map_build(r);
    if (!map) return;
    fprintf(out, "# deepeel pce map: bank(16K) start end class — physical offsets\n");
    for (size_t i = 0; i < r->rom_size;) {
        size_t j = i;
        while (j < r->rom_size && map[j] == map[i] && (j >> 14) == (i >> 14)) j++;
        fprintf(out, "%x %04x %04x %s\n", (unsigned)(i >> 14), (unsigned)(i & 0x3FFF),
                (unsigned)((j - 1) & 0x3FFF), dp_pmap_names[map[i]]);
        i = j;
    }
    free(map);
}

void dp_pce_report(FILE *out, const DpPceResults *r) {
    uint8_t *map = dp_pce_map_build(r);
    uint64_t cls[DP_PMAP_COUNT] = { 0 }, observed = 0;
    for (size_t i = 0; i < r->rom_size; i++) {
        if (map) cls[map[i]]++;
        if (r->flags[i] & (DP_PCE_NATURAL | DP_PCE_FORCED | DP_PCE_DATA | DP_PCE_COPIED)) observed++;
    }
    free(map);
    double sz = r->rom_size ? (double)r->rom_size : 1.0;
    fprintf(out, "PC Engine exploration report\n");
    if (dp_pce_multitap_enabled()) fprintf(out, "Input: five-pad multitap model on (pads 2-5 held at rest; observation model, not a hardware measurement)\n");
    fprintf(out, "  rom %zu KiB (%zu pages of 8 KiB)\n", r->rom_size / 1024, r->rom_size / 0x2000);
    fprintf(out, "  observed %llu bytes (%.2f%%), classified %llu bytes (%.2f%%)\n",
            (unsigned long long)observed, 100.0 * (double)observed / sz,
            (unsigned long long)(r->rom_size - cls[0]), 100.0 * (double)(r->rom_size - cls[0]) / sz);
    for (int c = 0; c < DP_PMAP_COUNT; c++)
        fprintf(out, "    %-17s %8llu  %6.2f%%\n", dp_pmap_names[c], (unsigned long long)cls[c],
                100.0 * (double)cls[c] / sz);
    fprintf(out, "  steps %llu paths %llu (forced %llu) forks %llu pad-forks %llu irq-forks %llu evicted %llu\n",
            (unsigned long long)r->steps, (unsigned long long)r->paths,
            (unsigned long long)r->forced_paths, (unsigned long long)r->forks,
            (unsigned long long)r->pad_forks, (unsigned long long)r->irq_forks,
            (unsigned long long)r->evicted);
    fprintf(out, "  cuts: step %llu loop %llu fork %llu pad-fork %llu irq-fork %llu queue %llu global %llu illegal %llu\n",
            (unsigned long long)r->step_cuts, (unsigned long long)r->loop_cuts,
            (unsigned long long)r->fork_cuts, (unsigned long long)r->pad_fork_cuts,
            (unsigned long long)r->irq_fork_cuts, (unsigned long long)r->queue_cuts,
            (unsigned long long)r->global_cuts, (unsigned long long)r->illegal_cuts);
    if (r->lot_written || r->lot_read || r->lot_pending)
        fprintf(out, "  lot: %llu written, %llu read back, %llu left on disk\n", (unsigned long long)r->lot_written,
                (unsigned long long)r->lot_read, (unsigned long long)r->lot_pending);
    fprintf(out, "  frontier: parked %llu resumed %llu lost %llu\n", (unsigned long long)r->parked,
            (unsigned long long)r->parked_resumed, (unsigned long long)r->parked_lost);
    fprintf(out, "  pending paths dropped at the wall %llu\n", (unsigned long long)r->pending_dropped);
    fprintf(out, "  data reads %llu, VDC data writes %llu, ROM->VRAM bytes %llu, io writes %llu\n",
            (unsigned long long)r->data_reads, (unsigned long long)r->vdc_data_writes,
            (unsigned long long)r->copied_bytes, (unsigned long long)r->io_writes);
    fprintf(out, "  VCE colour-data writes %llu, palette bytes from ROM %llu\n",
            (unsigned long long)r->vce_data_writes, (unsigned long long)r->palette_bytes);
    fprintf(out, "  index inference (abs,X/Y): %u sites, %u bytes declared, %u refused over code, %u sites biased (start at index >= 1), "
            "%llu reads off the base page, %llu on forced paths (ignored), %llu sites dropped (cap)\n",
            r->nidx, r->idx_declared, r->idx_refused_code, r->idx_biased,
            (unsigned long long)r->idx_cross_page, (unsigned long long)r->idx_forced_ignored,
            (unsigned long long)r->idx_overflow);
    if (r->ctx_evicted) fprintf(out, "  context eviction: %llu states closed (coarse pc/regs/MPR context past the cap)\n", (unsigned long long)r->ctx_evicted);
    fprintf(out, "  indirect jumps: %llu sites, %llu targets, overflow %llu\n", (unsigned long long)r->indirect_sites, (unsigned long long)r->indirect_targets, (unsigned long long)r->indirect_overflow);
    fprintf(out, "  stream replay (S6): %llu decoders, %llu tables, %llu candidates, %llu queued replays (%llu saturated)\n",
            (unsigned long long)r->s6_decoders, (unsigned long long)r->s6_tables, (unsigned long long)r->s6_candidates,
            (unsigned long long)r->s6_runs, (unsigned long long)r->s6_sat);
    fprintf(out, "  provenance: %llu jump-table words traced from ROM reads\n", (unsigned long long)r->prov_sites);
    if (r->tree_nodes) fprintf(out, "  choice tree: %llu nodes\n", (unsigned long long)r->tree_nodes);
    if (r->pass2_seeds || r->pass2_refused || r->pass2_capped) fprintf(out, "  second pass: %llu seeds, %llu refused (page unmappable), %llu capped\n", (unsigned long long)r->pass2_seeds, (unsigned long long)r->pass2_refused, (unsigned long long)r->pass2_capped);
    if (r->disp_tables) fprintf(out, "  bank dispatch: %llu tables, %llu seeds, %llu refused views\n", (unsigned long long)r->disp_tables, (unsigned long long)r->disp_seeds, (unsigned long long)r->disp_refused_view);
    fprintf(out, "  copy siblings: %llu tables, %llu entries, %llu bytes typed, %llu table caps\n", (unsigned long long)r->sib_tables, (unsigned long long)r->sib_entries, (unsigned long long)r->sib_bytes, (unsigned long long)r->sib_caps);
    fprintf(out, "  entries: %llu seeded, %llu refused (outside the ROM mapping)\n", (unsigned long long)r->entries_seeded, (unsigned long long)r->entries_refused);
    fprintf(out, "  padding cuts: %llu\n", (unsigned long long)r->pad_cuts);
    fprintf(out, "  faults %llu, exec outside ROM %llu, queued state bytes %llu\n",
            (unsigned long long)r->faults, (unsigned long long)r->exec_outside,
            (unsigned long long)r->state_bytes_peak);
    fprintf(out, "  play: %llu sessions, %llu frames (TAS %llu)\n",
            (unsigned long long)r->play_sessions_run, (unsigned long long)r->play_frames_run,
            (unsigned long long)r->tas_frames_run);
    fprintf(out, "  tas identity: %llu same ROM, %llu different, %llu unrecorded\n", (unsigned long long)r->tas_same, (unsigned long long)r->tas_different, (unsigned long long)r->tas_unrecorded);
    if (r->state_exported) fprintf(out, "  state exported: deepest cell, %u frames from power-on\n", r->state_exported_depth);
    if (r->watch_frames) fprintf(out, "  watch: %llu frames published\n", (unsigned long long)r->watch_frames);
    fprintf(out, "  novelty: depth %u frames, %llu trials, %llu frames, %llu hits, %llu deaths, %llu archived (%llu replaced)\n",
            r->novelty_depth, (unsigned long long)r->novelty_trials, (unsigned long long)r->novelty_frames_run,
            (unsigned long long)r->novelty_hits, (unsigned long long)r->novelty_deaths,
            (unsigned long long)r->novelty_archived,
            (unsigned long long)r->novelty_replaced);
    fprintf(out, "  static closure %llu, tables %llu, padding %llu, inferred code %llu data %llu padding %llu\n",
            (unsigned long long)r->static_bytes, (unsigned long long)r->table_bytes,
            (unsigned long long)r->padding_bytes, (unsigned long long)r->inferred_code,
            (unsigned long long)r->inferred_data, (unsigned long long)r->inferred_pad);
    fprintf(out, "  interrupts: IRQ2/BRK %llu IRQ1 %llu timer %llu (log %u, overflow %llu)\n",
            (unsigned long long)r->irq_counts[0], (unsigned long long)r->irq_counts[1],
            (unsigned long long)r->irq_counts[2], r->interrupt_count,
            (unsigned long long)r->interrupt_overflow);
    fprintf(out, "  sections %u:\n", r->section_count);
    for (uint32_t i = 0; i < r->section_count; i++) {
        const DpPceSection *s = &r->sections[i];
        fprintf(out, "   section %u page $%02X $%05X-$%05X natural %llu forced %llu verdict %s\n",
                s->id, s->lo >> 13, s->lo, s->hi, (unsigned long long)s->natural_hits,
                (unsigned long long)s->forced_hits, dp_pce_verdict_names[s->verdict]);
    }
    fprintf(out, "  copies %u (overflow %llu):\n", r->copy_count, (unsigned long long)r->copy_overflow);
    for (unsigned i = 0; i < r->copy_count; i++) {
        const DpPceCopy *c = &r->copies[i];
        fprintf(out, "   %s src $%05X len %u pc $%04X x%llu\n", c->channel == 2 ? "block" : c->channel == 3 ? "cpu->vce" :
                c->channel == 4 ? "block->vce" : "cpu",
                c->source, c->length, c->pc, (unsigned long long)c->occurrences);
    }
    fprintf(out, "  MPR changes %u (overflow %llu):\n", r->bank_count, (unsigned long long)r->bank_overflow);
    for (unsigned i = 0; i < r->bank_count && i < 256; i++) {
        const DpPceBankEvent *b = &r->banks[i];
        fprintf(out, "   MPR%u $%02X -> $%02X pc $%04X step %llu\n", b->window, b->old_page,
                b->new_page, b->pc, (unsigned long long)b->step);
    }
    if (r->smc_writes) fprintf(out, "  self-modifying WRAM code: %llu stores into executed bytes (first at pc $%04X)\n",
                               (unsigned long long)r->smc_writes, r->smc_first_pc);
    fprintf(out, "  register writes %u logged (overflow %llu)\n", r->register_count,
            (unsigned long long)r->register_overflow);
    fputs("Limits: HuCard only. No CD-ROM2/Super CD (System Card, ADPCM, CD audio), no SF2 mapper beyond the "
          "modelled pages, no SuperGrafx (VPC, second VDC); the multitap is an opt-in model. Observations of "
          "the tool on a concrete machine, not hardware measurements.\n", out);
}

void dp_pce_evidence(FILE *out, const DpPceResults *r) {
    fputs("# physical-bank offset-start offset-end evidence-mask (inclusive, 8 KiB banks)\n"
          "# 01=code-natural 02=code-forced 04=data-read 08=data-copied 10=irq 20=padding\n"
          "# 40=static 80=table (unflagged cells are not listed)\n", out);
    for (size_t at = 0; at < r->rom_size;) {
        size_t end = at + 1;
        while (end < r->rom_size && (end >> 13) == (at >> 13) && r->flags[end] == r->flags[at]) end++;
        if (r->flags[at])
            fprintf(out, "%04zX %04zX %04zX %02X\n", at >> 13, at & 0x1fff, (end - 1) & 0x1fff, (unsigned)r->flags[at]);
        at = end;
    }
}
