// core.c: generic exploration kernel over DpMachine + DpIsa (see core.h).
#include <sys/stat.h>
#include <unistd.h>
#include "core.h"
#include "../report/report.h"
// weak: the standalone kernel tests link without the framebuffer watch (src/gui/fbwatch.c)
extern void dp_watch_frame(const uint32_t *argb, int w, int h, uint64_t frame, uint32_t movie_frame, uint32_t total, int done) __attribute__((weak));

#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *state;
    uint32_t pc;       // where the forced path resumes
    int forced;
    uint32_t site;     // fork site rom index + 1 (0: none), for the per-site novelty score
    uint32_t input;    // held pad mask + 1 (0: none); inherited by every fork of the path
    uint8_t seed;      // a configured seed: dropped at pick time when its start is already covered
    uint8_t vector;    // a registry vector entry: real machine state, no forced inversion (the taint RET cut does not apply)
    uint16_t origin;   // A6: DP_SRC_* bit of a fabricated or session start, inherited by every fork (0: the game's own flow)
} Pending;

// C13: dense RAM index of an address (the machine's own, else the low 16 bits); -1 outside RAM
static inline int64_t ridx(DpMachine *m, uint32_t a) { return m->ram_index ? m->ram_index(m, a) : (int64_t)(a & 0xFFFF); }
static inline uint32_t rsize(DpMachine *m) { return m->ram_size ? m->ram_size : 0x10000u; }
static inline uint32_t raddr(DpMachine *m, uint32_t i) { return m->ram_addr ? m->ram_addr(m, i) : i; }
// A3: per-site tables (visits, capped, rng) keyed on the ROM index (bank-aware), folded when the ROM exceeds the table
#define SITE_BITS 20u
static inline uint32_t site_slot(DpMachine *m, uint32_t pc, uint32_t n, uint32_t *key) {
    int64_t k = m->rom_index(m, pc);
    uint32_t kk = k >= 0 ? (uint32_t)k + 1u : (pc | 0x80000000u);
    if (key) *key = kk;
    if (k >= 0 && (uint64_t)k < n) return (uint32_t)k;
    return (uint32_t)((kk * 2654435761u) >> 8) & (n - 1);
}

static unsigned org_enc(uint16_t o) { unsigned i = 0; while (o) { i++; o >>= 1; } return i; }   // bit index + 1, 0: none
static uint16_t org_dec(unsigned i) { return i ? (uint16_t)(1u << (i - 1)) : 0; }

typedef struct {
    uint32_t pc;
    uint32_t count;
} Site;

void dp_core_config_default(DpCoreConfig *c) {
    memset(c, 0, sizeof *c);
    c->max_total = 5000000;
    c->progress_secs = 5;
    c->max_path_steps = 100000;
    c->fork_site_cap = 4;      // DP_DEFAULT_BRANCH_FORK_CAP
    c->visits_per_pc = 64;
    c->min_secs = 0; c->max_secs = 0; c->stall_secs = 0; c->attract_frames = 6000; c->attract_stall = 1200; c->play_frames = 0; c->play_sessions = 1;   // long sessions run wherever the machine has a frame counter (+ set_input for play)
    c->queue_max = 65536;
    c->probe_ctx_cap = 8;
    c->bank_fork_cap = 16;
    c->held_steps = 300000;
    c->tas_ckpt_every = 600; c->tas_ckpt_cap = 32; c->ckpt_burst_frames = 60;
}

int dp_core_parse_option(DpCoreConfig *c, int argc, char **argv, int i) {
    const char *a = argv[i];
    if (!strcmp(a, "--cells") && i + 1 < argc) {   // LO-HI: RAM range hashed into a cell
        char *e; unsigned long lo = strtoul(argv[i + 1], &e, 0);
        if (*e == '-') { c->cell_lo = (uint32_t)lo; c->cell_hi = (uint32_t)strtoul(e + 1, NULL, 0); }
        return 2;
    }
    if (!strcmp(a, "--unbounded")) { c->unbounded = 1; c->max_total = 0; c->max_path_steps = 0; c->fork_site_cap = 0; return 1; }
    if (!strcmp(a, "--state-dir") && i + 1 < argc) { c->state_dir = argv[i + 1]; return 2; }
    if (!strcmp(a, "--resume")) { c->resume = 1; return 1; }
    if (!strcmp(a, "--no-closure")) { c->no_closure = 1; return 1; }
    if (!strcmp(a, "--no-infer")) { c->no_infer = 1; return 1; }
    if (!strcmp(a, "--no-cells")) { c->no_cells = 1; return 1; }
    if (!strcmp(a, "--probe-ctx-cap") && i + 1 < argc) { c->probe_ctx_cap = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-auto-seed")) { c->no_auto_seed = 1; return 1; }
    if (!strcmp(a, "--no-ckpt-bursts")) { c->ckpt_burst_frames = 0; return 1; }
    if (!strcmp(a, "--ckpt-burst-frames") && i + 1 < argc) { c->ckpt_burst_frames = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-tas-ckpt")) { c->no_tas_ckpt = 1; return 1; }
    if (!strcmp(a, "--tas-ckpt-every") && i + 1 < argc) { c->tas_ckpt_every = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--tas-ckpt-cap") && i + 1 < argc) { c->tas_ckpt_cap = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-held-budget")) { c->no_held_budget = 1; return 1; }
    if (!strcmp(a, "--held-steps") && i + 1 < argc) { c->held_steps = strtoull(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-exact-state")) { c->no_exact = 1; return 1; }
    if (!strcmp(a, "--no-spec")) { c->no_spec = 1; return 1; }
    if (!strcmp(a, "--no-bank-forks")) { c->no_bank_forks = 1; return 1; }
    if (!strcmp(a, "--bank-fork-cap") && i + 1 < argc) { c->bank_fork_cap = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--pad-full")) { c->pad_full = 1; return 1; }
    if (!strcmp(a, "--pad-full6")) { c->pad_full = 2; return 1; }
    if (!strcmp(a, "--no-event-probes") || !strcmp(a, "--no-event-fork")) { c->no_event_probes = 1; return 1; }
    if (!strcmp(a, "--seed-ram") && i + 1 < argc) {   // LO-HI[=V,V,...]: technique 3 start states
        char *e; unsigned long lo = strtoul(argv[i + 1], &e, 0);
        if (*e != '-') return 0;
        unsigned long hi = strtoul(e + 1, &e, 0);
        c->seed_lo = (uint32_t)lo; c->seed_hi = (uint32_t)hi; c->seed_n = 0;
        if (*e == '=') {
            for (++e; *e && c->seed_n < 8; ) { c->seed_vals[c->seed_n++] = (uint8_t)strtoul(e, &e, 0); if (*e == ',') e++; else break; }
        } else { static const uint8_t d[4] = {0x00, 0xFF, 0x55, 0xAA}; memcpy(c->seed_vals, d, 4); c->seed_n = 4; }
        return 2;
    }
    if (!strcmp(a, "--dsy") && i + 1 < argc) { c->dsy_path = argv[i + 1]; return 2; }
    if (!strcmp(a, "--no-tas-jitter")) { c->no_tas_jitter = 1; return 1; }
    if (!strcmp(a, "--no-sessions")) { c->no_sessions = 1; return 1; }
    if (!strcmp(a, "--no-nonsense")) { c->no_nonsense = 1; return 1; }
    if (!strcmp(a, "--no-vectors")) { c->no_vectors = 1; return 1; }
    if (!strcmp(a, "--sessions") && i + 1 < argc) { c->sessions = (uint32_t)strtoul(argv[i + 1], NULL, 0); c->no_sessions = 0; return 2; }
    if (!strcmp(a, "--no-irq-jitter")) { c->no_irq_jitter = 1; return 1; }
    if (!strcmp(a, "--no-decompress")) { c->no_decompress = 1; return 1; }
    if (!strcmp(a, "--no-copies")) { c->no_copies = 1; return 1; }
    if (!strcmp(a, "--no-live-seeds")) { c->no_live_seeds = 1; return 1; }
    if (!strcmp(a, "--index-max") && i + 1 < argc) { unsigned long v = strtoul(argv[i + 1], NULL, 0); c->index_max = v > 255 ? 255 : (unsigned)v; return 2; }
    if (!strcmp(a, "--no-index-replay")) { c->no_index_replay = 1; return 1; }
    if (!strcmp(a, "--no-rng-learn")) { c->no_rng_learn = 1; return 1; }
    if (!strcmp(a, "--no-solve")) { c->no_solve = 1; return 1; }
    if (!strcmp(a, "--no-learn-sig")) { c->no_learn_sig = 1; return 1; }
    if (!strcmp(a, "--no-taint-cut")) { c->no_taint_cut = 1; return 1; }
    if (!strcmp(a, "--no-forced-ram-cut")) { c->no_forced_ram_cut = 1; return 1; }
    if (!strcmp(a, "--no-site-score")) { c->no_site_score = 1; return 1; }
    if (!strcmp(a, "--no-novelty-order")) { c->no_novelty_order = 1; return 1; }
    if (!strcmp(a, "--attract-frames") && i + 1 < argc) { c->attract_frames = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--min-time") && i + 1 < argc) { c->min_secs = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--max-time") && i + 1 < argc) { c->max_secs = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--stall-time") && i + 1 < argc) { c->stall_secs = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--attract-stall") && i + 1 < argc) { c->attract_stall = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--play-frames") && i + 1 < argc) { c->play_frames = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--play-sessions") && i + 1 < argc) { c->play_sessions = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--visits-per-pc") && i + 1 < argc) { c->visits_per_pc = (uint32_t)strtoul(argv[i + 1], NULL, 0); return 2; }
    return 0;
}

size_t dp_core_sections(const DpCoreResult *r, DpCoreSection *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < r->rom_size;) {
        if (!(r->flags[i] & (DP_CORE_NATURAL | DP_CORE_FORCED))) { i++; continue; }
        DpCoreSection sec = { i, i, 0, 0, 0, 0 };
        for (; i < r->rom_size && (r->flags[i] & (DP_CORE_NATURAL | DP_CORE_FORCED)); i++) {
            if (r->flags[i] & DP_CORE_NATURAL) sec.natural++;
            else sec.forced++;
            sec.hi = i;
        }
        sec.required = sec.natural > 0;
        if (r->live && r->live_w) {   // union of the pages' RAM-read bitmaps
            for (size_t pg = sec.lo >> 8; pg <= (sec.hi >> 8); pg++)
                for (uint32_t w = 0; w < r->live_w; w++) {
                    uint32_t bits = r->live[pg * r->live_w + w];
                    if (!bits) continue;
                    // count a bit once per section: only the first ROM page that sets it counts it
                    uint32_t seen = 0;
                    for (size_t q = sec.lo >> 8; q < pg; q++) seen |= r->live[q * r->live_w + w];
                    sec.live_pages += (size_t)__builtin_popcount(bits & ~seen);
                }
        }
        if (n < cap) out[n] = sec;
        n++;
    }
    return n;
}

void dp_core_result_free(DpCoreResult *r) {
    dp_ramarch_free(&r->ra);
    free(r->flags);
    free(r->dest); free(r->spec); free(r->refs); free(r->page_win); free(r->inferred); free(r->live);
    free(r->src_flags);
    free(r->first_seen);
    memset(r, 0, sizeof *r);
}

typedef struct { Site *s; size_t cap, n; } SiteTab;

// state shared by every worker; the machines themselves are private
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Pending *q;
    size_t qn, qcap;
    Pending *def; size_t defn, defcap;   // B6: states of paths stopped by a bounded cap, kept for the frontier (never re-run in this run)
    SiteTab sites;
    uint32_t *site_picks, *site_gain;   // per fork site (rom index + 1): paths run / new ROM bytes they found
    int active;                // workers currently running a path
    uint64_t steps;            // global instruction count (atomic)
    unsigned ckpts;            // A22/B3: movie checkpoints queued so far (atomic)
    unsigned sess_ckpts;       // B3: attract/play session checkpoints queued so far (atomic, own cap)
    uint8_t live_sig[4][256]; unsigned live_chosen;   // B3: sampled work-RAM signatures of the checkpoints the live vector seeds start from (guarded by mu)
    uint64_t paths_done;       // finished paths (atomic), for the progress line
    int stop_monitor;
    int halt;                  // 0 running, 1 max-time reached, 2 stagnation (atomic): workers wind down
    int spool_fd, spool_write_failed, spool_read_failed, checkpoint_failed; uint64_t spool_n, spool_total; uint32_t spool_rs;   // disk overflow of the pending queue: fixed-size records, LIFO
    unsigned allowed;          // adaptive mode: workers with index < allowed may take work (atomic)
    unsigned wmin, wmax;
    DpCoreResult *out;
    const DpCoreConfig *cfg;
    uint8_t *fed;
    uint32_t *fedsrc;
    uint32_t *fedprod;
    uint8_t *ramread;
    uint32_t sbytes;
    uint64_t *cellset;         // technique 8: open-addressing set of (target, RAM cell) keys
    size_t cellcap, celln;
    uint64_t *probeset;        // technique 2: sites already probed
    size_t probecap, proben;
    uint8_t *probectx;         // B1: contexts a probe site has been re-armed under (by rom index)
    uint64_t *exactset;        // B5: exact (pc, machine state) keys already reached, clocks excluded
    size_t exactcap, exactn;
    uint8_t *tas_start; uint32_t tas_pc; int tas_shifts_left;   // power-on state kept to queue the shifted movie replays once the unshifted one ends
} Shared;

typedef struct {
    DpMachine *m;
    DpCoreResult *r;
    int64_t last_rom;     // ROM index of the oldest unconsumed ROM read (FIFO head), -1 when empty
    uint8_t last_val;     // its value
    int64_t rq_idx[8]; uint8_t rq_val[8]; unsigned rq_n;   // A2: pending ROM reads, paired in order with the writes they feed (move.w/.l = 2/4 reads then 2/4 writes)
    uint8_t *fed;         // 64 KiB bitmap: CPU addresses written by a natural verbatim ROM copy
    int forced;           // the current path is a forced fork: speculative state
    int mapper_w;         // B7: the instruction just run wrote into the ROM address window (a mapper register)
    int age;              // instructions since that read; a stale read is not a copy
    // technique 6: a sequential ROM read stream and the RAM writes it feeds
    int64_t run_lo, run_hi;
    uint32_t run_writes;
    int no_decompress;
    // technique 1 (solve): non-ROM addresses read inside the current window
    uint32_t ram[4]; unsigned nram; int track;
    uint8_t *ramread;     // technique 8: 64 KiB bitmap of non-ROM data addresses any path has read (the semantic cell)
    uint32_t *fedsrc;     // parallel to fed: rom index + 1 of the source byte of that copy
    uint32_t *fedprod;    // rom index + 1 of the last ROM cell read before a non-copy store (approximate producer)
} CopyObs;

#define RUN_MIN 16
// closes the current stream: a long one whose RAM output is not 1:1 is a decompressor input
static void run_flush(CopyObs *o) {
    if (o->run_hi >= 0 && !o->no_decompress && !o->forced) {
        int64_t reads = o->run_hi - o->run_lo + 1;
        int64_t w = o->run_writes;
        int64_t diff = w > reads ? w - reads : reads - w;
        if (reads >= RUN_MIN && w >= RUN_MIN && diff * 4 > reads) {
            for (int64_t k = o->run_lo; k <= o->run_hi; k++) {
                uint8_t old = __atomic_fetch_or(&o->r->flags[k], DP_CORE_COMPRESSED, __ATOMIC_RELAXED);
                if (!(old & DP_CORE_COMPRESSED)) o->r->compressed_bytes++;
            }
            o->r->decompress_runs++;
        }
    }
    o->run_lo = o->run_hi = -1; o->run_writes = 0;
}

typedef struct {
    Shared *sh;
    DpMachine *m;
    DpCoreResult loc;     // private counters; the coverage arrays are the shared ones
    CopyObs cobs;
    uint32_t site_n;       // size of the per-site tables below (power of two)
    uint32_t *visits, *stamp;
    uint32_t *rng_stamp;   // per ld a,(nn) site: path id of the last visit (technique 4)
    uint8_t *rng_last;
    uint32_t path_id;
    int ok;
    unsigned idx;
    uint32_t *capped;      // direct-mapped cache of conditional sites that reached fork_site_cap (pc + 1)
    unsigned step_batch;   // steps not yet added to the shared counter (flushed every 256)
    uint32_t pcring[32]; unsigned pcring_n;   // A12: the last executed instruction starts of this path (never operand bytes)
    uint8_t *wsnap;        // technique 1: state at the start of the window before a conditional
    uint32_t *winmemo;     // direct-mapped: (rom index + 1) * 2 + (a conditional follows within 5 sequential instructions)
    int win_on; uint32_t win_pc; unsigned win_n;
    uint8_t win_imm[24]; unsigned win_ni;
    pthread_t th;
} Worker;

// 1 when a conditional follows `pc` within 5 sequential instructions (memoised per ROM site)
static int cond_ahead(Worker *w, const DpIsa *isa, uint32_t pc) {
    DpMachine *m = w->m;
    int64_t k = m->rom_index(m, pc);
    if (k < 0 || !isa->flow) return 0;
    uint32_t *e = &w->winmemo[(uint64_t)k * 2654435761u >> 16 & 0xFFFF];
    if ((*e >> 1) == (uint32_t)k + 1) return *e & 1;
    int yes = 0;
    uint32_t p = pc;
    for (int i = 0; i < 5; i++) {
        uint8_t b[8];
        for (unsigned j = 0; j < sizeof b; j++) b[j] = m->fetch(m, p + j);
        DpFlowInfo f;
        memset(&f, 0, sizeof f);
        if (!isa->flow(b, p, &f) || !f.length) break;
        if (f.kind == DP_FLOW_COND) { yes = i > 0; break; }   // the conditional itself starts no window
        if (f.kind != DP_FLOW_SEQ) break;
        p += f.length;
    }
    *e = ((uint32_t)k + 1) << 1 | (uint32_t)yes;
    return yes;
}

