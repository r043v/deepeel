// explore.c: the bounded NES explorer over m0boy's NES runtime.  Same
// contract as the GB and Z80 explorers: at every conditional both sides are
// explored (fork), at every pending NMI a no-NMI fork is attempted, and
// eviction keeps the work bounded — exact-state dedup, per-path step
// budget, per-site back-branch and fork caps, global step budget.  Every
// saturation lands in a counter, never in silence.  Everything observed
// lands on physical PRG cells (bank * $4000 + (addr & $3FFF)); execution
// outside PRG counts as exec_outside and never enters the cartridge maps.
#include "explore.h"
#include "../../mach/nesmach.h"
#include "../../cpu/m6502/m6502.h"
#include "nes_trace.h"
#include "tas.h"
#include <stdlib.h>
#include <string.h>

// one queued exploration state: a machine snapshot plus its provenance
typedef struct {
    uint8_t *state;      // nesmach blob, NULL once drained
    uint8_t forced;      // entered through a flipped branch
    uint8_t from_irq;    // entered through a cleared NMI edge
    uint64_t parent_path, parent_step;
    uint32_t steps;
} DpNesPathState;

#define DP_NES_STATE_MAX 65536
#define DP_NES_CKPT_RING 16

// back-branch counter entry: branch site rom cell + taken target
typedef struct { uint64_t key; uint32_t count; } DpNesLoop;

typedef struct {
    DpNesMach *m;
    const uint8_t *image;
    size_t image_size;
    int mapper;
    DpNesOptions opt;
    DpNesResults *r;
    DpNesPathState *queue;
    uint32_t queue_len, queue_cap;
    DpHashSet seen;                    // exact state dedup
    DpNesLoop *loops;                  // back-branch counters
    size_t loop_len, loop_cap;
    uint32_t *fork_sites;              // per-cell fork counts (parallel to flags)
    size_t fork_sites_len;
    uint64_t total_steps, path_steps, paths_opened;
    unsigned cur_pc, cur_len;          // current instruction's fetch window
    int was_outside;                   // previous step ran outside PRG
    // play/TAS session state (the on_frame hook drives it)
    int play_mode;                     // 0: fork exploration, 1: play/TAS
    uint8_t (*frame_masks)[2];         // inputs per frame (TAS or generated)
    uint8_t *frame_cmds;               // per-frame command byte (reset bit)
    uint32_t frame_count, frame_target;
    uint8_t (*ring)[DP_NES_STATE_MAX]; // frame-boundary checkpoints
    uint8_t snapshot[DP_NES_STATE_MAX];
    int jam_pending, failed;
    uint32_t rng;
} DpNesRun;

static uint32_t loop_bump(DpNesRun *x, uint64_t key);
static int snap_push(DpNesRun *x, DpNesPathState *s);

static void record_copy(DpNesRun *x, uint64_t path, uint64_t step, uint16_t pc,
                        uint32_t source, uint32_t destination, uint8_t channel) {
    DpNesResults *r = x->r;
    if (r->copy_count < DP_NES_COPY_CAP) {
        DpNesCopy *c = &r->copies[r->copy_count++];
        memset(c, 0, sizeof(*c));
        c->path = path;
        c->first_step = c->last_step = step;
        c->occurrences = 1;
        c->pc = pc;
        c->source = source;
        c->destination = destination;
        c->channel = channel;
    } else {
        r->copy_overflow++;
    }
}

static void record_bank(DpNesRun *x, uint64_t path, uint64_t step, uint16_t pc,
                        uint8_t window, uint8_t old_bank, uint8_t new_bank) {
    DpNesResults *r = x->r;
    if (r->bank_count < DP_NES_BANK_LOG_CAP) {
        DpNesBankEvent *b = &r->banks[r->bank_count++];
        b->seq = r->bank_count;
        b->path = path;
        b->step = step;
        b->pc = pc;
        b->window = window;
        b->old_bank = old_bank;
        b->new_bank = new_bank;
    } else {
        r->bank_overflow++;
    }
}

