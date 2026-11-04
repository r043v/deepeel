// explore.c: the bounded NES explorer over m0boy's NES runtime.  Same
// contract as the GB and Z80 explorers: at every conditional both sides are
// explored (fork), at every pending NMI a no-NMI fork is attempted, and
// eviction keeps the work bounded — exact-state dedup, per-path step
// budget, per-site back-branch and fork caps, global step budget.  Every
// saturation lands in a counter, never in silence.  Everything observed
// lands on physical PRG cells (bank * $4000 + (addr & $3FFF)); execution
// outside PRG counts as exec_outside and never enters the cartridge maps.
#include "explore.h"
#include "../../explore/stop.h"
#include "../../explore/lots.h"
#include "../../mach/nesmach.h"
#include "../../cpu/m6502/m6502.h"
#include "nes_trace.h"
#include "tas.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include "../../gui/fbwatch.h"
#include "../../core/statefile.h"

// one queued exploration state: a machine snapshot plus its provenance
typedef struct {
    uint8_t *state;      // nesmach blob, NULL once drained
    uint8_t forced;      // entered through a flipped branch
    uint8_t from_irq;    // entered through a cleared NMI edge
    uint64_t parent_path, parent_step;
    uint32_t steps;
    uint32_t choice;     // choice-tree node id (0: not yet registered)
    uint8_t kind;        // DP_NES_TREE_* when the creator set it, else derived
} DpNesPathState;

#define DP_NES_STATE_MAX 65536
#define DP_NES_CKPT_RING 16

// back-branch counter entry: branch site rom cell + taken target
typedef struct { uint64_t key; uint32_t count; } DpNesLoop;