// static closure (all ISAs): from a natural executed direct transfer, the target and the fall-through are
// swept linearly under the live bank mapping, following direct jumps/calls/conditionals, until a return, an
// unconditional jump, a computed transfer, an illegal opcode, a known byte or a non-ROM address.  The bytes are
// marked DP_CORE_STATIC: reachable code that no path executed, never counted as natural.
#define CLOSURE_INSTR 4096
// C3: one static walker over a "view" of the code: the live machine (its mapping at this moment) or a ROM image
// laid out bank*window + offset.  The two views differ only in how a pc becomes a compact ROM index and where the bytes come from.
// Geometry of the image layout (bank * win + offset): where the two windows sit and which one is the fixed bank.
typedef struct { uint32_t base; unsigned win; uint32_t nbanks; int fixed_high; } ImgGeo;
static ImgGeo img_geo(const DpMachine *m, const DpCoreConfig *cfg, unsigned win) {
    ImgGeo g = { m ? m->code_base : 0, win, cfg->rom_image_size / win, m ? m->fixed_high : 0 };
    return g;
}
static uint32_t geo_fixed_bank(const ImgGeo *g) { return g->fixed_high ? (g->nbanks ? g->nbanks - 1 : 0) : 0; }
// image index of address `pc` seen while `bk` is the switchable bank; -1 outside the two windows
static int64_t geo_index(const ImgGeo *g, uint32_t pc, uint32_t bk) {
    if (pc < g->base || pc - g->base >= 2u * g->win) return -1;
    uint32_t off = pc - g->base, w = off >= g->win;
    if (g->nbanks <= 1 && w) return -1;   // a single window (linear ROM, or one 16 KiB bank): the second window is not ROM
    return (int64_t)(((unsigned)w == (unsigned)g->fixed_high ? geo_fixed_bank(g) : bk) * g->win + (off & (g->win - 1u)));
}
// address and window of image bank `b` at offset `o`
static uint32_t geo_pc(const ImgGeo *g, uint32_t b, uint32_t o) {
    unsigned w = (b == geo_fixed_bank(g)) ? (unsigned)g->fixed_high : 1u - (unsigned)g->fixed_high;
    return g->base + w * g->win + o;
}
typedef struct { DpMachine *m; const DpCoreConfig *cfg; ImgGeo g; } ClosureView;
static int64_t cview_index(const ClosureView *v, const DpCoreResult *r, uint32_t pc, uint32_t bk) {
    if (v->m) { int64_t k = v->m->rom_index(v->m, pc); return (k >= 0 && (size_t)k < r->rom_size) ? k : -1; }
    int64_t ix = geo_index(&v->g, pc, bk);
    return (ix >= 0 && (size_t)ix + 1 <= v->cfg->rom_image_size && (size_t)ix < r->rom_size) ? ix : -1;
}
static void closure_run(DpCoreResult *r, const ClosureView *v, const DpIsa *isa, uint32_t root, uint32_t bank, uint32_t trust0) {
    uint32_t stack[256][3]; unsigned sp = 0, budget = CLOSURE_INSTR;
    uint32_t trust = trust0;   // A13: code reached from executed code is trusted; the fall-through after a CALL is not (inline data idiom)
    uint32_t pc = root, bk = bank;
    if (v->m) {   // the live walk counts its roots and skips a root already known
        int64_t k0 = cview_index(v, r, pc, bk);
        if (k0 < 0 || (__atomic_load_n(&r->flags[k0], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC))) return;
        r->closure_roots++;
    }
    for (;;) {
        int64_t k = cview_index(v, r, pc, bk);
        int known = k >= 0 && (__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC));
        if (k < 0 || !budget || known) {
            if (v->m && k >= 0 && !budget) r->closure_capped++;
            if (!sp) return;
            sp--; pc = stack[sp][0]; bk = stack[sp][1]; trust = stack[sp][2];
            if (!budget) budget = 1;
            continue;
        }
        if (!trust && (__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & (DP_CORE_DATA | DP_CORE_COPIED))) {   // untrusted code stops on data
            r->closure_stopped_data++;
            if (!sp) return;
            sp--; pc = stack[sp][0]; bk = stack[sp][1]; trust = stack[sp][2];
            continue;
        }
        uint8_t b[8] = {0};
        if (v->m) for (unsigned i = 0; i < sizeof b; i++) b[i] = v->m->fetch(v->m, pc + i);
        else for (unsigned i = 0; i < sizeof b && (size_t)k + i < v->cfg->rom_image_size; i++) b[i] = v->cfg->rom_image[k + i];
        if (!trust && !isa->big_endian) {   // untrusted code stops inside a run of 3 pointer-like words (dispatch table), either alignment
            uint8_t w[12] = {0};   // bytes pc-4 .. pc+7
            if (v->m) for (int i = 0; i < 12; i++) w[i] = v->m->fetch(v->m, (uint32_t)((int)pc - 4 + i));
            else if (v->cfg && v->cfg->rom_image)
                for (int i = 0; i < 12; i++) { int64_t q = (int64_t)k - 4 + i; if (q >= 0 && (size_t)q < v->cfg->rom_image_size) w[i] = v->cfg->rom_image[q]; }
            int hit = 0;
            for (int o = 0; o <= 4 && !hit; o++) {   // a run of 3 words that covers the current byte (offset 4)
                int ok = 1;
                for (int q = 0; q < 3 && ok; q++) { uint16_t wd = (uint16_t)(w[o + 2 * q] | w[o + 2 * q + 1] << 8); ok = wd >= 0x4000 && wd < 0x8000; }
                hit = ok;
            }
            if (hit) {
                r->closure_stopped_data++;
                if (!sp) return;
                sp--; pc = stack[sp][0]; bk = stack[sp][1]; trust = stack[sp][2];
                continue;
            }
        }
        if (!trust && (b[0] == 0x00 || b[0] == 0xFF) && b[1] == b[0] && b[2] == b[0] && b[3] == b[0] && b[4] == b[0] && b[5] == b[0] && b[6] == b[0] && b[7] == b[0]) {   // untrusted code stops on a padding run (8 bytes)
            r->closure_stopped_data++;
            if (!sp) return;
            sp--; pc = stack[sp][0]; bk = stack[sp][1]; trust = stack[sp][2];
            continue;
        }
        DpFlowInfo f; memset(&f, 0, sizeof f);
        if (!isa->flow(b, pc, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) {
            if (!sp) return;
            sp--; pc = stack[sp][0]; bk = stack[sp][1]; trust = stack[sp][2];
            continue;
        }
        budget--;
        for (unsigned i = 0; i < f.length; i++) {
            int64_t q = v->m ? cview_index(v, r, pc + i, bk) : (k + i < (int64_t)r->rom_size ? k + i : -1);
            if (q >= 0) {
                uint8_t old = __atomic_fetch_or(&r->flags[q], DP_CORE_STATIC, __ATOMIC_RELAXED);
                if (!(old & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED))) r->closure_bytes++;
            }
        }
        int fall = f.fall || f.kind == DP_FLOW_SEQ || f.kind == DP_FLOW_COND || f.kind == DP_FLOW_CALL;
        if (f.kind != DP_FLOW_RET && f.kind != DP_FLOW_INDIRECT && f.target >= 0 &&
            (f.kind == DP_FLOW_JUMP || f.kind == DP_FLOW_COND || f.kind == DP_FLOW_CALL)) {
            if (sp < 256) { stack[sp][0] = (uint32_t)f.target; stack[sp][1] = bk; stack[sp][2] = trust; sp++; }
            else r->closure_stack_dropped++;   // a root lost to the fixed stack is counted, never silent
        }
        if (f.kind == DP_FLOW_JUMP || f.kind == DP_FLOW_RET || f.kind == DP_FLOW_INDIRECT || f.halt) fall = 0;
        if (fall) {
            pc += f.length;
            if (f.kind == DP_FLOW_CALL) trust = 0;   // A13: no trust after a call unless the callee was observed to return
        }
        else { if (!sp) return; sp--; pc = stack[sp][0]; bk = stack[sp][1]; trust = stack[sp][2]; }
    }
}
static void closure_walk(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t root) {
    ClosureView v = { m, NULL, { 0, 0, 0, 0 } };
    closure_run(r, &v, isa, root, 0, 1);
}
static void closure_walk_untrusted(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t root) {
    ClosureView v = { m, NULL, { 0, 0, 0, 0 } };
    closure_run(r, &v, isa, root, 0, 0);   // A13: the return address of a call may be inline data
}

// B8: speculative disassembly of the holes (never "known"): in the gaps left by every other technique, a candidate chain of
// legal instructions is accepted when it ends in a return / unconditional jump / halt, every direct target lands on known
// code or on an instruction start of the chain itself, and it is not padding.  Works on the 16 KiB-window image layout.
// Marks r->spec only; the bytes stay out of the natural / forced / static classes.
// `only` >= 0: scan that one image page only, executed at the 8 KiB window address `wbase` (PCE page journal)
static void speculative_gaps_page(DpCoreResult *r, const DpIsa *isa, const DpCoreConfig *cfg, const DpMachine *mach, unsigned win, int64_t only, uint32_t wbase) {
    ImgGeo g = img_geo(mach, cfg, win);
    if (only >= 0) g.base = only ? wbase - win : wbase;   // page 0 is the fixed bank (window 0 of the geometry); others the switchable one
    if (!cfg->rom_image || !r->spec || !isa->flow) return;
    const uint8_t known = DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC;
    size_t img = cfg->rom_image_size < r->rom_size ? cfg->rom_image_size : r->rom_size;
    for (size_t bank = only >= 0 ? (size_t)only : 0; bank * win < img && (only < 0 || bank == (size_t)only); bank++) {
        for (uint32_t o = 0; o < win && bank * win + o < img; ) {
            size_t ix = bank * win + o;
            if ((r->flags[ix] & (known | DP_CORE_DATA | DP_CORE_COPIED)) || r->spec[ix] || (!bank && o < isa->code_min && (isa->big_endian || o >= 0x100)) || ((o & 1u) && isa->big_endian)) { o++; continue; }   // the cartridge header is data
            uint32_t starts[64]; unsigned ns = 0, len = 0; int ok = 0, same = 1; uint8_t first_op = 0;
            if (cfg->rom_image[ix] == 0x00 || cfg->rom_image[ix] == 0xFF) { o++; continue; }   // padding never starts a routine
            uint32_t tg[64]; unsigned nt = 0;
            int refd = r->refs && r->refs[ix]; unsigned need = refd ? 2 : 3;   // B9: a reference from executed code reinforces the candidate
            uint32_t cur = o;
            while (ns < 64) {
                size_t cx = bank * win + cur;
                if (cx >= img || cur >= win) break;
                if ((r->flags[cx] & (DP_CORE_DATA | DP_CORE_COPIED)) || r->spec[cx]) break;
                if (r->flags[cx] & known) { ok = ns >= need; break; }   // runs into known code: the chain is consistent with it
                uint8_t b[8] = {0};
                for (unsigned i = 0; i < sizeof b && cx + i < img; i++) b[i] = cfg->rom_image[cx + i];
                uint32_t pc = geo_pc(&g, (uint32_t)bank, cur);
                DpFlowInfo f; memset(&f, 0, sizeof f);
                if (!isa->flow(b, pc, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL || cur + f.length > win) break;
                if (ns && b[0] != first_op) same = 0;
                if (!ns) first_op = b[0];
                starts[ns++] = cur;
                if (f.target >= 0 && (f.kind == DP_FLOW_JUMP || f.kind == DP_FLOW_COND || f.kind == DP_FLOW_CALL) && nt < 64) {
                    uint32_t t = (uint32_t)f.target;
                    if (geo_index(&g, t, (uint32_t)bank) < 0 || (only > 0 && t < wbase)) { ns = 0; break; }   // a target outside the ROM windows: not a routine
                    tg[nt++] = t;
                }
                len = cur + f.length - o;
                int term = f.kind == DP_FLOW_RET || f.kind == DP_FLOW_INDIRECT || f.halt || (f.kind == DP_FLOW_JUMP && !f.fall);
                cur += f.length;
                if (term) { ok = ns >= need && !same; break; }
            }
            if (ok) {   // every direct target: known code, or an instruction start of this chain
                for (unsigned i = 0; i < nt && ok; i++) {
                    uint32_t t = tg[i];
                    int64_t txi = geo_index(&g, t, (uint32_t)bank); size_t tx = txi < 0 ? (size_t)-1 : (size_t)txi;
                    int good = tx < img && (r->flags[tx] & known);
                    if (!good && txi >= 0 && (size_t)txi / win == bank) {
                        uint32_t off = t & (win - 1u);
                        for (unsigned k = 0; k < ns; k++) if (starts[k] == off) good = 1;
                    }
                    if (!good) ok = 0;
                }
            }
            if (ok && len) {
                for (unsigned i = 0; i < len; i++) { r->spec[ix + i] = 1; r->spec_bytes++; }
                r->spec_runs++; if (refd) r->spec_ref_runs++;
                o += len;
            } else o++;
        }
    }
}

static void speculative_gaps(DpCoreResult *r, const DpIsa *isa, const DpCoreConfig *cfg, const DpMachine *mach, unsigned win) {
    speculative_gaps_page(r, isa, cfg, mach, win, -1, 0);
}
// B8 on a mapper whose pages run at journaled windows: every page that was executed is scanned at its own address
static void speculative_gaps_pages(DpCoreResult *r, const DpIsa *isa, const DpCoreConfig *cfg, const DpMachine *mach) {
    unsigned sh = mach->page_shift, win = 1u << sh;
    if (!cfg->rom_image || !r->page_win) return;
    size_t img = cfg->rom_image_size < r->rom_size ? cfg->rom_image_size : r->rom_size;
    // windows the mapper was seen to run code at; a page never executed is tried at each of them (a candidate is accepted
    // only when it decodes cleanly, so a wrong window yields nothing, and the first window to accept keeps the bytes)
    unsigned seen_w = 0;
    for (size_t p = 0; p * win < img; p++) if (r->page_win[p]) seen_w |= 1u << (r->page_win[p] - 1u);
    for (size_t p = 0; p * win < img; p++) {
        unsigned w = r->page_win[p];
        if (w) { speculative_gaps_page(r, isa, cfg, mach, win, (int64_t)p, (uint32_t)(w - 1u) << sh); continue; }
        for (unsigned k = 0; k < 8; k++)
            if ((seen_w & (1u << k))) speculative_gaps_page(r, isa, cfg, mach, win, (int64_t)p, (uint32_t)k << sh);
    }
}

// S4 (sm83/z80): an executed `jp hl` preceded (within 24 bytes) by `ld hl,nn` reads a table of code
// pointers at nn; each entry that starts a legal instruction is a static root.  Stride 2, up to 64 words.
// Static walk straight on the ROM image, for a target in a bank the machine does not currently map (S4 stride 3).
// Addresses < 0x4000 are bank 0, the others fold into the target's own bank.
static void closure_walk_img(DpCoreResult *r, const DpCoreConfig *cfg, const DpIsa *isa, uint32_t bank, uint32_t root, unsigned win) {
    ClosureView v = { NULL, cfg, img_geo(NULL, cfg, win) };
    closure_run(r, &v, isa, root, bank, 1);
}

// S4 stride 3: entries are {addr lo, addr hi, bank}; the table sits in the currently mapped window.
static int closure_jp_table3(DpCoreResult *r, DpMachine *m, const DpCoreConfig *cfg, const DpIsa *isa, uint32_t base, int min_entries) {
    if (!cfg->rom_image || base >= 0x8000) return 0;
    unsigned win = m->bank_shift ? 1u << m->bank_shift : 0x4000u;   // the machine's window geometry (E5)
    int64_t bi = base < win ? (int64_t)base : m->rom_index(m, base);
    if (bi < 0 || (size_t)bi + 6 > cfg->rom_image_size) return 0;
    uint32_t nbanks = (uint32_t)(cfg->rom_image_size / win);
    int cnt = 0;
    uint32_t tg[64][2];
    for (int e = 0; e < 64 && (size_t)bi + (size_t)e * 3 + 2 < cfg->rom_image_size; e++) {
        const uint8_t *en = cfg->rom_image + bi + (size_t)e * 3;
        uint32_t t = en[0] | (uint32_t)en[1] << 8, eb = en[2];
        if (eb >= nbanks || t < 0x150 || t >= 2u * win) break;
        uint32_t ix = (t < win ? 0 : eb) * win + (t & (win - 1u));
        if (ix >= r->rom_size || ix >= cfg->rom_image_size) break;
        uint8_t b[8] = {0};
        for (unsigned i = 0; i < sizeof b && ix + i < cfg->rom_image_size; i++) b[i] = cfg->rom_image[ix + i];
        DpFlowInfo f; memset(&f, 0, sizeof f);
        if (!isa->flow(b, t, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) break;
        tg[cnt][0] = t; tg[cnt][1] = eb; cnt++;
    }
    if (cnt < min_entries) return 0;   // validated first: nothing is marked for a table that does not hold up
    for (int e = 0; e < cnt; e++) {
        r->closure_jp_entries++; r->prov_data_bytes += 3; r->closure_roots++;
        closure_walk_img(r, cfg, isa, tg[e][1], tg[e][0], win);
    }
    if (cnt) r->closure_jp_tables++;
    return cnt;
}

// S4 (6502/huc6280): `jmp (abs)` ($6C) or `jmp (abs,x)` ($7C, HuC6280) whose
// operand sits in the mapped ROM window — the words that follow are a table of
// code pointers in the same window; each target that starts a legal instruction
// is a static root (stride 2, up to 64 words).  The kernel's first banked
// static-closure form for these ISAs (audit E5): the same-window semantics are
// the machine's own view, the walk needs no per-mapper geometry.
static void closure_abs_table(DpCoreResult *r, DpMachine *m, const DpIsa *isa, const uint8_t *bytes) {
    if (bytes[0] != 0x6C && bytes[0] != 0x7C) return;
    uint32_t ta = (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8);
    int cnt = 0;
    for (unsigned e = 0; e < 64; e++) {
        uint32_t a = ta + e * 2u;
        uint32_t t = (uint32_t)m->fetch(m, a) | ((uint32_t)m->fetch(m, a + 1) << 8);
        if (m->rom_index(m, t) < 0) break;   // outside the ROM: end of the table
        uint8_t b[8] = {0};
        for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, t + i);
        DpFlowInfo f; memset(&f, 0, sizeof f);
        if (!isa->flow(b, t, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) break;
        cnt++;
        closure_walk(r, m, isa, t);
    }
    if (cnt) { r->closure_jp_tables++; r->closure_jp_entries += (uint64_t)cnt; r->prov_data_bytes += (uint64_t)cnt * 2; }
}

// A11: legal code target for a jp-table entry: the GB header ($0000-$014F) is not code on sm83, but z80
// (SMS/MSX/SG) code legitimately sits low; the upper bound is the machine's own ROM window, not $8000.
static int jp_target_ok(DpMachine *m, const DpIsa *isa, uint32_t t) {
    uint32_t lo = isa->code_min;
    return t >= lo && t < 0x10000u && m->rom_index(m, t) >= 0;
}

static int ring_has(const uint32_t *ring, unsigned n, uint32_t pc) {
    for (unsigned i = 0; i < n && i < 32; i++) if (ring[i] == pc) return 1;
    return 0;
}

static void closure_jp_table(DpCoreResult *r, DpMachine *m, const DpCoreConfig *cfg, const DpIsa *isa, uint32_t pc, const uint32_t *ring, unsigned nring) {
    if (!isa->hl_jp_table) return;
    {   // byte-granular: ld b,n / ld c,m then ld h,b / ld l,c (or the reverse pair) before the jump
        int hb = -1, lc = -1, mv = 0;
        for (uint32_t d = 1; d <= 24 && d <= pc; d++) {
            uint8_t o = m->fetch(m, pc - d);
            if (o == 0x60 || o == 0x69) mv |= o == 0x60 ? 1 : 2;
            if (d >= 2 && hb < 0 && m->fetch(m, pc - d) == 0x06) hb = m->fetch(m, pc - d + 1);
            if (d >= 2 && lc < 0 && m->fetch(m, pc - d) == 0x0E) lc = m->fetch(m, pc - d + 1);
        }
        if (mv == 3 && hb >= 0 && lc >= 0 && jp_target_ok(m, isa, (uint32_t)(hb << 8 | lc))) {
            uint32_t t = (uint32_t)(hb << 8 | lc);
            int64_t kt = m->rom_index(m, t);
            if (kt >= 0 && (size_t)kt < r->rom_size && !(r->flags[kt] & DP_CORE_STATIC)) r->closure_jp_entries++;
            closure_walk(r, m, isa, t);
            return;
        }
    }
    for (uint32_t d = 3; d <= 24 && d <= pc; d++) {
        uint8_t lop = m->fetch(m, pc - d);
        int base_ok = lop == 0x21;
        if (lop == 0x11 || lop == 0x01)   // ld de/bc,nn then add hl,de/bc, or ld a,(de/bc) pointer reads, before the jump
            for (uint32_t q = pc - d + 3; q < pc; q++) { uint8_t o = m->fetch(m, q); if (o == (lop == 0x11 ? 0x19 : 0x09) || o == (lop == 0x11 ? 0x1A : 0x0A)) { base_ok = 1; break; } }
        if (!base_ok) continue;
        if (ring && !ring_has(ring, nring, pc - d)) continue;   // A12: the load must be an executed opcode, not an operand byte that looks like one
        uint32_t base = m->fetch(m, pc - d + 1) | (uint32_t)m->fetch(m, pc - d + 2) << 8;
        if (base < 0x100 || m->rom_index(m, base) < 0) continue;
        int cnt = 0;
        for (int e = 0; e < 64; e++) {
            uint32_t t = m->fetch(m, base + e * 2) | (uint32_t)m->fetch(m, base + e * 2 + 1) << 8;
            if (!jp_target_ok(m, isa, t)) break;
            uint8_t b[8]; for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, t + i);
            DpFlowInfo f; memset(&f, 0, sizeof f);
            if (!isa->flow(b, t, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) break;
            uint32_t ea = base + (uint32_t)e * 2; int fresh = 1;
            for (unsigned z = 0; z < r->jp_seen_n; z++) if (r->jp_seen[z] == ea) { fresh = 0; break; }
            if (fresh) {
                if (r->jp_seen_n == r->jp_seen_cap) {
                    unsigned nc = r->jp_seen_cap ? r->jp_seen_cap * 2 : 256;
                    uint32_t *ns = realloc(r->jp_seen, nc * sizeof *ns);
                    if (ns) { r->jp_seen = ns; r->jp_seen_cap = nc; }
                }
                if (r->jp_seen_n < r->jp_seen_cap) r->jp_seen[r->jp_seen_n++] = ea;
            }
            if (fresh) {
                r->closure_jp_entries++; r->prov_data_bytes += 2;
            }
            closure_walk(r, m, isa, t);
            cnt++;
        }
        if (cnt) { r->closure_jp_tables++; if (cnt < 2) closure_jp_table3(r, m, cfg, isa, base, 2); return; }   // a lone stride-2 word may be the head of a stride-3 table
        if (closure_jp_table3(r, m, cfg, isa, base, 1)) return;   // stride 3: the entries name their own bank
    }
}

static void mark(Shared *sh, DpCoreResult *r, DpMachine *m, uint32_t pc, unsigned len, uint8_t how, uint16_t origin) {
    for (unsigned i = 0; i < len; i++) {
        int64_t k = m->rom_index(m, pc + i);
        if (k >= 0 && (size_t)k < r->rom_size) {
            if (m->page_shift && r->page_win && how & DP_CORE_NATURAL) __atomic_store_n(&r->page_win[(size_t)k >> m->page_shift], (uint8_t)(1u + (((pc + i) >> m->page_shift) & 7u)), __ATOMIC_RELAXED);
            uint32_t srcbit = (how & DP_CORE_NATURAL) ? (origin ? origin : DP_SRC_FORK_NATURAL) : DP_SRC_FORK_FORCED;   // A6: only a natural flow can be fabricated
            uint8_t bits = how | (i == 0 ? ((how & DP_CORE_NATURAL) ? DP_CORE_START_NATURAL : DP_CORE_START_FORCED) : 0);
            // read first: an atomic read-modify-write on an already covered byte only bounces the cache line
            uint8_t cur = __atomic_load_n(&r->flags[k], __ATOMIC_RELAXED);
            if ((cur & bits) == bits && (__atomic_load_n(&r->src_flags[k], __ATOMIC_RELAXED) & srcbit)) continue;
            uint8_t old = __atomic_fetch_or(&r->flags[k], bits, __ATOMIC_RELAXED);
            if (!(old & (DP_CORE_NATURAL | DP_CORE_FORCED))) {
                r->first_seen[k] = __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) + 1;
                if (r->in_novelty_path) r->novelty_new_bytes++; else r->plain_new_bytes++;
            }
            __atomic_fetch_or(&r->src_flags[k], srcbit, __ATOMIC_RELAXED);
        }
    }
}

static int covered_natural(DpCoreResult *r, DpMachine *m, uint32_t pc) {
    int64_t k = m->rom_index(m, pc);
    return k >= 0 && (size_t)k < r->rom_size && (__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & DP_CORE_NATURAL);
}

static int covered(DpCoreResult *r, DpMachine *m, uint32_t pc) {
    int64_t k = m->rom_index(m, pc);
    return k >= 0 && (size_t)k < r->rom_size && (__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED));
}