static void record_interrupt(DpNesRun *x, uint64_t path, uint64_t step,
                             uint16_t pc, uint8_t kind) {
    DpNesResults *r = x->r;
    if (r->interrupt_count < DP_NES_IRQ_LOG_CAP) {
        DpNesInterrupt *i = &r->interrupts[r->interrupt_count++];
        i->path = path;
        i->step = step;
        i->pc = pc;
        i->kind = kind;
        i->target_rom = dp_nesmach_rom_index(x->m, 0xFFFA); // vectors live in PRG
    } else {
        r->interrupt_overflow++;
    }
}

static void record_register(DpNesRun *x, uint64_t path, uint64_t step,
                            uint16_t pc, uint16_t addr, uint8_t value) {
    DpNesResults *r = x->r;
    if (r->register_count < 256) {
        DpNesRegisterWrite *w = &r->register_writes[r->register_count++];
        w->path = path;
        w->step = step;
        w->pc = pc;
        w->addr = addr;
        w->reg = (uint8_t)(addr & 0xFF);
        w->value = value;
    } else {
        r->register_overflow++;
    }
}

// --- hooks over m0boy's NES_TRACE ---------------------------------------------

static void hook_step(void *user, unsigned pc, unsigned op) {
    DpNesRun *x = user;
    DpNesResults *r = x->r;
    x->total_steps++;
    x->path_steps++;
    r->steps++;
    if (x->play_mode) r->play_steps_accounted++;

    // budgets: per-path first, then the global one
    if (!x->play_mode && x->path_steps >= x->opt.max_steps) {
        r->step_cuts++;
        nes_trace.stop = 1;
        return;
    }
    if (x->total_steps >= x->opt.max_total) {
        r->global_cuts++;
        nes_trace.stop = 1;
        return;
    }

    // decode from the live bus
    uint8_t bytes[3] = {
        dp_nesmach_read(x->m, pc),
        dp_nesmach_read(x->m, pc + 1),
        dp_nesmach_read(x->m, pc + 2),
    };
    DpM6502Instruction in;
    if (!dp_m6502_decode(bytes, 3, (uint16_t)pc, &in)) {
        r->decode_cuts++;
        nes_trace.stop = 1;
        return;
    }
    x->cur_pc = pc;
    x->cur_len = in.length;

    // executed PRG cells (before execution, from this exact state)
    if (!x->play_mode) {
        for (unsigned i = 0; i < in.length; i++) {
            int64_t cell = dp_nesmach_rom_index(x->m, pc + i);
            if (cell >= 0) {
                if (!r->flags[cell])
                    r->flags[cell] = (uint8_t)(x->queue[x->queue_len - 1].forced
                                                   ? DP_NES_FORCED : DP_NES_NATURAL);
                else if (x->queue[x->queue_len - 1].forced)
                    r->flags[cell] |= DP_NES_FORCED;
            }
        }
    } else {
        for (unsigned i = 0; i < in.length; i++) {
            int64_t cell = dp_nesmach_rom_index(x->m, pc + i);
            if (cell >= 0) {
                if (!r->flags[cell]) r->flags[cell] = DP_NES_NATURAL;
            } else {
                r->exec_outside++;
            }
        }
    }

    // execution outside PRG (RAM trampolines): counted per transition, the
    // path continues
    int outside = dp_nesmach_rom_index(x->m, pc) < 0;
    if (outside && !x->was_outside) r->exec_outside++;
    x->was_outside = outside;

    // a jam ends the path (the runtime runs illegals as NOPs; the explorer
    // stops on the KIL family before the machine drifts)
    if (in.flow == DP_M6502_JAM) {
        r->faults++;
        if (x->play_mode) x->jam_pending = 1;
        nes_trace.stop = 1;
        return;
    }

    if (x->play_mode) return; // no forks during a play/TAS session

    // fork the other branch side before executing, from this exact state
    if (in.flow == DP_M6502_BRANCH) {
        int64_t site = dp_nesmach_rom_index(x->m, pc);
        if (site >= 0 && x->fork_sites && x->fork_sites[site] < x->opt.fork_cap) {
            // the machine is live, so the fork force applies inside a fresh
            // snapshot: save, flip the flags so the branch takes the other
            // side, queue
            uint8_t *snap = malloc(dp_nesmach_state_size());
            if (snap) {
                dp_nesmach_save(x->m, snap);
                m6502_t t;
                memset(&t, 0, sizeof t);
                t.p = *dp_nesmach_state_p(snap);
                int taken = dp_m6502_condition(in.condition, &t);
                dp_m6502_force(in.condition, &t, !taken);
                *dp_nesmach_state_p(snap) = t.p;
                DpNesPathState f = { snap, 1, 0, x->paths_opened, x->total_steps, 0 };
                // the per-site budget only pays for forks that were queued
                if (snap_push(x, &f)) { x->fork_sites[site]++; r->forks++; }
            }
        } else if (site >= 0) {
            r->fork_cuts++;
        }
        // back-branch guard on taken jumps into lower addresses
        m6502_t t;
        memset(&t, 0, sizeof t);
        t.p = (uint8_t)dp_nesmach_p(x->m);
        if (dp_m6502_condition(in.condition, &t) && in.target < pc) {
            uint64_t key = site >= 0 ? ((uint64_t)site << 16) | (in.target & 0xFFFF) : 0;
            if (loop_bump(x, key) > x->opt.loop_cap) {
                r->loop_cuts++;
                nes_trace.stop = 1;
                return;
            }
        }
    }

    // NMI fork: the edge is pending, queue a state where it is cleared so
    // the no-NMI side is explored too (the natural path takes the NMI)
    if (x->opt.seed_irq && dp_nesmach_nmi_pending(x->m) &&
        x->path_steps + 1 < x->opt.max_steps) {
        uint8_t *snap = malloc(dp_nesmach_state_size());
        if (snap) {
            dp_nesmach_save(x->m, snap);
            *dp_nesmach_state_nmi_edge(snap) = 0;
            DpNesPathState f = { snap, 0, 1, x->paths_opened, x->total_steps, 0 };
            snap_push(x, &f);
            r->irq_forks++;
        }
    }
}

