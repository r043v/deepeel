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
#include "pce.h"
#include "tas.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    uint8_t *z;            // compressed bridge state
    uLongf zlen;
    uint8_t forced;
} PceQueued;

enum { LOOP_SLOTS = 4096 };
typedef struct { uint64_t key; uint32_t count; } PceLoop;

typedef struct {
    DpPceOptions opt;
    DpPceResults *r;
    const uint8_t *rom;
    size_t rom_size;
    PceQueued *queue;
    uint32_t queue_len;
    PceQueued *park; uint32_t park_len, park_cap; // deferred holding area (queue full)
    uint64_t *seen;        // open-addressed dedup set
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

static void obs_rom_read(void *user, uint32_t cell, uint16_t addr) {
    PceRun *x = user;
    if (addr >= x->cur_pc && (unsigned)addr < (unsigned)x->cur_pc + x->cur_len) return; // fetch
    if (cell >= x->rom_size) return;
    x->r->data_reads++;
    mark(x, cell, DP_PCE_DATA);
    x->last_read_cell = cell;
    x->last_read_step = x->total_steps;
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

// queue the live machine state (dedup on its control-state hash)
static int push_live(PceRun *x, int forced) {
    DpPceResults *r = x->r;
    if (!seen_insert(x, dp_pce_state_hash())) { r->evicted++; return 0; }
    int full = x->queue_len >= x->opt.queue_cap;
    if (full && x->park_len >= 4096) { r->queue_cuts++; r->parked_lost++; return 0; }
    dp_pce_save(x->tmp2);
    uLongf zl = x->zcap;
    if (compress2(x->ztmp, &zl, x->tmp2, x->state_size, 1) != Z_OK) { x->failed = 1; return 0; }
    uint8_t *z = malloc(zl);
    if (!z) { x->failed = 1; return 0; }
    memcpy(z, x->ztmp, zl);
    if (full) { // a full queue is not a loss: the state waits aside (bounded, counted)
        if (x->park_len == x->park_cap) {
            uint32_t nc = x->park_cap ? x->park_cap * 2 : 64;
            PceQueued *g = realloc(x->park, nc * sizeof *g);
            if (!g) { free(z); r->queue_cuts++; r->parked_lost++; return 0; }
            x->park = g; x->park_cap = nc;
        }
        x->park[x->park_len].z = z; x->park[x->park_len].zlen = zl; x->park[x->park_len].forced = (uint8_t)forced;
        x->park_len++; r->parked++;
        return 1;
    }
    x->queue[x->queue_len].z = z;
    x->queue[x->queue_len].zlen = zl;
    x->queue[x->queue_len].forced = (uint8_t)forced;
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
    int outside = site < 0;
    if (outside && !x->was_outside) r->exec_outside++;
    x->was_outside = outside;

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
    x->path_id = r->paths++;
    r->forced_paths += q.forced != 0;
    x->last_read_cell = -1;
    for (;;) {
        if (x->path_steps >= x->opt.max_steps) { r->step_cuts++; break; }
        if (x->total_steps >= x->opt.max_total) { r->global_cuts++; break; }
        int frame;
        if (!step_one(x, &frame)) break;
    }
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
        if (x->total_steps >= x->opt.max_total) { r->global_cuts++; break; }
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
        if (x->total_steps >= x->opt.max_total) { r->global_cuts++; break; }
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
    free(r->flags);
    free(r->sections);
    r->flags = NULL;
    r->sections = NULL;
}

static uint8_t *g_inferred; // the last run's inference (map building)
static size_t g_inferred_size;

int dp_pce_explore(const uint8_t *data, size_t size, const DpPceOptions *options,
                   DpPceResults *results) {
    if (!data || !results) return 0;
    if (!dp_pce_init(data, size)) {
        fprintf(stderr, "deepeel: not a HuCard image (size %zu)\n", size);
        return 0;
    }
    PceRun *x = calloc(1, sizeof *x);
    if (!x) return 0;
    DpPceResults *r = results;
    memset(r, 0, sizeof *r);
    x->opt = options ? *options : dp_pce_options();
    r->options = x->opt;
    x->r = r;
    x->rom = dp_pce_rom();
    x->rom_size = r->rom_size = dp_pce_rom_size();
    x->last_read_cell = -1;
    memset(x->page_win, 0xFF, sizeof x->page_win);
    r->flags = calloc(x->rom_size, 1);
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
    if (!x->failed && x->opt.queue_cap) push_live(x, 0);

    // TAS movies, then the play sessions; their checkpoints join the queue
    for (int i = 0; i < x->opt.tas_count && !x->failed; i++) {
        DpPceTas t;
        char err[256];
        if (!dp_pce_tas_load(x->opt.tas_paths[i], &t, err, sizeof err)) {
            fprintf(stderr, "deepeel: TAS %s: %s\n", x->opt.tas_paths[i], err);
            continue;
        }
        fprintf(stderr, "deepeel: TAS %s: %u frames\n", x->opt.tas_paths[i], t.frames);
        play_session(x, x->opt.play_frames > t.frames ? x->opt.play_frames : t.frames, 0, &t);
        dp_pce_tas_free(&t);
    }
    if (x->opt.play_frames)
        for (uint32_t i = 0; i < x->opt.play_sessions && !x->failed; i++) {
            if (x->total_steps >= x->opt.max_total) break;
            play_session(x, x->opt.play_frames, x->opt.play_seed + i * 0x2545F491u, NULL);
        }

    novelty_search(x);

    // depth-first drain of the fork queue
    while ((x->queue_len || x->park_len) && !x->failed) {
        if (!x->queue_len) { x->queue[x->queue_len++] = x->park[--x->park_len]; r->parked_resumed++; }
        if (x->total_steps >= x->opt.max_total) { r->global_cuts++; break; }
        PceQueued q = x->queue[--x->queue_len];
        run_path(x, q);
    }

    if (x->opt.static_closure) static_closure(x);
    tables(x);
    sections_build(x);
    free(g_inferred);
    g_inferred = calloc(x->rom_size, 1);
    g_inferred_size = x->rom_size;
    if (g_inferred && x->opt.infer) infer(x, g_inferred);

    dp_pce_observe(NULL);
    // states still queued when the global wall stopped the drain: counted
    r->pending_dropped += x->queue_len + x->park_len;
    for (uint32_t i = 0; i < x->park_len; i++) free(x->park[i].z);
    free(x->park);
    for (uint32_t i = 0; i < x->queue_len; i++) free(x->queue[i].z);
    free(x->queue);
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
    fprintf(out, "  frontier: parked %llu resumed %llu lost %llu\n", (unsigned long long)r->parked,
            (unsigned long long)r->parked_resumed, (unsigned long long)r->parked_lost);
    fprintf(out, "  pending paths dropped at the wall %llu\n", (unsigned long long)r->pending_dropped);
    fprintf(out, "  data reads %llu, VDC data writes %llu, ROM->VRAM bytes %llu, io writes %llu\n",
            (unsigned long long)r->data_reads, (unsigned long long)r->vdc_data_writes,
            (unsigned long long)r->copied_bytes, (unsigned long long)r->io_writes);
    fprintf(out, "  faults %llu, exec outside ROM %llu, queued state bytes %llu\n",
            (unsigned long long)r->faults, (unsigned long long)r->exec_outside,
            (unsigned long long)r->state_bytes_peak);
    fprintf(out, "  play: %llu sessions, %llu frames (TAS %llu)\n",
            (unsigned long long)r->play_sessions_run, (unsigned long long)r->play_frames_run,
            (unsigned long long)r->tas_frames_run);
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
        fprintf(out, "   %s src $%05X len %u pc $%04X x%llu\n", c->channel == 2 ? "block" : "cpu",
                c->source, c->length, c->pc, (unsigned long long)c->occurrences);
    }
    fprintf(out, "  MPR changes %u (overflow %llu):\n", r->bank_count, (unsigned long long)r->bank_overflow);
    for (unsigned i = 0; i < r->bank_count && i < 256; i++) {
        const DpPceBankEvent *b = &r->banks[i];
        fprintf(out, "   MPR%u $%02X -> $%02X pc $%04X step %llu\n", b->window, b->old_page,
                b->new_page, b->pc, (unsigned long long)b->step);
    }
    fprintf(out, "  register writes %u logged (overflow %llu)\n", r->register_count,
            (unsigned long long)r->register_overflow);
}