// per-site fork counter, open addressing on the pc
static uint32_t *site_count(SiteTab *t, uint32_t pc) {
    if (t->n * 2 >= t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 256;
        Site *ns = calloc(nc, sizeof *ns);
        for (size_t i = 0; i < t->cap; i++)
            if (t->s[i].count) {
                size_t h = (t->s[i].pc * 2654435761u) & (nc - 1);
                while (ns[h].count) h = (h + 1) & (nc - 1);
                ns[h] = t->s[i];
            }
        free(t->s); t->s = ns; t->cap = nc;
    }
    size_t h = (pc * 2654435761u) & (t->cap - 1);
    while (t->s[h].count && t->s[h].pc != pc) h = (h + 1) & (t->cap - 1);
    if (!t->s[h].count) { t->s[h].pc = pc; t->n++; }
    return &t->s[h].count;
}

static void obs_read(void *u, uint32_t addr, uint8_t v) {
    CopyObs *o = u;
    int64_t k = o->m->rom_index(o->m, addr);
    int64_t ri = k < 0 && o->ramread ? ridx(o->m, addr) : -1;
    if (ri >= 0 && !o->ramread[ri]) __atomic_store_n(&o->ramread[ri], 1, __ATOMIC_RELAXED);
    if (k < 0 && o->track && o->nram < 4) {
        unsigned i = 0;
        while (i < o->nram && o->ram[i] != addr) i++;
        if (i == o->nram) o->ram[o->nram++] = addr;
    }
    if (ri >= 0 && !o->forced && o->r->live && o->m->pc) {   // C11: the instruction that read this RAM page
        int64_t ci = o->m->rom_index(o->m, o->m->pc(o->m));
        uint32_t pg = (uint32_t)ri >> 8;
        if (ci >= 0 && (size_t)ci < o->r->rom_size && pg < o->r->live_w * 32u)
            __atomic_fetch_or(&o->r->live[((size_t)ci >> 8) * o->r->live_w + (pg >> 5)], 1u << (pg & 31u), __ATOMIC_RELAXED);
    }
    if (k >= 0 && (size_t)k < o->r->rom_size) {
        if (!o->forced && !(__atomic_load_n(&o->r->flags[k], __ATOMIC_RELAXED) & DP_CORE_DATA)) __atomic_fetch_or(&o->r->flags[k], DP_CORE_DATA, __ATOMIC_RELAXED);
        if (!o->forced) {   // A13: a byte read as data by a natural path is not static code unless it was ever executed
            uint8_t fl = __atomic_load_n(&o->r->flags[k], __ATOMIC_RELAXED);
            if ((fl & DP_CORE_STATIC) && !(fl & (DP_CORE_NATURAL | DP_CORE_FORCED))) {
                __atomic_fetch_and(&o->r->flags[k], (uint8_t)~DP_CORE_STATIC, __ATOMIC_RELAXED);
                __atomic_fetch_add(&o->r->closure_stopped_data, 1, __ATOMIC_RELAXED);
            }
        }
        if (o->age > 4) o->rq_n = 0;   // stale reads are not copy sources
        if (o->rq_n == 8) { memmove(o->rq_idx, o->rq_idx + 1, 7 * sizeof o->rq_idx[0]); memmove(o->rq_val, o->rq_val + 1, 7); o->rq_n--; }
        o->rq_idx[o->rq_n] = k; o->rq_val[o->rq_n] = v; o->rq_n++;
        o->last_rom = o->rq_idx[0]; o->last_val = o->rq_val[0]; o->age = 0;
        if (o->run_hi >= 0 && k >= o->run_hi && k <= o->run_hi + 2) o->run_hi = k;
        else { run_flush(o); o->run_lo = o->run_hi = k; }
    }
}

// B11: a hardware copy (DMA) moves ROM bytes the CPU never read: the source is data that was copied
// B3: a checkpoint's cell is the code location it was taken at: the per-site pick count (least visited first) orders it
static inline uint32_t ckpt_site(DpMachine *m, const DpCoreResult *loc) {
    int64_t k = m->rom_index(m, m->pc(m));
    return k >= 0 && (size_t)k < loc->rom_size ? (uint32_t)k + 1u : 0;
}

static void obs_copy(void *u, uint32_t src, uint32_t len, unsigned channel) {
    CopyObs *o = u;
    (void)channel;
    if (o->forced) { o->r->copy_forced += len; return; }
    for (uint32_t i = 0; i < len; i++) {
        int64_t k = o->m->rom_index(o->m, src + i);
        if (k < 0 || (size_t)k >= o->r->rom_size) continue;
        uint8_t was = __atomic_fetch_or(&o->r->flags[k], DP_CORE_DATA | DP_CORE_COPIED, __ATOMIC_RELAXED);
        __atomic_fetch_or(&o->r->src_flags[k], DP_SRC_COPY, __ATOMIC_RELAXED);
        if (!(was & DP_CORE_COPIED)) o->r->copy_bytes++;
    }
}

static void obs_write(void *u, uint32_t addr, uint8_t v) {
    CopyObs *o = u;
    if (o->m->rom_index(o->m, addr) >= 0) o->mapper_w = 1;
    if (o->run_hi >= 0 && o->m->rom_index(o->m, addr) < 0) o->run_writes++;
    if (o->last_rom < 0 || o->age > 4) return;
    int64_t wi = o->m->rom_index(o->m, addr) < 0 ? ridx(o->m, addr) : -1;
    if (wi >= 0 && v != o->last_val && o->fedprod && !o->forced) o->fedprod[wi] = (uint32_t)o->last_rom + 1;
    if (o->m->data_dest) {   // the write's hardware destination types the data the last ROM read fed
        uint8_t dest = o->m->data_dest(o->m, addr);
        if (dest && o->last_rom >= 0 && (size_t)o->last_rom < o->r->rom_size)
            __atomic_store_n(&o->r->dest[o->last_rom], dest, __ATOMIC_RELAXED);
    }
    if (o->m->rom_index(o->m, addr) < 0 && v == o->last_val) {
        if (o->forced) o->r->copy_forced++;
        else {
            __atomic_fetch_or(&o->r->flags[o->last_rom], DP_CORE_COPIED, __ATOMIC_RELAXED);
            __atomic_fetch_or(&o->r->src_flags[o->last_rom], DP_SRC_COPY, __ATOMIC_RELAXED);
            o->r->copy_bytes++;
            if (o->fed && wi >= 0) o->fed[wi] = 1;
            if (o->fedsrc && wi >= 0) o->fedsrc[wi] = (uint32_t)o->last_rom + 1;
        }
    }
    if (o->rq_n) {   // consume the head, the next pending read becomes the head
        o->rq_n--; memmove(o->rq_idx, o->rq_idx + 1, o->rq_n * sizeof o->rq_idx[0]); memmove(o->rq_val, o->rq_val + 1, o->rq_n);
    }
    if (o->rq_n) { o->last_rom = o->rq_idx[0]; o->last_val = o->rq_val[0]; } else o->last_rom = -1;
}

// FNV-1a over the configured RAM range, mixed with the fork target
static uint64_t cell_key(const DpCoreConfig *cfg, const uint8_t *ramread, DpMachine *m, uint32_t other) {
    uint64_t h = 1469598103934665603ull ^ other;
    if (!(cfg->cell_hi > cfg->cell_lo)) {   // semantic cell: the RAM bytes the code has been seen to read (at most 4096), no game knowledge
        unsigned n = 0;
        for (uint32_t a = 0, rn = rsize(m); a < rn && n < 4096; a++)
            if (ramread[a]) { h ^= m->fetch(m, raddr(m, a)) | ((uint64_t)n << 8); h *= 1099511628211ull; n++; }
        return h | 1;
    }
    for (uint32_t a = cfg->cell_lo; a <= cfg->cell_hi; a++) { h ^= m->fetch(m, a); h *= 1099511628211ull; }
    return h | 1;   // never 0: 0 marks an empty slot
}

// 1 when the key is new (inserted), 0 when already present; caller holds sh->mu
static inline size_t keyhash(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (size_t)k; }
static int keyset_insert(uint64_t **set, size_t *cap, size_t *n, uint64_t key) {
    if (*n * 2 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        uint64_t *ns = calloc(nc, sizeof *ns);
        if (!ns) return 1;
        for (size_t i = 0; i < *cap; i++)
            if ((*set)[i]) { size_t h = keyhash((*set)[i]) & (nc - 1); while (ns[h]) h = (h + 1) & (nc - 1); ns[h] = (*set)[i]; }
        free(*set); *set = ns; *cap = nc;
    }
    size_t h = keyhash(key) & (*cap - 1);
    while ((*set)[h]) { if ((*set)[h] == key) return 0; h = (h + 1) & (*cap - 1); }
    (*set)[h] = key; (*n)++;
    return 1;
}
static int cell_insert(Shared *sh, uint64_t key) { return keyset_insert(&sh->cellset, &sh->cellcap, &sh->celln, key); }


// Queue overflow goes to disk, never away: a record is {pc, forced, site, input} + the saved state.
// All called with sh->mu held.  If the disk is unusable the state stays in RAM beyond queue_max.
typedef struct { uint32_t pc, forced, site, input; } SpoolHead;   // forced bit 0, seed bit 1, vector bit 2
static void ram_push(Shared *sh, Pending p) {
    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
    sh->q[sh->qn++] = p;
}
static int spool_open(Shared *sh) {
    if (sh->spool_fd >= 0) return 1;
    if (sh->spool_fd == -1) return 0;   // failed once: stay in RAM
    const char *dir = getenv("TMPDIR"); if (!dir || !*dir) dir = "/tmp";
    char path[512]; snprintf(path, sizeof path, "%s/deepeel-spool-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) { perror("deepeel: kernel spool unavailable; retaining states in RAM"); sh->spool_fd = -1; return 0; }
    unlink(path);
    sh->spool_fd = fd; sh->spool_rs = (uint32_t)(sizeof(SpoolHead) + sh->sbytes);
    return 1;
}
// pushes p; when the RAM queue is full, appends it to the spool.  Returns 1 if spilled.
static int queue_push(Shared *sh, Pending p) {
    const DpCoreConfig *cfg = sh->cfg;
    if (cfg->queue_max && sh->qn >= cfg->queue_max && !sh->spool_write_failed && spool_open(sh)) {
        uint8_t *rec = malloc(sh->spool_rs);
        if (rec) {
            SpoolHead h = { p.pc, (uint32_t)!!p.forced | ((uint32_t)!!p.seed << 1) | ((uint32_t)!!p.vector << 2) | (org_enc(p.origin) << 3), p.site, p.input };
            memcpy(rec, &h, sizeof h); memcpy(rec + sizeof h, p.state, sh->sbytes);
            ssize_t w = pwrite(sh->spool_fd, rec, sh->spool_rs, (off_t)(sh->spool_n * sh->spool_rs));
            free(rec);
            if (w == (ssize_t)sh->spool_rs) { sh->spool_n++; sh->spool_total++; free(p.state); return 1; }
        }
        fprintf(stderr, "deepeel: kernel spool write failed; retaining further states in RAM\n");
        sh->spool_write_failed = 1;
    }
    ram_push(sh, p);
    return 0;
}
// B6: a path stopped by sat_path / sat_loop reports its state instead of losing it: with a state dir the
// snapshot joins the frontier (resumed later, e.g. under --unbounded) and is counted as deferred.
static void defer_state(Shared *sh, DpMachine *m, DpCoreResult *loc, Pending p) {
    if (!sh->cfg->state_dir || sh->cfg->unbounded) return;
    uint8_t *snap = malloc(sh->sbytes ? sh->sbytes : 1);
    if (!snap) return;
    m->save(m, snap);
    p.state = snap; p.pc = m->pc(m);
    pthread_mutex_lock(&sh->mu);
    if (sh->defn == sh->defcap) { sh->defcap = sh->defcap ? sh->defcap * 2 : 64; sh->def = realloc(sh->def, sh->defcap * sizeof *sh->def); }
    sh->def[sh->defn++] = p;
    pthread_mutex_unlock(&sh->mu);
    loc->deferred++;
}
// the queue is empty: bring back up to queue_max/2 of the newest spilled records
static int spool_refill(Shared *sh) {
    size_t want = sh->cfg->queue_max ? sh->cfg->queue_max / 2 : 64;
    if (want < 1) want = 1;
    uint8_t *rec = malloc(sh->spool_rs);
    if (!rec) { fprintf(stderr, "deepeel: cannot allocate kernel spool read buffer\n"); return 0; }
    int ok = 1;
    while (want-- && sh->spool_n) {
        uint64_t i = sh->spool_n - 1;
        if (pread(sh->spool_fd, rec, sh->spool_rs, (off_t)(i * sh->spool_rs)) != (ssize_t)sh->spool_rs) { ok = 0; break; }
        SpoolHead h; memcpy(&h, rec, sizeof h);
        uint8_t *st = malloc(sh->sbytes);
        if (!st) { ok = 0; break; }
        memcpy(st, rec + sizeof h, sh->sbytes);
        sh->spool_n = i;
        ram_push(sh, (Pending){ st, h.pc, (int)(h.forced & 1), h.site, h.input, (uint8_t)((h.forced >> 1) & 1), (uint8_t)((h.forced >> 2) & 1), org_dec((h.forced >> 3) & 31) });
    }
    free(rec);
    if (ftruncate(sh->spool_fd, (off_t)(sh->spool_n * sh->spool_rs)) != 0) { /* the next appends overwrite it anyway */ }
    if (!ok) fprintf(stderr, "deepeel: cannot read complete kernel spool; exploration stopped\n");
    return ok;
}

// Load the entire declared frontier or fail before starting exploration.  A
// truncated or incompatible file must not be replaced by a partial replay.
static int frontier_load(Shared *sh, const char *path, DpCoreResult *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 1;
        perror("deepeel: cannot open kernel frontier");
        return 0;
    }
    struct stat st;
    uint32_t hd[3];
    uint64_t record = sizeof(SpoolHead) + (uint64_t)sh->sbytes;
    int valid = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
                fread(hd, sizeof *hd, 3, f) == 3 && hd[0] == 0x31524644u &&
                hd[1] == sh->sbytes && hd[2] <= (UINT64_MAX - 12) / record &&
                (uint64_t)st.st_size == 12 + (uint64_t)hd[2] * record;
    if (!valid) {
        fprintf(stderr, "deepeel: invalid, incompatible or truncated kernel frontier: %s\n", path);
        fclose(f);
        return 0;
    }
    for (uint32_t i = 0; i < hd[2]; i++) {
        SpoolHead h;
        uint8_t *state = malloc(sh->sbytes ? sh->sbytes : 1);
        if (!state || fread(&h, sizeof h, 1, f) != 1 || fread(state, 1, sh->sbytes, f) != sh->sbytes) {
            free(state);
            fprintf(stderr, "deepeel: cannot read complete kernel frontier: %s\n", path);
            fclose(f);
            return 0;
        }
        queue_push(sh, (Pending){ state, h.pc, (int)(h.forced & 1), h.site, h.input, (uint8_t)((h.forced >> 1) & 1), (uint8_t)((h.forced >> 2) & 1), org_dec((h.forced >> 3) & 31) });
        out->resumed++;
    }
    if (fclose(f) != 0) { perror("deepeel: closing kernel frontier"); return 0; }
    return 1;
}

// Commit a complete frontier by rename so a failed write cannot replace the
// previous file.  The v1 header stores a 32-bit count: reject overflow.
static int frontier_save(Shared *sh, const char *path, DpCoreResult *out) {
    uint64_t count = sh->qn + sh->spool_n + sh->defn;
    if (!count) {
        if (unlink(path) == 0 || errno == ENOENT) return 1;
        perror("deepeel: cannot remove empty kernel frontier");
        return 0;
    }
    if (count > UINT32_MAX) {
        fprintf(stderr, "deepeel: kernel frontier too large for v1 format\n");
        return 0;
    }
    char temp[640];
    if (snprintf(temp, sizeof temp, "%s.tmp-XXXXXX", path) >= (int)sizeof temp) return 0;
    int fd = mkstemp(temp);
    if (fd < 0) { perror("deepeel: cannot create kernel frontier"); return 0; }
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(temp); return 0; }
    uint32_t hd[3] = { 0x31524644u, (uint32_t)sh->sbytes, (uint32_t)count };
    int ok = fwrite(hd, sizeof *hd, 3, f) == 3;
    for (size_t i = 0; ok && i < sh->qn; i++) {
        SpoolHead h = { sh->q[i].pc, (uint32_t)!!sh->q[i].forced | ((uint32_t)!!sh->q[i].seed << 1) | ((uint32_t)!!sh->q[i].vector << 2) | (org_enc(sh->q[i].origin) << 3), sh->q[i].site, sh->q[i].input };
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(sh->q[i].state, 1, sh->sbytes, f) == sh->sbytes;
    }
    for (size_t i = 0; ok && i < sh->defn; i++) {
        SpoolHead h = { sh->def[i].pc, (uint32_t)!!sh->def[i].forced | ((uint32_t)!!sh->def[i].seed << 1) | ((uint32_t)!!sh->def[i].vector << 2) | (org_enc(sh->def[i].origin) << 3), sh->def[i].site, sh->def[i].input };
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(sh->def[i].state, 1, sh->sbytes, f) == sh->sbytes;
    }
    uint8_t *rec = sh->spool_n ? malloc(sh->spool_rs) : NULL;
    if (sh->spool_n && !rec) ok = 0;
    for (uint64_t i = 0; ok && i < sh->spool_n; i++)
        ok = pread(sh->spool_fd, rec, sh->spool_rs, (off_t)(i * sh->spool_rs)) == (ssize_t)sh->spool_rs &&
             fwrite(rec, 1, sh->spool_rs, f) == sh->spool_rs;
    free(rec);
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(temp, path) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "deepeel: failed to save complete kernel frontier: %s\n", path);
        unlink(temp);
        return 0;
    }
    out->saved = count;
    return 1;
}

enum { TAS_INPUT = 0x40000000, SESSION_ATTRACT = 0x20000000, SESSION_PLAY = 0x20001000 };
// pad masks cycled by play sessions (kernel mask bits: up down left right button1 button2 start)
static const uint32_t play_catalog[10] = {0, 0x10, 0x40, 0x20, 0x01, 0x02, 0x04, 0x08, 0x80, 0x100};   // C12: + Select, C
// technique 1 (concolic form): the window start state `wsnap` is replayed with each RAM byte the window read set
// to a few candidate values (immediates of the window, +-1, 0, 1, 0xFF); a value that makes the conditional at
// `condpc` land on `other` is queued as a *natural* state (source: solved), no forced inversion.  Returns the
// number of states queued; the caller falls back to the forced fork when 0.
static unsigned solve_cond(Worker *w, const DpCoreConfig *cfg, uint32_t condpc, uint32_t other, uint32_t input) {
    Shared *sh = w->sh; DpMachine *m = w->m; DpCoreResult *loc = &w->loc;
    uint8_t cand[64]; unsigned nc = 0;
    #define ADDC(v) do { uint8_t v_ = (uint8_t)(v); unsigned q_ = 0; while (q_ < nc && cand[q_] != v_) q_++; if (q_ == nc && nc < sizeof cand) cand[nc++] = v_; } while (0)
    for (unsigned i = 0; i < w->win_ni; i++) { ADDC(w->win_imm[i]); ADDC(w->win_imm[i] + 1); ADDC(w->win_imm[i] - 1); }
    ADDC(0); ADDC(1); ADDC(0xFF);
    for (unsigned b = 1; b < 8; b++) ADDC(1u << b);   // B2: powers of two (bit tests)
    unsigned queued = 0;
    uint8_t *cur = malloc(sh->sbytes), *snap = malloc(sh->sbytes);
    if (!cur || !snap) { free(cur); free(snap); return 0; }
    m->save(m, cur);
    int saved_forced = w->cobs.forced, saved_track = w->cobs.track;
    w->cobs.forced = 1; w->cobs.track = 0;
    for (unsigned a = 0; a < w->cobs.nram; a++) {
        uint32_t addr = w->cobs.ram[a];
        pthread_mutex_lock(&sh->mu);
        int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, (keyhash(((uint64_t)m->rom_index(m, condpc) + 1) * 0x9E3779B97F4A7C15ull ^ ((uint64_t)addr << 8) ^ ((uint64_t)other << 40)) & ((1ull << 60) - 1)) | (9ull << 60));   // A23: hashed (site, address, side)
        pthread_mutex_unlock(&sh->mu);
        if (!fresh) continue;
        loc->solve_tries++;
        // B2: the likely candidates first, then (full enumeration) every other byte value
        int hit = 0;
        for (unsigned c = 0; c < nc + 256 && !hit; c++) {
            uint8_t val;
            if (c < nc) val = cand[c];
            else {
                unsigned v = c - nc, q = 0;
                while (q < nc && cand[q] != (uint8_t)v) q++;
                if (q < nc) continue;   // already tried
                val = (uint8_t)v;
            }
            m->load(m, w->wsnap);
            m->poke(m, addr, val);
            m->save(m, snap);
            int reached = 0;
            for (int st = 0; st < 16 && m->pc(m) != condpc; st++) if (!m->run(m)) break;
            if (m->pc(m) == condpc) { m->run(m); reached = 1; }
            if (reached && m->pc(m) == other) {
                pthread_mutex_lock(&sh->mu);
                queue_push(sh, (Pending){ snap, w->win_pc, 0, 0, input, 0, 0, DP_SRC_SOLVED });
                pthread_cond_signal(&sh->cv);
                pthread_mutex_unlock(&sh->mu);
                snap = malloc(sh->sbytes);
                if (!snap) goto out;
                queued++; loc->solved_forks++;
                hit = 1;
            }
        }
    }
out:
    w->cobs.forced = saved_forced; w->cobs.track = saved_track;
    m->load(m, cur);
    free(cur); free(snap);
    (void)cfg;
    return queued;
    #undef ADDC
}