static void hook_read(void *user, unsigned addr) {
    DpNesRun *x = user;
    if (addr < x->cur_pc || addr >= x->cur_pc + x->cur_len) {
        int64_t cell = dp_nesmach_rom_index(x->m, addr);
        if (cell >= 0) {
            x->r->data_reads++;
            if (!x->r->flags[cell]) x->r->flags[cell] = DP_NES_DATA;
        }
    }
}

static void hook_write(void *user, unsigned addr, unsigned value) {
    DpNesRun *x = user;
    DpNesResults *r = x->r;
    if (addr >= 0x8000) {
        // UxROM bank switch (fires before the runtime applies it)
        if (dp_nesmach_banked(x->m)) {
            uint8_t old = (uint8_t)dp_nesmach_bank(x->m);
            uint8_t nb = (uint8_t)(value & (dp_nesmach_bank_count(x->m) - 1));
            if (old != nb)
                record_bank(x, 0, x->total_steps, (uint16_t)x->cur_pc, 0, old, nb);
        }
        return;
    }
    if (addr >= 0x2000 && addr < 0x4020) {
        r->io_writes++;
        unsigned idx = (addr - 0x2000) & 0x3F;
        r->io_writes_io_reg[idx < 32 ? idx : 31]++;
        if (addr == 0x4014) {
            // OAM DMA: a full page lands in OAM; pages at or above $80 read PRG
            uint32_t src_page = value << 8;
            for (unsigned i = 0; i < 256 && value >= 0x80; i++) {
                int64_t cell = dp_nesmach_rom_index(x->m, 0x8000u | ((src_page + i) & 0x7FFF));
                if (cell >= 0) r->flags[cell] |= DP_NES_COPIED;
            }
            if (value >= 0x80) r->oam_copies++;
            int64_t base = value >= 0x80
                ? (uint32_t)dp_nesmach_rom_index(x->m, 0x8000u | (src_page & 0x7FFF))
                : src_page;
            record_copy(x, 0, x->total_steps, (uint16_t)x->cur_pc,
                        (uint32_t)base, 0, 1);
        }
        record_register(x, 0, x->total_steps, (uint16_t)x->cur_pc, (uint16_t)addr,
                        (uint8_t)value);
    }
}