typedef struct {
    uint8_t dmc_addr, dmc_len;   // $4012 / $4013 as last written
    DpNesMach *m;
    const uint8_t *image;
    size_t image_size;
    int mapper;
    DpNesOptions opt;
    DpNesResults *r;
    DpNesPathState *queue;
    uint32_t queue_len, queue_cap;
    DpNesPathState *park; uint32_t park_len, park_cap; // deferred holding area (queue full)
    DpLotFile *lot;                        // disk lot: park overflow, wall leftovers, --lot resume
    uint8_t ram_exec[0x800];           // RAM bytes executed since their last write (SMC tracking)
    int32_t ram_prod[0x800];           // last ROM cell read at any earlier time before the store (approximate producer, -1 none)
    int32_t ram_src[0x800];            // last ROM cell whose read fed the store to each RAM byte (-1 none)
    int64_t *prov_anchor; uint32_t prov_len; // table-word cells that fed an indirect jmp pointer
    uint32_t cur_choice;               // tree node of the path being run
    DpHashSet ctx_seen;                // per-context admission counters (C2)
    DpHashSet seen;                    // exact state dedup
    DpHashSet ind_sites, ind_pairs;    // observed jmp (ind): sites and (site,target) pairs
    DpHashSet probe_seen;              // strobe-write sites already probed
    DpNesLoop *loops;                  // back-branch counters
    size_t loop_len, loop_cap;
    uint32_t *fork_sites;              // per-cell fork counts (parallel to flags)
    size_t fork_sites_len;
    uint64_t total_steps, path_steps, paths_opened;
    unsigned cur_pc, cur_len;          // current instruction's fetch window
    int64_t last_read_cell; uint64_t last_read_step; // newest PRG data read (CPU-loop copy source)
    uint16_t *ipc;                     // per PRG cell: CPU address of an executed instruction start
    uint8_t *istart;                   // per PRG cell: 1 when an instruction started there
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
    int idle;              // demo session: no button pressed
    uint64_t wframe;                   // frames seen (watch sampling)
    int nov;                           // novelty search running (frame hook stops it, counts apart)
    uint64_t novel;                    // PRG cells executed for the first time (novelty gain)
    struct NStm *stm;                  // S6 decoder replay state
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


// --- S6: decoder replay on pointer tables ---------------------------------------------
// A JSR into PRG whose routine's first ROM data read equals a zero-page word (the pointer
// the caller set) is a decoder; two starts at a constant table stride locate a pointer
// table (same 16 KiB cell block, pointers in the same CPU slot); every other entry is
// replayed from the pre-call snapshot with that zero-page word set to the pointer.
enum { NS_DEC = 64, NS_START = 16, NS_FR = 16, NS_CAND = 2048 };
typedef struct { uint32_t key; uint8_t zp, nstarts, replayed; uint16_t starts[NS_START]; uint8_t *snap; } NDec;
typedef struct NStm {
    NDec dec[NS_DEC]; unsigned ndec;
    struct { uint32_t key; uint8_t sp, open; uint8_t zp[256]; } fr[NS_FR]; unsigned nfr;
} NStm;
static void nstm_free(DpNesRun *x) {
    if (!x->stm) return;
    for (unsigned i = 0; i < x->stm->ndec; i++) free(x->stm->dec[i].snap);
    free(x->stm); x->stm = NULL;
}
static void nstm_call(DpNesRun *x, uint32_t key) {
    if (!x->stm && !(x->stm = calloc(1, sizeof *x->stm))) return;
    NStm *s = x->stm;
    for (unsigned i = 0; i < s->ndec; i++)
        if (s->dec[i].key == key && !s->dec[i].snap) {
            s->dec[i].snap = malloc(dp_nesmach_state_size());
            if (s->dec[i].snap) dp_nesmach_save(x->m, s->dec[i].snap);
        }
    if (s->nfr == NS_FR) return;
    s->fr[s->nfr].key = key; s->fr[s->nfr].sp = (uint8_t)(dp_nesmach_sp(x->m) - 2); s->fr[s->nfr].open = 1;
    memcpy(s->fr[s->nfr].zp, dp_nesmach_ram(x->m), 256);
    s->nfr++;
}
static void nstm_step(DpNesRun *x) {
    NStm *s = x->stm;
    if (!s) return;
    uint8_t sp = (uint8_t)dp_nesmach_sp(x->m);
    while (s->nfr && sp > s->fr[s->nfr - 1].sp) s->nfr--;
}
static void nstm_read(DpNesRun *x, unsigned a) {
    NStm *s = x->stm;
    if (!s || !s->nfr) return;
    for (unsigned k = s->nfr; k-- > 0;) {
        if (!s->fr[k].open) break;
        s->fr[k].open = 0;
        for (unsigned p = 0; p < 255; p++) {
            if ((unsigned)(s->fr[k].zp[p] | s->fr[k].zp[p + 1] << 8) != a) continue;
            NDec *d = NULL;
            for (unsigned i = 0; i < s->ndec; i++)
                if (s->dec[i].key == s->fr[k].key && s->dec[i].zp == p) d = &s->dec[i];
            if (!d) {
                if (s->ndec == NS_DEC) break;
                d = &s->dec[s->ndec++]; memset(d, 0, sizeof *d);
                d->key = s->fr[k].key; d->zp = (uint8_t)p; x->r->s6_decoders++;
            }
            unsigned j;
            for (j = 0; j < d->nstarts && d->starts[j] != a; j++) {}
            if (j == d->nstarts && j < NS_START && a >= 0x8000) d->starts[d->nstarts++] = (uint16_t)a;
            break;
        }
    }
}
static int nstm_ptr(const DpNesRun *x, size_t at, size_t lo, size_t hi, unsigned slot, uint16_t *v) {
    if (at < lo || at + 2 > hi) return 0;
    *v = (uint16_t)(x->image[at] | x->image[at + 1] << 8);
    return *v >= 0x8000 && (unsigned)(*v >> 14) == slot;
}
static int nstm_replay(DpNesRun *x) {
    NStm *s = x->stm;
    if (!s) return 0;
    DpNesResults *r = x->r; int pushed = 0;
    for (unsigned di = 0; di < s->ndec; di++) {
        NDec *d = &s->dec[di];
        if (d->nstarts < 2 || d->nstarts <= d->replayed || !d->snap) continue;
        d->replayed = d->nstarts;
        unsigned slot = d->starts[0] >> 14;
        uint8_t *tried = calloc(65536, 1);
        if (!tried) continue;
        for (unsigned j = 0; j < d->nstarts; j++) tried[d->starts[j]] = 1;
        size_t pos[256]; unsigned np = 0;
        for (size_t at = 0; at + 2 <= x->image_size && np < 256; at++) {
            uint16_t v = (uint16_t)(x->image[at] | x->image[at + 1] << 8);
            for (unsigned j = 0; j < d->nstarts; j++) if (d->starts[j] == v) { pos[np++] = at; break; }
        }
        unsigned cands = 0;
        for (unsigned i = 0; i + 1 < np; i++) {
            size_t k = pos[i + 1] - pos[i];
            if (k < 2 || k > 16 || (pos[i] >> 14) != (pos[i + 1] >> 14)) continue;
            r->s6_tables++;
            size_t blo = (pos[i] >> 14) << 14, bhi = blo + 0x4000, lo = pos[i], hi = pos[i + 1];
            uint16_t v;
            while (lo >= k && nstm_ptr(x, lo - k, blo, bhi, slot, &v)) lo -= k;
            while (nstm_ptr(x, hi + k, blo, bhi, slot, &v)) hi += k;
            for (size_t at = lo; at <= hi; at += k) {
                if (!nstm_ptr(x, at, blo, bhi, slot, &v) || tried[v]) continue;
                tried[v] = 1;
                if (cands >= NS_CAND) { r->s6_sat++; break; }
                cands++; r->s6_candidates++;
                uint8_t *img = malloc(dp_nesmach_state_size());
                if (!img) continue;
                memcpy(img, d->snap, dp_nesmach_state_size());
                uint8_t *ram = dp_nesmach_state_ram(img);
                ram[d->zp] = (uint8_t)v; ram[d->zp + 1] = (uint8_t)(v >> 8);
                DpNesPathState ps = { img, 1, 0, 0, 0, 0, 0, DP_NES_TREE_BRANCH };
                if (snap_push(x, &ps)) { r->s6_runs++; pushed++; }
            }
        }
        free(tried);
    }
    return pushed;
}

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
    if ((x->total_steps >= x->opt.max_total || dp_stop)) {
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
    if (!x->play_mode) {
        nstm_step(x);
        if (op == 0x20) {
            int64_t tk = dp_nesmach_rom_index(x->m, bytes[1] | bytes[2] << 8);
            if (tk >= 0) nstm_call(x, (uint32_t)tk);
        }
    }
    // padding slide: a path that runs into a run of 8+ $00/$FF bytes in PRG has
    // left the code (brk / isc sleds); cut it, counted in pad_cuts
    if ((op == 0x00 || op == 0xFF) && !x->play_mode) {
        int64_t pc0 = dp_nesmach_rom_index(x->m, pc);
        const uint8_t *prg = dp_nesmach_prg_rom(x->m);
        if (pc0 >= 0 && prg && (size_t)pc0 + 8 <= r->prg_size) {
            unsigned same = 1;
            for (unsigned k = 1; k < 8; k++) if (prg[pc0 + k] == (uint8_t)op) same++;
            if (same == 8) { r->pad_cuts++; nes_trace.stop = 1; return; }
        }
    }
    if (x->ipc && x->istart && !x->play_mode) {
        int64_t c0 = dp_nesmach_rom_index(x->m, pc);
        if (c0 >= 0 && (size_t)c0 < x->r->prg_size) { x->ipc[c0] = (uint16_t)pc; x->istart[c0] = 1; }
    }

    {   // opcode-byte hit counter (saturating), both explored and played paths
        int64_t oc = dp_nesmach_rom_index(x->m, pc);
        if (oc >= 0 && r->exec_hits && r->exec_hits[oc] != UINT32_MAX) r->exec_hits[oc]++;
    }
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
                if (!r->flags[cell]) { r->flags[cell] = DP_NES_NATURAL; x->novel++; }
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
    if (outside && !x->play_mode) dp_ramarch_add_p(&r->ra, (uint16_t)pc, bytes, (int)in.length,
                                                   pc < 0x800 ? x->ram_src[pc] : -1, pc < 0x800 ? x->ram_prod[pc] : -1);   // RAM-code archive, ROM source of the first byte
    if (outside && pc < 0x2000)
        for (int i = 0; i < (int)in.length; i++) x->ram_exec[(pc + (unsigned)i) & 0x7FF] = 1;


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

    // observed indirect jumps: jmp ($xxxx) reads its target from the live bus
    // (with the 6502 page-wrap alias); sites and distinct targets are counted,
    // the sets are bounded and an overflow is reported
    if (in.flow == DP_M6502_INDIRECT && in.length == 3) {
        int64_t site = dp_nesmach_rom_index(x->m, pc);
        unsigned ptr = bytes[1] | (bytes[2] << 8);
        unsigned tgt = dp_nesmach_read(x->m, ptr) | (dp_nesmach_read(x->m, (ptr & 0xFF00u) | ((ptr + 1) & 0xFFu)) << 8);
        if (ptr + 1 < 0x800 && x->ram_src[ptr] >= 0 && x->ram_src[ptr + 1] == x->ram_src[ptr] + 1 && x->prov_len < 4096) {
            // S3: both pointer bytes were loaded from consecutive PRG cells: a table word
            uint32_t a;
            for (a = 0; a < x->prov_len && x->prov_anchor[a] != x->ram_src[ptr]; a++) {}
            if (a == x->prov_len) {
                int64_t *g = realloc(x->prov_anchor, (x->prov_len + 1) * sizeof *g);
                if (g) { x->prov_anchor = g; x->prov_anchor[x->prov_len++] = x->ram_src[ptr]; r->prov_sites++; }
            }
        }
        if (site >= 0) {
            if (x->ind_pairs.cnt >= 65536) r->indirect_overflow++;
            else {
                if (dp_set_insert(&x->ind_sites, (uint64_t)site + 1) > 0) r->indirect_sites++;
                if (dp_set_insert(&x->ind_pairs, ((uint64_t)(site + 1) << 16) | tgt) > 0) r->indirect_targets++;
            }
        }
    }

    // pad probes: the first strobe write ($4016) at a site forks one state per
    // single button, so the code that reads the pad is seen under each mask.
    // One visit per site (bounded by the PRG size); refusals are counted.
    if ((op == 0x8D || op == 0x8E || op == 0x8C) && bytes[1] == 0x16 && bytes[2] == 0x40 &&
        !x->opt.no_pad_probe) {
        int64_t site = dp_nesmach_rom_index(x->m, pc);
        if (site >= 0 && dp_set_insert(&x->probe_seen, (uint64_t)site + 1) > 0) {
            r->pad_probe_sites++;
            unsigned keep = dp_nesmach_input(x->m);
            for (unsigned b = 0; b < 8; b++) {
                if ((1u << b) == keep) continue;
                uint8_t *snap = malloc(dp_nesmach_state_size());
                if (!snap) { r->pad_probe_refused++; continue; }
                dp_nesmach_set_input(x->m, (uint8_t)(1u << b), 0);
                dp_nesmach_save(x->m, snap);
                DpNesPathState f = { snap, 0, 0, x->paths_opened, x->total_steps, 0 };
                if (snap_push(x, &f)) r->pad_probes++; else r->pad_probe_refused++;
            }
            dp_nesmach_set_input(x->m, (uint8_t)keep, 0);
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
    if (dp_nes_io_slot(addr) >= 0) { x->r->io_reads++; x->r->io_reg_reads[dp_nes_io_slot(addr)]++; }
    if (addr == 0x2007 && x->r->chr_flags) {
        int64_t c = dp_nesmach_chr_index(x->m, dp_nesmach_ppu_v(x->m));
        if (c >= 0 && (size_t)c < x->r->chr_size && dp_nesmach_ppu_v(x->m) < 0x2000) {
            x->r->chr_flags[c] |= DP_NES_CHR_READ;
            x->r->chr_reads++;
        }
    }
    if (addr < x->cur_pc || addr >= x->cur_pc + x->cur_len) {
        int64_t cell = dp_nesmach_rom_index(x->m, addr);
        if (cell >= 0) {
            x->r->data_reads++;
            if (x->r->data_hits && x->r->data_hits[cell] != UINT32_MAX) x->r->data_hits[cell]++;
            if (!x->r->flags[cell]) x->r->flags[cell] = DP_NES_DATA;
            x->last_read_cell = cell; x->last_read_step = x->total_steps;
            if (!x->play_mode) nstm_read(x, addr);
        }
    }
}

static void hook_write(void *user, unsigned addr, unsigned value) {
    DpNesRun *x = user;
    DpNesResults *r = x->r;
    if (addr < 0x2000 && x->ram_exec[addr & 0x7FF]) {   // an executed RAM byte is rewritten
        x->ram_exec[addr & 0x7FF] = 0;
        if (!r->smc_writes++) r->smc_first_pc = (uint16_t)x->cur_pc;
    }
    if (addr < 0x800)   // provenance: a store right after a PRG data read carries that cell
        {
            x->ram_src[addr] = (x->last_read_cell >= 0 && x->total_steps - x->last_read_step <= 4) ? (int32_t)x->last_read_cell : -1;
            x->ram_prod[addr] = x->ram_src[addr] >= 0 ? x->ram_src[addr] : (x->last_read_cell >= 0 ? (int32_t)x->last_read_cell : -1);
        }
    if (addr >= 0x8000) {
        // mapper register write (fires before the runtime/bridge applies it)
        unsigned os, ns;
        int kind = dp_nesmach_mapper_write(x->m, addr, value, &os, &ns);
        if (kind && os != ns)
            record_bank(x, 0, x->total_steps, (uint16_t)x->cur_pc, (uint8_t)(kind == 3 ? 1 : kind == 2 ? 2 : 0),
                        (uint8_t)os, (uint8_t)ns);  // window 0/1 = PRG, 2 = CHR
        return;
    }
    if (addr >= 0x2000 && addr < 0x4020) {
        r->io_writes++;
        r->io_reg_writes[dp_nes_io_slot(addr)]++;
        if (addr == 0x2007 && x->last_read_cell >= 0 && x->total_steps - x->last_read_step <= 4) {
            // CPU loop copy: a PRG data read right before a PPUDATA write.  A
            // run at the same pc whose source continues is one record.
            uint32_t src = (uint32_t)x->last_read_cell, dst = dp_nesmach_ppu_v(x->m);
            DpNesCopy *l = r->copy_count ? &r->copies[r->copy_count - 1] : NULL;
            if (l && l->channel == 3 && l->pc == (uint16_t)x->cur_pc && l->source + l->length == src) {
                l->length++; l->last_step = x->total_steps; l->occurrences++;
            } else {
                record_copy(x, 0, x->total_steps, (uint16_t)x->cur_pc, src, dst, 3);
                if (r->copy_count && r->copies[r->copy_count - 1].channel == 3 &&
                    r->copies[r->copy_count - 1].source == src) r->copies[r->copy_count - 1].length = 1;
            }
            r->flags[x->last_read_cell] |= DP_NES_COPIED;
            r->cpu_copy_bytes++;
            x->last_read_cell = -1;
        }
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
        if (addr == 0x4012) x->dmc_addr = (uint8_t)value;
        if (addr == 0x4013) x->dmc_len = (uint8_t)value;
        if (addr == 0x4015 && (value & 0x10)) {
            // DMC DMA: sample at $C000 + A*64 (wrapping into $8000), L*16+1 bytes
            unsigned len = (unsigned)x->dmc_len * 16u + 1u;
            uint32_t base = 0xC000u + (uint32_t)x->dmc_addr * 64u;
            int64_t first = -1;
            for (unsigned i = 0; i < len; i++) {
                uint32_t a = base + i;
                if (a > 0xFFFFu) a = 0x8000u + (a - 0x10000u);
                int64_t cell = dp_nesmach_rom_index(x->m, a);
                if (cell < 0) continue;
                if (first < 0) first = cell;
                if (!(r->flags[cell] & DP_NES_SAMPLE)) r->dmc_bytes++;
                r->flags[cell] |= DP_NES_SAMPLE | DP_NES_COPIED;
            }
            r->dmc_starts++;
            if (first >= 0) record_copy(x, 0, x->total_steps, (uint16_t)x->cur_pc, (uint32_t)first, 0, 2);
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

static void chr_note_tiles(DpNesRun *x) {
    DpNesResults *r = x->r;
    if (!r->chr_flags) return;
    uint64_t t[2048 + 128];
    size_t n = dp_nesmach_chr_tiles(x->m, t, sizeof t / sizeof *t);
    for (size_t i = 0; i < n; i++) {
        uint64_t c = t[i] >> 1;
        uint8_t bit = (t[i] & 1) ? DP_NES_CHR_SPR : DP_NES_CHR_BG;
        if (c + 16 > r->chr_size || (r->chr_flags[c] & bit)) continue;
        for (unsigned k = 0; k < 16; k++) r->chr_flags[c + k] |= bit;
        if (bit == DP_NES_CHR_BG) r->chr_tiles_bg++; else r->chr_tiles_spr++;
    }
}

static void hook_frame(void *user) {
    DpNesRun *x = user;
    chr_note_tiles(x);
    if (x->opt.watch_every && ++x->wframe % x->opt.watch_every == 0) {
        static uint32_t px[160 * 128];
        dp_nesmach_render(x->m, px);
        dp_fb_publish(px, 160, 128, x->wframe);
        x->r->watch_frames++;
    }
    if (!x->play_mode) return;
    x->frame_count++;
    if (!x->nov) x->r->play_frames_run++;
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

static uint32_t tree_add(DpNesRun *x, const DpNesPathState *s) {
    DpNesResults *r = x->r;
    if (!x->opt.tree_path) return 0;
    if (r->tree_count == r->tree_capacity) {
        uint32_t cap = r->tree_capacity ? r->tree_capacity * 2 : 256;
        DpNesTreeNode *g = realloc(r->tree_nodes, (size_t)cap * sizeof *g);
        if (!g) return 0;
        r->tree_nodes = g; r->tree_capacity = cap;
    }
    DpNesTreeNode *n = &r->tree_nodes[r->tree_count];
    memset(n, 0, sizeof *n);
    n->parent = x->cur_choice;
    n->kind = s->kind ? s->kind : s->from_irq ? DP_NES_TREE_IRQ : s->forced ? DP_NES_TREE_BRANCH : DP_NES_TREE_PROBE;
    n->pc = s->state ? (uint16_t)dp_nesmach_state_pc(s->state) : 0;
    n->site = 0xFFFFFFFFu;
    return ++r->tree_count;
}

static void tree_close(DpNesRun *x, uint32_t id, int issue) {
    if (id && id <= x->r->tree_count) x->r->tree_nodes[id - 1].issue = (uint8_t)issue;
}

static int queue_push(DpNesRun *x, const DpNesPathState *s) {
    if (x->queue_len >= x->opt.queue_cap) { x->r->queue_cuts++; return 0; }
    if (x->queue_len == x->queue_cap) return 0;
    DpNesPathState c = *s;
    if (!c.choice) c.choice = tree_add(x, &c);
    x->queue[x->queue_len++] = c;
    return 1;
}

// exact-state dedup: the snapshot hash (volatile clocks excluded)
// state to the disk lot (takes the blob); 0 = no lot or write failed (blob kept)
static int lot_spill(DpNesRun *x, DpNesPathState *s) {
    if (!x->lot) return 0;
    DpNesPathState m = *s; m.state = NULL;
    tree_close(x, s->choice, DP_NES_ISSUE_SPOOLED);
    if (!dp_lf_put(x->lot, &m, sizeof m, s->state, (uint32_t)dp_nesmach_state_size())) return 0;
    free(s->state); s->state = NULL;
    return 1;
}

// returns 1 when the state was queued, 2 when parked (queue full), 0 when not kept
static int snap_push(DpNesRun *x, DpNesPathState *s) {
    uint64_t h = dp_nesmach_state_hash(s->state);
    if (!dp_set_insert(&x->seen, h)) {
        x->r->evicted++;
        tree_close(x, tree_add(x, s), DP_NES_ISSUE_EVICTED);
        free(s->state); return 0;
    }
    if (x->opt.ctx_cap) {
        // coarse same-context eviction: the k-th admission of a context is the
        // key hash(ctx, k); a context past ctx_cap admissions is closed unrun
        uint64_t c = dp_nesmach_state_ctx_hash(s->state);
        unsigned k = 0;
        while (k < x->opt.ctx_cap && dp_set_insert(&x->ctx_seen, (c ^ (k * 0x9E3779B97F4A7C15ull)) | 1) == 0) k++;
        if (k >= x->opt.ctx_cap) {
            x->r->ctx_evicted++;
            tree_close(x, tree_add(x, s), DP_NES_ISSUE_EVICTED);
            free(s->state); return 0;
        }
    }
    if (!s->choice) s->choice = tree_add(x, s);
    if (x->queue_len >= x->opt.queue_cap) {
        // a full queue is not a loss: the state waits aside (bounded, counted)
        if (x->park_len < 4096) {
            if (x->park_len == x->park_cap) {
                uint32_t nc = x->park_cap ? x->park_cap * 2 : 64;
                DpNesPathState *g = realloc(x->park, nc * sizeof *g);
                if (g) { x->park = g; x->park_cap = nc; }
            }
            if (x->park_len < x->park_cap) { x->park[x->park_len++] = *s; x->r->parked++; return 2; }
        }
        if (lot_spill(x, s)) { x->r->parked++; return 2; }
        x->r->parked_lost++;
    }
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
    x->cur_choice = self->choice;
    if (self->from_irq) {
        r->irq_forks++;
        record_interrupt(x, path, self->parent_step,
                         (uint16_t)dp_nesmach_pc(x->m), 0);
        r->irq_counts[0]++;
    }
    r->forced_paths += self->forced != 0;

    // runs until the hook's budgets stop it (step, global, jam, loop)
    dp_nesmach_run(x->m, UINT64_MAX);
    tree_close(x, x->cur_choice, DP_NES_ISSUE_DONE);
    x->cur_choice = 0;
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
            x->frame_masks[f][0] = x->idle ? 0 : (uint8_t)(nes_rng(&x->rng) & 0xFF);
            x->frame_masks[f][1] = x->idle ? 0 : (uint8_t)(nes_rng(&x->rng) & 0xFF);
            x->frame_cmds[f] = 0;
        }
    }
    dp_nesmach_reset(x->m);
    if (x->frame_cmds[0] & 2) dp_nesmach_reset(x->m);
    dp_nesmach_set_input(x->m, x->frame_masks[0][0], x->frame_masks[0][1]);
    uint32_t f = 0;
    while (f < frames) {
        if ((x->total_steps >= x->opt.max_total || dp_stop)) { r->global_cuts++; break; }
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

// --- novelty search (Go-Explore) ------------------------------------------------------

typedef struct {
    uint8_t *z;
    uLongf zlen;
    uint64_t gain;         // cells found by the trial that produced it + its children
    uint32_t picks;
    uint32_t depth;        // frames played from power-on along its chain
    uint8_t lives[2048];   // CPU RAM at archiving (the lives byte is learned later)
} NesCell;

// a pad byte held 4..43 frames; no opposing directions (bits: A B Sel Start U D L R)
static uint8_t nes_random_input(DpNesRun *x, uint8_t *held, uint32_t *hold) {
    if (!*hold) {
        uint32_t v = nes_rng(&x->rng);
        uint8_t h = (uint8_t)(v & 0xFF);
        if ((h & 0x30) == 0x30) h &= (uint8_t)~0x20;
        if ((h & 0xC0) == 0xC0) h &= (uint8_t)~0x40;
        *held = h;
        *hold = 4 + (v >> 16) % 40;
    }
    (*hold)--;
    return *held;
}

// one frame of play with the given pad; 0 when the budget stops it
static int nes_nov_frame(DpNesRun *x, uint8_t in) {
    x->frame_count = 0;
    x->frame_target = 1;
    x->jam_pending = 0;
    dp_nesmach_set_input(x->m, in, 0);
    dp_nesmach_run(x->m, UINT64_MAX);
    return !x->jam_pending;
}

static int nes_archive_add(DpNesRun *x, NesCell *a, uint32_t *n, uint64_t gain, uint8_t *tmp, Bytef *ztmp, uLongf zcap) {
    DpNesResults *r = x->r;
    dp_nesmach_save(x->m, tmp);
    uLongf zl = zcap;
    if (compress2(ztmp, &zl, tmp, (uLong)dp_nesmach_state_size(), 1) != Z_OK) return -1;
    uint8_t *z = malloc(zl);
    if (!z) return -1;
    memcpy(z, ztmp, zl);
    uint32_t slot = *n;
    if (*n >= x->opt.novelty_archive) {
        double worst = 1e300;
        for (uint32_t i = 0; i < *n; i++) {
            double sc = (double)a[i].gain / (1.0 + a[i].picks);
            if (sc < worst) { worst = sc; slot = i; }
        }
        free(a[slot].z);
        r->novelty_replaced++;
    } else (*n)++;
    a[slot].z = z;
    a[slot].zlen = zl;
    memcpy(a[slot].lives, dp_nesmach_ram(x->m), 2048);
    a[slot].gain = gain;
    a[slot].picks = 0;
    r->novelty_archived++;
    return (int)slot;
}

// lives without game knowledge: a RAM byte <= 9 whose only decreases are single steps at
// least 200 frames apart and which rarely increases; a trial where such a byte drops lost a life
typedef struct {
    uint8_t prev[2048], maxv[2048], bad[2048];
    uint16_t dec[2048], inc[2048];
    uint32_t last_dec[2048], frame;
    int lives_byte;
} NesLives;

static int nes_lives_frame(NesLives *L, const uint8_t *w, int learn) {
    int died = 0;
    for (int i = 0; i < 2048; i++) {
        uint8_t a = L->prev[i], b = w[i];
        if (b > L->maxv[i]) L->maxv[i] = b;
        if (a == b) continue;
        if (b + 1 == a) {
            int cand = !L->bad[i] && L->dec[i] >= 2 && L->maxv[i] <= 9 && L->inc[i] <= 1 + L->dec[i] / 4;
            if (cand && !learn) { died = 1; L->lives_byte = i; }
            if (L->frame - L->last_dec[i] < 200) L->bad[i] = 1;
            L->dec[i]++;
            L->last_dec[i] = L->frame;
        } else if (b < a) L->bad[i] = 1;
        else if (b == a + 1 && L->inc[i] < 0xFFFF) L->inc[i]++;
    }
    memcpy(L->prev, w, 2048);
    L->frame++;
    return died;
}

enum { NES_KEY_SET = 1 << 18 };
static int nes_key_seen(const uint64_t *set, uint64_t k) {
    k |= 1;
    for (uint32_t i = (uint32_t)(k >> 7) & (NES_KEY_SET - 1);; i = (i + 1) & (NES_KEY_SET - 1)) {
        if (!set[i]) return 0;
        if (set[i] == k) return 1;
    }
}
static void nes_key_add(uint64_t *set, uint64_t k) {
    k |= 1;
    for (uint32_t i = (uint32_t)(k >> 7) & (NES_KEY_SET - 1);; i = (i + 1) & (NES_KEY_SET - 1)) {
        if (set[i] == k) return;
        if (!set[i]) { set[i] = k; return; }
    }
}

static void novelty_search(DpNesRun *x) {
    DpNesResults *r = x->r;
    if (!x->opt.novelty_steps || !x->opt.novelty_archive) return;
    size_t ssz = dp_nesmach_state_size();
    uLongf zcap = compressBound((uLong)ssz);
    NesCell *a = calloc(x->opt.novelty_archive, sizeof *a);
    NesLives *L = calloc(1, sizeof *L);
    uint64_t *keys = calloc(NES_KEY_SET, sizeof *keys);
    uint8_t *tmp = malloc(ssz), *tmp2 = malloc(ssz);
    Bytef *ztmp = malloc(zcap);
    if (!a || !L || !keys || !tmp || !tmp2 || !ztmp) {
        free(a); free(L); free(keys); free(tmp); free(tmp2); free(ztmp);
        x->failed = 1;
        return;
    }
    L->lives_byte = -1;
    uint32_t n = 0;
    const int cells = x->opt.novelty_cells;
    x->play_mode = 1;
    x->nov = 1;
    x->rng = x->opt.play_seed * 0x2545F491u + 7;
    dp_nesmach_reset(x->m);
    nes_archive_add(x, a, &n, 1, tmp, ztmp, zcap);
    uint64_t stop = x->total_steps + x->opt.novelty_steps;
    while (!x->failed) {
        if (x->total_steps >= stop) break;
        if ((x->total_steps >= x->opt.max_total || dp_stop)) { r->global_cuts++; break; }
        uint8_t maxl = 0;
        if (L->lives_byte >= 0)
            for (uint32_t i = 0; i < n; i++)
                if (a[i].lives[L->lives_byte] > maxl && a[i].lives[L->lives_byte] <= 9) maxl = a[i].lives[L->lives_byte];
        uint32_t best = n - 1;
        double bs = -1;
        for (int k = 0; k < 8; k++) {
            uint32_t i = k ? nes_rng(&x->rng) % n : n - 1;
            if (!cells) {
                double g = (double)a[i].gain / (1.0 + a[i].picks);
                if (g > bs) { bs = g; best = i; }
                continue;
            }
            double lf = (L->lives_byte >= 0 && a[i].lives[L->lives_byte] < maxl) ? 1e-3 : 1.0;
            double sc = lf * (1.0 + (double)a[i].gain) * (1.0 + a[i].depth / 200.0) / sqrt(1.0 + a[i].picks);
            if (sc > bs) { bs = sc; best = i; }
        }
        uLongf sz = (uLongf)ssz;
        if (uncompress(tmp2, &sz, a[best].z, a[best].zlen) != Z_OK) { x->failed = 1; break; }
        dp_nesmach_load(x->m, tmp2);
        a[best].picks++;
        r->novelty_trials++;
        uint64_t before = x->novel;
        uint8_t held = 0;
        uint32_t hold = 0, frames_since = 0;
        memcpy(L->prev, dp_nesmach_ram(x->m), 2048);
        L->frame += 200;   // a restore is no decrement sequence
        int learn = r->novelty_trials < 64;
        int recent[64];
        uint32_t recent_f[64];
        unsigned nrecent = 0;
        for (uint32_t f = 0; f < x->opt.novelty_frames; f++) {
            if (x->total_steps >= stop || (x->total_steps >= x->opt.max_total || dp_stop)) break;
            if (!nes_nov_frame(x, nes_random_input(x, &held, &hold))) break;
            r->novelty_frames_run++;
            if (cells && nes_lives_frame(L, dp_nesmach_ram(x->m), learn)) {
                for (unsigned k = 0; k < nrecent; k++)
                    if (recent[k] >= 0 && f - recent_f[k] < 120) { a[recent[k]].picks = 1u << 30; a[recent[k]].gain = 0; }
                r->novelty_deaths++;
                break;
            }
            frames_since++;
            if (f % 8 == 7) {
                uint64_t k = cells ? dp_nesmach_screen_key(x->m) : 0;
                int fresh = cells && !nes_key_seen(keys, k);
                if (fresh || (x->novel > before && frames_since >= 60)) {
                    uint64_t g = x->novel - before;
                    a[best].gain += g;
                    int slot = nes_archive_add(x, a, &n, cells ? g + (fresh ? 1 : 0) : g + 1, tmp, ztmp, zcap);
                    if (nrecent < 64) { recent[nrecent] = slot; recent_f[nrecent++] = f; }
                    if (slot >= 0) {
                        a[slot].depth = a[best].depth + f + 1;
                        if (a[slot].depth > r->novelty_depth) r->novelty_depth = a[slot].depth;
                    }
                    nes_key_add(keys, k);
                    if (g) r->novelty_hits++;
                    before = x->novel;
                    frames_since = 0;
                }
            }
        }
    }
    x->nov = 0;
    x->play_mode = 0;
    // the archive's freshest checkpoints seed the fork queue (exact-state dedup applies)
    for (uint32_t i = n; i > 0 && x->opt.queue_cap && !x->failed; i--) {
        if (i + 64 < n) break;
        uLongf sz = (uLongf)ssz;
        if (uncompress(tmp2, &sz, a[i - 1].z, a[i - 1].zlen) != Z_OK) continue;
        uint8_t *snap = malloc(ssz);
        if (!snap) { x->failed = 1; break; }
        memcpy(snap, tmp2, ssz);
        DpNesPathState s = { snap, 0, 0, 0, 0, 0, 0, DP_NES_TREE_ENTRY };
        if (snap_push(x, &s)) r->seeds_play++;
    }
    if (x->opt.export_state && n) {
        uint32_t d = 0;
        for (uint32_t i = 1; i < n; i++) if (a[i].depth > a[d].depth) d = i;
        // the PRG image identifies the ROM (the iNES header is not kept by the machine)
        if (!dp_state_export(x->opt.export_state, "nes", dp_nesmach_prg_rom(x->m), dp_nesmach_prg_size(x->m),
                             a[d].depth, a[d].z, (uint32_t)a[d].zlen, (uint32_t)ssz)) {
            r->state_exported = 1;
            r->state_exported_depth = a[d].depth;
        }
    }
    for (uint32_t i = 0; i < n; i++) free(a[i].z);
    free(a); free(L); free(keys); free(tmp); free(tmp2); free(ztmp);
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
    o.play_frames = 0; o.demo_frames = 0;
    o.play_sessions = 8;
    o.novelty_frames = 240;
    o.novelty_archive = 4096;
    o.skip_root = o.no_finalize = 0;
    o.pre_finalize = NULL; o.pre_user = NULL;
    o.play_seed = 1;
    return o;
}

static void infer_gaps(DpNesRun *x);
static void static_closure(DpNesRun *x);
static void second_pass_seed(DpNesRun *x);
static int64_t table_target(const DpNesRun *x, size_t a, unsigned v);
static void table_scan(DpNesRun *x);
static void copy_siblings(DpNesRun *x);

void dp_nes_rtcode(FILE *out, const DpNesResults *r) { dp_ramarch_write(&r->ra, out, "nes"); }

void dp_nes_results_free(DpNesResults *r) {
    if (!r) return;
    dp_ramarch_free(&r->ra);
    free(r->flags);
    free(r->tree_nodes);
    r->tree_nodes = NULL;
    free(r->exec_hits);
    free(r->data_hits);
    r->exec_hits = r->data_hits = NULL;
    free(r->chr_flags);
    free(r->inferred);
    r->inferred = NULL;
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
    dp_nesmach_watch_enable(x->m, x->opt.watch_every != 0);
}

int dp_nes_explore(const uint8_t *data, size_t size, int mapper,
                   const DpNesOptions *options, DpNesResults *results) {
    DpNesRun x;
    memset(&x, 0, sizeof(x));
    x.last_read_cell = -1;
    for (unsigned i = 0; i < 0x800; i++) { x.ram_src[i] = -1; x.ram_prod[i] = -1; }
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
    r->exec_hits = calloc(r->prg_size, sizeof(uint32_t));
    r->data_hits = calloc(r->prg_size, sizeof(uint32_t));
    r->chr_size = dp_nesmach_chr_size(x.m);
    if (r->chr_size) r->chr_flags = calloc(r->chr_size, 1);
    if (!r->flags) { dp_nesmach_close(x.m); return 0; }
    if (dp_nesmach_state_size() > DP_NES_STATE_MAX) { dp_nesmach_close(x.m); return 0; }

    // play/TAS scratch
    uint32_t fmax = x.opt.play_frames > x.opt.demo_frames ? x.opt.play_frames : x.opt.demo_frames;
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

    if (x.opt.lot_path) {
        x.lot = dp_lf_open(x.opt.lot_path, "NES", 1);
        if (!x.lot) fprintf(stderr, "deepeel: cannot open the lot %s\n", x.opt.lot_path);
    }
    x.queue_cap = x.opt.queue_cap;
    x.queue = calloc(x.queue_cap ? x.queue_cap : 1, sizeof(DpNesPathState));
    x.ipc = calloc(r->prg_size ? r->prg_size : 1, sizeof *x.ipc);
    x.istart = calloc(r->prg_size ? r->prg_size : 1, 1);
    x.fork_sites_len = r->prg_size;
    x.fork_sites = calloc(x.fork_sites_len ? x.fork_sites_len : 1, sizeof(uint32_t));
    dp_set_init(&x.seen, 1 << 22);
    dp_set_init(&x.probe_seen, 1 << 10);
    dp_set_init(&x.ctx_seen, 1 << 10);
    dp_set_init(&x.ind_sites, 1 << 10);
    dp_set_init(&x.ind_pairs, 1 << 12);
    if (!x.queue || !x.fork_sites) x.failed = 1;
    hooks_install(&x);

    // play and TAS sessions first: their visited bytes land in the shared
    // map, their end states seed the fork queue
    if (!x.failed) {
        if (x.opt.demo_frames) { // the attract mode first: no button, from power-on
            x.idle = 1;
            play_session(&x, x.opt.demo_frames, 0, NULL);
            x.idle = 0;
            r->demo_frames_run = x.opt.demo_frames;
        }
        for (int i = 0; i < x.opt.tas_count && !x.failed; i++) {
            if ((x.total_steps >= x.opt.max_total || dp_stop)) break;
            DpNesTas t;
            if (!dp_nes_tas_load(x.opt.tas_paths[i], &t, err, sizeof err)) {
                fprintf(stderr, "deepeel: TAS %s: %s\n", x.opt.tas_paths[i], err);
                continue;
            }
            int id = dp_nes_tas_identity(&t, data, size);
            if (id == 1) r->tas_same++;
            else if (id == 2) {
                r->tas_different++;
                fprintf(stderr, "deepeel: TAS %s: recorded for a different ROM (SHA-1 mismatch); played anyway\n", x.opt.tas_paths[i]);
            } else r->tas_unrecorded++;
            uint32_t frames = x.opt.play_frames ? x.opt.play_frames : t.frames;
            if (t.rom_name[0])
                fprintf(stderr, "deepeel: TAS %s: rom '%s', %u frames\n",
                        x.opt.tas_paths[i], t.rom_name, t.frames);
            play_session(&x, frames, 0, &t);
            dp_nes_tas_free(&t);
        }
        if (x.opt.play_frames) {
            for (uint32_t i = 0; i < x.opt.play_sessions; i++) {
                if ((x.total_steps >= x.opt.max_total || dp_stop)) break;
                play_session(&x, x.opt.play_frames, x.opt.play_seed + i * 0x2545F491u, NULL);
            }
        }
    }

    if (!x.failed) novelty_search(&x);

    // fork exploration: one root state (reset), depth-first drain
    if (!x.failed && x.opt.queue_cap > 0) {
        int sk = x.opt.skip_root;
        uint8_t *snap = malloc(dp_nesmach_state_size());
        if (!snap) { x.failed = 1; }
        else {
            dp_nesmach_reset(x.m);
            dp_nesmach_save(x.m, snap);
            DpNesPathState root = { snap, 0, 0, 0, 0, 0, 0, DP_NES_TREE_RESET };
            r->seeds_reset = 1;
            if ((x.opt.lot_drain && dp_lf_pending(x.lot)) || sk) { free(snap); r->seeds_reset = 0; } // drain: saved states only; secondary: no root
            else queue_push(&x, &root);
            // explicit entries: each is a root state with the bank and PC set
            // --seed-banks adds one entry per switchable bank at $8000 (UxROM)
            unsigned nb = (x.opt.seed_banks && dp_nesmach_mapper(x.m) == 2) ? dp_nesmach_bank_count(x.m) : 0;
            for (int i = 0; i < x.opt.entry_count + (int)nb && !x.opt.lot_drain && !sk; i++) {
                unsigned eb = i < x.opt.entry_count ? x.opt.entry_bank[i] : (unsigned)(i - x.opt.entry_count);
                unsigned ea = i < x.opt.entry_count ? x.opt.entry_addr[i] : 0x8000u;
                uint8_t *es = malloc(dp_nesmach_state_size());
                if (!es) { x.failed = 1; break; }
                dp_nesmach_reset(x.m);
                if (!dp_nesmach_enter(x.m, eb, ea)) {
                    free(es); r->entries_refused++; continue;
                }
                dp_nesmach_save(x.m, es);
                DpNesPathState er = { es, 0, 0, 0, 0, 0, 0, DP_NES_TREE_ENTRY };
                if (queue_push(&x, &er)) r->entries_seeded++; else { free(es); r->seeds_refused_queue++; }
            }
            // --seed-vectors: the NMI ($FFFA) and IRQ/BRK ($FFFE) targets as roots
            for (int v = 0; v < 2 && x.opt.seed_vectors && !x.opt.lot_drain && !sk; v++) {
                const uint8_t *prg = dp_nesmach_prg_rom(x.m);
                if (!prg || r->prg_size < 16384) break;
                unsigned va = prg[r->prg_size - (v ? 2 : 6)] | (prg[r->prg_size - (v ? 2 : 6) + 1] << 8);
                if (va < 0x8000) { r->entries_refused++; continue; }
                unsigned vb = 0; // vectors live in the fixed last bank; the switchable window starts at bank 0
                uint8_t *es = malloc(dp_nesmach_state_size());
                if (!es) { x.failed = 1; break; }
                dp_nesmach_reset(x.m);
                if (!dp_nesmach_enter(x.m, vb, va)) { free(es); r->entries_refused++; continue; }
                dp_nesmach_save(x.m, es);
                DpNesPathState er = { es, 0, 0, 0, 0, 0, 0, DP_NES_TREE_VECTOR };
                if (queue_push(&x, &er)) r->vectors_seeded++; else { free(es); r->seeds_refused_queue++; }
            }
            for (int pass = 0; pass < 2; pass++) {
                for (;;) {
                    if (x.failed) break;
                    if (x.queue_len == 0 && x.park_len > 0) { x.queue[x.queue_len++] = x.park[--x.park_len]; r->parked_resumed++; }
                    if (x.queue_len == 0 && dp_lf_pending(x.lot)) { // the disk backlog comes back
                        DpNesPathState m; uint32_t ml = 0, bl = 0; uint8_t *blob = NULL;
                        if (dp_lf_get(x.lot, &m, &ml, &blob, &bl)) {
                            if (ml == sizeof m && bl == dp_nesmach_state_size()) { m.state = blob; x.queue[x.queue_len++] = m; }
                            else free(blob);
                        }
                        if (x.queue_len == 0) continue; // unreadable record: next one
                    }
                    if (x.queue_len == 0 && (x.total_steps < x.opt.max_total && !dp_stop) && nstm_replay(&x)) continue;
                    if (x.queue_len == 0) break;
                    uint32_t slot = x.queue_len - 1;
                    run_path(&x, slot);
                    // the slot is drained (run_path cleared its state); the
                    // forks it queued sit above it — take the last one's place
                    if (x.queue_len - 1 > slot)
                        x.queue[slot] = x.queue[x.queue_len - 1];
                    x.queue_len--;
                    if ((x.total_steps >= x.opt.max_total || dp_stop)) break;
                }
                if (pass == 1 || !x.opt.second_pass || x.failed || x.queue_len || (x.total_steps >= x.opt.max_total || dp_stop)) break;
                second_pass_seed(&x);
                if (x.queue_len == 0) break;   // nothing seeded: no second drain
            }
        }
    }

    if (x.opt.pre_finalize) x.opt.pre_finalize(r, x.opt.pre_user);
    if (!x.opt.no_finalize) sections_build(&x);

    // states still queued when a wall stopped the drain: saved in the lot when
    // there is one (the next run resumes them), else counted, not silent
    for (uint32_t i = 0; i < x.queue_len; i++) {
        if (x.queue[i].state && !lot_spill(&x, &x.queue[i])) { r->pending_dropped++; tree_close(&x, x.queue[i].choice, DP_NES_ISSUE_BUDGET); }
        free(x.queue[i].state);
    }
    for (uint32_t i = 0; i < x.park_len; i++) {
        if (!lot_spill(&x, &x.park[i])) { r->pending_dropped++; tree_close(&x, x.park[i].choice, DP_NES_ISSUE_BUDGET); }
        free(x.park[i].state);
    }
    if (x.lot) {
        r->lot_written = x.lot->written; r->lot_read = x.lot->read_back;
        r->lot_pending = dp_lf_pending(x.lot);
        dp_lf_close(x.lot, x.opt.lot_path);
    }
    free(x.park);
    if (x.opt.tree_path && r->tree_count && !x.opt.no_finalize) {
        FILE *tf = fopen(x.opt.tree_path, "wb");
        if (tf) {
            // 'DPTZ', version 2, node count; records parent, site, pc, kind, issue
            uint32_t hdr[3] = { 0x5a545044u, 2, r->tree_count };
            if (fwrite(hdr, sizeof hdr, 1, tf) == 1)
                for (uint32_t i = 0; i < r->tree_count; i++) {
                    const DpNesTreeNode *tn = &r->tree_nodes[i];
                    uint32_t rec[2] = { tn->parent, tn->site };
                    uint8_t ki[2] = { tn->kind, tn->issue };
                    if (fwrite(rec, sizeof rec, 1, tf) != 1 || fwrite(&tn->pc, 2, 1, tf) != 1 ||
                        fwrite(ki, 1, 2, tf) != 2) break;
                }
            if (fclose(tf)) x.failed = 1;
        } else x.failed = 1;
    }
    if (!x.failed && !x.opt.no_finalize) static_closure(&x);
    if (!x.failed && !x.opt.no_finalize) table_scan(&x);
    if (!x.failed && !x.opt.no_finalize) copy_siblings(&x);
    if (!x.failed && !x.opt.no_finalize) infer_gaps(&x);
    free(x.prov_anchor);
    free(x.ipc);
    free(x.istart);
    nstm_free(&x);
    free(x.queue);
    free(x.loops);
    free(x.fork_sites);
    free(x.frame_masks);
    free(x.frame_cmds);
    free(x.ring);
    dp_set_free(&x.seen);
    dp_set_free(&x.probe_seen);
    dp_set_free(&x.ctx_seen);
    dp_set_free(&x.ind_sites);
    dp_set_free(&x.ind_pairs);
    int ok = !x.failed;
    dp_nesmach_close(x.m);
    return ok;
}

// --- second pass (G4) -----------------------------------------------------------
//
// Targets of executed jmp/jsr/branch instructions that no path executed (the
// static closure's roots) are entered as forced roots and the queue drains
// again, under the same total budget.  At most NES_PASS2_MAX seeds; roots over
// the cap, refused entries and seeds are all counted.
#define NES_PASS2_MAX 256
static void second_pass_seed(DpNesRun *x) {
    DpNesResults *r = x->r;
    const uint8_t *rom = dp_nesmach_prg_rom(x->m);
    size_t n = r->prg_size;
    if (!rom || !x->ipc || !x->istart) return;
    uint8_t *sd = calloc(n, 1);
    if (!sd) return;
    for (size_t c = 0; c < n; c++) {
        if (!x->istart[c]) continue;
        uint8_t b[3] = { rom[c], c + 1 < n ? rom[c + 1] : 0, c + 2 < n ? rom[c + 2] : 0 };
        DpM6502Instruction in;
        if (!dp_m6502_decode(b, 3, x->ipc[c], &in)) continue;
        if (in.flow != DP_M6502_JUMP && in.flow != DP_M6502_CALL && in.flow != DP_M6502_BRANCH) continue;
        unsigned t = in.target;
        if (t < 0x8000u) continue;
        int64_t tc;
        if ((t & 0xC000u) == (x->ipc[c] & 0xC000u)) tc = (int64_t)(c - (x->ipc[c] & 0x3FFFu)) + (int64_t)(t & 0x3FFFu);
        else tc = dp_nesmach_rom_index(x->m, t);
        if (tc < 0 || (size_t)tc >= n || (r->flags[tc] & (DP_NES_NATURAL | DP_NES_FORCED)) || sd[tc]) continue;
        sd[tc] = 1;
        if (r->pass2_seeds + r->pass2_refused >= NES_PASS2_MAX) { r->pass2_capped++; continue; }
        // the machine enters the target's bank (window $8000-$BFFF) or the fixed one
        unsigned bank = (t < 0xC000u) ? (unsigned)((size_t)tc >> 14) : 0;
        uint8_t *es = malloc(dp_nesmach_state_size());
        if (!es) { x->failed = 1; free(sd); return; }
        dp_nesmach_reset(x->m);
        if (!dp_nesmach_enter(x->m, bank, t)) { free(es); r->pass2_refused++; continue; }
        dp_nesmach_save(x->m, es);
        DpNesPathState er = { es, 1, 0, 0, 0, 0, 0, DP_NES_TREE_PASS2 };
        if (queue_push(x, &er)) r->pass2_seeds++;
        else { free(es); r->seeds_refused_queue++; }
    }
    // S3: neighbours of a provenance-traced table word (aligned words in the
    // same table, up to 64 each way) that point at unexecuted PRG are roots too
    for (uint32_t pa = 0; pa < x->prov_len; pa++) {
        size_t a0 = (size_t)x->prov_anchor[pa];
        for (int dir = -1; dir <= 1; dir += 2) {
            size_t a = a0;
            for (unsigned e = 0; e < 64; e++) {
                if (dir < 0) { if (a < 2) break; a -= 2; } else { a += 2; if (a + 1 >= n) break; }
                unsigned v = rom[a] | (rom[a + 1] << 8);
                int64_t tc = table_target(x, a, v);
                if (tc < 0 || (size_t)tc >= n) break;
                if ((r->flags[tc] & (DP_NES_NATURAL | DP_NES_FORCED)) || sd[tc]) continue;
                sd[tc] = 1;
                if (r->pass2_seeds + r->pass2_refused >= NES_PASS2_MAX) { r->pass2_capped++; continue; }
                unsigned bank = (v < 0xC000u) ? (unsigned)((size_t)tc >> 14) : 0;
                uint8_t *es = malloc(dp_nesmach_state_size());
                if (!es) { x->failed = 1; free(sd); return; }
                dp_nesmach_reset(x->m);
                if (!dp_nesmach_enter(x->m, bank, v)) { free(es); r->pass2_refused++; continue; }
                dp_nesmach_save(x->m, es);
                DpNesPathState er = { es, 1, 0, 0, 0, 0, 0, DP_NES_TREE_PASS2 };
                if (queue_push(x, &er)) { r->pass2_seeds++; r->prov_roots++; }
                else { free(es); r->seeds_refused_queue++; }
            }
        }
    }
    // G2: UxROM dispatch by bank views.  Runs of >= 3 aligned words in the
    // fixed bank that all point into $8000-$BFFF are a dispatch table; each
    // distinct target is entered in every bank view whose bytes there decode
    // as code (6 instructions without a jam, not an 8-byte constant run).
    unsigned mp = dp_nesmach_mapper(x->m);
    if (x->opt.bank_dispatch && mp == 2 && n >= 0x8000) {
        size_t fixed = n - 0x4000;
        unsigned banks = (unsigned)(n >> 14);
        for (size_t k = fixed; k + 1 < n;) {
            size_t e = k;
            while (e + 1 < n && (rom[e + 1] & 0xC0) == 0x80) e += 2;   // word high byte in $80-$BF
            if (e - k >= 6) {
                r->dispatch_tables++;
                for (size_t w = k; w < e; w += 2) {
                    unsigned v = rom[w] | (rom[w + 1] << 8);
                    for (unsigned b = 0; b < banks; b++) {
                        size_t cell = (size_t)b * 0x4000 + (v & 0x3FFF);
                        if (sd[cell] || (r->flags[cell] & (DP_NES_NATURAL | DP_NES_FORCED))) continue;
                        // plausibility: decodes as code, not a padding run
                        int ok = 1, run = 1;
                        for (unsigned q = 1; q < 8 && cell + q < n; q++) run += rom[cell + q] == rom[cell];
                        if (run >= 8) ok = 0;
                        size_t c = cell; unsigned pcv = v;
                        for (unsigned ins = 0; ok && ins < 6; ins++) {
                            uint8_t bb[3] = { rom[c], c + 1 < n ? rom[c + 1] : 0, c + 2 < n ? rom[c + 2] : 0 };
                            DpM6502Instruction in;
                            if (!dp_m6502_decode(bb, 3, (uint16_t)pcv, &in) || in.flow == DP_M6502_JAM || (c & 0x3FFF) + in.length > 0x4000) { ok = 0; break; }
                            if (in.flow == DP_M6502_JUMP || in.flow == DP_M6502_RETURN) break;
                            c += in.length; pcv += in.length;
                        }
                        if (!ok) { r->dispatch_refused_view++; continue; }
                        sd[cell] = 1;
                        if (r->pass2_seeds + r->pass2_refused >= NES_PASS2_MAX) { r->pass2_capped++; continue; }
                        uint8_t *es = malloc(dp_nesmach_state_size());
                        if (!es) { x->failed = 1; free(sd); return; }
                        dp_nesmach_reset(x->m);
                        if (!dp_nesmach_enter(x->m, b, v)) { free(es); r->pass2_refused++; continue; }
                        dp_nesmach_save(x->m, es);
                        DpNesPathState er = { es, 1, 0, 0, 0, 0, 0, DP_NES_TREE_PASS2 };
                        if (queue_push(x, &er)) { r->pass2_seeds++; r->dispatch_seeds++; }
                        else { free(es); r->seeds_refused_queue++; }
                    }
                }
            }
            k = e > k ? e + 2 : k + 1;
        }
    }
    free(sd);
}

// --- static flow closure ----------------------------------------------------------
//
// From every executed instruction with a known target (jmp, jsr, branch) whose
// target cell no path executed, decode linearly until a jmp/rts/rti/indirect/
// jam; the swept cells are tagged DP_NES_STATIC (code reached statically, never
// observed).  A target in the source's own 16 KiB window resolves inside the
// source's bank; other windows use the final live mapping.  Bounded by a
// work cap (one visit per cell) and by an instruction cap per sweep; hits of the
// cap are counted in static_cap_hits.
static void static_closure(DpNesRun *x) {
    DpNesResults *r = x->r;
    const uint8_t *rom = dp_nesmach_prg_rom(x->m);
    size_t n = r->prg_size;
    if (!rom || !n || !x->ipc || !x->istart) return;
    uint8_t *seen = calloc(n, 1);
    size_t *work = malloc(n * sizeof *work);
    uint16_t *wpc = malloc(n * sizeof *wpc);
    if (!seen || !work || !wpc) { free(seen); free(work); free(wpc); return; }
    size_t wl = 0;
    // roots: targets of executed instructions
    for (size_t c = 0; c < n; c++) {
        if (!x->istart[c]) continue;
        uint8_t b[3] = { rom[c], c + 1 < n ? rom[c + 1] : 0, c + 2 < n ? rom[c + 2] : 0 };
        DpM6502Instruction in;
        if (!dp_m6502_decode(b, 3, x->ipc[c], &in)) continue;
        if (in.flow != DP_M6502_JUMP && in.flow != DP_M6502_CALL && in.flow != DP_M6502_BRANCH) continue;
        unsigned t = in.target;
        if (t < 0x8000u) continue;
        int64_t tc;
        if ((t & 0xC000u) == (x->ipc[c] & 0xC000u)) tc = (int64_t)(c - (x->ipc[c] & 0x3FFFu)) + (int64_t)(t & 0x3FFFu);
        else tc = dp_nesmach_rom_index(x->m, t);
        if (tc < 0 || (size_t)tc >= n || (r->flags[tc] & (DP_NES_NATURAL | DP_NES_FORCED | DP_NES_STATIC)) || seen[tc]) continue;
        if (wl >= n) { r->static_cap_hits++; continue; }
        seen[tc] = 1; work[wl] = (size_t)tc; wpc[wl++] = (uint16_t)t; r->static_roots++;
    }
    while (wl) {
        size_t c = work[--wl]; uint16_t pc = wpc[wl];
        for (unsigned guard = 0; guard < 4096 && c < n; guard++) {
            if (r->flags[c] & (DP_NES_NATURAL | DP_NES_FORCED)) break;
            uint8_t b[3] = { rom[c], c + 1 < n ? rom[c + 1] : 0, c + 2 < n ? rom[c + 2] : 0 };
            DpM6502Instruction in;
            if (!dp_m6502_decode(b, 3, pc, &in) || in.flow == DP_M6502_JAM || c + in.length > n) break;
            for (unsigned k = 0; k < in.length; k++)
                if (!(r->flags[c + k] & DP_NES_STATIC)) { r->flags[c + k] |= DP_NES_STATIC; r->static_bytes++; }
            if (in.flow == DP_M6502_JUMP || in.flow == DP_M6502_CALL || in.flow == DP_M6502_BRANCH) {
                unsigned t = in.target;
                if (t >= 0x8000u && (t & 0xC000u) == (pc & 0xC000u)) {
                    size_t tc = (size_t)((int64_t)(c) - (int64_t)(pc & 0x3FFFu) + (int64_t)(t & 0x3FFFu));
                    if (tc < n && !seen[tc] && !(r->flags[tc] & (DP_NES_NATURAL | DP_NES_FORCED | DP_NES_STATIC))) {
                        if (wl < n) { seen[tc] = 1; work[wl] = tc; wpc[wl++] = (uint16_t)t; }
                        else r->static_cap_hits++;
                    }
                }
            }
            if (in.flow == DP_M6502_JUMP || in.flow == DP_M6502_RETURN || in.flow == DP_M6502_INDIRECT) break;
            c += in.length; pc = (uint16_t)(pc + in.length);
            if (guard == 4095) r->static_cap_hits++;
        }
    }
    free(seen); free(work); free(wpc);
}

// --- pointer tables ----------------------------------------------------------------
//
// A run of >= 2 aligned little-endian words, each pointing at an instruction
// that a path executed, around a cell the program read as data, is a jump /
// pointer table.  Resolution of a word to a PRG cell: NROM linearly, UxROM
// with $C000+ in the fixed bank and $8000-$BFFF in the table's own bank;
// other boards are skipped.  Tables are inferences (class `table`), bounded to
// 256 entries each.
static int64_t table_target(const DpNesRun *x, size_t a, unsigned v) {
    size_t n = x->r->prg_size;
    unsigned mp = dp_nesmach_mapper(x->m);
    if (v < 0x8000u) return -1;
    if (mp == 0) return n == 0x4000 ? (int64_t)(v & 0x3FFFu) : (int64_t)(v - 0x8000u);
    if (mp == 2) return v >= 0xC000u ? (int64_t)(n - 0x4000 + (v & 0x3FFFu)) : (int64_t)((a & ~(size_t)0x3FFF) + (v & 0x3FFFu));
    return -1;
}

static int table_word(const DpNesRun *x, const uint8_t *rom, size_t a) {
    size_t n = x->r->prg_size;
    if (a + 1 >= n) return 0;
    int64_t t = table_target(x, a, (unsigned)(rom[a] | rom[a + 1] << 8));
    return t >= 0 && (size_t)t < n && x->istart[t] && (x->r->flags[t] & (DP_NES_NATURAL | DP_NES_FORCED));
}

static void table_scan(DpNesRun *x) {
    DpNesResults *r = x->r;
    const uint8_t *rom = dp_nesmach_prg_rom(x->m);
    size_t n = r->prg_size;
    if (!rom || !n || !x->istart) return;
    if (!r->inferred) r->inferred = calloc(n, 1);
    if (!r->inferred) return;
    for (size_t i = 0; i < n; i++) {
        if (!(r->flags[i] & DP_NES_DATA) || (r->flags[i] & (DP_NES_NATURAL | DP_NES_FORCED))) continue;
        for (unsigned al = 0; al < 2; al++) {
            if (i < al) continue;
            size_t a = i - al;
            if (!table_word(x, rom, a) || r->inferred[a] == DP_NMAP_TABLE) continue;
            size_t lo = a, hi = a;
            while (lo >= 2 && (a - lo) / 2 < 256 && table_word(x, rom, lo - 2)) lo -= 2;
            while ((hi - a) / 2 < 256 && table_word(x, rom, hi + 2)) hi += 2;
            if ((hi - lo) / 2 + 1 < 2) continue;
            for (size_t k = lo; k < hi + 2; k++) {
                if (r->flags[k] & (DP_NES_NATURAL | DP_NES_FORCED)) continue;
                if (r->inferred[k] != DP_NMAP_TABLE) { r->inferred[k] = DP_NMAP_TABLE; r->table_bytes++; }
            }
            r->table_count++;
        }
    }
}

// --- copy siblings (S5) ----------------------------------------------------------
//
// A ROM->VRAM/OAM copy of >= 16 bytes proves its source is data.  When the
// source's CPU address is stored as a word in a table the game read as data,
// the table's other entries are the same loader's other sources: they get the
// observed length (clipped to the 16 KiB bank on UxROM) and class
// inf-data.  Entries are capped at 64 per table (cap counted), ranges over
// natural code are refused and counted; observations always win.
#define NES_SIB_MAX 64
static int nes_sib_word(const DpNesRun *x, const uint8_t *rom, size_t a) {
    size_t n = x->r->prg_size;
    if (a + 1 >= n) return 0;
    for (size_t i = a; i < a + 2; i++)
        if (x->r->flags[i] & (DP_NES_NATURAL | DP_NES_FORCED | DP_NES_STATIC)) return 0;
    int64_t t = table_target(x, a, (unsigned)(rom[a] | rom[a + 1] << 8));
    return t >= 0 && (size_t)t < n;
}

static void copy_siblings(DpNesRun *x) {
    DpNesResults *r = x->r;
    const uint8_t *rom = dp_nesmach_prg_rom(x->m);
    size_t n = r->prg_size;
    if (!rom || !n) return;
    if (!r->inferred) r->inferred = calloc(n, 1);
    if (!r->inferred) return;
    DpHashSet seen;
    dp_set_init(&seen, 256);
    for (unsigned c = 0; c < r->copy_count; c++) {
        const DpNesCopy *rec = &r->copies[c];
        uint32_t len = rec->channel == 1 ? 256u : rec->channel == 3 ? rec->length : 0;
        if (len < 16 || rec->source >= n) continue;
        for (size_t k = 0; k + 1 < n; k++) {
            if (!(r->flags[k] & DP_NES_DATA) && !(k && (r->flags[k - 1] & DP_NES_DATA))) continue;
            if (!nes_sib_word(x, rom, k)) continue;
            int64_t t = table_target(x, k, (unsigned)(rom[k] | rom[k + 1] << 8));
            if (t != (int64_t)rec->source) continue;
            size_t t0 = k, t1 = k + 2;
            unsigned entries = 1;
            while (t0 >= 2 && entries < NES_SIB_MAX && nes_sib_word(x, rom, t0 - 2)) { t0 -= 2; entries++; }
            while (t1 + 1 < n && entries < NES_SIB_MAX && nes_sib_word(x, rom, t1)) { t1 += 2; entries++; }
            if (entries >= NES_SIB_MAX) r->sib_caps++;
            if (entries < 2) continue;
            if (dp_set_insert(&seen, ((uint64_t)t0 << 20) ^ ((uint64_t)len << 4) ^ rec->channel) <= 0) continue;
            r->sib_tables++;
            for (size_t e = t0; e < t1; e += 2) {
                int64_t b = table_target(x, e, (unsigned)(rom[e] | rom[e + 1] << 8));
                if (b < 0 || b == (int64_t)rec->source) continue;
                size_t lim = dp_nesmach_mapper(x->m) == 2 ? ((size_t)b & ~(size_t)0x3FFF) + 0x4000 : n;
                size_t end = (size_t)b + len > lim ? lim : (size_t)b + len;
                if (end > n) end = n;
                int code = 0;
                for (size_t i = (size_t)b; i < end && !code; i++) if (r->flags[i] & DP_NES_NATURAL) code = 1;
                if (code) { r->sib_refused++; continue; }
                r->sib_ranges++;
                for (size_t i = (size_t)b; i < end; i++)
                    if (!r->flags[i] && !r->inferred[i]) { r->inferred[i] = DP_NMAP_INF_DATA; r->sib_bytes++; }
            }
        }
    }
    dp_set_free(&seen);
}

// --- padding and gap inference (static, over the classified map) ---------------

static int nes_cls_code(uint8_t f) { return (f & (DP_NES_NATURAL | DP_NES_FORCED)) != 0; }

// Runs of one byte value ($00/$FF from 8 bytes, others from 32) that no path
// touched are padding; small unclassified gaps between classified cells are
// inferred code/padding/data.  Every cell stays tagged as an inference, never
// as an observation.
static void infer_gaps(DpNesRun *x) {
    DpNesResults *r = x->r;
    const uint8_t *rom = dp_nesmach_prg_rom(x->m);
    size_t n = r->prg_size;
    if (!rom || !n) return;
    if (!r->inferred) r->inferred = calloc(n, 1);
    if (!r->inferred) return;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && rom[j] == rom[i] && !r->flags[j]) j++;
        size_t len = j - i;
        if (len >= ((rom[i] == 0 || rom[i] == 0xFF) ? 8u : 32u)) {
            for (size_t k = i; k < j; k++) r->flags[k] |= DP_NES_PAD;
            r->padding_bytes += len;
        }
        i = j > i ? j : i + 1;
    }
    for (size_t i = 0; i < n;) {
        if (r->flags[i] || r->inferred[i]) { i++; continue; }
        size_t j = i;
        while (j < n && !r->flags[j] && !r->inferred[j]) j++;
        size_t len = j - i;
        uint8_t lf = i ? r->flags[i - 1] : 0, rf = j < n ? r->flags[j] : 0, c = 0;
        if (len <= 3 && nes_cls_code(lf) && nes_cls_code(rf)) c = DP_NMAP_INF_CODE;
        else if (len <= 1024 && (lf & DP_NES_PAD) && (!rf || (rf & DP_NES_PAD))) c = DP_NMAP_INF_PAD;
        else if (len <= 1024 && (lf || rf)) c = DP_NMAP_INF_DATA;
        if (c) {
            for (size_t k = i; k < j; k++) r->inferred[k] = c;
            if (c == DP_NMAP_INF_CODE) r->inferred_code += len;
            else if (c == DP_NMAP_INF_PAD) r->inferred_pad += len;
            else r->inferred_data += len;
        }
        i = j;
    }
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
        else if ((f & DP_NES_STATIC) && !(f & (DP_NES_NATURAL | DP_NES_FORCED))) cls = DP_NMAP_CODE_STATIC;
        else if ((f & (DP_NES_NATURAL | DP_NES_FORCED)) && (f & DP_NES_DATA)) cls = DP_NMAP_CODE_STATIC;
        else if (f & DP_NES_FORCED) cls = DP_NMAP_CODE_FORCED;
        else if (f & DP_NES_NATURAL) cls = DP_NMAP_CODE_NAT;
        else if (r->inferred && r->inferred[i] == DP_NMAP_TABLE && !(f & (DP_NES_NATURAL | DP_NES_FORCED))) cls = DP_NMAP_TABLE;
        else if (f & DP_NES_DATA) cls = DP_NMAP_DATA_READ;
        else if (f & DP_NES_PAD) cls = DP_NMAP_PAD;
        else if (r->inferred && r->inferred[i]) cls = r->inferred[i];
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

void dp_nes_cov(FILE *out, const DpNesResults *r) {
    fputs("# deepeel nes coverage v1: bank offset flags(1=exec,4=speculative,8=data) hits\n", out);
    for (size_t k = 0; k < r->prg_size; k++) {
        uint8_t f = r->flags[k]; unsigned flags; uint32_t hits;
        if (f & (DP_NES_NATURAL | DP_NES_FORCED)) { flags = 1u | ((f & DP_NES_NATURAL) ? 0u : 4u); hits = r->exec_hits ? r->exec_hits[k] : 0; }
        else if (f & (DP_NES_DATA | DP_NES_COPIED | DP_NES_STATIC)) { flags = 8; hits = r->data_hits ? r->data_hits[k] : 0; }
        else continue;
        fprintf(out, "%02zx %04zx %x %x\n", k >> 14, k & 0x3fff, flags, (unsigned)hits);
    }
}

void dp_nes_romdata(FILE *out, const DpNesResults *r) {
    for (size_t p = 0; p < (r->prg_size + 255) / 256; p++) {
        uint64_t n = 0;
        for (size_t i = p * 256; i < p * 256 + 256 && i < r->prg_size; i++) n += r->data_hits ? r->data_hits[i] : 0;
        if (n) fprintf(out, "%zx %llu\n", p, (unsigned long long)n);
    }
}

void dp_nes_evidence(FILE *out, const DpNesResults *r) {
    fputs("# physical-bank offset-start offset-end evidence-mask (inclusive, 16 KiB banks)\n"
          "# 01=code-natural 02=code-forced 04=data-read 08=data-copied 10=irq 20=padding\n"
          "# 40=sampled 80=static (unflagged cells are not listed)\n", out);
    for (size_t at = 0; at < r->prg_size;) {
        size_t end = at + 1;
        while (end < r->prg_size && (end >> 14) == (at >> 14) && r->flags[end] == r->flags[at]) end++;
        if (r->flags[at])
            fprintf(out, "%04zX %04zX %04zX %02X\n", at >> 14, at & 0x3fff, (end - 1) & 0x3fff, (unsigned)r->flags[at]);
        at = end;
    }
}

void dp_nes_report(FILE *out, const DpNesResults *r) {
    uint64_t touched = 0;
    for (size_t i = 0; i < r->prg_size; i++)
        if (r->flags[i] & ~(DP_NES_STATIC | DP_NES_PAD)) touched++;
    fprintf(out, "NES exploration report\n");
    fprintf(out, "  prg %zu KiB, touched %llu bytes, coverage %.2f%%\n",
        r->prg_size / 1024, (unsigned long long)touched,
        r->prg_size ? 100.0 * (double)touched / (double)r->prg_size : 0.0);
    fprintf(out, "  stream replay (S6): %llu decoders, %llu tables, %llu candidates, %llu queued replays, %llu saturated\n",
        (unsigned long long)r->s6_decoders, (unsigned long long)r->s6_tables, (unsigned long long)r->s6_candidates,
        (unsigned long long)r->s6_runs, (unsigned long long)r->s6_sat);
    fprintf(out, "  steps %llu paths %llu (forced %llu) forks %llu irq-forks %llu evicted %llu\n",
        (unsigned long long)r->steps, (unsigned long long)r->paths,
        (unsigned long long)r->forced_paths,
        (unsigned long long)r->forks, (unsigned long long)r->irq_forks,
        (unsigned long long)r->evicted);
    fprintf(out, "  cuts: step %llu loop %llu fork %llu queue %llu global %llu path %llu\n",
        (unsigned long long)r->step_cuts, (unsigned long long)r->loop_cuts,
        (unsigned long long)r->fork_cuts, (unsigned long long)r->queue_cuts,
        (unsigned long long)r->global_cuts, (unsigned long long)r->path_cuts);
    if (r->smc_writes) fprintf(out, "  self-modifying RAM code: %llu writes into executed bytes (first at pc $%04X)\n",
                               (unsigned long long)r->smc_writes, r->smc_first_pc);
    fprintf(out, "  data reads %llu, OAM DMA copies %llu, ram writes %llu, io writes %llu\n",
        (unsigned long long)r->data_reads, (unsigned long long)r->oam_copies,
        (unsigned long long)r->ram_writes, (unsigned long long)r->io_writes);
    fprintf(out, "  io reads %llu\n", (unsigned long long)r->io_reads);
    for (int k = 0; k < 40; k++) {
        if (!r->io_reg_reads[k] && !r->io_reg_writes[k]) continue;
        fprintf(out, "   reg $%04X reads %llu writes %llu\n", k < 8 ? 0x2000 + k : 0x4000 + (k - 8),
            (unsigned long long)r->io_reg_reads[k], (unsigned long long)r->io_reg_writes[k]);
    }
    fprintf(out, "  DMC DMA starts %llu, distinct sample bytes from ROM %llu\n",
        (unsigned long long)r->dmc_starts, (unsigned long long)r->dmc_bytes);
    fprintf(out, "  faults %llu decode cuts %llu exec outside PRG %llu\n",
        (unsigned long long)r->faults, (unsigned long long)r->decode_cuts,
        (unsigned long long)r->exec_outside);
    if (r->ra.n) fprintf(out, "  runtime code archive: %u variants, %u attached to a ROM copy source, %u generated with an approximate ROM producer, %u rewrote an earlier variant\n",
        r->ra.n, r->ra.attached, r->ra.derived, r->ra.poly);
    if (r->demo_frames_run) fprintf(out, "  demo (no button pressed, from power-on): %llu frames\n", (unsigned long long)r->demo_frames_run);
    fprintf(out, "  play: %llu sessions x frames %llu\n",
        (unsigned long long)r->play_sessions_run, (unsigned long long)r->play_frames_run);
    if (r->state_exported) fprintf(out, "  state exported: deepest cell, %u frames from power-on\n", r->state_exported_depth);
    if (r->watch_frames) fprintf(out, "  watch: %llu frames published\n", (unsigned long long)r->watch_frames);
    if (r->novelty_trials)
        fprintf(out, "  novelty: depth %u frames, %llu trials, %llu frames, %llu hits, %llu deaths, %llu archived (%llu replaced)\n",
                r->novelty_depth, (unsigned long long)r->novelty_trials, (unsigned long long)r->novelty_frames_run,
                (unsigned long long)r->novelty_hits, (unsigned long long)r->novelty_deaths,
                (unsigned long long)r->novelty_archived, (unsigned long long)r->novelty_replaced);
    if (r->jobs_folded) fprintf(out, "  jobs: %u secondary instance folded (TAS/play phases)\n", r->jobs_folded);
    if (r->lot_written || r->lot_read || r->lot_pending)
        fprintf(out, "  lot: %llu written, %llu read back, %llu left on disk\n", (unsigned long long)r->lot_written,
            (unsigned long long)r->lot_read, (unsigned long long)r->lot_pending);
    fprintf(out, "  frontier: parked %llu resumed %llu lost %llu\n", (unsigned long long)r->parked,
        (unsigned long long)r->parked_resumed, (unsigned long long)r->parked_lost);
    fprintf(out, "  pending paths dropped at the wall %llu\n",
        (unsigned long long)r->pending_dropped);
    fprintf(out, "  tas identity: same ROM %llu, different ROM %llu, not recorded %llu\n", (unsigned long long)r->tas_same, (unsigned long long)r->tas_different, (unsigned long long)r->tas_unrecorded);
    fprintf(out, "  padding cuts: %llu\n", (unsigned long long)r->pad_cuts);
    fprintf(out, "  pointer tables: %llu tables, %llu bytes (inferred)\n", (unsigned long long)r->table_count, (unsigned long long)r->table_bytes);
    fprintf(out, "  static closure: %llu bytes from %llu roots, cap hits %llu\n", (unsigned long long)r->static_bytes, (unsigned long long)r->static_roots, (unsigned long long)r->static_cap_hits);
    fprintf(out, "  padding %llu bytes; inferred code %llu data %llu padding %llu\n", (unsigned long long)r->padding_bytes, (unsigned long long)r->inferred_code, (unsigned long long)r->inferred_data, (unsigned long long)r->inferred_pad);
    fprintf(out, "  cpu loop copy bytes %llu\n", (unsigned long long)r->cpu_copy_bytes);
    fprintf(out, "  indirect jumps: %llu sites, %llu targets, overflow %llu\n", (unsigned long long)r->indirect_sites, (unsigned long long)r->indirect_targets, (unsigned long long)r->indirect_overflow);
    fprintf(out, "  pad probes: %llu forks at %llu sites, refused %llu\n", (unsigned long long)r->pad_probes, (unsigned long long)r->pad_probe_sites, (unsigned long long)r->pad_probe_refused);
    fprintf(out, "  copy siblings: %llu tables; %llu ranges (%llu bytes); %llu refused over executed code; %llu tables cut at the cap\n", (unsigned long long)r->sib_tables, (unsigned long long)r->sib_ranges, (unsigned long long)r->sib_bytes, (unsigned long long)r->sib_refused, (unsigned long long)r->sib_caps);
    fprintf(out, "  choice tree: %u nodes\n", r->tree_count);
    fprintf(out, "  provenance: %llu jump-table words traced from PRG reads, %llu roots seeded\n", (unsigned long long)r->prov_sites, (unsigned long long)r->prov_roots);
    if (r->ctx_evicted) fprintf(out, "  context eviction: %llu states closed (coarse pc/regs/mapper context past the cap)\n", (unsigned long long)r->ctx_evicted);
    fprintf(out, "  bank dispatch: %llu tables, %llu seeds, %llu bank views refused (not code)\n", (unsigned long long)r->dispatch_tables, (unsigned long long)r->dispatch_seeds, (unsigned long long)r->dispatch_refused_view);
    fprintf(out, "  second pass: %llu seeds, %llu refused, %llu roots over the cap\n", (unsigned long long)r->pass2_seeds, (unsigned long long)r->pass2_refused, (unsigned long long)r->pass2_capped);
    fprintf(out, "  vector seeds: %llu\n", (unsigned long long)r->vectors_seeded);
    fprintf(out, "  entries: seeded %llu refused %llu\n", (unsigned long long)r->entries_seeded, (unsigned long long)r->entries_refused);
    fprintf(out, "  seeds: reset %llu irq %llu bank %llu play %llu refused %llu\n",
        (unsigned long long)r->seeds_reset, (unsigned long long)r->seeds_irq,
        (unsigned long long)r->seeds_bank, (unsigned long long)r->seeds_play,
        (unsigned long long)r->seeds_refused_queue);
    if (r->chr_size)
        fprintf(out, "  chr: %zu KiB ROM, tiles referenced: background %llu sprites %llu"
                     " (of %zu), $2007 reads %llu\n", r->chr_size / 1024,
                (unsigned long long)r->chr_tiles_bg, (unsigned long long)r->chr_tiles_spr,
                r->chr_size / 16, (unsigned long long)r->chr_reads);
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
        fprintf(out, "   ch%u src $%08X dst $%08X pc $%04X step %llu",
            c->channel, c->source, c->destination, c->pc,
            (unsigned long long)c->first_step);
        if (c->channel == 3) fprintf(out, " len %u", c->length);
        fputc('\n', out);
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