// Direct RAM loads into the accumulator/index register, per ISA (techniques 3, 4, 7 on every ISA).
// Returns the load length and fills the address, the width in bytes (big-endian ISAs: the low-order byte is the last)
// and the destination data register (68000) when `bytes` is such a load of a non-ROM byte, else 0.
static int direct_ram_load(DpMachine *m, const DpIsa *isa, const uint8_t *b, uint32_t *addr, int *idx_reg, int *width, int *dreg) {
    const char *n = isa->name;
    if (!n) return 0;
    *idx_reg = 0; *width = 1; *dreg = -1;
    int len = 0; uint32_t a = 0;
    if (!strcmp(n, "z80") && b[0] == 0x3A) { len = 3; a = b[1] | (b[2] << 8); }
    else if (!strcmp(n, "sm83") && b[0] == 0xFA) { len = 3; a = b[1] | (b[2] << 8); }
    else if (!strcmp(n, "sm83") && b[0] == 0xF0) { len = 2; a = 0xFF00 + b[1]; }
    else if ((!strcmp(n, "m6502") || !strcmp(n, "huc6280")) && (b[0] == 0xAD || b[0] == 0xAE || b[0] == 0xAC)) { len = 3; a = b[1] | (b[2] << 8); *idx_reg = b[0] != 0xAD; }
    else if ((!strcmp(n, "m6502") || !strcmp(n, "huc6280")) && (b[0] == 0xA5 || b[0] == 0xA6 || b[0] == 0xA4)) { len = 2; a = b[1]; *idx_reg = b[0] != 0xA5; }
    else if (!strcmp(n, "68000")) {
        // move.{b,w,l} abs.w|abs.l,Dn : 00 ss DDD 000 111 00x
        unsigned w = (b[0] << 8) | b[1], sz = (w >> 12) & 3;
        if (sz && ((w >> 6) & 7) == 0 && ((w >> 3) & 7) == 7 && (w & 7) <= 1) {
            *width = sz == 1 ? 1 : sz == 3 ? 2 : 4;
            *dreg = (int)((w >> 9) & 7);
            if ((w & 7) == 0) { len = 4; a = (uint32_t)(int32_t)(int16_t)((b[2] << 8) | b[3]) & 0xFFFFFF; }
            else { len = 6; a = ((uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]) & 0xFFFFFF; }
        }
    }
    if (!len || m->rom_index(m, a) >= 0) return 0;
    *addr = a;
    return len;
}

// 68000 immediate of `width` bytes at b: returns its length in bytes, value in *v
static int m68k_imm(const uint8_t *b, int width, uint32_t *v) {
    if (width == 4) { *v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; return 4; }
    if (width == 2) { *v = (uint32_t)b[0] << 8 | b[1]; return 2; }
    *v = b[1]; return 2;
}

// technique 3: a RAM value compared with a constant, as one site.  Fills the address, value and width.
static int ram_signature(DpMachine *m, const DpIsa *isa, uint32_t pc, const uint8_t *b, int dl_len, int idx_reg, uint32_t dl_addr,
                         int dl_width, int dl_dreg, uint32_t *addr, uint32_t *val, int *width) {
    const char *n = isa->name;
    uint8_t f[10];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = m->fetch(m, pc + (dl_len ? (unsigned)dl_len : 0) + i);
    if (!strcmp(n, "68000")) {
        unsigned w0 = (b[0] << 8) | b[1];
        if (w0 == 0x0C38 || w0 == 0x0C39 || w0 == 0x0C78 || w0 == 0x0C79 || w0 == 0x0CB8 || w0 == 0x0CB9) {   // cmpi.x #imm,abs
            int sz = (w0 >> 6) & 3, wd = sz == 0 ? 1 : sz == 1 ? 2 : 4;
            uint8_t g[12];
            for (unsigned i = 0; i < sizeof g; i++) g[i] = m->fetch(m, pc + 2 + i);
            uint32_t imm; int il = m68k_imm(g, wd, &imm);
            uint32_t a;
            if (w0 & 1) a = ((uint32_t)g[il] << 24 | (uint32_t)g[il + 1] << 16 | (uint32_t)g[il + 2] << 8 | g[il + 3]) & 0xFFFFFF;
            else a = (uint32_t)(int32_t)(int16_t)((g[il] << 8) | g[il + 1]) & 0xFFFFFF;
            if (m->rom_index(m, a) >= 0) return 0;
            *addr = a; *val = imm; *width = wd;
            return 1;
        }
        if (!dl_len || dl_dreg < 0) return 0;
        unsigned w = (f[0] << 8) | f[1];
        int sz = dl_width == 1 ? 0 : dl_width == 2 ? 1 : 2;
        if (w == (0x0C00u | (unsigned)sz << 6 | (unsigned)dl_dreg) ||                       // cmpi.x #imm,Dn
            w == (0xB03Cu | (unsigned)dl_dreg << 9 | (unsigned)sz << 6)) {                   // cmp.x #imm,Dn
            uint32_t imm; m68k_imm(f + 2, dl_width, &imm);
            *addr = dl_addr; *val = imm; *width = dl_width;
            return 1;
        }
        return 0;
    }
    if (!dl_len) return 0;
    uint8_t cmpop = (!strcmp(n, "m6502") || !strcmp(n, "huc6280")) ? (idx_reg ? 0 : 0xC9) : 0xFE;
    if (!cmpop || f[0] != cmpop) return 0;
    *addr = dl_addr; *val = f[1]; *width = 1;
    return 1;
}

// 1 when a ROM table access indexed by the loaded value follows within 16 bytes (technique 7)
// B4: proven length (in entries) of the word table whose base `ld hl,nn` follows the load; 0 when unproven.
// An entry is proven while its word designates the ROM at or above isa->code_min (a legal code target).
static unsigned table_proven_len(DpMachine *m, const DpIsa *isa, uint32_t pc, int len) {
    if (!strcmp(isa->name, "m6502") || !strcmp(isa->name, "6502") || !strcmp(isa->name, "huc6280")) {
        // `lda abs,x` / `lda abs,y` from a ROM base: a word table of code pointers while the words designate ROM
        for (unsigned i = 0; i < 13; i++) {
            uint8_t op = m->fetch(m, pc + len + i);
            if (op != 0xBD && op != 0xB9) continue;
            uint32_t base = m->fetch(m, pc + len + i + 1) | (uint32_t)m->fetch(m, pc + len + i + 2) << 8;
            if (m->rom_index(m, base) < 0) continue;
            unsigned n = 0;
            while (n < 128) {
                uint32_t t = m->fetch(m, base + 2 * n) | (uint32_t)m->fetch(m, base + 2 * n + 1) << 8;
                if (t < 0x8000u || m->rom_index(m, t) < 0) break;
                n++;
            }
            return n >= 2 ? n : 0;
        }
        return 0;
    }
    if (strcmp(isa->name, "z80") && strcmp(isa->name, "sm83")) return 0;
    for (unsigned i = 0; i < 13; i++) {
        if (m->fetch(m, pc + len + i) != 0x21) continue;
        uint32_t base = m->fetch(m, pc + len + i + 1) | (uint32_t)m->fetch(m, pc + len + i + 2) << 8;
        if (m->rom_index(m, base) < 0) continue;
        unsigned n = 0;
        while (n < 128) {
            uint32_t t = m->fetch(m, base + 2 * n) | (uint32_t)m->fetch(m, base + 2 * n + 1) << 8;
            if (t < isa->code_min || m->rom_index(m, t) < 0) break;
            n++;
        }
        return n;
    }
    return 0;
}
static int rom_table_follows(DpMachine *m, const DpIsa *isa, uint32_t pc, int len) {
    uint8_t win[16];
    for (unsigned i = 0; i < sizeof win; i++) win[i] = m->fetch(m, pc + len + i);
    const char *n = isa->name;
    int tab = 0;
    if (!strcmp(n, "68000")) {
        // ROM base into An (lea abs.l / lea d16(pc) / movea.l #imm), then an (d8,An,Xn) operand of a move/ALU word
        for (unsigned i = 0; i + 1 < sizeof win; i += 2) {
            unsigned w = (win[i] << 8) | win[i + 1];
            if (i + 5 < sizeof win && ((w & 0xF1FF) == 0x41F9 || w == (0x207Cu | (w & 0x0E00)))) {
                uint32_t a = ((uint32_t)win[i + 2] << 24 | (uint32_t)win[i + 3] << 16 | (uint32_t)win[i + 4] << 8 | win[i + 5]) & 0xFFFFFF;
                if (m->rom_index(m, a) >= 0) tab = 1;
                i += 4;   // skip the address extension
                continue;
            } else if (i + 3 < sizeof win && (w & 0xF1FF) == 0x41FA) {
                uint32_t a = (pc + (uint32_t)len + i + 2 + (uint32_t)(int32_t)(int16_t)((win[i + 2] << 8) | win[i + 3])) & 0xFFFFFF;
                if (m->rom_index(m, a) >= 0) tab = 1;
                i += 2;
                continue;
            }
            if (tab && ((w >> 3) & 7) == 6 && (w & 0xF000) != 0x4000 && (w & 0xF000) != 0x6000) return 1;
        }
        return 0;
    }
    if (!strcmp(n, "z80") || !strcmp(n, "sm83")) {
        for (unsigned i = 0; i + 2 < sizeof win; i++) {
            if (win[i] == 0x21 && m->rom_index(m, win[i + 1] | (win[i + 2] << 8)) >= 0) tab = 1;
            if (tab && (win[i] == 0x19 || win[i] == 0x09 || win[i] == 0x85 || win[i] == 0x83)) return 1;
        }
        return 0;
    }
    // 6502 family: abs,X / abs,Y load/ALU from a ROM base address
    for (unsigned i = 0; i + 2 < sizeof win; i++) {
        uint8_t o = win[i];
        if ((o == 0xBD || o == 0xB9 || o == 0x7D || o == 0x79 || o == 0x3D || o == 0x39 || o == 0xDD || o == 0xD9) &&
            m->rom_index(m, win[i + 1] | (win[i + 2] << 8)) >= 0) return 1;
    }
    return 0;
}

// writes `v` over `width` bytes at `addr`, most significant byte first
static void poke_wide(DpMachine *m, uint32_t addr, int width, uint32_t v) {
    for (int i = 0; i < width; i++) m->poke(m, addr + (uint32_t)i, (uint8_t)(v >> (8 * (width - 1 - i))));
}


// --dsy FILE: the unshifted movie replay writes its work RAM per frame in DSY1 (src/core/desync.h) for `deepeel desync`
// against a reference emulator (tools/lr_dsy).  One writer: the first replay to reach a frame owns it (frames are monotone).
static pthread_mutex_t dsy_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *dsy_file; static uint64_t dsy_last;
// the generic game-screen watch: every watch_every-th frame of the replayed movie / a session goes to the framebuffer window
static void core_watch_publish(const DpCoreConfig *cfg, DpMachine *m, uint64_t f, uint32_t movie_frame, uint32_t total) {
    static __thread uint32_t px[512 * 512];
    if (!cfg->watch_every || !m->render || !dp_watch_frame || f % cfg->watch_every) return;
    int w = 0, h = 0;
    if (m->render(m, px, 512u * 512u, &w, &h) && w > 0 && h > 0) dp_watch_frame(px, w, h, f, movie_frame, total, 0);
}

static void core_dsy_frame(const DpCoreConfig *cfg, DpMachine *m, uint64_t f) {
    uint32_t base = m->work_ram_len ? m->work_ram_base : 0xC000u, len = m->work_ram_len ? m->work_ram_len : 0x2000u;   // declared by the machine (A25)
    pthread_mutex_lock(&dsy_mu);
    if (!dsy_file) { dsy_file = fopen(cfg->dsy_path, "wb"); if (dsy_file) fwrite("DSY1", 1, 4, dsy_file); }
    if (dsy_file && f > dsy_last) {
        static uint8_t buf[0x10000];
        if (len > sizeof buf) len = sizeof buf;
        for (uint32_t i = 0; i < len; i++) buf[i] = m->fetch(m, base + i);
        uint32_t fn = (uint32_t)f; uint16_t nb = 1; uint8_t nl = 4;
        fwrite(&fn, 4, 1, dsy_file); fwrite(&nb, 2, 1, dsy_file); fwrite(&nl, 1, 1, dsy_file); fwrite("wram", 1, 4, dsy_file);
        fwrite(&len, 4, 1, dsy_file); fwrite(buf, 1, len, dsy_file); fflush(dsy_file);
        dsy_last = f;
    }
    pthread_mutex_unlock(&dsy_mu);
}