static void hook_interrupt(void *user, unsigned vector) {
    DpNesRun *x = user;
    record_interrupt(x, 0, x->total_steps, (uint16_t)x->cur_pc,
                     vector == 0xFFFA ? 0 : 1);
    x->r->irq_counts[vector == 0xFFFA ? 0 : 1]++;
}

static void hook_frame(void *user) {
    DpNesRun *x = user;
    if (!x->play_mode) return;
    x->frame_count++;
    x->r->play_frames_run++;
    if (x->frame_count >= x->frame_target) { nes_trace.stop = 1; return; }
    // next frame's inputs (TAS command byte may demand a reset first)
    if (x->frame_cmds && x->frame_cmds[x->frame_count] & 2) dp_nesmach_reset(x->m);
    dp_nesmach_set_input(x->m, x->frame_masks[x->frame_count][0],
                         x->frame_masks[x->frame_count][1]);
}

// --- loop guard: per-site back-branch counters --------------------------------

static uint32_t loop_bump(DpNesRun *x, uint64_t key) {
    for (size_t i = 0; i < x->loop_len; i++)
        if (x->loops[i].key == key) return ++x->loops[i].count;
    if (x->loop_len == x->loop_cap) {
        x->loop_cap = x->loop_cap ? x->loop_cap * 2 : 256;
        x->loops = realloc(x->loops, x->loop_cap * sizeof(*x->loops));
        if (!x->loops) { x->failed = 1; return 0; }
    }
    x->loops[x->loop_len].key = key;
    x->loops[x->loop_len].count = 1;
    x->loop_len++;
    return 1;
}

// --- queue --------------------------------------------------------------------

static int queue_push(DpNesRun *x, const DpNesPathState *s) {
    if (x->queue_len >= x->opt.queue_cap) { x->r->queue_cuts++; return 0; }
    if (x->queue_len == x->queue_cap) return 0;
    x->queue[x->queue_len++] = *s;
    return 1;
}

// exact-state dedup: the snapshot hash (volatile clocks excluded)
// returns 1 when the state was queued
static int snap_push(DpNesRun *x, DpNesPathState *s) {
    uint64_t h = dp_nesmach_state_hash(s->state);
    if (!dp_set_insert(&x->seen, h)) { x->r->evicted++; free(s->state); return 0; }
    if (!queue_push(x, s)) { x->r->seeds_refused_queue++; free(s->state); return 0; }
    return 1;
}

// --- per-path execution ---------------------------------------------------------

// run one path from its snapshot; forks enqueue new states
static void run_path(DpNesRun *x, uint32_t slot) {
    DpNesPathState *self = &x->queue[slot];
    DpNesResults *r = x->r;
    // the snapshot blob stays owned by the slot until a fork needs a copy
    uint8_t *own = self->state;
    self->state = NULL;

    dp_nesmach_load(x->m, own);
    x->path_steps = 0;
    x->was_outside = 0;
    x->jam_pending = 0;
    uint64_t path = x->paths_opened++;
    r->paths++;
    if (self->from_irq) {
        r->irq_forks++;
        record_interrupt(x, path, self->parent_step,
                         (uint16_t)dp_nesmach_pc(x->m), 0);
        r->irq_counts[0]++;
    }
    r->forced_paths += self->forced != 0;

    // runs until the hook's budgets stop it (step, global, jam, loop)
    dp_nesmach_run(x->m, UINT64_MAX);
    free(own);
}

// --- sections -------------------------------------------------------------------

static void sections_build(DpNesRun *x) {
    DpNesResults *r = x->r;
    size_t n = r->prg_size;
    r->section_capacity = 64;
    r->sections = calloc(r->section_capacity, sizeof(DpNesSection));
    r->section_of = calloc(n, sizeof(uint32_t));
    if (!r->sections || !r->section_of) { x->failed = 1; return; }
    uint8_t code_mask = (uint8_t)(DP_NES_NATURAL | DP_NES_FORCED);
    size_t i = 0;
    while (i < n) {
        if (!(r->flags[i] & code_mask)) { i++; continue; }
        size_t j = i;
        uint8_t bank = (uint8_t)(i / 0x4000);
        while (j < n && (r->flags[j] & code_mask) && j / 0x4000 == bank) j++;
        if (r->section_count == r->section_capacity) {
            r->section_capacity *= 2;
            r->sections = realloc(r->sections, r->section_capacity * sizeof(DpNesSection));
            if (!r->sections) { x->failed = 1; return; }
        }
        DpNesSection *s = &r->sections[r->section_count];
        memset(s, 0, sizeof(*s));
        s->id = r->section_count + 1;
        s->bank = bank;
        s->lo = (uint16_t)(i - bank * 0x4000);
        s->hi = (uint16_t)(j - 1 - bank * 0x4000);
        s->entries = 1;
        for (size_t k = i; k < j; k++) {
            r->section_of[k] = s->id;
            if (r->flags[k] & DP_NES_FORCED) s->forced_hits++;
            else s->natural_hits++;
        }
        r->section_count++;
        i = j;
    }
    // verdicts: forced-only sections are candidates data-with-code
    for (uint32_t k = 0; k < r->section_count; k++) {
        DpNesSection *s = &r->sections[k];
        if (s->natural_hits) s->verdict = DP_NES_VERDICT_REQUIRED;
        else s->verdict = DP_NES_VERDICT_FORCED;
    }
}

// --- play / TAS sessions ---------------------------------------------------------

// xorshift32: enough randomness for input policies, cheap and portable
static uint32_t nes_rng(uint32_t *state) {
    uint32_t v = *state;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return *state = v;
}

// one play or TAS session: `frames` frames from power-on (or from a reset
// the movie commands).  The machine runs through the hooks; a jam rewinds
// eight frames (a deterministic jam always recurs, an earlier one has new
// inputs — TAS frames replay as recorded, play frames get fresh inputs).
static void play_session(DpNesRun *x, uint32_t frames, uint32_t seed,
                         const DpNesTas *tas) {
    DpNesResults *r = x->r;
    x->play_mode = 1;
    x->frame_count = 0;
    x->frame_target = frames;
    x->rng = seed ? seed : 0x9e3779b9u;
    for (uint32_t f = 0; f < frames; f++) {
        // inputs for frame f: the movie's when one is loaded, random otherwise
        if (tas && f < tas->frames) {
            x->frame_masks[f][0] = tas->masks[f];
            x->frame_masks[f][1] = (uint8_t)(tas->masks[f] >> 4);
            x->frame_cmds[f] = tas->cmds ? tas->cmds[f] : 0;
        } else {
            x->frame_masks[f][0] = (uint8_t)(nes_rng(&x->rng) & 0xFF);
            x->frame_masks[f][1] = (uint8_t)(nes_rng(&x->rng) & 0xFF);
            x->frame_cmds[f] = 0;
        }
    }
    dp_nesmach_reset(x->m);
    if (x->frame_cmds[0] & 2) dp_nesmach_reset(x->m);
    dp_nesmach_set_input(x->m, x->frame_masks[0][0], x->frame_masks[0][1]);
    uint32_t f = 0;
    while (f < frames) {
        if (x->total_steps >= x->opt.max_total) { r->global_cuts++; break; }
        dp_nesmach_save(x->m, x->ring[f % DP_NES_CKPT_RING]);
        x->jam_pending = 0;
        dp_nesmach_run(x->m, UINT64_MAX); // one frame (hook_frame stops it)
        if (x->jam_pending) {
            r->faults++;
            // rewind eight frames: the deterministic jam recurs there
            uint32_t back = f >= 8 ? f - 8 : 0;
            dp_nesmach_load(x->m, x->ring[back % DP_NES_CKPT_RING]);
            if (x->frame_cmds[(f + 1) % frames] & 2) dp_nesmach_reset(x->m);
            dp_nesmach_set_input(x->m, x->frame_masks[(f + 1) % frames][0],
                                 x->frame_masks[(f + 1) % frames][1]);
        }
        f++;
    }
    x->play_mode = 0;
    r->play_sessions_run++;
}