static void run_path(Worker *w, Pending *p) {
    Shared *sh = w->sh;
    const DpCoreConfig *cfg = sh->cfg;
    DpMachine *m = w->m;
    DpCoreResult *loc = &w->loc;
    const DpIsa *isa = m->isa;
    m->load(m, p->state);
    free(p->state);
    if (!cfg->unbounded && p->seed && covered(loc, m, p->pc)) return;   // bounded pointer seed policy
    if (p->forced) m->set_pc(m, p->pc);
    uint32_t cur_input = p->input;
    // TAS replay: input >= TAS_INPUT encodes the frame shift (value - TAS_INPUT - 3, -3..+3)
    int tas = cur_input >= TAS_INPUT && (cfg->tas_masks || cfg->tas_masks32) && m->frame && m->set_input;
    int tas_shift = tas ? (int)(cur_input - TAS_INPUT) - 3 : 0;
    uint64_t tas_last = ~0ull; int tas_ended = 0;
    if (cur_input >= TAS_INPUT) cur_input = 0;
    // generic long sessions (every platform with a frame counter): attract = no input for attract_frames frames;
    // play = a pad epoch schedule (nine epochs over play_frames frames, each session starting at another catalog entry)
    int attract = cur_input == SESSION_ATTRACT && m->frame;
    int play = cur_input >= SESSION_PLAY && cur_input < TAS_INPUT && m->frame && m->set_input;
    int held = cur_input > 0 && cur_input < SESSION_ATTRACT && m->set_input && !cfg->no_sessions && !cfg->no_held_budget;
    uint64_t held_n = 0;
    unsigned play_k = play ? cur_input - SESSION_PLAY : 0;
    int burst = play && play_k >= 16;   // B3: a pad burst from a session checkpoint: bounded by ckpt_burst_frames, spawns no checkpoints
    if (burst) play_k &= 15u;
    if (cur_input == SESSION_ATTRACT || cur_input >= SESSION_PLAY) cur_input = 0;
    uint64_t sess_limit = attract ? cfg->attract_frames : burst ? cfg->ckpt_burst_frames : play ? cfg->play_frames : 0;
    uint64_t sess_f0 = (attract || play) ? m->frame(m) : 0, sess_last = ~0ull;
    if (cfg->watch_every && m->watch && ((tas && tas_shift == 0) || ((attract || play) && !burst))) m->watch(m, 1);
    unsigned sess_epoch = ~0u; uint64_t sess_last_cov = 0;
    uint64_t sess_cov = 0, sess_prog_f = sess_f0;
    uint8_t *sess_seen = attract && cfg->attract_stall ? calloc(loc->rom_size / 8 + 1, 1) : NULL;   // ROM bytes this very session executed
    int framed = m->frame && (attract || play || cur_input);   // visit counts restart every frame: a game loop is not a runaway loop
    if (attract) loc->attract_paths++;
    if (play) loc->play_paths++;
    if (tas) loc->tas_paths++;
    // a session shares half the total budget with its siblings: it must not starve the natural path
    uint64_t session_cap = cfg->max_total / (2 * (cfg->sessions ? (cfg->sessions > 16 ? 16 : cfg->sessions) : 16));
    if (m->set_input && cur_input) m->set_input(m, cur_input - 1);   // input 0: keep the loaded state's held pad (probe forks hold theirs)
    loc->paths++;
    run_flush(&w->cobs);
    w->cobs.forced = p->forced; w->cobs.last_rom = -1; w->cobs.rq_n = 0; w->pcring_n = 0;
    w->win_on = w->cobs.track = 0;
    uint32_t path_id = ++w->path_id, vid = path_id, steps = 0, slice_steps = 0;
    int cur_forced = p->forced;
    uint16_t cur_origin = p->origin;
    unsigned fdepth = 0;   // calls made since the forced fork: a ret below that pops state the path did not build
    uint8_t last_imm = 0; unsigned imm_age = 99;   // B9: the previous 8-bit immediate load and its distance in instructions
    unsigned pad_slide = 0;   // consecutive executed steps inside a pure 00/FF run (padding is not code)
    for (;;) {
        if (cfg->stop_requested && *cfg->stop_requested) {
            __atomic_store_n(&sh->halt, 4, __ATOMIC_RELAXED);
            pthread_mutex_lock(&sh->mu); pthread_cond_broadcast(&sh->cv); pthread_mutex_unlock(&sh->mu);
        }
        int halt = __atomic_load_n(&sh->halt, __ATOMIC_RELAXED);
        if (halt) {
            if ((halt == 1 || halt == 2 || halt == 4) && cfg->state_dir) {
                uint8_t *snapshot = malloc(sh->sbytes ? sh->sbytes : 1);
                if (snapshot) {
                    m->save(m, snapshot);
                    pthread_mutex_lock(&sh->mu);
                    queue_push(sh, (Pending){ snapshot, m->pc(m), cur_forced, 0, p->input, 0, 0, cur_origin });
                    pthread_mutex_unlock(&sh->mu);
                } else {
                    fprintf(stderr, "deepeel: cannot preserve stopped kernel path\n");
                    __atomic_store_n(&sh->checkpoint_failed, 1, __ATOMIC_RELAXED);
                }
            }
            break;
        }
        if (cfg->unbounded && slice_steps >= 65536) {
            // An unlimited path may loop forever. Requeue its machine state
            // behind pending work instead of starving other forks.
            slice_steps = 0;
            pthread_mutex_lock(&sh->mu);
            if (!sh->qn && sh->spool_n && !spool_refill(sh)) {
                sh->spool_read_failed = 1;
                __atomic_store_n(&sh->halt, 3, __ATOMIC_RELAXED);
                pthread_cond_broadcast(&sh->cv);
            }
            if (sh->qn && !__atomic_load_n(&sh->halt, __ATOMIC_RELAXED)) {
                uint8_t *snapshot = malloc(sh->sbytes ? sh->sbytes : 1);
                if (snapshot) {
                    m->save(m, snapshot);
                    ram_push(sh, (Pending){ snapshot, m->pc(m), cur_forced, 0, p->input, 0, 0, cur_origin });
                    Pending yielded = sh->q[sh->qn - 1];
                    memmove(sh->q + 1, sh->q, (sh->qn - 1) * sizeof *sh->q);
                    sh->q[0] = yielded;
                    loc->cooperative_yields++;
                    pthread_cond_broadcast(&sh->cv);
                    pthread_mutex_unlock(&sh->mu);
                    break;
                }
                fprintf(stderr, "deepeel: cannot yield kernel path; continuing it in place\n");
            }
            pthread_mutex_unlock(&sh->mu);
        }
        if (!(attract || play || held) && cfg->max_total && __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) + w->step_batch >= cfg->max_total) {
            loc->sat_total++;
            pthread_mutex_lock(&sh->mu);
            if (!cfg->state_dir) {
                loc->pending_dropped += sh->qn + sh->spool_n;
                for (size_t i = 0; i < sh->qn; i++) free(sh->q[i].state);
                sh->qn = 0;
            } else {   // with --state-dir the backlog stays for the frontier file and the run winds down
                __atomic_store_n(&sh->halt, 1, __ATOMIC_RELAXED);
                pthread_cond_broadcast(&sh->cv);
            }
            pthread_mutex_unlock(&sh->mu);
            if (cfg->state_dir) continue;  // checkpoint this active path at the next boundary
            break;
        }
        if (tas) {
            uint64_t f = m->frame(m);
            if (f != tas_last) {
                tas_last = f;
                if (tas_shift == 0 && cfg->dsy_path && f > 0) core_dsy_frame(cfg, m, f);
                if (tas_shift == 0) core_watch_publish(cfg, m, f, (uint32_t)f, (uint32_t)cfg->tas_frames);
                if (tas_shift == 0) {   // DEEPEEL_TAS_HRAM=<file>: HRAM per movie frame, comparable with the old engine's dump
                    static FILE *hf; static int hinit;
                    if (!hinit) { hinit = 1; const char *hn = getenv("DEEPEEL_TAS_HRAM"); if (hn) hf = fopen(hn, "wb"); }
                    if (hf) { uint8_t hb[0x80]; for (unsigned i = 0; i < 0x7F; i++) hb[i] = m->fetch(m, 0xFF80 + i); hb[0x7F] = 0; fwrite(hb, 1, 0x80, hf); }
                }
                if (tas_shift == 0) {   // DEEPEEL_TAS_WRAM=<file>: 8 KiB at $C000 per movie frame (Z80 machines), comparable with DP_SEGA_TAS_RAM_DUMP
                    static FILE *wf; static int winit;
                    if (!winit) { winit = 1; const char *wn = getenv("DEEPEEL_TAS_WRAM"); if (wn) wf = fopen(wn, "wb"); }
                    if (wf) { uint8_t wb[0x2000]; for (unsigned i = 0; i < 0x2000; i++) wb[i] = m->fetch(m, 0xC000 + i); fwrite(wb, 1, sizeof wb, wf); fflush(wf); }
                }
                int64_t k = (int64_t)f - tas_shift;
                // A22/B3: live movie states feed the exploration. A checkpoint every tas_ckpt_every frames (bounded) and the
                // end state each continue as a free natural path, so branches reachable only from a late state are forked
                if (tas_shift == 0 && !cfg->no_tas_ckpt && cfg->tas_ckpt_every && f > 0 &&
                    (k >= (int64_t)cfg->tas_frames || (f % cfg->tas_ckpt_every == 0 && __atomic_load_n(&sh->ckpts, __ATOMIC_RELAXED) < cfg->tas_ckpt_cap))) {
                    uint8_t *cp = malloc(sh->sbytes);
                    if (cp) {
                        m->save(m, cp);
                        __atomic_fetch_add(&sh->ckpts, 1, __ATOMIC_RELAXED);
                        pthread_mutex_lock(&sh->mu);
                        queue_push(sh, (Pending){ cp, m->pc(m), 0, ckpt_site(m, loc), 0, 0, 0, DP_SRC_SESSION });
                        pthread_cond_signal(&sh->cv);
                        pthread_mutex_unlock(&sh->mu);
                        loc->tas_ckpts++;
                        // B3: the vector seeds also start from the first live checkpoints (a game state, not the power-on one)
                        int diverse = 0;
                        if (m->plat && !cfg->no_vectors && !cfg->no_live_seeds) {   // B3: seed from the most diverse live states (>= 16 of 256 sampled RAM bytes differ from every chosen one), at most 4
                            uint8_t sig[256]; uint32_t rb = m->work_ram_len ? m->work_ram_base : 0xC000u, rl = m->work_ram_len ? m->work_ram_len : 0x2000u;
                            for (unsigned q = 0; q < 256; q++) sig[q] = m->fetch(m, rb + (uint32_t)((uint64_t)q * rl / 256));
                            pthread_mutex_lock(&sh->mu);
                            if (sh->live_chosen < 4) {
                                diverse = 1;
                                for (unsigned c = 0; c < sh->live_chosen && diverse; c++) {
                                    unsigned d = 0; for (unsigned q = 0; q < 256; q++) d += sig[q] != sh->live_sig[c][q];
                                    if (d < 16) diverse = 0;
                                }
                                if (diverse) memcpy(sh->live_sig[sh->live_chosen++], sig, sizeof sig);
                            }
                            pthread_mutex_unlock(&sh->mu);
                        }
                        if (diverse) {
                            for (int ei = 0; ei < m->plat->entry_count; ei++) {
                                const DpEntry *e = &m->plat->entries[ei];
                                if (e->kind == DP_SEED_RESET) continue;
                                uint32_t vpc = e->addr;
                                if (e->cell) vpc = (uint32_t)(m->fetch(m, e->addr) | (m->fetch(m, (uint32_t)e->addr + 1) << 8));
                                int pad = 1;
                                for (unsigned q = 0; q < 8 && pad; q++) { uint8_t bb = m->fetch(m, vpc + q); pad = bb == 0x00 || bb == 0xFF; }
                                uint8_t *lp = pad ? NULL : malloc(sh->sbytes);
                                if (!lp) continue;
                                memcpy(lp, cp, sh->sbytes);
                                pthread_mutex_lock(&sh->mu);
                                queue_push(sh, (Pending){ lp, vpc, 1, 0, 0, 1, 1, DP_SRC_SESSION });
                                pthread_mutex_unlock(&sh->mu);
                                loc->live_seeds++;
                            }
                        }
                    }
                }
                if (k >= (int64_t)cfg->tas_frames) {
                    m->set_input(m, 0); tas = 0;
                    tas_ended = 1; if (tas_shift == 0) fprintf(stderr, "deepeel: TAS movie played to its end (%llu frames)\n", (unsigned long long)cfg->tas_frames);
                    // the unshifted movie is over: queue the shifted replays on top, so they run next
                    if (tas_shift == 0 && sh->tas_start) {
                        pthread_mutex_lock(&sh->mu);
                        if (sh->tas_shifts_left) {
                            sh->tas_shifts_left = 0;
                            static const int order[6] = {3, 2, 1, -1, -2, -3};
                            for (int i = 0; i < 6; i++) {
                                uint8_t *cp = malloc(sh->sbytes);
                                if (!cp) break;
                                memcpy(cp, sh->tas_start, sh->sbytes);
                                queue_push(sh, (Pending){ cp, sh->tas_pc, 0, 0, TAS_INPUT + 3 + order[i], 0, 0, DP_SRC_SESSION });
                            }
                        }
                        pthread_mutex_unlock(&sh->mu);
                    }
                }
                else m->set_input(m, k < 0 ? 0 : cfg->tas_masks32 ? cfg->tas_masks32[k] : cfg->tas_masks[k]);
            }
        }
        if (tas_ended) break;   // the movie is over: its forks (queued along the way) carry the exploration on, the path does not eat the budget
        if (framed) {
            uint64_t f = m->frame(m);
            if (f != sess_last) {
                sess_last = f;
                if (!burst) core_watch_publish(cfg, m, f, (uint32_t)(f - sess_f0), (uint32_t)sess_limit);   // the live screen also follows the attract/play sessions
                vid = ++w->path_id;
                // B3: attract and play sessions drop live states into the same pool as the movie: a checkpoint every tas_ckpt_every
                // session frames (bounded by tas_ckpt_cap) continues as a free natural path
                if ((attract || play) && !burst && !cfg->no_tas_ckpt && cfg->tas_ckpt_every && f > sess_f0 && (f - sess_f0) % cfg->tas_ckpt_every == 0 &&
                    __atomic_load_n(&sh->sess_ckpts, __ATOMIC_RELAXED) < cfg->tas_ckpt_cap) {
                    uint8_t *cp = malloc(sh->sbytes ? sh->sbytes : 1);
                    if (cp) {
                        m->save(m, cp);
                        __atomic_fetch_add(&sh->sess_ckpts, 1, __ATOMIC_RELAXED);
                        pthread_mutex_lock(&sh->mu);
                        queue_push(sh, (Pending){ cp, m->pc(m), 0, ckpt_site(m, loc), 0, 0, 0, DP_SRC_SESSION });
                        pthread_cond_signal(&sh->cv);
                        pthread_mutex_unlock(&sh->mu);
                        loc->sess_ckpts++;
                        if (cfg->ckpt_burst_frames && m->set_input && m->frame) {   // B3: two bursts of pad input from the live state
                            for (unsigned b = 0; b < 2; b++) {
                                uint8_t *bp = malloc(sh->sbytes ? sh->sbytes : 1);
                                if (!bp) break;
                                m->save(m, bp);
                                pthread_mutex_lock(&sh->mu);
                                queue_push(sh, (Pending){ bp, m->pc(m), 0, 0, SESSION_PLAY + 16u + (unsigned)((loc->sess_ckpts * 3u + b * 5u) % 10u), 0, 0, DP_SRC_SESSION });
                                pthread_cond_signal(&sh->cv);
                                pthread_mutex_unlock(&sh->mu);
                                loc->ckpt_bursts++;
                            }
                        }
                    }
                }
                if (attract && cfg->attract_stall) {   // stop after attract_stall frames without a newly covered ROM byte
                    uint64_t cov = sess_cov;
                    if (cov != sess_last_cov) { sess_last_cov = cov; sess_prog_f = f; }
                    else if (f - sess_prog_f >= cfg->attract_stall) { loc->session_frames += f - sess_f0; loc->session_stalls++; break; }
                }
                if ((attract || play) && sess_limit && f - sess_f0 >= sess_limit) {
                    loc->session_frames += f - sess_f0;
                    break;
                }
                if (play && sess_limit) {
                    unsigned ep = (unsigned)((f - sess_f0) / (sess_limit / 9 ? sess_limit / 9 : 1));
                    if (ep != sess_epoch) {
                        sess_epoch = ep;
                        m->set_input(m, play_catalog[(ep + play_k) % 10]);
                        loc->play_epochs++;
                    }
                }
            }
        }
        if (!cfg->unbounded && (attract || play) && steps >= (cfg->max_total ? cfg->max_total / 2 : 400000000u)) { loc->sat_path++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin }); loc->session_frames += m->frame(m) - sess_f0; break; }   // a long session shares the budget
        if (!tas && !attract && !play && cfg->max_path_steps && steps >= cfg->max_path_steps) { loc->sat_path++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin }); break; }
        if (!attract && !play && cur_input && cfg->max_total && steps >= session_cap) { loc->sat_path++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin }); break; }
        uint32_t pc = m->pc(m);
        if (cfg->hooks && cfg->hooks->before_step &&
            !cfg->hooks->before_step(cfg->hooks->user, m, pc)) break;
        if (sess_seen) {
            int64_t sk = m->rom_index(m, pc);
            if (sk >= 0 && (size_t)sk < loc->rom_size && !(sess_seen[sk >> 3] & (1u << (sk & 7)))) { sess_seen[sk >> 3] |= (uint8_t)(1u << (sk & 7)); sess_cov++; }
        }
        uint32_t skey, vi = site_slot(m, pc, w->site_n, &skey);
        if (w->stamp[vi] != vid) { w->stamp[vi] = vid; w->visits[vi] = 0; }
        if (!tas && (cfg->visits_per_pc || (cfg->unbounded && m->state_hash && !cfg->no_exact))) ++w->visits[vi];   // B5 counts visits without a cap
        if (!tas && cfg->visits_per_pc && w->visits[vi] > cfg->visits_per_pc) { loc->sat_loop++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin }); break; }
        if (cfg->unbounded && !tas && m->state_hash && !cfg->no_exact && w->visits[vi] >= 4 && (!(w->visits[vi] & (w->visits[vi] - 1)) || !(w->visits[vi] & 63))) {
            // B5: the pc was revisited 4, 8, 16... and every 64 times on this path (a loop whose period is a frame needs the dense samples); the same machine state at the same pc means
            // the continuation was (or is being) explored already: a spin loop ends here instead of running forever
            uint64_t ek = (m->state_hash(m) ^ ((uint64_t)pc + 1) * 0x9E3779B97F4A7C15ull) | 1ull;
            pthread_mutex_lock(&sh->mu);
            int fresh = keyset_insert(&sh->exactset, &sh->exactcap, &sh->exactn, ek);
            pthread_mutex_unlock(&sh->mu);
            if (!fresh) { loc->state_repeats++; break; }
        }
        uint8_t bytes[8] = {0};
        for (unsigned i = 0; i < sizeof bytes; i++) bytes[i] = m->fetch(m, pc + i);
        w->cobs.age++;
        w->pcring[w->pcring_n++ & 31] = pc;
        int len = isa->length(bytes[0]);
        DpFlowInfo fi;
        memset(&fi, 0, sizeof fi);
        int have_flow = isa->flow && isa->flow(bytes, pc, &fi);
        if (have_flow && fi.length) len = fi.length;
        if (len <= 0) len = 1;
        // nonsense cuts (E2, the GB legacy eviction mutualized): an executed
        // instruction in echo/OAM/I-O space, a RET whose stack left the RAM
        // space, or a slide through pure 00/FF padding ends the path — the
        // executed byte itself stays covered (the legacy rule: cut after the
        // instruction ran), the reason is counted, never silent
        int ns_pc = !cfg->no_nonsense && !tas && !attract && !play && m->cut_pc && m->cut_pc(m, pc);
        int ns_ret = !cfg->no_nonsense && !tas && !attract && !play && m->cut_sp &&
                     have_flow && fi.kind == DP_FLOW_RET;
        if (m->rom_index(m, pc) < 0) {
            int64_t pri = ridx(m, pc);
            if (cur_forced && !cfg->no_forced_ram_cut && !(sh->fed && pri >= 0 && sh->fed[pri])) {
                loc->forced_ram_cuts++;
                break;
            }
            loc->exec_outside++;
            if (sh->fedsrc && pri >= 0) {
                uint32_t s0 = sh->fedsrc[pri], p0 = sh->fedprod[pri];
                dp_ramarch_add_p(&loc->ra, (uint16_t)pc, bytes, len > 10 ? 10 : len, s0 ? (int32_t)(s0 - 1) : -1, s0 ? -1 : (p0 ? (int32_t)(p0 - 1) : -1));
            }
            if (!cur_forced && sh->fedsrc) {   // executed RAM bytes that a verbatim ROM copy produced: attribute them to their ROM source
                int any = 0;
                for (int i = 0; i < len; i++) {
                    int64_t si = ridx(m, pc + (uint32_t)i);
                    uint32_t src = si >= 0 ? sh->fedsrc[si] : 0;
                    if (src && src - 1 < loc->rom_size) {
                        __atomic_fetch_or(&loc->flags[src - 1], DP_CORE_NATURAL, __ATOMIC_RELAXED);
                        any = 1;
                    }
                }
                if (any) loc->exec_wram_rom++;
            }
        }
        // technique 7 (restricted form): z80 `ld a,(RAM)` whose value then indexes a ROM table
        // (`ld hl,ROM` + `add hl,de` within 12 bytes): replay the site with each index 0..index_max (default 15, --index-max up to 255)
        uint32_t dl_addr = 0; int dl_idx = 0, dl_width = 1, dl_dreg = -1;
        int dl_len = (m->poke && !cur_forced) ? direct_ram_load(m, isa, bytes, &dl_addr, &dl_idx, &dl_width, &dl_dreg) : 0;
        if (dl_len) dl_addr += (uint32_t)dl_width - 1;   // big-endian: the low-order byte is the last; 1 byte elsewhere
        if (!cfg->no_index_replay && dl_len) {
            if (rom_table_follows(m, isa, pc, dl_len)) {
                uint32_t addr = dl_addr;
                pthread_mutex_lock(&sh->mu);
                int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ((uint64_t)addr | ((uint64_t)(m->rom_index(m, pc) + 1) << 24)) | (7ull << 60));   // A7: (site, address)
                pthread_mutex_unlock(&sh->mu);
                uint8_t *tmp = fresh ? malloc(sh->sbytes) : NULL;
                if (tmp) m->save(m, tmp);
                unsigned proven = table_proven_len(m, isa, pc, dl_len), kmax = cfg->index_max ? cfg->index_max : cfg->unbounded ? 255 : 15;
                if (!cfg->index_max && !cfg->unbounded && proven * 2 > kmax) kmax = proven * 2 > 255 ? 255 : proven * 2;   // B4: the whole proven table, index scaled by the entry size
                for (unsigned k = 0; tmp && k <= kmax; k++) {   // B4: --unbounded enumerates the whole byte
                    uint8_t *snap = malloc(sh->sbytes);
                    if (!snap) break;
                    m->poke(m, addr, (uint8_t)k);
                    m->save(m, snap);
                    pthread_mutex_lock(&sh->mu);
                    queue_push(sh, (Pending){ snap, pc, 0, 0, cur_input, 0, 0, DP_SRC_INDEX });
                    pthread_mutex_unlock(&sh->mu);
                    loc->index_replays++;
                }
                if (tmp) { m->load(m, tmp); free(tmp); }
            }
        }
        // technique 4 (restricted form): a z80 `ld a,(nn)` of RAM that returns a different value on a later
        // visit within one path reads a byte the game updates (RNG, timer); fork it on a few values
        if (!cfg->no_rng_learn && dl_len) {
            uint32_t addr = dl_addr;
            uint8_t v = m->fetch(m, addr);
            int volatile_byte = w->rng_stamp[vi] == path_id && w->rng_last[vi] != v;
            w->rng_stamp[vi] = path_id; w->rng_last[vi] = v;
            int fresh = 0;
            if (volatile_byte) {
                pthread_mutex_lock(&sh->mu);
                fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ((uint64_t)addr | ((uint64_t)(m->rom_index(m, pc) + 1) << 24)) | (5ull << 60));   // A7: (site, address)
                pthread_mutex_unlock(&sh->mu);
            }
            if (fresh) {
                static const uint8_t rv[4] = {0x00, 0x01, 0x80, 0xFF};
                uint8_t *tmp = malloc(sh->sbytes);
                if (tmp) m->save(m, tmp);
                for (unsigned k = 0; tmp && k < 4; k++) {
                    uint8_t *snap = malloc(sh->sbytes);
                    if (!snap) break;
                    m->poke(m, addr, rv[k]);
                    m->save(m, snap);
                    pthread_mutex_lock(&sh->mu);
                    queue_push(sh, (Pending){ snap, pc, 0, 0, cur_input, 0, 0, DP_SRC_RNG });
                    pthread_mutex_unlock(&sh->mu);
                    loc->rng_forks++;
                }
                if (tmp) { m->load(m, tmp); free(tmp); }
            }
        }
        // technique 3 (learned form): z80 `ld a,(nn)` of RAM straight followed by `cp n` is a signature test;
        // queue a state whose byte already holds n so the matching side runs as natural
        uint32_t sg_addr = 0, sg_val = 0; int sg_w = 1;
        if (!cfg->no_learn_sig && m->poke && !cur_forced &&
            ram_signature(m, isa, pc, bytes, dl_len, dl_idx, dl_len ? dl_addr - (uint32_t)dl_width + 1 : 0, dl_width, dl_dreg, &sg_addr, &sg_val, &sg_w)) {
            uint32_t addr = sg_addr; uint32_t sigv = sg_val;
            pthread_mutex_lock(&sh->mu);
            int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben,
                                      ((uint64_t)addr << 32 | sigv) | (3ull << 60));
            pthread_mutex_unlock(&sh->mu);
            if (fresh) {
                uint8_t *tmp = malloc(sh->sbytes), *snap = malloc(sh->sbytes);
                if (tmp && snap) {
                    m->save(m, tmp);
                    poke_wide(m, addr, sg_w, sigv);
                    m->save(m, snap);
                    m->load(m, tmp);
                    pthread_mutex_lock(&sh->mu);
                    queue_push(sh, (Pending){ snap, pc, 0, 0, cur_input, 0, 0, DP_SRC_SIG });
                    pthread_mutex_unlock(&sh->mu);
                    loc->learned_sigs++;
                    snap = NULL;
                }
                free(tmp); free(snap);
            }
        }
        if (cur_forced && !p->vector && !cfg->no_taint_cut && have_flow) {
            // technique 1.b: the inverted flag may have corrupted registers/stack; a transfer whose target comes from them is untrusted
            if (fi.kind == DP_FLOW_INDIRECT) { loc->taint_cut_indirect++; break; }
            if (fi.kind == DP_FLOW_RET) { if (!fdepth) { loc->taint_cut_ret++; break; } fdepth--; }
            else if (fi.call) fdepth++;
        }
        mark(sh, loc, m, pc, (unsigned)len, cur_forced ? DP_CORE_FORCED : DP_CORE_NATURAL, cur_origin);
        if (!cur_forced && isa->imm_ptr && loc->refs) {   // B9: an executed immediate that designates the ROM is a typed reference
            uint32_t imm;
            if (isa->imm_ptr(bytes, &imm)) {
                int64_t ri = m->rom_index(m, imm);
                if (ri >= 0 && (size_t)ri < loc->rom_size && !__atomic_exchange_n(&loc->refs[ri], 1, __ATOMIC_RELAXED)) loc->imm_refs++;
            }
        }
        if (!cur_forced && isa->imm_byte && loc->refs) {   // B9: two nearby 8-bit immediates (either order) forming a ROM address
            uint8_t iv;
            if (isa->imm_byte(bytes, &iv)) {
                if (imm_age <= 3) {
                    uint32_t cand[2] = { (uint32_t)last_imm | (uint32_t)iv << 8, (uint32_t)iv | (uint32_t)last_imm << 8 };
                    for (int ci = 0; ci < 2; ci++) {
                        int64_t ri = cand[ci] >= 0x100 ? m->rom_index(m, cand[ci]) : -1;
                        if (ri >= 0 && (size_t)ri < loc->rom_size && !__atomic_exchange_n(&loc->refs[ri], 1, __ATOMIC_RELAXED)) loc->imm_refs++;
                    }
                }
                last_imm = iv; imm_age = 0;
            } else if (imm_age < 99) imm_age++;
        }
        if (!cfg->no_closure && !cur_forced && have_flow && fi.kind != DP_FLOW_SEQ && fi.kind != DP_FLOW_ILLEGAL) {
            if (fi.target >= 0 && (fi.kind == DP_FLOW_JUMP || fi.kind == DP_FLOW_COND || fi.kind == DP_FLOW_CALL)) closure_walk(loc, m, isa, (uint32_t)fi.target);
            if (fi.kind == DP_FLOW_COND) closure_walk(loc, m, isa, pc + fi.length);
            else if (fi.kind == DP_FLOW_CALL) closure_walk_untrusted(loc, m, isa, pc + fi.length);
            if (fi.kind == DP_FLOW_INDIRECT && bytes[0] == 0xE9) closure_jp_table(loc, m, cfg, isa, pc, w->pcring_n >= 8 ? w->pcring : NULL, w->pcring_n < 32 ? w->pcring_n : 32);   // a path too young to hold the load is not filtered
            if (fi.kind == DP_FLOW_INDIRECT && (bytes[0] == 0x6C || bytes[0] == 0x7C) &&
                isa->abs_ind_table)
                closure_abs_table(loc, m, isa, bytes);
        }
        if (!cfg->no_solve && !cur_forced && m->poke && have_flow) {
            // window: the sequential instructions before a conditional; their start state and RAM reads feed solve_cond
            if (fi.kind == DP_FLOW_COND) { /* closes the window below */ }
            else if (fi.kind != DP_FLOW_SEQ) w->win_on = w->cobs.track = 0;
            else if (!w->win_on && cond_ahead(w, isa, pc)) {
                m->save(m, w->wsnap);
                w->win_on = 1; w->win_pc = pc; w->win_n = 0; w->win_ni = 0; w->cobs.nram = 0; w->cobs.track = 1;
            }
            if (w->win_on) {
                if (++w->win_n > 8) w->win_on = w->cobs.track = 0;
                for (int i = 1; i < len && w->win_ni < sizeof w->win_imm; i++) w->win_imm[w->win_ni++] = bytes[i];
            }
        }
        if (!cfg->no_event_probes && cfg->hooks && cfg->hooks->event_probe && m->rom_index(m, pc) >= 0) {
            pthread_mutex_lock(&sh->mu);
            int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, (uint64_t)m->rom_index(m, pc) + 1);
            pthread_mutex_unlock(&sh->mu);
            int rearm = 0;
            if (!fresh && cfg->probe_ctx_cap > 1 && w->visits[vi] && w->visits[vi] <= 8 && !(w->visits[vi] & (w->visits[vi] - 1)) &&
                sh->probectx && sh->probectx[m->rom_index(m, pc)]) {
                // B1: a site that produced probes is re-armed when it is reached under another semantic cell (the RAM the
                // code has been seen to read), up to probe_ctx_cap contexts per site (saturation counted, never silent)
                int64_t ri = m->rom_index(m, pc);
                if (sh->probectx[ri] >= cfg->probe_ctx_cap) loc->probe_ctx_sat++;
                else {
                    uint64_t ck = cell_key(cfg, sh->ramread, m, (uint32_t)ri) ^ 0x5bd1e995u;
                    pthread_mutex_lock(&sh->mu);
                    if (keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, (keyhash(ck ^ ((uint64_t)ri << 32)) & ((1ull << 60) - 1)) | (11ull << 60))) fresh = rearm = 1;
                    pthread_mutex_unlock(&sh->mu);
                }
            }
            if (fresh) {
                unsigned k = 0;
                m->pad_full = cfg->pad_full;
                uint8_t *snap = malloc(sh->sbytes);
                int any = 0;
                while (snap && cfg->hooks->event_probe(cfg->hooks->user, m, pc, bytes, k++, snap)) {
                    any = 1;
                    pthread_mutex_lock(&sh->mu);
                    {
                        queue_push(sh, (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input, 0, 0, DP_SRC_PROBE });
                        loc->event_probes++;
                        pthread_cond_signal(&sh->cv);
                        snap = malloc(sh->sbytes);
                    }
                    pthread_mutex_unlock(&sh->mu);
                }
                free(snap);
                if (any && sh->probectx) { int64_t ri = m->rom_index(m, pc); if (sh->probectx[ri] < 255) sh->probectx[ri]++; }
                if (any && rearm) loc->probe_rearms++;
                if (any && !rearm) loc->event_probe_sites++;
            }
        }
        if (cfg->hooks && cfg->hooks->irq_alt && m->rom_index(m, pc) >= 0) {
            // delay d = 1 is the plain IRQ fork; d = 2..3 is technique 9 (temporal
            // perturbation: the interrupt lands a few instructions later).  The hook
            // answers 0 unless an interrupt is pending, so the per-(site, delay) key is
            // spent only when a fork is really produced.
            for (unsigned d = 1; d <= (cfg->no_irq_jitter ? 1u : 3u); d++) {
                uint64_t ikey = ((uint64_t)m->rom_index(m, pc) + 1) | ((uint64_t)d << 40);
                int known = 0;   // (site, delay) already forked: skip the costly hook and the 17 KiB snapshot
                pthread_mutex_lock(&sh->mu);
                if (sh->probecap) { size_t h = keyhash(ikey) & (sh->probecap - 1); while (sh->probeset[h]) { if (sh->probeset[h] == ikey) { known = 1; break; } h = (h + 1) & (sh->probecap - 1); } }
                pthread_mutex_unlock(&sh->mu);
                if (known) continue;
                uint8_t *snap = malloc(sh->sbytes);
                if (!snap) break;
                if (!cfg->hooks->irq_alt(cfg->hooks->user, m, d, snap)) { free(snap); break; }
                pthread_mutex_lock(&sh->mu);
                int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ((uint64_t)m->rom_index(m, pc) + 1) | ((uint64_t)d << 40));
                if (!fresh) free(snap);
                else {
                    queue_push(sh, (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input, 0, 0, DP_SRC_IRQ });
                    if (d == 1) loc->irq_forks++; else loc->jitter_forks++;
                    pthread_cond_signal(&sh->cv);
                }
                pthread_mutex_unlock(&sh->mu);
            }
        }
        int cond = have_flow && fi.kind == DP_FLOW_COND && fi.target >= 0;
        if (cond && cfg->fork_site_cap && w->capped[site_slot(m, pc, w->site_n, NULL)] == skey) { loc->sat_site++; cond = 0; }   // site already saturated: no snapshot, no lock
        uint8_t *pre = NULL;
        if (cond) {
            pre = malloc(sh->sbytes);
            m->save(m, pre);
        }
        if (!m->run(m)) { free(pre); break; }
        if (w->cobs.mapper_w) {
            w->cobs.mapper_w = 0;
            if (!cfg->no_bank_forks && m->bank_view && !tas && !w->cobs.forced) {
                // B7: a write into the ROM window is a mapper register: fork once per (site, bank) on every ROM block this
                // site has not shown yet, in the state the game really reached (bounded per site, saturation counted)
                unsigned nb = (unsigned)(loc->rom_size / 0x4000), made = 0;
                int64_t site = m->rom_index(m, pc);
                uint8_t *cur = nb > 1 ? malloc(sh->sbytes) : NULL;
                if (cur) m->save(m, cur);
                for (unsigned b = 0; cur && b < nb; b++) {
                    uint64_t bk = ((uint64_t)(site + 1) << 20 ^ (uint64_t)b * 0x9E3779B97F4A7C15ull) & ((1ull << 60) - 1);
                    pthread_mutex_lock(&sh->mu);
                    int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, keyhash(bk) & ((1ull << 60) - 1) | (13ull << 60));
                    pthread_mutex_unlock(&sh->mu);
                    if (!fresh) continue;
                    if (cfg->bank_fork_cap && made >= cfg->bank_fork_cap) { loc->bank_fork_sat++; break; }
                    uint8_t *snap = malloc(sh->sbytes);
                    if (!snap) break;
                    int ok = m->bank_view(m, b);
                    if (ok) m->save(m, snap);
                    m->load(m, cur);
                    if (!ok) { free(snap); continue; }
                    pthread_mutex_lock(&sh->mu);
                    queue_push(sh, (Pending){ snap, m->pc(m), 0, (uint32_t)(site + 1), cur_input, 0, 0, DP_SRC_BANK });
                    pthread_cond_signal(&sh->cv);
                    pthread_mutex_unlock(&sh->mu);
                    loc->bank_forks++; made++;
                }
                free(cur);
            }
        }
        if (ns_pc) { loc->nonsense_pc++; break; }
        if (ns_ret && m->cut_sp(m)) { loc->nonsense_sp++; break; }
        if (!cfg->no_nonsense && !tas && !attract && !play) {
            // padding slide: an executed instruction inside a pure 00/FF run — the
            // code after the run is often the point of a forced entry, so keep going,
            // but garbage loops stay bounded (the 32-byte window keeps short real
            // NOP loops alive)
            if ((bytes[0] == 0x00 || bytes[0] == 0xFF) && len == 1) {
                int inpad = 1;
                for (int i = -16; i <= 16 && inpad; i++) {
                    if (!i) continue;
                    uint8_t b = m->fetch(m, pc + (uint32_t)i);
                    inpad = b == 0x00 || b == 0xFF;
                }
                if (inpad) { if (++pad_slide > 4096) { loc->pad_cuts++; break; } }
                else pad_slide = 0;
            } else pad_slide = 0;
        }
        steps++; slice_steps++;
        if (held) {   // A26: a held-pad session path (and its forks) spends its own budget, never the shared one
            loc->session_steps++;
            if (cfg->held_steps && ++held_n > cfg->held_steps) { loc->held_stops++; break; }
        } else if (attract || play) loc->session_steps++;   // a long session has its own budget: it neither spends nor waits for the shared one
        else if (++w->step_batch == 256) { __atomic_fetch_add(&sh->steps, 256, __ATOMIC_RELAXED); w->step_batch = 0; }
        if (cfg->hooks && cfg->hooks->after_step) cfg->hooks->after_step(cfg->hooks->user, m, pc);
        if (cond) {
            uint32_t after = m->pc(m);
            uint32_t fall = pc + (uint32_t)len, tgt = (uint32_t)fi.target;
            uint32_t other = after == tgt ? fall : tgt;
            if (after != tgt && after != fall) { free(pre); continue; }   // an IRQ intervened
            if (covered(loc, m, other) && covered(loc, m, after)) {
                // A9: an other side covered only by forced paths is still unproven as natural: try to solve it
                if (w->win_on && w->cobs.nram && !covered_natural(loc, m, other) && solve_cond(w, cfg, pc, other, cur_input)) { loc->promoted_forced++; free(pre); }
                else { loc->pruned_covered++; free(pre); }
            } else if (cfg->hooks && cfg->hooks->veto_fork &&
                       !cfg->hooks->veto_fork(cfg->hooks->user, m, pc, other)) {
                loc->hook_vetoed++; free(pre);
            } else if (w->win_on && w->cobs.nram && !covered(loc, m, other) && solve_cond(w, cfg, pc, other, cur_input)) {
                free(pre);   // proved a legal way to the other side: no forced inversion
            } else {
                pthread_mutex_lock(&sh->mu);
                int64_t sk_ = m->rom_index(m, pc);
                uint32_t *cnt = site_count(&sh->sites, sk_ >= 0 ? (uint32_t)sk_ + 1u : (pc | 0x80000000u));   // A5: per ROM site (bank-aware), not per pc
                int pushed = 0;
                if (cfg->cell_hi > cfg->cell_lo || !cfg->no_cells) {
                    if (cell_insert(sh, cell_key(cfg, sh->ramread, m, other))) loc->cells++;
                    else { loc->cell_pruned++; pthread_mutex_unlock(&sh->mu); free(pre); continue; }
                }
                if (cfg->fork_site_cap && *cnt >= cfg->fork_site_cap) { loc->sat_site++; w->capped[site_slot(m, pc, w->site_n, NULL)] = skey; }
                else {
                    queue_push(sh, (Pending){ pre, other, 1, (uint32_t)(m->rom_index(m, pc) + 1), cur_input, 0, 0, cur_origin });
                    (*cnt)++;
                    loc->forks++;
                    pushed = 1;
                    pthread_cond_signal(&sh->cv);
                }
                pthread_mutex_unlock(&sh->mu);
                if (!pushed) free(pre);
            }
        }
        if (fi.kind == DP_FLOW_COND) w->win_on = w->cobs.track = 0;
    }
    w->win_on = w->cobs.track = 0;
    free(sess_seen);
}

// progress line: steps, paths, queue, workers busy, ROM bytes seen so far
static void *monitor_main(void *arg) {
    Shared *sh = arg;
    unsigned secs = sh->cfg->progress_secs, t = 0;
    time_t t0 = time(NULL);
    while (!__atomic_load_n(&sh->stop_monitor, __ATOMIC_RELAXED)) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        if (++t < secs * 10) continue;
        t = 0;
        size_t nat = 0, forced = 0;
        for (size_t i = 0; i < sh->out->rom_size; i++) {
            uint8_t f = sh->out->flags[i];
            if (f & DP_CORE_NATURAL) nat++;
            if (f & DP_CORE_FORCED) forced++;
        }
        pthread_mutex_lock(&sh->mu);
        size_t qn = sh->qn; int act = sh->active;
        pthread_mutex_unlock(&sh->mu);
        unsigned long long st = __atomic_load_n(&sh->steps, __ATOMIC_RELAXED);
        long el = (long)(time(NULL) - t0);
        fprintf(stderr, "[%ldm%02lds] steps %llu (%.1f M/s) paths %llu queue %zu busy %d ROM bytes natural %zu forced %zu%s\n",
                el / 60, el % 60, st, el ? st / 1e6 / el : 0.0,
                (unsigned long long)__atomic_load_n(&sh->paths_done, __ATOMIC_RELAXED), qn, act, nat, forced,
                sh->cfg->max_total ? "" : " (no step limit)");
    }
    return NULL;
}


// adaptive mode: every second, allowed = cores not used by other processes, lowered when
// MemAvailable is under 10% of the total or 1 GiB.  Worker 0 always runs.
static int read_cpu(unsigned long long *idle, unsigned long long *total) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return 0;
    unsigned long long t = 0;
    for (int i = 0; i < 8; i++) t += v[i];
    *idle = v[3] + v[4]; *total = t;
    return 1;
}
static int read_mem(unsigned long long *avail_kb, unsigned long long *total_kb) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128]; int got = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %llu", total_kb) == 1) got |= 1;
        else if (sscanf(line, "MemAvailable: %llu", avail_kb) == 1) got |= 2;
    }
    fclose(f);
    return got == 3;
}
static void *controller_main(void *arg) {
    Shared *sh = arg;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 1;
    unsigned long long i0 = 0, t0 = 0, i1, t1;
    int have = read_cpu(&i0, &t0);
    unsigned ram_cap = sh->wmax;
    struct timespec pc0, pc1, w0, w1;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pc0); clock_gettime(CLOCK_MONOTONIC, &w0);
    while (!__atomic_load_n(&sh->stop_monitor, __ATOMIC_RELAXED)) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        if (!have || !read_cpu(&i1, &t1) || t1 <= t0) { have = read_cpu(&i0, &t0); continue; }
        double busy = ncpu * (1.0 - (double)(i1 - i0) / (double)(t1 - t0));   // cores in use, ours included
        i0 = i1; t0 = t1;
        unsigned cur = __atomic_load_n(&sh->allowed, __ATOMIC_RELAXED);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pc1); clock_gettime(CLOCK_MONOTONIC, &w1);
        double dw = (w1.tv_sec - w0.tv_sec) + (w1.tv_nsec - w0.tv_nsec) / 1e9;
        double ours_cpu = dw > 0 ? ((pc1.tv_sec - pc0.tv_sec) + (pc1.tv_nsec - pc0.tv_nsec) / 1e9) / dw : 0;   // cores we really got
        pc0 = pc1; w0 = w1;
        double others = busy - ours_cpu; if (others < 0) others = 0;   // cores taken by other processes
        unsigned ours = (unsigned)(ours_cpu + 0.5);
        long want = (long)(ncpu - others + 0.5);
        unsigned long long av, tot;
        if (read_mem(&av, &tot)) {   // RAM cap: persistent, lowered under 10% / 1 GiB free, lifted only above 20% / 2 GiB
            if (av < tot / 10 || av < 1048576ull) { if (ram_cap > 1) ram_cap--; }
            else if (av > tot / 5 && av > 2097152ull && ram_cap < sh->wmax) ram_cap++;
        }
        if (want > (long)ram_cap) want = ram_cap;
        if (want < (long)sh->wmin) want = sh->wmin;
        if (want > (long)sh->wmax) want = sh->wmax;
        if (want != (long)cur && (labs(want - (long)cur) > 1 || want == (long)sh->wmin || want == (long)sh->wmax)) {
            fprintf(stderr, "core: auto workers %u -> %ld (cores busy %.1f, ours %u)\n", cur, want, busy, ours);
            __atomic_store_n(&sh->allowed, (unsigned)want, __ATOMIC_RELAXED);
            pthread_mutex_lock(&sh->mu); pthread_cond_broadcast(&sh->cv); pthread_mutex_unlock(&sh->mu);
        }
    }
    return NULL;
}

// generic time limits: --min-time S (no stagnation stop before), --max-time S (hard stop),
// --stall-time S (stop after S seconds without a newly covered ROM byte).  All optional, all platforms.
static void *limits_main(void *arg) {
    Shared *sh = arg;
    const DpCoreConfig *cfg = sh->cfg;
    time_t t0 = time(NULL), last_gain = t0;
    size_t seen = 0;
    while (!__atomic_load_n(&sh->stop_monitor, __ATOMIC_RELAXED)) {
        struct timespec ts = {0, 200000000};
        nanosleep(&ts, NULL);
        time_t now = time(NULL);
        size_t n = 0;
        for (size_t i = 0; i < sh->out->rom_size; i++) if (sh->out->flags[i] & (DP_CORE_NATURAL | DP_CORE_FORCED)) n++;
        if (n != seen) { seen = n; last_gain = now; }
        int why = 0;
        if (cfg->max_secs && (uint32_t)(now - t0) >= cfg->max_secs) why = 1;
        else if (cfg->stall_secs && (uint32_t)(now - t0) >= cfg->min_secs && (uint32_t)(now - last_gain) >= cfg->stall_secs) why = 2;
        if (why) {
            __atomic_store_n(&sh->halt, why, __ATOMIC_RELAXED);
            pthread_mutex_lock(&sh->mu); pthread_cond_broadcast(&sh->cv); pthread_mutex_unlock(&sh->mu);
            break;
        }
    }
    return NULL;
}


// C4: static inference over the classified map, one implementation for every ISA.  Padding runs, small gaps between
// classified cells and the siblings of copied tables are tagged in r->inferred (DP_MAP_*): inferences, never
// observations, and a classified byte is never overwritten.
static void infer_static(DpCoreResult *r, DpMachine *m, const DpCoreConfig *cfg) {
    const uint8_t *img = cfg->rom_image;
    size_t n = r->rom_size;
    if (!img || cfg->rom_image_size < n || !n) return;
    r->inferred = calloc(n, 1);
    if (!r->inferred) return;
    const uint8_t any = DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_COPIED | DP_CORE_COMPRESSED | DP_CORE_DATA | DP_CORE_STATIC;
    const uint8_t code = DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC;
    // padding: a run of one value over bytes nothing classified ($00/$FF from 8 bytes, the others from 32)
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && img[j] == img[i] && !(r->flags[j] & any)) j++;
        size_t len = j - i;
        if (len >= ((img[i] == 0 || img[i] == 0xFF) ? 8u : 32u)) {
            for (size_t k = i; k < j; k++) r->inferred[k] = DP_MAP_PAD;
            r->padding_bytes += len;
        }
        i = j > i ? j : i + 1;
    }
    // copy-table siblings: a verbatim-copied run whose source address appears as a word of a data table; the other
    // entries of that table point at same-length siblings.  The word resolves through the machine's final mapping.
    const unsigned pw = m->isa && m->isa->ptr_width ? m->isa->ptr_width : 0;
    if (pw == 2 || pw == 4) {
        unsigned be = m->isa->big_endian;
        for (size_t s = 0; s < n;) {
            if (!(r->flags[s] & DP_CORE_COPIED)) { s++; continue; }
            size_t e = s; while (e < n && (r->flags[e] & DP_CORE_COPIED)) e++;
            size_t len = e - s;
            if (len >= 8 && len <= 4096) {
                for (size_t k = 0; k + pw <= n; k++) {
                    if (!(r->flags[k] & DP_CORE_DATA) && !(k && (r->flags[k - 1] & DP_CORE_DATA))) continue;
                    uint32_t w = 0;
                    for (unsigned b = 0; b < pw; b++) w |= (uint32_t)img[k + (be ? pw - 1 - b : b)] << (8 * b);
                    int64_t head = m->rom_index(m, w);   // the copy's first byte can be missed by the read/write pairing: accept a table word a few bytes before the run
                    if (head < 0 || (size_t)head > s || s - (size_t)head > 3) continue;
                    size_t blen = len + (s - (size_t)head);
                    if (blen < 16) continue;
                    size_t t0 = k, t1 = k + pw; unsigned entries = 1;
                    for (;;) {   // walk the table both ways while the words resolve into the ROM
                        if (entries >= 256 || t0 < pw) break;
                        uint32_t v = 0; for (unsigned b = 0; b < pw; b++) v |= (uint32_t)img[t0 - pw + (be ? pw - 1 - b : b)] << (8 * b);
                        if (m->rom_index(m, v) < 0) break;
                        t0 -= pw; entries++;
                    }
                    for (;;) {
                        if (entries >= 256 || t1 + pw > n) break;
                        uint32_t v = 0; for (unsigned b = 0; b < pw; b++) v |= (uint32_t)img[t1 + (be ? pw - 1 - b : b)] << (8 * b);
                        if (m->rom_index(m, v) < 0) break;
                        t1 += pw; entries++;
                    }
                    if (entries < 2) continue;
                    r->sib_tables++;
                    for (size_t q = t0; q < t1; q += pw) {
                        uint32_t v = 0; for (unsigned b = 0; b < pw; b++) v |= (uint32_t)img[q + (be ? pw - 1 - b : b)] << (8 * b);
                        int64_t tb = m->rom_index(m, v);
                        if (tb < 0 || tb == head) continue;
                        size_t end = (size_t)tb + blen > n ? n : (size_t)tb + blen;
                        int hit = 0;
                        for (size_t i = (size_t)tb; i < end && !hit; i++) if (r->flags[i] & code) hit = 1;
                        if (hit) { r->sib_refused++; continue; }
                        for (size_t i = (size_t)tb; i < end; i++)
                            if (!(r->flags[i] & any) && !r->inferred[i]) { r->inferred[i] = DP_MAP_INF_DATA; r->sib_bytes++; }
                    }
                    for (size_t q = t0; q < t1; q++) if (!(r->flags[q] & any) && !r->inferred[q]) r->inferred[q] = DP_MAP_TABLE;
                }
            }
            s = e;
        }
    }
    // jump tables: around a data cell the run read, contiguous words that resolve to executed code form a dispatch table
    if (pw == 2 || pw == 4) {
        unsigned be = m->isa->big_endian;
        #define JT_WORD(p, out) do { uint32_t v_ = 0; for (unsigned b_ = 0; b_ < pw; b_++) v_ |= (uint32_t)img[(p) + (be ? pw - 1 - b_ : b_)] << (8 * b_); \
            int64_t t_ = m->rom_index(m, v_); (out) = (t_ >= 0 && (size_t)t_ < n && (r->flags[t_] & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC))); } while (0)
        for (size_t k = 0; k + pw <= n; k++) {
            if (!(r->flags[k] & DP_CORE_DATA) || (r->flags[k] & (DP_CORE_NATURAL | DP_CORE_FORCED))) continue;
            for (unsigned al = 0; al < pw; al++) {
                if (k < al) continue;
                size_t a = k - al; int ok;
                if (a + pw > n) continue;
                JT_WORD(a, ok);
                if (!ok || r->inferred[a] == DP_MAP_TABLE) continue;
                size_t lo = a, hi = a; unsigned entries = 1;
                for (;;) { if (lo < pw || entries >= 256) break; JT_WORD(lo - pw, ok); if (!ok) break; lo -= pw; entries++; }
                for (;;) { if (hi + 2 * pw > n || entries >= 256) break; JT_WORD(hi + pw, ok); if (!ok) break; hi += pw; entries++; }
                if (entries < 2) continue;
                r->jt_tables++;
                for (size_t q = lo; q < hi + pw; q++)
                    if (!(r->flags[q] & (DP_CORE_NATURAL | DP_CORE_FORCED)) && r->inferred[q] != DP_MAP_TABLE) { r->inferred[q] = DP_MAP_TABLE; r->jt_bytes++; }
                break;
            }
        }
        #undef JT_WORD
    }
    // small gaps between classified cells
    for (size_t i = 0; i < n;) {
        if ((r->flags[i] & any) || r->inferred[i]) { i++; continue; }
        size_t j = i;
        while (j < n && !(r->flags[j] & any) && !r->inferred[j]) j++;
        size_t len = j - i;
        uint8_t lf = i ? r->flags[i - 1] : 0, rf = j < n ? r->flags[j] : 0, c = 0;
        int lpad = i && r->inferred[i - 1] == DP_MAP_PAD, rpad = j < n && r->inferred[j] == DP_MAP_PAD;
        if (len <= 3 && (lf & code) && (rf & code)) c = DP_MAP_INF_CODE;
        else if (len <= 1024 && lpad && (j >= n || rpad || !(rf & any))) c = DP_MAP_INF_PAD;
        else if (len <= 1024 && ((lf & any) || (rf & any))) c = DP_MAP_INF_DATA;
        if (c) {
            for (size_t k = i; k < j; k++) r->inferred[k] = c;
            if (c == DP_MAP_INF_CODE) r->inferred_code += len; else if (c == DP_MAP_INF_PAD) r->inferred_pad += len; else r->inferred_data += len;
        }
        i = j;
    }
}