// --- driver -----------------------------------------------------------------------

DpNesOptions dp_nes_options(void) {
    DpNesOptions o = { 0 };
    o.max_total = 20000000;
    o.max_steps = 200000;
    o.max_paths = 8192;
    o.queue_cap = 8192;
    o.loop_cap = 4096;
    o.fork_cap = 4;
    o.seed_irq = 1;
    o.play_frames = 0;
    o.play_sessions = 8;
    o.play_seed = 1;
    return o;
}

void dp_nes_results_free(DpNesResults *r) {
    if (!r) return;
    free(r->flags);
    free(r->sections);
    free(r->section_of);
    r->flags = NULL;
    r->sections = NULL;
    r->section_of = NULL;
}

static void hooks_install(DpNesRun *x) {
    DpNesHooks h;
    memset(&h, 0, sizeof h);
    h.user = x;
    h.on_step = hook_step;
    h.on_read = hook_read;
    h.on_write = hook_write;
    h.on_interrupt = hook_interrupt;
    h.on_frame = hook_frame;
    dp_nesmach_set_hooks(x->m, &h);
}

int dp_nes_explore(const uint8_t *data, size_t size, int mapper,
                   const DpNesOptions *options, DpNesResults *results) {
    DpNesRun x;
    memset(&x, 0, sizeof(x));
    if (!data || !results) return 0;
    char err[256];
    x.m = dp_nesmach_open(data, size, mapper, err, sizeof err);
    if (!x.m) {
        fprintf(stderr, "deepeel: %s\n", err);
        return 0;
    }
    x.image = data;
    x.image_size = size;
    x.mapper = mapper;
    x.opt = options ? *options : dp_nes_options();

    DpNesResults *r = results;
    memset(r, 0, sizeof(*r));
    x.r = r;
    r->options = x.opt;
    r->prg_size = dp_nesmach_prg_size(x.m);
    r->flags = calloc(r->prg_size, 1);
    if (!r->flags) { dp_nesmach_close(x.m); return 0; }
    if (dp_nesmach_state_size() > DP_NES_STATE_MAX) { dp_nesmach_close(x.m); return 0; }

    // play/TAS scratch
    uint32_t fmax = x.opt.play_frames;
    for (int i = 0; i < x.opt.tas_count; i++) {
        DpNesTas t;
        if (dp_nes_tas_load(x.opt.tas_paths[i], &t, err, sizeof err)) {
            if (t.frames > fmax) fmax = t.frames;
            dp_nes_tas_free(&t);
        }
    }
    if (fmax) {
        x.frame_masks = calloc(fmax, 2);
        x.frame_cmds = calloc(fmax, 1);
        x.ring = calloc(DP_NES_CKPT_RING, DP_NES_STATE_MAX);
        if (!x.frame_masks || !x.frame_cmds || !x.ring) x.failed = 1;
    }

    x.queue_cap = x.opt.queue_cap;
    x.queue = calloc(x.queue_cap ? x.queue_cap : 1, sizeof(DpNesPathState));
    x.fork_sites_len = r->prg_size;
    x.fork_sites = calloc(x.fork_sites_len ? x.fork_sites_len : 1, sizeof(uint32_t));
    dp_set_init(&x.seen, 1 << 22);
    if (!x.queue || !x.fork_sites) x.failed = 1;
    hooks_install(&x);

    // play and TAS sessions first: their visited bytes land in the shared
    // map, their end states seed the fork queue
    if (!x.failed) {
        for (int i = 0; i < x.opt.tas_count && !x.failed; i++) {
            if (x.total_steps >= x.opt.max_total) break;
            DpNesTas t;
            if (!dp_nes_tas_load(x.opt.tas_paths[i], &t, err, sizeof err)) {
                fprintf(stderr, "deepeel: TAS %s: %s\n", x.opt.tas_paths[i], err);
                continue;
            }
            uint32_t frames = x.opt.play_frames ? x.opt.play_frames : t.frames;
            if (t.rom_name[0])
                fprintf(stderr, "deepeel: TAS %s: rom '%s', %u frames\n",
                        x.opt.tas_paths[i], t.rom_name, t.frames);
            play_session(&x, frames, 0, &t);
            dp_nes_tas_free(&t);
        }
        if (x.opt.play_frames) {
            for (uint32_t i = 0; i < x.opt.play_sessions; i++) {
                if (x.total_steps >= x.opt.max_total) break;
                play_session(&x, x.opt.play_frames, x.opt.play_seed + i * 0x2545F491u, NULL);
            }
        }
    }

    // fork exploration: one root state (reset), depth-first drain
    if (!x.failed && x.opt.queue_cap > 0) {
        uint8_t *snap = malloc(dp_nesmach_state_size());
        if (!snap) { x.failed = 1; }
        else {
            dp_nesmach_reset(x.m);
            dp_nesmach_save(x.m, snap);
            DpNesPathState root = { snap, 0, 0, 0, 0, 0 };
            r->seeds_reset = 1;
            queue_push(&x, &root);
            while (x.queue_len > 0 && !x.failed) {
                uint32_t slot = x.queue_len - 1;
                run_path(&x, slot);
                // the slot is drained (run_path cleared its state); the
                // forks it queued sit above it — take the last one's place
                if (x.queue_len - 1 > slot)
                    x.queue[slot] = x.queue[x.queue_len - 1];
                x.queue_len--;
                if (x.total_steps >= x.opt.max_total) break;
            }
        }
    }

    sections_build(&x);

    // states still queued when a wall stopped the drain: counted, not silent
    for (uint32_t i = 0; i < x.queue_len; i++) {
        if (x.queue[i].state) r->pending_dropped++;
        free(x.queue[i].state);
    }
    free(x.queue);
    free(x.loops);
    free(x.fork_sites);
    free(x.frame_masks);
    free(x.frame_cmds);
    free(x.ring);
    dp_set_free(&x.seen);
    int ok = !x.failed;
    dp_nesmach_close(x.m);
    return ok;
}