static void *worker_main(void *arg) {
    Worker *w = arg;
    Shared *sh = w->sh;
    const DpCoreConfig *cfg = sh->cfg;
    if (w->m->attach && !w->m->attach(w->m)) {
        fprintf(stderr, "core: worker could not build its machine, retired\n");
        return NULL;
    }
    // each worker hooks its own machine from its own thread (platform state may be thread-local)
    DpObservers obsv = { NULL, obs_read, obs_write, obs_copy };
    if (!cfg->no_copies && w->m->observe) w->m->observe(w->m, &obsv, &w->cobs);
    pthread_mutex_lock(&sh->mu);
    for (;;) {
        for (;;) {
            if (__atomic_load_n(&sh->halt, __ATOMIC_RELAXED)) {   // drop the backlog (the disk spool stays reported as left)
                if (!cfg->state_dir) {   // with --state-dir the backlog is kept for the frontier file
                    w->loc.pending_dropped += sh->qn;
                    for (size_t i = 0; i < sh->qn; i++) free(sh->q[i].state);
                    sh->qn = 0;
                }
                break;
            }
            if (!sh->qn && sh->spool_n && !spool_refill(sh)) {
                sh->spool_read_failed = 1;
                __atomic_store_n(&sh->halt, 3, __ATOMIC_RELAXED);
                pthread_cond_broadcast(&sh->cv);
                continue;
            }
            if (sh->qn || sh->active == 0) break;
            pthread_cond_wait(&sh->cv, &sh->mu);
        }
        if (!sh->qn || (cfg->state_dir && __atomic_load_n(&sh->halt, __ATOMIC_RELAXED))) break;
        if (w->idx > 0 && w->idx >= __atomic_load_n(&sh->allowed, __ATOMIC_RELAXED)) {   // adaptive: this worker is parked
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 1;
            pthread_cond_timedwait(&sh->cv, &sh->mu, &dl);
            continue;
        }
        size_t pick = sh->qn - 1;
        w->loc.in_novelty_path = 0;
        int session_first = 0;
        for (size_t i = sh->qn; i-- > 0;)   // the few long sessions (attract / play) start before the fork backlog
            if (sh->q[i].input >= SESSION_ATTRACT) { pick = i; session_first = 1; break; }
        if (!session_first && !cfg->no_novelty_order) {
            double best = -1;
            for (size_t i = sh->qn; i-- > 0;) {
                if (sh->q[i].origin != DP_SRC_SESSION && covered(&w->loc, w->m, sh->q[i].pc)) continue;   // B3: a live checkpoint resumes from covered code by design; its cell's visit count orders it
                double sc = 0;   // AFL-like: rare, productive parent sites first; latest wins ties
                uint32_t st = sh->q[i].site;
                if (!cfg->no_site_score && st && st <= sh->out->rom_size)
                    sc = (1.0 + sh->site_gain[st - 1]) / (1.0 + sh->site_picks[st - 1]);
                if (sc > best) { best = sc; pick = i; }
                if (cfg->no_site_score) break;
            }
            if (best > 0 && pick != sh->qn - 1) w->loc.site_scored_picks++;
            if (pick != sh->qn - 1) { w->loc.novelty_picks++; w->loc.in_novelty_path = 1; }
        }
        Pending p = sh->q[pick];
        sh->q[pick] = sh->q[sh->qn - 1];
        sh->qn--;
        sh->active++;
        pthread_mutex_unlock(&sh->mu);
        uint64_t before = w->loc.novelty_new_bytes + w->loc.plain_new_bytes;
        run_path(w, &p);
        run_flush(&w->cobs);
        __atomic_fetch_add(&sh->paths_done, 1, __ATOMIC_RELAXED);
        if (p.site && p.site <= sh->out->rom_size) {
            __atomic_fetch_add(&sh->site_picks[p.site - 1], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&sh->site_gain[p.site - 1], (uint32_t)(w->loc.novelty_new_bytes + w->loc.plain_new_bytes - before), __ATOMIC_RELAXED);
        }
        pthread_mutex_lock(&sh->mu);
        sh->active--;
        pthread_cond_broadcast(&sh->cv);
    }
    pthread_cond_broadcast(&sh->cv);
    pthread_mutex_unlock(&sh->mu);
    __atomic_fetch_add(&sh->steps, w->step_batch, __ATOMIC_RELAXED); w->step_batch = 0;
    return NULL;
}

static void (*g_live_cb)(void *, const struct DpCoreResult *);
static void *g_live_user;
void dp_core_live_default(void (*cb)(void *, const struct DpCoreResult *), void *user) { g_live_cb = cb; g_live_user = user; }

int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg_in, DpCoreResult *out) {
    DpCoreConfig cfg_unb = *cfg_in;
    if (cfg_unb.unbounded) {
        cfg_unb.max_total = 0; cfg_unb.max_path_steps = 0; cfg_unb.fork_site_cap = 0;
        cfg_unb.visits_per_pc = 0; cfg_unb.max_secs = cfg_unb.stall_secs = 0;
        cfg_unb.no_cells = 1; cfg_unb.cell_lo = cfg_unb.cell_hi = 0;
        cfg_unb.no_taint_cut = cfg_unb.no_forced_ram_cut = 1;
    }
    const DpCoreConfig *cfg = &cfg_unb;
    memset(out, 0, sizeof *out);
    out->rom_size = rom_size;
    if (!m || !m->isa || !m->run || !m->save || !m->load || !m->fetch || !m->rom_index) return 0;
    if (!m->pc || !m->set_pc) { out->no_pc = 1; return 1; }
    out->flags = calloc(rom_size ? rom_size : 1, 1);
    out->dest = calloc(rom_size ? rom_size : 1, 1);
    out->spec = calloc(rom_size ? rom_size : 1, 1);
    out->refs = calloc(rom_size ? rom_size : 1, 1);
    out->live_w = ((rsize(m) >> 8) + 31) / 32;
    out->live = calloc(((rom_size >> 8) + 1) * out->live_w, sizeof *out->live);
    out->page_win = calloc((rom_size >> 8) + 2, 1);   // one byte per 256 bytes: enough for any page_shift >= 8
    out->src_flags = calloc(rom_size ? rom_size : 1, sizeof *out->src_flags);
    out->first_seen = calloc(rom_size ? rom_size : 1, sizeof *out->first_seen);
    if (!out->flags || !out->dest || !out->src_flags || !out->first_seen) { dp_core_result_free(out); return 0; }
    if (cfg->on_live) cfg->on_live(cfg->on_live_user, out);
    else if (g_live_cb) g_live_cb(g_live_user, out);

    Shared sh;
    memset(&sh, 0, sizeof sh);
    pthread_mutex_init(&sh.mu, NULL);
    pthread_cond_init(&sh.cv, NULL);
    sh.out = out; sh.cfg = cfg;
    sh.sbytes = m->state_bytes(m);
    sh.spool_fd = -2;   // not opened yet (-1 means the open failed)
    uint32_t ramn = rsize(m);
    uint32_t site_n = 0x10000u; while (site_n < rom_size && site_n < (1u << SITE_BITS)) site_n <<= 1;   // A3
    sh.fed = calloc(ramn, 1);
    sh.fedsrc = calloc(ramn, sizeof(uint32_t));
    sh.fedprod = calloc(ramn, sizeof(uint32_t));
    sh.ramread = calloc(ramn, 1);
    sh.probectx = calloc(rom_size ? rom_size : 1, 1);
    sh.site_picks = calloc(rom_size + 1, sizeof *sh.site_picks);
    sh.site_gain = calloc(rom_size + 1, sizeof *sh.site_gain);

    unsigned nw = cfg->jobs > 1 ? cfg->jobs : 1;
    Worker *ws = calloc(nw, sizeof *ws);
    int ok = sh.fed && sh.ramread && ws;
    unsigned started = 0;
    for (unsigned i = 0; ok && i < nw; i++) {
        Worker *w = &ws[i];
        DpMachine *wm = i == 0 ? m : (m->clone ? m->clone(m, i) : NULL);
        if (!wm) break;                       // fewer workers than asked: reported
        w->sh = &sh; w->m = wm; w->idx = i;
        w->loc.refs = out->refs; w->loc.page_win = out->page_win; w->loc.live = out->live; w->loc.live_w = out->live_w; w->loc.flags = out->flags; w->loc.src_flags = out->src_flags;
        w->loc.dest = out->dest;
        w->loc.first_seen = out->first_seen; w->loc.rom_size = rom_size;
        w->site_n = site_n;
        w->visits = calloc(site_n, sizeof *w->visits);
        w->capped = calloc(site_n, sizeof *w->capped);
        w->stamp = calloc(site_n, sizeof *w->stamp);
        w->wsnap = malloc(sh.sbytes ? sh.sbytes : 1); w->winmemo = calloc(0x10000, sizeof *w->winmemo);
        w->rng_stamp = calloc(site_n, sizeof *w->rng_stamp); w->rng_last = calloc(site_n, 1);
        if (!w->visits || !w->stamp || !w->capped) { ok = 0; break; }
        w->cobs = (CopyObs){ .m = wm, .r = &w->loc, .last_rom = -1, .fed = sh.fed, .age = 99, .run_lo = -1, .run_hi = -1, .no_decompress = cfg->no_decompress };
        w->cobs.fedsrc = sh.fedsrc; w->cobs.fedprod = sh.fedprod;
        w->cobs.ramread = sh.ramread;
        started++;
    }
    out->workers = started;
    sh.wmin = 1; sh.wmax = started; sh.allowed = started;
    if (cfg->adaptive && started > 1) {
        long nc = sysconf(_SC_NPROCESSORS_ONLN);
        sh.allowed = nc > 0 && (unsigned)nc < started ? (unsigned)nc : started;
    }
    out->workers_min = 1; out->workers_max = started;
    if (started < nw && ok) out->workers_short = nw - started;
    ok = ok && started > 0;

    if (ok) {
        // every worker observes its own machine; the start state is the main one's
        uint8_t *start = malloc(sh.sbytes);
        m->save(m, start);
        uint32_t start_pc = m->pc(m);
        sh.q = NULL;
    if (cfg->state_dir && cfg->resume) {   // the coverage proved by previous runs is OR-ed back: a resumed report never knows less
        char cp[600]; snprintf(cp, sizeof cp, "%s/coverage.bin", cfg->state_dir);
        FILE *cf = fopen(cp, "rb");
        if (cf) {
            uint32_t hd[2];
            if (fread(hd, 4, 2, cf) == 2 && hd[0] == 0x31564F43u && hd[1] == rom_size) {
                uint8_t *buf = malloc(rom_size ? rom_size : 1);
                if (buf && fread(buf, 1, rom_size, cf) == rom_size) {
                    size_t n = 0;
                    for (size_t i = 0; i < rom_size; i++) { if (buf[i] & ~out->flags[i]) n++; out->flags[i] |= buf[i]; }
                    fprintf(stderr, "deepeel: resumed coverage map (%zu bytes added)\n", n);
                    // Older coverage files end after the byte map.  A tagged,
                    // complete extension preserves method attribution on resume.
                    uint32_t ext[2];
                    size_t ext_bytes = fread(ext, 1, sizeof ext, cf);
                    if (ext_bytes == sizeof ext && ext[0] == 0x31524353u && ext[1] == rom_size) {
                        uint32_t *src = malloc((rom_size ? rom_size : 1) * sizeof *src);
                        if (src && fread(src, sizeof *src, rom_size, cf) == rom_size) {
                            for (size_t i = 0; i < rom_size; i++) out->src_flags[i] |= src[i];
                            fprintf(stderr, "deepeel: resumed coverage sources\n");
                        } else fprintf(stderr, "deepeel: incomplete coverage sources; attribution not restored\n");
                        free(src);
                    } else if (ext_bytes || ferror(cf)) fprintf(stderr, "deepeel: invalid coverage source extension; attribution not restored\n");
                }
                free(buf);
            }
            fclose(cf);
        }
    }
    if (cfg->state_dir && cfg->resume) {   // cells already archived by previous runs are not archived again
        char kp[600]; snprintf(kp, sizeof kp, "%s/cells.bin", cfg->state_dir);
        FILE *kf = fopen(kp, "rb");
        if (kf) { uint64_t k; while (fread(&k, sizeof k, 1, kf) == 1) cell_insert(&sh, k); fclose(kf); }
    }
    if (cfg->state_dir && cfg->resume) {   // discovery order: the earliest step at which each ROM byte was ever covered
        char dp[600]; snprintf(dp, sizeof dp, "%s/disc.bin", cfg->state_dir);
        FILE *df = fopen(dp, "rb");
        if (df) {
            uint32_t hd[2];
            if (fread(hd, 4, 2, df) == 2 && hd[0] == 0x31435344u && hd[1] == rom_size) {
                uint64_t *buf = malloc((rom_size ? rom_size : 1) * sizeof *buf);
                if (buf && fread(buf, sizeof *buf, rom_size, df) == rom_size) {
                    size_t n = 0;
                    for (size_t i = 0; i < rom_size; i++) if (buf[i] && (!out->first_seen[i] || buf[i] < out->first_seen[i])) { out->first_seen[i] = buf[i]; n++; }
                    fprintf(stderr, "deepeel: resumed discovery log (%zu entries)\n", n);
                }
                free(buf);
            }
            fclose(df);
        }
    }
    if (cfg->state_dir && cfg->resume) {   // states left pending by a previous run (same machine state size)
        char fp[600]; snprintf(fp, sizeof fp, "%s/frontier.bin", cfg->state_dir);
        if (!frontier_load(&sh, fp, out)) { free(start); ok = 0; goto cleanup; }
        char ep[600]; snprintf(ep, sizeof ep, "%s/exact.bin", cfg->state_dir);
        FILE *ef = fopen(ep, "rb");
        if (ef) {   // B5: the exact states of the previous run
            uint64_t hd[2];
            if (fread(hd, sizeof *hd, 2, ef) == 2 && hd[0] == 0x3143455845504445ull)
                for (uint64_t i = 0, k; i < hd[1] && fread(&k, sizeof k, 1, ef) == 1; i++) keyset_insert(&sh.exactset, &sh.exactcap, &sh.exactn, k);
            fclose(ef);
        }
    }
        for (size_t i = 0; i < cfg->n_seeds; i++) {
            uint8_t *cp = malloc(sh.sbytes);
            if (!cp) break;
            memcpy(cp, start, sh.sbytes);
            const DpSeedRegs *sr = cfg->seed_regs ? &cfg->seed_regs[i] : NULL;
            int prep = 0;
            if (cfg->seed_banks && cfg->seed_banks[i] != 0xFFFF) {   // bank 0 included: the reset view is not assumed; 0xFFFF: the current mapping
                if (m->bank_view) {
                    if (!prep) m->load(m, start);
                    if (m->bank_view(m, cfg->seed_banks[i])) prep = 1;
                    else out->seed_banks_absent++;     // this board cannot show the block
                } else out->seed_banks_absent++;       // no mapper control on this machine
            }
            if (sr && (sr->a != 0xFF || sr->sp != 0xFFFF)) {
                if (m->seed_regs) {
                    if (!prep) m->load(m, start);
                    m->seed_regs(m, sr->a, sr->sp);
                    prep = 1;
                } else out->seed_regs_absent++;   // requested but unsupported: never silent
            }
            if (prep) { m->save(m, cp); m->load(m, start); }
            queue_push(&sh, (Pending){ cp, cfg->seeds[i], 1, 0, 0, 1, 0, prep ? DP_SRC_SEEDRAM : 0 });
            out->seeded++;
        }
        // the platform registry's RST/IRQ vector entries as forced starts (the
        // legacy engines' vector seeding, G1, mutualized): a vector cell's
        // handler address is read through the machine; an entry whose bytes are
        // pure padding is skipped, every seed is counted
        if (m->plat && !cfg->no_vectors) {
            for (int i = 0; i < m->plat->entry_count; i++) {
                const DpEntry *e = &m->plat->entries[i];
                if (e->kind == DP_SEED_RESET) continue;   // the reset flow is the natural path
                uint32_t vpc = e->addr;
                if (e->cell) vpc = (uint32_t)(m->fetch(m, e->addr) | (m->fetch(m, (uint32_t)e->addr + 1) << 8));
                int pad = 1;
                for (unsigned k = 0; k < 8 && pad; k++) { uint8_t b = m->fetch(m, vpc + k); pad = b == 0x00 || b == 0xFF; }
                if (pad) continue;   // the legacy rule: a padding vector holds no handler
                uint8_t *cp = malloc(sh.sbytes);
                if (!cp) break;
                memcpy(cp, start, sh.sbytes);
                if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
                sh.q[sh.qn++] = (Pending){ cp, vpc, 1, 0, 0, 1, 1 };
                out->vector_seeds++;
            }
        }
        // the natural path goes last: it is the first one taken
        if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
        sh.q[sh.qn++] = (Pending){ start, start_pc, 0, 0, 0 };
        // technique 3: start states whose RAM range already holds a plausible valid save
        uint32_t sd_lo = cfg->seed_lo, sd_hi = cfg->seed_hi; unsigned sd_n = cfg->seed_n; uint8_t sd_v[8];
        memcpy(sd_v, cfg->seed_vals, sizeof sd_v);
        if (!sd_n && !cfg->no_auto_seed && m->save_ram_len) {   // B10: the cart-RAM window with the default patterns
            static const uint8_t d[4] = {0x00, 0xFF, 0x55, 0xAA};
            sd_lo = m->save_ram_base; sd_hi = m->save_ram_base + m->save_ram_len - 1; sd_n = 4; memcpy(sd_v, d, 4);
        }
        if (m->poke && sd_n && sd_hi >= sd_lo) {
            for (unsigned v = 0; v < sd_n; v++) {
                m->load(m, start);
                if (!cfg->seed_n && m->save_ram_en) m->poke(m, m->save_ram_en_addr, m->save_ram_en_val);   // the machine's cart-RAM enable
                for (uint32_t a = sd_lo; a <= sd_hi; a++) m->poke(m, a, sd_v[v]);
                uint8_t *cp = malloc(sh.sbytes);
                if (!cp) break;
                m->save(m, cp);
                if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
                sh.q[sh.qn++] = (Pending){ cp, start_pc, 0, 0, 0 };
                out->seeded_ram++;
            }
            m->load(m, start);
        }
        // held-pad sessions: copies of the power-on state, one per pad mask; they run
        // after the natural path (queue is LIFO-picked from the back) and fork like any path
        if (m->set_input && !cfg->no_sessions) {
            static const uint32_t masks[16] = {0x10, 0x20, 0x01, 0x02, 0x04, 0x08, 0x30, 0x11, 0x21, 0x14,
                                               0x18, 0x24, 0x28, 0x3F, 0x40, 0x15};   // 0x40: Start
            unsigned n = cfg->sessions ? cfg->sessions : 16;
            if (n > 16) n = 16;
            for (unsigned i = 0; i < n; i++) {
                uint8_t *cp = malloc(sh.sbytes);
                if (!cp) break;
                memcpy(cp, start, sh.sbytes);
                if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
                sh.q[sh.qn++] = (Pending){ cp, start_pc, 0, 0, masks[i] + 1 };
                out->sessions++;
            }
        }
        // generic long sessions from power-on (need a frame counter): attract (no input) and play (pad epochs)
        if (m->frame) {
            unsigned na = cfg->attract_frames ? 1 : 0, np = (m->set_input && cfg->play_frames) ? (cfg->play_sessions ? cfg->play_sessions : 1) : 0;
            if (np > 8) np = 8;
            for (unsigned i = 0; i < na + np; i++) {
                uint8_t *cp = malloc(sh.sbytes);
                if (!cp) break;
                memcpy(cp, start, sh.sbytes);
                if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
                sh.q[sh.qn++] = (Pending){ cp, start_pc, 0, 0, i < na ? SESSION_ATTRACT : SESSION_PLAY + (i - na), 0, 0, DP_SRC_SESSION };
            }
        }
        // technique 9 (TAS form): the movie from power-on, unshifted and shifted by 1..3 frames
        if (m->frame && m->set_input && (cfg->tas_masks || cfg->tas_masks32) && cfg->tas_frames) {
            static const int order[6] = {-3, -2, -1, 1, 2, 3};
            for (int i = 6; i < 7; i++) {
                int sh_ = i < 6 ? order[i] : 0;
                uint8_t *cp = malloc(sh.sbytes);
                if (!cp) break;
                memcpy(cp, start, sh.sbytes);
                if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
                sh.q[sh.qn++] = (Pending){ cp, start_pc, 0, 0, TAS_INPUT + 3 + sh_ };
            }
        }
        if (m->frame && m->set_input && (cfg->tas_masks || cfg->tas_masks32) && cfg->tas_frames && !cfg->no_tas_jitter) {
            sh.tas_start = malloc(sh.sbytes);
            if (sh.tas_start) { memcpy(sh.tas_start, sh.q[sh.qn - 1].state, sh.sbytes); sh.tas_pc = start_pc; sh.tas_shifts_left = 1; }
        }
        // clones start from power-on, the same state the main machine saved
        for (unsigned i = 1; i < started; i++) pthread_create(&ws[i].th, NULL, worker_main, &ws[i]);
        pthread_t mon, ctl, lim; int has_mon = 0, has_ctl = 0, has_lim = 0;
        if ((cfg->max_secs || cfg->stall_secs) && pthread_create(&lim, NULL, limits_main, &sh) == 0) has_lim = 1;
        if (cfg->adaptive && started > 1 && pthread_create(&ctl, NULL, controller_main, &sh) == 0) has_ctl = 1;
        if (cfg->progress_secs && pthread_create(&mon, NULL, monitor_main, &sh) == 0) has_mon = 1;
        worker_main(&ws[0]);
        for (unsigned i = 1; i < started; i++) pthread_join(ws[i].th, NULL);
        __atomic_store_n(&sh.stop_monitor, 1, __ATOMIC_RELAXED);
        if (has_mon) pthread_join(mon, NULL);
        if (has_ctl) pthread_join(ctl, NULL);
        if (has_lim) pthread_join(lim, NULL);
        out->halted = (unsigned)sh.halt;
    }

cleanup:
    free(sh.tas_start);
    if (cfg->hooks && cfg->hooks->finish && out->flags) cfg->hooks->finish(cfg->hooks->user, out);
    if (!cfg->no_infer && out->flags && cfg->rom_image && m->isa) infer_static(out, m, cfg);
    if (!cfg->no_spec && m->bank_shift == 14 && m->isa && out->flags) speculative_gaps(out, m->isa, cfg, m, 0x4000u);
    else if (!cfg->no_spec && m->page_shift && m->isa && out->flags) speculative_gaps_pages(out, m->isa, cfg, m);
    else if (!cfg->no_spec && m->linear && m->isa && out->flags && cfg->rom_image && cfg->rom_image_size) {   // B8 on a linear ROM: one window covering the image
        unsigned lw = 1; while (lw < cfg->rom_image_size && lw < (1u << 30)) lw <<= 1;
        speculative_gaps(out, m->isa, cfg, m, lw);
    }
    for (unsigned i = 0; i < started; i++) {
        DpCoreResult *l = &ws[i].loc;
        out->paths += l->paths; out->forks += l->forks;
        out->cooperative_yields += l->cooperative_yields;
        out->sat_total += l->sat_total; out->sat_path += l->sat_path; out->sat_loop += l->sat_loop;
        out->attract_paths += l->attract_paths; out->play_paths += l->play_paths; out->play_epochs += l->play_epochs; out->session_frames += l->session_frames; out->session_steps += l->session_steps; out->session_stalls += l->session_stalls;
        out->sat_site += l->sat_site; out->sat_queue += l->sat_queue;
        out->pruned_covered += l->pruned_covered; out->copy_bytes += l->copy_bytes;
        out->copy_forced += l->copy_forced; out->forced_ram_cuts += l->forced_ram_cuts;
        out->taint_cut_indirect += l->taint_cut_indirect; out->taint_cut_ret += l->taint_cut_ret;
        out->novelty_picks += l->novelty_picks; out->closure_stopped_data += l->closure_stopped_data; out->closure_stack_dropped += l->closure_stack_dropped; out->site_scored_picks += l->site_scored_picks; out->learned_sigs += l->learned_sigs; out->solved_forks += l->solved_forks; out->promoted_forced += l->promoted_forced; out->solve_tries += l->solve_tries; out->rng_forks += l->rng_forks; out->index_replays += l->index_replays; out->compressed_bytes += l->compressed_bytes; out->decompress_runs += l->decompress_runs; out->cells += l->cells; out->closure_roots += l->closure_roots; out->closure_bytes += l->closure_bytes; out->closure_capped += l->closure_capped; out->closure_stack_dropped += l->closure_stack_dropped; out->pending_dropped += l->pending_dropped; out->deferred += l->deferred; out->state_repeats += l->state_repeats; out->probe_ctx_sat += l->probe_ctx_sat; out->probe_rearms += l->probe_rearms; out->bank_forks += l->bank_forks; out->tas_ckpts += l->tas_ckpts; out->sess_ckpts += l->sess_ckpts; out->ckpt_bursts += l->ckpt_bursts; out->live_seeds += l->live_seeds; out->imm_refs += l->imm_refs; out->held_stops += l->held_stops; out->bank_fork_sat += l->bank_fork_sat; out->closure_jp_tables += l->closure_jp_tables; out->closure_jp_entries += l->closure_jp_entries; out->prov_data_bytes += l->prov_data_bytes; out->event_probes += l->event_probes; out->event_probe_sites += l->event_probe_sites; out->irq_forks += l->irq_forks; out->jitter_forks += l->jitter_forks; out->cell_pruned += l->cell_pruned; out->novelty_new_bytes += l->novelty_new_bytes; out->plain_new_bytes += l->plain_new_bytes; out->hook_vetoed += l->hook_vetoed; out->tas_paths += l->tas_paths;
        out->exec_outside += l->exec_outside; out->exec_wram_rom += l->exec_wram_rom; dp_ramarch_merge(&out->ra, &l->ra);
        free(l->jp_seen); l->jp_seen = NULL; free(ws[i].visits); free(ws[i].wsnap); free(ws[i].winmemo); free(ws[i].capped); free(ws[i].stamp);
        if (i && ws[i].m && ws[i].m->close) ws[i].m->close(ws[i].m);
    }
    out->steps = sh.steps;
    for (unsigned i = 0; i < started; i++) out->steps += 0;   /* session steps are reported separately */
    out->spool_left = sh.spool_n; out->spooled = sh.spool_total;
    // Do not publish any new state after detecting an unreadable spool: even
    // later reads could return holes after a subsequent append extended it.
    if (sh.spool_read_failed || sh.checkpoint_failed) ok = 0;
    if (ok && cfg->state_dir && sh.celln) {   // archived (target, RAM) cells for --resume
        char kp[600]; snprintf(kp, sizeof kp, "%s/cells.bin", cfg->state_dir);
        mkdir(cfg->state_dir, 0777);
        FILE *kf = fopen(kp, "wb");
        if (kf) { for (size_t i = 0; i < sh.cellcap; i++) if (sh.cellset[i]) fwrite(&sh.cellset[i], sizeof sh.cellset[i], 1, kf); fclose(kf); }
    }
    if (ok && cfg->state_dir) {   // discovery log for --resume
        char dp[600]; snprintf(dp, sizeof dp, "%s/disc.bin", cfg->state_dir);
        mkdir(cfg->state_dir, 0777);
        FILE *df = fopen(dp, "wb");
        if (df) { uint32_t hd[2] = { 0x31435344u, (uint32_t)rom_size }; fwrite(hd, 4, 2, df); fwrite(out->first_seen, sizeof *out->first_seen, rom_size, df); fclose(df); }
    }
    if (ok && cfg->state_dir) {   // coverage map for --resume
        char cp[600]; snprintf(cp, sizeof cp, "%s/coverage.bin", cfg->state_dir);
        mkdir(cfg->state_dir, 0777);
        FILE *cf = fopen(cp, "wb");
        if (cf) {
            uint32_t hd[2] = { 0x31564F43u, (uint32_t)rom_size };
            uint32_t ext[2] = { 0x31524353u, (uint32_t)rom_size };
            fwrite(hd, 4, 2, cf); fwrite(out->flags, 1, rom_size, cf);
            fwrite(ext, sizeof *ext, 2, cf); fwrite(out->src_flags, sizeof *out->src_flags, rom_size, cf);
            fclose(cf);
        }
    }
    out->exact_states = sh.exactn;
    if (ok && cfg->state_dir && sh.exactn) {   // B5: exact.bin, the states reached, reloaded by --resume
        char ep[600]; snprintf(ep, sizeof ep, "%s/exact.bin", cfg->state_dir);
        FILE *ef = fopen(ep, "wb");
        if (ef) {
            uint64_t hd[2] = { 0x3143455845504445ull, sh.exactn };
            fwrite(hd, sizeof *hd, 2, ef);
            for (size_t i = 0; i < sh.exactcap; i++) if (sh.exactset[i]) fwrite(&sh.exactset[i], sizeof *sh.exactset, 1, ef);
            fclose(ef);
        }
    }
    if (ok && cfg->state_dir) {   // the pending frontier (RAM queue, then the disk spool) for --resume
        char fp[600]; snprintf(fp, sizeof fp, "%s/frontier.bin", cfg->state_dir);
        if (!frontier_save(&sh, fp, out)) ok = 0;
    }
    if (sh.spool_fd >= 0) close(sh.spool_fd);
    for (size_t i = 0; i < sh.qn; i++) free(sh.q[i].state);
    for (size_t i = 0; i < sh.defn; i++) free(sh.def[i].state);
    free(sh.def);
    free(sh.probectx); free(sh.exactset); free(sh.probeset); free(sh.cellset); free(sh.q); free(sh.sites.s); free(sh.fed); free(sh.fedsrc); free(sh.fedprod); free(sh.ramread); free(sh.site_picks); free(sh.site_gain); free(ws);
    return ok;
}