// --- outputs ------------------------------------------------------------------------

uint8_t *dp_nes_map_build(const DpNesResults *r) {
    if (!r || !r->flags) return NULL;
    uint8_t *map = malloc(r->prg_size);
    if (!map) return NULL;
    for (size_t i = 0; i < r->prg_size; i++) {
        uint8_t f = r->flags[i];
        uint8_t cls;
        if (f & DP_NES_COPIED) cls = DP_NMAP_DATA_COPIED;
        else if ((f & (DP_NES_NATURAL | DP_NES_FORCED)) && (f & DP_NES_DATA)) cls = DP_NMAP_CODE_STATIC;
        else if (f & DP_NES_FORCED) cls = DP_NMAP_CODE_FORCED;
        else if (f & DP_NES_NATURAL) cls = DP_NMAP_CODE_NAT;
        else if (f & DP_NES_DATA) cls = DP_NMAP_DATA_READ;
        else cls = DP_NMAP_UNKNOWN;
        map[i] = cls;
    }
    return map;
}

void dp_nes_map(FILE *out, const DpNesResults *r) {
    uint8_t *map = dp_nes_map_build(r);
    if (!map) return;
    fputs("nes map v1\n", out);
    for (size_t i = 0; i < r->prg_size; i++)
        fputc('0' + map[i], out);
    fputc('\n', out);
    free(map);
}