static volatile sig_atomic_t core_cli_interrupted;
static void core_cli_sigint(int sig) { (void)sig; core_cli_interrupted = 1; }

int dp_core_explore_cli(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg,
                        DpCoreResult *out, const char *output_hint) {
    DpCoreConfig local = *cfg;
    char state_path[1024];
    memset(out, 0, sizeof *out);
    if (!local.state_dir && (local.unbounded || local.resume)) {
        const char *base = output_hint && *output_hint ? output_hint : "deepeel-unbounded";
        if (snprintf(state_path, sizeof state_path, "%s.state", base) >= (int)sizeof state_path) {
            fprintf(stderr, "deepeel: kernel state directory path is too long\n"); return 0;
        }
        local.state_dir = state_path;
    }
    if (local.state_dir && local.unbounded) {
        if (mkdir(local.state_dir, 0777) != 0 && errno != EEXIST) {
            perror("deepeel: cannot create kernel state directory"); return 0;
        }
        struct stat st;
        if (stat(local.state_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "deepeel: kernel state path is not a directory: %s\n", local.state_dir); return 0;
        }
    }
    struct sigaction act = {0}, old;
    act.sa_handler = core_cli_sigint;
    sigemptyset(&act.sa_mask);
    core_cli_interrupted = 0;
    if (sigaction(SIGINT, &act, &old) != 0) { perror("deepeel: cannot install SIGINT handler"); return 0; }
    local.stop_requested = &core_cli_interrupted;
    int ok = dp_core_explore(m, rom_size, &local, out);
    if (sigaction(SIGINT, &old, NULL) != 0) { perror("deepeel: cannot restore SIGINT handler"); ok = 0; }
    return ok;
}

void dp_core_report(FILE *out, const char *label, const DpCoreResult *r) {
    size_t nat = 0, forced = 0, copied = 0;
    for (size_t i = 0; i < r->rom_size; i++) {
        if (r->flags[i] & DP_CORE_NATURAL) nat++;
        if (r->flags[i] & DP_CORE_FORCED) forced++;
        if (r->flags[i] & DP_CORE_COPIED) copied++;
    }
    #define U(x) ((unsigned long long)(x))
    fprintf(out, "deepeel %s (generic kernel)\n", label);
    fprintf(out, "steps %llu paths %llu forks %llu pruned-covered %llu\n", U(r->steps), U(r->paths), U(r->forks), U(r->pruned_covered));
    if (r->halted) fprintf(out, "halted: %s\n", r->halted == 1 ? "--max-time reached" : r->halted == 2 ? "--stall-time: no new ROM byte" : r->halted == 4 ? "interrupted" : "spool error");
    if (r->attract_paths || r->play_paths) fprintf(out, "sessions: %llu attract, %llu play (%llu pad epochs), %llu frames played, %llu steps, %llu stopped by stagnation\n", U(r->attract_paths), U(r->play_paths), U(r->play_epochs), U(r->session_frames), U(r->session_steps), U(r->session_stalls));
    if (r->resumed || r->saved) fprintf(out, "frontier: %llu states resumed, %llu saved\n", U(r->resumed), U(r->saved));
    fprintf(out, "spool: %llu states spilled to disk, %llu left unexplored\n", U(r->spooled), U(r->spool_left));
    if (r->cooperative_yields) fprintf(out, "cooperative yields: %llu\n", U(r->cooperative_yields));
    fprintf(out, "saturation: total %llu path %llu loop %llu site %llu queue %llu\n", U(r->sat_total), U(r->sat_path), U(r->sat_loop), U(r->sat_site), U(r->sat_queue));
    fprintf(out, "losses: closure_stack_dropped %llu pending_dropped %llu deferred %llu\nexact state: repeats cut %llu, states %llu\n", U(r->closure_stack_dropped), U(r->pending_dropped), U(r->deferred), U(r->state_repeats), U(r->exact_states));
    fprintf(out, "static walks stopped or retracted on data %llu (A13)\n", U(r->closure_stopped_data));
    fprintf(out, "index replays %llu\n", U(r->index_replays));
    fprintf(out, "rng-byte forks %llu\n", U(r->rng_forks));
    fprintf(out, "learned signatures %llu\n", U(r->learned_sigs));
    fprintf(out, "solved forks %llu (tries %llu, promoted from forced %llu)\n", U(r->solved_forks), U(r->solve_tries), U(r->promoted_forced));
    fprintf(out, "tas replay paths %llu\n", U(r->tas_paths));
    fprintf(out, "site-scored picks %llu\n", U(r->site_scored_picks));
    fprintf(out, "novelty picks %llu (new ROM bytes: novelty %llu, plain %llu)\n", U(r->novelty_picks), U(r->novelty_new_bytes), U(r->plain_new_bytes));
    fprintf(out, "workers %llu (short %llu)%s\n", U(r->workers), U(r->workers_short), r->workers_max ? "" : "");
    fprintf(out, "cells %llu, cell-pruned forks %llu\n", U(r->cells), U(r->cell_pruned));
    fprintf(out, "deepeel: %llu (target, RAM) cells archived into the checkpoint pool\n", U(r->cells));
    fprintf(out, "decompress runs %llu, compressed ROM bytes %llu\n", U(r->decompress_runs), U(r->compressed_bytes));
    fprintf(out, "seeded RAM states %llu\n", U(r->seeded_ram));
    fprintf(out, "held-pad sessions %llu\n", U(r->sessions));
    fprintf(out, "IRQ forks %llu (timing jitter %llu)\n", U(r->irq_forks), U(r->jitter_forks));
    fprintf(out, "movie checkpoints continued as free paths: %llu (vector seeds from live states: %llu); session checkpoints continued as free paths: %llu, pad bursts from them: %llu\n", U(r->tas_ckpts), U(r->live_seeds), U(r->sess_ckpts), U(r->ckpt_bursts));
    fprintf(out, "held-pad session paths stopped by their own budget: %llu\n", U(r->held_stops));
    fprintf(out, "bank forks: %llu (site cap hit %llu); probe re-arms: %llu (context cap hit %llu)\n", U(r->bank_forks), U(r->bank_fork_sat), U(r->probe_rearms), U(r->probe_ctx_sat));
    fprintf(out, "event probes %llu at %llu sites: %llu forced variants\n", U(r->event_probes), U(r->event_probe_sites), U(r->event_probes));
    fprintf(out, "seeded entry points %llu\n", U(r->seeded));
    if (r->vector_seeds) fprintf(out, "vector seeds (RST/IRQ registry entries) %llu\n", U(r->vector_seeds));
    fprintf(out, "pointer seeds refused by cap %llu\n", U(r->seed_refused_cap));
    if (r->seed_regs_absent) fprintf(out, "register seeds unsupported by the machine: %llu (not seeded)\n", U(r->seed_regs_absent));
    if (r->seed_banks_absent) fprintf(out, "bank seeds the machine cannot show: %llu (not seeded)\n", U(r->seed_banks_absent));
    fprintf(out, "forced RAM cuts %llu\n", U(r->forced_ram_cuts));
    if (r->nonsense_pc || r->nonsense_sp || r->pad_cuts)
        fprintf(out, "nonsense cuts: echo/I-O pc %llu, stack outside RAM %llu, padding slides %llu\n",
                U(r->nonsense_pc), U(r->nonsense_sp), U(r->pad_cuts));
    fprintf(out, "forced taint cuts: indirect %llu, ret %llu\n", U(r->taint_cut_indirect), U(r->taint_cut_ret));
    if (r->exec_wram_rom) fprintf(out, "RAM-executed instructions traced to ROM copy sources: %llu\n", U(r->exec_wram_rom));
    if (r->ra.n) fprintf(out, "runtime code archive: %u distinct instructions executed from RAM/VRAM/HRAM, %u attached to a ROM copy source, %u generated (no ROM source; %u with an approximate ROM producer), %u rewrote an earlier instruction at the same address (self-modifying)\n", r->ra.n, r->ra.attached, r->ra.n - r->ra.attached, r->ra.derived, r->ra.poly);
    fprintf(out, "exec_outside %llu, rom->ram copy bytes %llu (%zu distinct ROM sources), forced %llu\n", U(r->exec_outside), U(r->copy_bytes), copied, U(r->copy_forced));
    DpCoreSection secs[64];
    size_t ns = dp_core_sections(r, secs, 64);
    fprintf(out, "sections %zu\n", ns);
    for (size_t i = 0; i < ns && i < 64; i++)
        fprintf(out, "  section %zu rom-index %zu-%zu natural %zu forced %zu verdict %s live-ram-pages %zu\n", i + 1, secs[i].lo, secs[i].hi, secs[i].natural, secs[i].forced, secs[i].required ? "required" : "speculative", secs[i].live_pages);
    size_t stat_only = 0;
    for (size_t i = 0; i < r->rom_size; i++) if ((r->flags[i] & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED)) == DP_CORE_STATIC) stat_only++;
    fprintf(out, "speculative gaps: %llu runs, %llu bytes (never counted as known); %llu runs reinforced by an immediate reference (%llu references seen)\n", U(r->spec_runs), U(r->spec_bytes), U(r->spec_ref_runs), U(r->imm_refs));
    fprintf(out, "static closure: roots %llu, bytes %llu, static-only %zu, capped walks %llu\n", U(r->closure_roots), U(r->closure_bytes), stat_only, U(r->closure_capped));
    if (r->inferred) fprintf(out, "static inference: padding %llu, gap code %llu, gap pad %llu, gap data %llu, copy tables %llu (%llu sibling bytes, %llu refused as code), jump tables %llu (%llu bytes)\n", U(r->padding_bytes), U(r->inferred_code), U(r->inferred_pad), U(r->inferred_data), U(r->sib_tables), U(r->sib_bytes), U(r->sib_refused), U(r->jt_tables), U(r->jt_bytes));
    if (r->closure_jp_entries) fprintf(out, "provenance: %llu jp-hl targets, %llu data bytes static\n", U(r->closure_jp_entries), U(r->prov_data_bytes));
    if (r->closure_jp_tables) fprintf(out, "S4 operand tables of executed JP HL: %llu tables, %llu entries\n", U(r->closure_jp_tables), U(r->closure_jp_entries));
    fprintf(out, "rom bytes: natural %zu, forced %zu (speculative), static-only %zu\n", nat, forced, stat_only);
    #undef U
}

// --cov FILE: coverage in the old engine's line format (bank offset flags hits; flags 1 = natural, 5 = forced)
void dp_core_write_cov(const char *path, const char *id, const DpCoreResult *r) {
    if (!path || !path[0]) return;
    FILE *cf = fopen(path, "w");
    if (!cf) return;
    fprintf(cf, "# deepeel %s coverage v1 (generic kernel): bank offset flags(1=exec,4=speculative,8=data read by a natural path) hits\n", id);
    for (size_t i = 0; i < r->rom_size; i++) {
        uint8_t f = r->flags[i]; if (!f) continue;
        unsigned out = (f & DP_CORE_NATURAL) ? 1 : (f & DP_CORE_FORCED) ? 5 : 0;
        if ((f & DP_CORE_DATA) && !((f & DP_CORE_COPIED) && (f & DP_CORE_NATURAL))) out |= 8;   // a copied byte later executed is code, not plain data
        if (!out) continue;
        fprintf(cf, "%02x %04x %x 1\n", (unsigned)(i >> 14), (unsigned)(i & 0x3fff) + ((i >> 14) && !strcmp(id, "gb") ? 0x4000u : 0u), out);
    }
    fclose(cf);
}