void dp_nes_report(FILE *out, const DpNesResults *r) {
    uint64_t touched = 0;
    for (size_t i = 0; i < r->prg_size; i++)
        if (r->flags[i]) touched++;
    fprintf(out, "NES exploration report\n");
    fprintf(out, "  prg %zu KiB, touched %llu bytes, coverage %.2f%%\n",
        r->prg_size / 1024, (unsigned long long)touched,
        r->prg_size ? 100.0 * (double)touched / (double)r->prg_size : 0.0);
    fprintf(out, "  steps %llu paths %llu (forced %llu) forks %llu irq-forks %llu evicted %llu\n",
        (unsigned long long)r->steps, (unsigned long long)r->paths,
        (unsigned long long)r->forced_paths,
        (unsigned long long)r->forks, (unsigned long long)r->irq_forks,
        (unsigned long long)r->evicted);
    fprintf(out, "  cuts: step %llu loop %llu fork %llu queue %llu global %llu path %llu\n",
        (unsigned long long)r->step_cuts, (unsigned long long)r->loop_cuts,
        (unsigned long long)r->fork_cuts, (unsigned long long)r->queue_cuts,
        (unsigned long long)r->global_cuts, (unsigned long long)r->path_cuts);
    fprintf(out, "  data reads %llu, OAM DMA copies %llu, ram writes %llu, io writes %llu\n",
        (unsigned long long)r->data_reads, (unsigned long long)r->oam_copies,
        (unsigned long long)r->ram_writes, (unsigned long long)r->io_writes);
    fprintf(out, "  faults %llu decode cuts %llu exec outside PRG %llu\n",
        (unsigned long long)r->faults, (unsigned long long)r->decode_cuts,
        (unsigned long long)r->exec_outside);
    fprintf(out, "  play: %llu sessions x frames %llu\n",
        (unsigned long long)r->play_sessions_run, (unsigned long long)r->play_frames_run);
    fprintf(out, "  pending paths dropped at the wall %llu\n",
        (unsigned long long)r->pending_dropped);
    fprintf(out, "  seeds: reset %llu irq %llu bank %llu play %llu refused %llu\n",
        (unsigned long long)r->seeds_reset, (unsigned long long)r->seeds_irq,
        (unsigned long long)r->seeds_bank, (unsigned long long)r->seeds_play,
        (unsigned long long)r->seeds_refused_queue);
    fprintf(out, "  sections %u: %s\n", r->section_count, dp_nmap_names[0]);
    for (uint32_t i = 0; i < r->section_count; i++) {
        const DpNesSection *s = &r->sections[i];
        fprintf(out, "  section %u bank %u $%04X-$%04X hits %llu natural %llu forced %llu"
                     " copies %llu verdict %s\n",
            s->id, s->bank, s->lo, s->hi,
            (unsigned long long)s->hits, (unsigned long long)s->natural_hits,
            (unsigned long long)s->forced_hits, (unsigned long long)s->copies,
            dp_nes_verdict_names[s->verdict]);
    }
    fprintf(out, "  copies %u (overflow %llu):\n", r->copy_count,
        (unsigned long long)r->copy_overflow);
    for (unsigned i = 0; i < r->copy_count; i++) {
        const DpNesCopy *c = &r->copies[i];
        fprintf(out, "   ch%u src $%08X dst $%08X pc $%04X step %llu\n",
            c->channel, c->source, c->destination, c->pc,
            (unsigned long long)c->first_step);
    }
    fprintf(out, "  banks %u (overflow %llu):\n", r->bank_count,
        (unsigned long long)r->bank_overflow);
    for (unsigned i = 0; i < r->bank_count; i++) {
        const DpNesBankEvent *b = &r->banks[i];
        fprintf(out, "   win%u bank %u -> %u pc $%04X step %llu\n",
            b->window, b->old_bank, b->new_bank, b->pc, (unsigned long long)b->step);
    }
    fprintf(out, "  interrupts %u (overflow %llu): NMI %llu IRQ %llu\n",
        r->interrupt_count, (unsigned long long)r->interrupt_overflow,
        (unsigned long long)r->irq_counts[0], (unsigned long long)r->irq_counts[1]);
    fprintf(out, "  register writes %u (overflow %llu)\n", r->register_count,
        (unsigned long long)r->register_overflow);
    for (unsigned i = 0; i < r->register_count; i++) {
        const DpNesRegisterWrite *w = &r->register_writes[i];
        fprintf(out, "   $%04X <- $%02X pc $%04X\n", w->addr, w->value, w->pc);
    }
}
