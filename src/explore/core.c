// core.c: generic exploration kernel over DpMachine + DpIsa (see core.h).
#include <sys/stat.h>
#include <unistd.h>
#include "core.h"
#include "../core/sha1.h"
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
    uint16_t epoch;    // B3: checkpoint epoch (0: none) the path descends from; the per-site fork cap restarts in each epoch
    uint32_t tri;      // P2/A19: target rom index + 1, resolved under the pushing mapping (0: unknown — pick falls back to covered())
    uint32_t call_depth; // calls already made since this forced path began
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

#define REG_SEED_SITE_CAP 4      // distinct regstate seeds per site by default (Sega closure: contexts/2 per (pc, view); --closure-seed-cap overrides)
#define REG_IND_CAP 8            // distinct resolutions followed at one indirect site
#define REG_SET_CAP (1u << 16)   // initial per-worker seed dedup; grows by rehashing up to REG_SET_CEIL (the Sega closure's node cap is 262144)
#define REG_SET_CEIL (1u << 20)  // hard ceiling of the dedup set: beyond it seeds are refused and counted (memory bound)

void dp_core_config_default(DpCoreConfig *c) {
    memset(c, 0, sizeof *c);
    c->max_total = 5000000;
    c->progress_secs = 5;
    c->snap_secs = 600;
    c->max_path_steps = 100000;
    c->fork_site_cap = 4;      // DP_DEFAULT_BRANCH_FORK_CAP
    c->visits_per_pc = 64;
    c->min_secs = 0; c->max_secs = 0; c->stall_secs = 0; c->attract_frames = 6000; c->attract_stall = 1200; c->play_frames = 0; c->play_sessions = 1;   // long sessions run wherever the machine has a frame counter (+ set_input for play)
    c->queue_max = 65536; c->queue_auto = 1;
    c->probe_ctx_cap = 8;
    c->bank_fork_cap = 16;
    c->held_steps = 300000;
    c->tas_ckpt_every = 600; c->tas_ckpt_cap = 32; c->ckpt_burst_frames = 60;
    c->closure_seed_cap = REG_SEED_SITE_CAP;
}

int dp_core_parse_option(DpCoreConfig *c, int argc, char **argv, int i) {
    const char *a = argv[i];
    if (!strcmp(a, "--cells") && i + 1 < argc) {   // LO-HI: RAM range hashed into a cell
        char *e; unsigned long lo = strtoul(argv[i + 1], &e, 0);
        if (*e == '-') { c->cell_lo = (uint32_t)lo; c->cell_hi = (uint32_t)strtoul(e + 1, NULL, 0); }
        return 2;
    }
    if (!strcmp(a, "--campaign")) { c->campaign = 1; return 1; }
    if (!strcmp(a, "--unbounded")) { c->unbounded = 1; c->max_total = 0; c->fork_site_cap = 0; c->closure_seed_cap = 0; return 1; }   // A8: max_path_steps stays — lifting the caps must not lift the platform's path shape (Zelda: 85 776 naturals bounded, 4 835 unbounded); --max-steps N reshapes it explicitly
    if (!strcmp(a, "--state-dir") && i + 1 < argc) { c->state_dir = argv[i + 1]; return 2; }
    if (!strcmp(a, "--resume")) { c->resume = 1; return 1; }
    if (!strcmp(a, "--cov-transfer") && i + 2 < argc) { c->cov_transfer_rom = argv[i + 1]; c->cov_transfer_cov = argv[i + 2]; return 3; }
    if (!strcmp(a, "--cov-hits") && i + 1 < argc) { c->cov_hits_path = argv[i + 1]; return 2; }
    if (!strcmp(a, "--queue-max") && i + 1 < argc) { c->queue_max = (size_t)strtoull(argv[i + 1], NULL, 0); c->queue_auto = 0; return 2; }
    if (!strcmp(a, "--no-closure")) { c->no_closure = 1; return 1; }
    if (!strcmp(a, "--no-infer")) { c->no_infer = 1; return 1; }
    if (!strcmp(a, "--no-proj")) { c->no_proj = 1; return 1; }
    if (!strcmp(a, "--no-cells")) { c->no_cells = 1; return 1; }
    if (!strcmp(a, "--probe-ctx-cap") && i + 1 < argc) { c->probe_ctx_cap = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-auto-seed")) { c->no_auto_seed = 1; return 1; }
    if (!strcmp(a, "--no-ckpt-bursts")) { c->ckpt_burst_frames = 0; return 1; }
    if (!strcmp(a, "--ckpt-burst-frames") && i + 1 < argc) { c->ckpt_burst_frames = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-tas-ckpt")) { c->no_tas_ckpt = 1; return 1; }
    if (!strcmp(a, "--tas-ckpt-every") && i + 1 < argc) { c->tas_ckpt_every = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--fork-site-cap") && i + 1 < argc) { c->fork_site_cap = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--tas-ckpt-cap") && i + 1 < argc) { c->tas_ckpt_cap = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-held-budget")) { c->no_held_budget = 1; return 1; }
    if (!strcmp(a, "--held-steps") && i + 1 < argc) { c->held_steps = strtoull(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--no-exact-state")) { c->no_exact = 1; return 1; }
    if (!strcmp(a, "--no-spec")) { c->no_spec = 1; return 1; }
    if (!strcmp(a, "--no-bank-forks")) { c->no_bank_forks = 1; return 1; }
    if (!strcmp(a, "--no-closure-ret")) { c->no_closure_ret = 1; return 1; }
    if (!strcmp(a, "--no-closure-banks")) { c->no_closure_banks = 1; return 1; }
    if (!strcmp(a, "--no-closure-regs")) { c->no_closure_regs = 1; return 1; }
    if (!strcmp(a, "--closure-seed-cap") && i + 1 < argc) { c->closure_seed_cap = (unsigned)strtoul(argv[i + 1], NULL, 0); return 2; }   // C3: distinct regstate seeds per site (0: no cap)
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
    free(r->prov);
    free(r->dest); free(r->spec); free(r->refs); free(r->page_win); free(r->inferred); free(r->live);
    free(r->src_flags); free(r->reg_slot); free(r->reg_reads); free(r->reg_writes); free(r->reg_vals);
    free(r->first_seen); free(r->first_actor); free(r->hits);
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
    int spool_fd, spool_write_failed, spool_read_failed, checkpoint_failed; uint64_t spool_n, spool_head, spool_total, picks; uint32_t spool_rs;   // disk overflow: fixed-size FIFO records
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
    uint64_t ctx_epoch;        // A2: bumped whenever the semantic cell's read-set grows (new ramread bit); guards the per-worker cell-key memo
    uint64_t *exactset;        // B5: exact (pc, machine state) keys already reached, clocks excluded
    size_t exactcap, exactn;
    uint8_t *tas_start; uint32_t tas_pc; int tas_shifts_left;   // power-on state kept to queue the shifted movie replays once the unshifted one ends
    const DpMachine *snap_m;   // geometry for the periodic coverage snapshot
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
    int run_sweep;         // the current read run is a whole-ROM style sweep (checksum, hash): its bytes are not "data read"
    int no_decompress;
    // technique 1 (solve): non-ROM addresses read inside the current window
    uint32_t ram[4]; unsigned nram; int track;
    uint8_t *ramread;     // technique 8: 64 KiB bitmap of non-ROM data addresses any path has read (the semantic cell)
    uint64_t *ctx_epoch;  // A2: shared cell-context epoch, to invalidate the per-worker memo
    uint32_t *fedsrc;     // parallel to fed: rom index + 1 of the source byte of that copy
    uint32_t *fedprod;    // rom index + 1 of the last ROM cell read before a non-copy store (approximate producer)
} CopyObs;

#define RUN_MIN 16
#define SWEEP_MIN 4096                 // a sequential read run this long with (almost) no writes is a sweep, not a table read
#define SRC_SWEEP    (1u << 16)        // src_flags: byte read only by a sweep (never counted as known data)
#define SRC_RUNFRESH (1u << 17)        // src_flags: DATA freshly set by the current run (undone if the run turns out a sweep)
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
    if (o->run_hi >= 0 && !o->forced && o->run_hi - o->run_lo < (int64_t)o->r->rom_size)
        for (int64_t k = o->run_lo; k <= o->run_hi; k++) __atomic_fetch_and(&o->r->src_flags[k], ~SRC_RUNFRESH, __ATOMIC_RELAXED);
    o->run_lo = o->run_hi = -1; o->run_writes = 0; o->run_sweep = 0;
}

typedef struct RegWalk RegWalk;   // C3: the regstate-aware closure state, defined below
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
    uint64_t *pcache; unsigned pcap;   // A5: memo of the projection-pruned forks (site, projection); the shared set only grows, so a spent key stays spent
    uint64_t *ckm_ep, *ckm_ck, *ckspent; int64_t *ckm_ri;   // A2: direct-mapped per-ri cell-key memo (4096) and known-spent composed keys
    uint32_t pcring[32]; unsigned pcring_n;   // A12: the last executed instruction starts of this path (never operand bytes)
    uint8_t *wsnap;        // technique 1: state at the start of the window before a conditional
    uint32_t *winmemo;     // direct-mapped: (rom index + 1) * 2 + (a conditional follows within 5 sequential instructions)
    RegWalk *regwalk;      // C3: the regstate-aware closure's per-worker caps and seed dedup
    uint8_t path_actor;   // P13/A9: DP_ACTOR_* of the running path, stamped into first_actor at first cover
    int win_on; uint32_t win_pc; unsigned win_n;
    uint8_t win_imm[24]; unsigned win_ni;
    uint8_t *irqscratch;       // IRQ hook output, kept per worker (the hook is asked before any lock)
    uint64_t keycache[1 << 17];   // direct-mapped: probeset keys known present (never removed), read without the shared mutex
    pthread_t th;
} Worker;

static inline int kc_known(const Worker *w, uint64_t key) { return w->keycache[(key * 0x9E3779B97F4A7C15ull) >> 47] == key; }
static inline void kc_add(Worker *w, uint64_t key) { w->keycache[(key * 0x9E3779B97F4A7C15ull) >> 47] = key; }

// 1 when a conditional follows `pc` within 5 sequential instructions (memoised per ROM site)
static int covered_natural(DpCoreResult *r, DpMachine *m, uint32_t pc);
static int cond_ahead(Worker *w, const DpIsa *isa, uint32_t pc) {
    DpMachine *m = w->m;
    int64_t k = m->rom_index(m, pc);
    if (k < 0 || !isa->flow) return 0;
    uint32_t *e = &w->winmemo[(uint64_t)k * 2654435761u >> 16 & 0xFFFF];
    if ((*e >> 1) == (uint32_t)k + 1) return *e & 1;
    int yes = 0;
    uint32_t p = pc, fall = 0, tgt = 0xFFFFFFFFu;
    for (int i = 0; i < 5; i++) {
        uint8_t b[10];
        for (unsigned j = 0; j < sizeof b; j++) b[j] = m->fetch(m, p + j);
        DpFlowInfo f;
        memset(&f, 0, sizeof f);
        if (!isa->flow(b, p, &f) || !f.length) break;
        if (f.kind == DP_FLOW_COND) { yes = i > 0; fall = p + f.length; tgt = f.target >= 0 ? (uint32_t)f.target : 0xFFFFFFFFu; break; }   // the conditional itself starts no window
        if (f.kind != DP_FLOW_SEQ) break;
        p += f.length;
    }
    e[0x10000] = fall; e[0x20000] = tgt;   // the two sides of the conditional the window leads to (a window is useless once both are natural)
    *e = ((uint32_t)k + 1) << 1 | (uint32_t)yes;
    return yes;
}

// the conditional a window leads to has both sides covered by natural paths: nothing left to solve, no snapshot
static int win_both_natural(Worker *w, DpCoreResult *loc, DpMachine *m, uint32_t pc) {
    int64_t k = m->rom_index(m, pc);
    const uint32_t *e = &w->winmemo[(uint64_t)k * 2654435761u >> 16 & 0xFFFF];
    uint32_t fall = e[0x10000], tgt = e[0x20000];
    return tgt != 0xFFFFFFFFu && covered_natural(loc, m, fall) && covered_natural(loc, m, tgt);
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
// --- C3: the regstate-aware closure (the generic port of the Sega closure's
// register tracking).  The ISA supplies the transfer table (DpIsa.reg_effect)
// and the machine the concrete state (DpMachine.get_regs); the walker carries
// a DpRegState per path.  Caps are per worker, counted, never silent; under
// --unbounded they are lifted (the seed set grows instead).
struct RegWalk {
    DpCoreResult *r;
    const DpCoreConfig *cfg;
    size_t rom_size;
    uint64_t *set;          // (rom index, regstate hash) already seeded
    unsigned set_cap, set_n;
    uint8_t *site_seeds;    // direct-mapped per site: seed walks accepted
    uint8_t *ind_vals;      // direct-mapped per site: resolutions followed at an indirect site
    uint32_t mask;          // direct-mapped mask of the two tables above
    uint8_t *provd;         // per rom index bit: the constant read was already recorded
};
static uint64_t reg_hash(const DpRegState *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < DP_REG_N; i++) h = (h ^ s->v[i]) * 0x100000001b3ull;
    for (unsigned i = 0; i < DP_REG_N; i++) h = (h ^ s->known[i]) * 0x100000001b3ull;
    return h;
}
static int regwalk_seen(RegWalk *rw, uint64_t key) {   // 1 only when already present
    size_t h = (size_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (rw->set_cap - 1);
    for (unsigned i = 0; i < rw->set_cap; i++, h = (h + 1) & (rw->set_cap - 1)) {
        if (!rw->set[h]) return 0;
        if (rw->set[h] == key) return 1;
    }
    return 0;
}
static int regwalk_grow(RegWalk *rw) {   // rehash into a twice-as-large table (1 on success)
    if (rw->set_cap > UINT_MAX / 2) return 0;
    unsigned ncap = rw->set_cap << 1;
    uint64_t *ns = calloc(ncap, sizeof *ns);
    if (!ns) return 0;
    for (unsigned i = 0; i < rw->set_cap; i++) {
        if (!rw->set[i]) continue;
        size_t h = (size_t)(rw->set[i] * 0x9E3779B97F4A7C15ull >> 40) & (ncap - 1);
        while (ns[h]) h = (h + 1) & (ncap - 1);
        ns[h] = rw->set[i];
    }
    free(rw->set);
    rw->set = ns;
    rw->set_cap = ncap;
    return 1;
}
static int regwalk_insert(RegWalk *rw, uint64_t key) {   // 1 inserted, 0 refused: the hard ceiling is reached
    if (rw->set_n >= rw->set_cap - rw->set_cap / 4 && ((!rw->cfg->unbounded && rw->set_cap >= REG_SET_CEIL) || !regwalk_grow(rw))) return 0;
    size_t h = (size_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (rw->set_cap - 1);
    for (unsigned i = 0; i < rw->set_cap; i++, h = (h + 1) & (rw->set_cap - 1)) {
        if (!rw->set[h]) { rw->set[h] = key; rw->set_n++; return 1; }
        if (rw->set[h] == key) return 1;
    }
    return 0;
}
#define CLOSURE_BANK_NODES 16
typedef struct { uint32_t addr; uint8_t val; int parent; } BankPoke;
static inline unsigned reg_of(const DpCoreResult *r, uint32_t addr);
// put the machine in the view of `node`: the saved live state, then the chain of mapper writes that led to the node
static void bank_apply(DpMachine *m, const uint8_t *snap, const BankPoke *pk, int node) {
    int chain[CLOSURE_BANK_NODES + 1], n = 0;
    for (int i = node; i > 0 && n < CLOSURE_BANK_NODES; i = pk[i].parent) chain[n++] = i;
    m->load(m, snap);
    while (n--) m->poke(m, pk[chain[n]].addr, pk[chain[n]].val);
}
// C3: the return stack of a walk path (the shadow of what the real stack would hold): a direct call pushes its return address and the walk
// continues there only when the callee reaches a return, so inline data after a call (the callee pops it and jumps past) is never swept
// as code and a callee that never returns never marks the caller's continuation.  Same rule as the Sega closure, ISA-neutral.
#define CLOSURE_RET 8
typedef struct { uint32_t pc[CLOSURE_RET], bk[CLOSURE_RET], trust[CLOSURE_RET]; unsigned n; } RetStk;
// a provable ROM read handed to DpIsa.reg_effect: the byte lands, the proof is
// recorded once per ROM byte (the explicit-constant-operand evidence)
typedef struct { RegWalk *rw; ClosureView v; const uint32_t *bk; } ProvCtx;
static int closure_prov_read(void *ctx, uint32_t addr, uint8_t *v) {
    ProvCtx *c = ctx;
    int64_t k = cview_index(&c->v, c->rw->r, addr, *c->bk);
    if (k < 0 || (size_t)k >= c->rw->rom_size) {
        // not ROM: the machine's RAM still answers (a pop or a read through a
        // concrete stack pointer) — no data proof is recorded for it
        if (c->v.m && c->v.m->fetch) { *v = c->v.m->fetch(c->v.m, addr); return 1; }
        return 0;
    }
    if (!(c->rw->provd[k >> 3] & (0x80u >> (k & 7)))) {
        c->rw->provd[k >> 3] |= (uint8_t)(0x80u >> (k & 7));
        c->rw->r->prov_data_bytes++;
    }
    *v = c->v.m ? c->v.m->fetch(c->v.m, addr) : c->v.cfg->rom_image[k];
    return 1;
}
// "zero data marked code" point 3: the first bytes of an untrusted entry (a
// call's fall-through, the inline-data idiom) are checked against the ISA's
// data signature (repeated rst $38, accumulator-store soup, undocumented
// no-op prefixes) and the entry is refused when they match.  A generic
// "eight sequential instructions with no control transfer" rule was tried and
// rejected on the bench: real straight-line leaf code (unrolled shifts,
// save/restore prologues) decodes exactly like that, and refusing its entry
// lost provably real code on Ace of Aces and Bank Panic.  The classic
// first-step guards (padding and pointer runs, data flags) are NOT
// duplicated: when the first step would hit one, the entry is left to the
// walk so it stops there with its own counter (closure_stopped_data).
static int closure_entry_is_data(const DpCoreResult *r, const DpIsa *isa, const ClosureView *v, uint32_t pc, int64_t k) {
    uint8_t w[10] = {0};
    if (v->m) for (unsigned i = 0; i < sizeof w; i++) w[i] = v->m->fetch(v->m, pc + i);
    else for (unsigned i = 0; i < sizeof w && (size_t)k + i < v->cfg->rom_image_size; i++) w[i] = v->cfg->rom_image[k + i];
    if ((w[0] == 0x00 || w[0] == 0xFF) && w[1] == w[0] && w[2] == w[0] && w[3] == w[0] && w[4] == w[0] && w[5] == w[0] && w[6] == w[0] && w[7] == w[0]) return 0;   // padding: the in-walk guard stops the first step
    int hit = 1;
    for (int o = 0; o <= 4 && hit; o++) {   // a run of 3 pointer-like words covering the first byte: the in-walk guard handles it
        int ok = 1;
        for (int q = 0; q < 3 && ok; q++) { uint16_t wd = (uint16_t)(w[o + 2 * q] | w[o + 2 * q + 1] << 8); ok = wd >= 0x4000 && wd < 0x8000; }
        hit = ok;
    }
    if (hit) return 0;
    if (k >= 0 && (__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & (DP_CORE_DATA | DP_CORE_COPIED))) return 0;   // a data-marked root: the walk stops on it
    return isa->entry_data_sig && isa->entry_data_sig(w, 8);
}
// A register-based table (DpCodeTable.base_reg): the base is the register's proven value plus the displacement.  `rs`: a walk's
// constants, or NULL for the concrete CPU.  Only ROM tables count (a table in RAM is data the program writes).  Returns 0 when
// the register is not fully known or the table is not in the ROM.
static int code_table_resolve(DpMachine *m, const DpIsa *isa, DpCodeTable *t, const DpRegState *rs) {
    if (t->base_reg < 0) return 1;
    DpRegState cur;
    if (!rs) { if (!m->get_regs || !m->get_regs(m, &cur)) return 0; rs = &cur; }
    unsigned pw = isa->ptr_width >= 4 ? 0x0Fu : (1u << (isa->ptr_width ? isa->ptr_width : 2)) - 1;
    if ((unsigned)t->base_reg >= DP_REG_N || (rs->known[t->base_reg] & pw) != pw) return 0;
    uint32_t vmask = isa->ptr_width >= 4 ? 0xFFFFFFFFu : isa->ptr_width == 3 ? 0x00FFFFFFu : 0x0000FFFFu;
    t->base = (rs->v[t->base_reg] + (uint32_t)t->disp) & vmask;
    t->base_reg = -1;
    return m->rom_index(m, t->base) >= 0;
}

// Targets of a code table (DpIsa.code_table): `rel` tables hold signed word offsets from the base and end where the first
// entry's target begins; absolute tables hold 24-bit long pointers and may start one slot early (object ids begin at 1).
// An entry must be even, inside the ROM and decode as a legal instruction.  Returns the number of targets written.
static unsigned code_table_targets(DpMachine *m, const DpIsa *isa, const DpCodeTable *t, uint32_t *out, unsigned max) {
    uint32_t first = 0xFFFFFFFFu; unsigned cnt = 0;
    const unsigned w = t->stride;
    for (unsigned e = 0; e < 128 && cnt < max; e++) {
        uint32_t ea = (t->base + e * w) & 0xFFFFFFu;
        if (t->rel && ea >= first) break;
        uint32_t v = 0; for (unsigned i = 0; i < w; i++) v = (v << 8) | m->fetch(m, ea + i);
        uint32_t tg;
        if (t->rel) tg = (t->base + (uint32_t)(int32_t)(int16_t)v) & 0xFFFFFFu;
        else { if (v >> 24) { if (e == 0) continue; break; } tg = v; }
        int bad = (tg & 1u) || m->rom_index(m, tg) < 0;
        if (!bad) {
            uint8_t b[10]; for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, tg + i);
            DpFlowInfo f; memset(&f, 0, sizeof f);
            bad = !isa->flow(b, tg, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL;
        }
        if (bad) { if (!t->rel && e == 0) continue; break; }   // absolute tables: slot 0 may be the code before the table
        if (t->rel && tg > ea && tg < first) first = tg;
        out[cnt++] = tg;
    }
    return cnt;
}

static void closure_run(DpCoreResult *r, const ClosureView *v, const DpIsa *isa, uint32_t root, uint32_t bank, uint32_t trust0,
                        const DpRegState *regs0, int thru0, unsigned budget0, RegWalk *rw) {
    uint32_t stack_s[256][4]; RetStk rets_s[256]; DpRegState rg_s[256];
    uint32_t (*stack)[4] = stack_s; RetStk *rets = rets_s; DpRegState *rg = rg_s;
    unsigned depth = 256, kthru_cap = 4096, seen_cap = 8192;
    uint32_t (*heap)[4] = NULL; RetStk *hret = NULL; DpRegState *hrg = NULL;
    if (v->cfg && v->cfg->unbounded) {   // P11: the fixed walk caps lift under --unbounded (budget and data rules still stop it); failures fall back silently to the small tables
        heap = malloc(4096 * sizeof *heap); hret = malloc(4096 * sizeof *hret); hrg = malloc(4096 * sizeof *hrg);
        if (heap && hret && hrg) { stack = heap; rets = hret; rg = hrg; depth = 4096; kthru_cap = 65536; seen_cap = 65536; }
    }
    RetStk cr; unsigned sp = 0, budget = budget0 ? budget0 : (unsigned)CLOSURE_INSTR, kthru = 0, thru = (unsigned)thru0; cr.n = 0; uint64_t *seen = NULL;
    // C3: bank-switch simulation.  `ld a,n` then a store of A to a mapper bank register is replayed through the machine's own
    // mapper (poke) so the rest of the walk sees the new ROM view; every walk restores the live state when it ends and each
    // pending root remembers the chain of writes it was pushed under (node 0 = the live view).
    BankPoke pk[CLOSURE_BANK_NODES + 1]; int npk = 0, cur = 0, acc_known = 0, acc_age = 0; uint8_t acc = 0; uint8_t *snap = NULL;
    // C3: the register-constant state of the walk path (one per pending side; a
    // seeded walk starts from the concrete CPU registers, a plain one from all-unknown)
    DpRegState regs;
    int have_regs = regs0 != NULL && isa->reg_effect != NULL;
    unsigned spw = isa->sp_width;   // the modeled stack: word size; the register width and stack page come from the ISA (6502 S is 8 bits above $0100)
    uint32_t spmask = isa->sp_addr_mask;                 // value mask of the SP bits that index memory
    unsigned spk = spmask, spb = 0;                      // known-bits mask of the same bytes (known[] is per byte)
    while (spk) { spb++; spk >>= 8; }
    uint32_t spknown = spb ? (1u << spb) - 1 : 0;
    if (regs0) regs = *regs0; else dp_regstate_clear(&regs);
#define CLOSURE_POP() do { sp--; cr = rets[sp]; thru = 0; pc = stack[sp][0]; bk = stack[sp][1]; trust = stack[sp][2]; acc_known = 0; regs = rg[sp]; \
        if (stack[sp][3] && budget < CLOSURE_INSTR / 8) budget = CLOSURE_INSTR / 8;   /* a simulated view gets its own bounded budget */ \
        if ((int)stack[sp][3] != cur) { bank_apply(v->m, snap, pk, (int)stack[sp][3]); cur = (int)stack[sp][3]; } } while (0)
    uint32_t trust = trust0;   // A13: code reached from executed code is trusted; the fall-through after a CALL is not (inline data idiom)
    uint32_t pc = root, bk = bank;
    ProvCtx pctx = { rw, *v, &bk };   // after bk: the provable reads follow the walk's live bank
    DpProvRom rom = { &pctx, closure_prov_read };
    if (v->m && !thru0) {   // the live walk counts its roots and skips a root already known (a seeded walk starts on executed code: it walks through)
        int64_t k0 = cview_index(v, r, pc, bk);
        if (k0 >= 0 && !trust0 && r->prov && r->prov[k0]) goto done;   // a provisional root holds nothing new for another untrusted walk
        if (k0 < 0 || (__atomic_load_n(&r->flags[k0], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC))) goto done;
        r->closure_roots++;
    }
    if (!trust0 && !thru0) {   // point 3: a plain untrusted entry (a call's fall-through root) whose first instructions decode like data is refused
        int64_t k0 = cview_index(v, r, pc, bk);
        if (k0 >= 0 && closure_entry_is_data(r, isa, v, pc, k0)) { r->closure_entry_refused++; goto done; }
    }
    for (;;) {
        int64_t k = cview_index(v, r, pc, bk);
        int pass = 0;
        int known = k >= 0 && ((__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC))
                               || (!trust && r->prov && r->prov[k]));   // a provisional byte holds nothing new for an untrusted walk; a trusted one must reach it to promote it
        if (known && r->closure_ret && (cr.n || thru) && kthru < kthru_cap) {   // each known address is walked through once per walk
            if (!seen) seen = calloc(seen_cap, sizeof *seen);
            if (seen) {
                uint64_t key = (uint64_t)k + 1; size_t hh = (size_t)(key * 0x9E3779B97F4A7C15ull >> 51) & (seen_cap - 1); int dup = 0;
                while (seen[hh]) { if (seen[hh] == key) { dup = 1; break; } hh = (hh + 1) & (seen_cap - 1); }
                if (!dup) { seen[hh] = key; known = 0; kthru++; pass = 1; }
            }
        }   // C3: a pending return walks through known code (a per-walk allowance of 2048 instructions) to find the callee's ret
        if (k < 0 || !budget || known) {
            if (v->m && k >= 0 && !budget) r->closure_capped++;
            if (!sp) goto done;
            CLOSURE_POP();
            if (!budget) budget = 1;
            continue;
        }
        if (!trust && (__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & (DP_CORE_DATA | DP_CORE_COPIED))) {   // untrusted code stops on data
            r->closure_stopped_data++;
            if (!sp) goto done;
            CLOSURE_POP();
            continue;
        }
        uint8_t b[10] = {0};
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
                if (!sp) goto done;
                CLOSURE_POP();
                continue;
            }
        }
        if (!trust && (b[0] == 0x00 || b[0] == 0xFF) && b[1] == b[0] && b[2] == b[0] && b[3] == b[0] && b[4] == b[0] && b[5] == b[0] && b[6] == b[0] && b[7] == b[0]) {   // untrusted code stops on a padding run (8 bytes)
            r->closure_stopped_data++;
            if (!sp) goto done;
            CLOSURE_POP();
            continue;
        }
        DpFlowInfo f; memset(&f, 0, sizeof f);
        if (!isa->flow(b, pc, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) {
            if (!sp) goto done;
            CLOSURE_POP();
            continue;
        }
        if (!pass) budget--;   // a walk through known code has its own allowance (kthru), it never eats the budget of new code
        if (v->m && r->closure_banks) {
            uint8_t av; uint32_t ad;
            if (isa->acc_imm && isa->acc_imm(b, &av)) { acc = av; acc_known = 1; acc_age = 0; }
            else if (isa->st_acc_abs && isa->st_acc_abs(b, &ad)) {
                int is_map = acc_known && v->m->poke && npk < CLOSURE_BANK_NODES &&
                             (v->m->rom_index(v->m, ad) >= 0 || (v->m->mapper_reg && v->m->mapper_reg(v->m, ad)));
                if (is_map) {
                    if (!snap) { snap = malloc(v->m->state_bytes(v->m)); if (snap) v->m->save(v->m, snap); }
                    if (snap) {
                        // additive and last: the walk goes on under the view it already has (exactly the plain walk); the continuation under
                        // the new view is queued behind it and only gets what budget is left
                        pk[++npk] = (BankPoke){ ad, acc, cur }; r->closure_bank_sims++;
                        if (sp < depth) { rets[sp] = cr; stack[sp][0] = pc + f.length; stack[sp][1] = bk; stack[sp][2] = trust; stack[sp][3] = (uint32_t)npk; rg[sp] = regs; sp++; }
                        else r->closure_stack_dropped++;
                    }
                }
            } else if (acc_known && (f.kind != DP_FLOW_SEQ || ++acc_age > 2)) acc_known = 0;
        }
        if (isa->out_port && v->m && v->m->port_control) {   // C3: an OUT to a memory-control port can hide the stack RAM and the mapped ROM:
            int pk = 0; uint16_t pv = 0;                     // the concrete SP and the ghost-return pile die (the Sega closure's port_effect)
            if (isa->out_port(b, &pk, &pv) && v->m->port_control(v->m, pv, pk)) {
                r->closure_stack_invalidations += cr.n;
                cr.n = 0;
                regs.known[isa->sp_slot] = 0;
            }
        }
        for (unsigned i = 0; i < f.length; i++) {
            int64_t q = v->m ? cview_index(v, r, pc + i, bk) : (k + i < (int64_t)r->rom_size ? k + i : -1);
            if (q >= 0) {
                // "zero data marked code" point 2: a byte reached by a trusted walk (a proven edge from
                // executed code) is static; a byte reached only by an untrusted one (a call's fall-through,
                // a concrete-stack return) is provisional — counted as code once execution, a trusted walk
                // or the Sega closure confirms it, never by the provisional marking itself
                if (trust) {
                    uint8_t old = __atomic_fetch_or(&r->flags[q], DP_CORE_STATIC, __ATOMIC_RELAXED);
                    if (!(old & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED))) r->closure_bytes++;
                } else if (!(__atomic_load_n(&r->flags[q], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC))) {
                    if (r->prov && !__atomic_exchange_n(&r->prov[q], 1, __ATOMIC_RELAXED)) r->closure_bytes++;
                }
            }
        }
        // C3: the transfer table rides the walk; a register-indirect transfer
        // resolves through the constants proven so far (per-site value cap,
        // counted, never silent; lifted under --unbounded)
        int indirect_ok = 0;
        int32_t tgt = f.target;
        if (have_regs && f.pair != DP_REG_PAIR_NONE && (f.kind == DP_FLOW_INDIRECT || (f.kind == DP_FLOW_CALL && f.target < 0))) {
            unsigned pw = isa->ptr_width, pm = pw >= 4 ? 0x0Fu : (1u << pw) - 1;
            uint32_t vmask = pw >= 4 ? 0xFFFFFFFFu : pw == 3 ? 0x00FFFFFFu : 0x0000FFFFu;
            if ((regs.known[f.pair] & pm) == pm) {
                tgt = (int32_t)(regs.v[f.pair] & vmask);
                int64_t kt = cview_index(v, r, (uint32_t)tgt, bk);
                if (kt >= 0 && rw && !rw->cfg->unbounded) {
                    uint32_t islot = (uint32_t)k & rw->mask;
                    if (rw->ind_vals[islot] >= REG_IND_CAP) { r->closure_reg_indirect_sat++; tgt = f.target; }
                    else { rw->ind_vals[islot]++; indirect_ok = 1; }
                } else if (kt >= 0) indirect_ok = 1;
                else r->closure_unresolved++;   // a resolved target outside the mapped view: counted, never silent
                if (indirect_ok) r->closure_reg_indirect++;
            } else r->closure_reg_miss++;
        }
        // C3: the concrete-stack return (the Sega closure's returns_resolved).
        // A seeded walk enters routines without having pushed their call: when
        // the walk carries a fully known SP (the machine's own stack), a `ret`
        // takes its continuation from the address the machine holds there.  The
        // entry state is read before reg_effect: the pop address is the SP the
        // instruction sees.  Per-site cap as for the indirect resolutions;
        // untrusted continuation (the path may have drifted from the machine's).
        if (spw && cr.n == 0 && (f.kind == DP_FLOW_RET || (f.kind == DP_FLOW_COND && f.target < 0)) &&
            (regs.known[isa->sp_slot] & spknown) == spknown && v->m) {
            unsigned retw = spw, retb = 0, retoff = 0;   // the return opcode's own pull: width, address bias (6502 rts adds one) and byte offset (6502 pushes pre-decrement); unmodeled opcodes (rti, rte) fall past
            uint32_t spv = 0, ra = 0;
            DpRegState rs = regs;
            if (isa->sp_ret && !isa->sp_ret(b, &retw, &retb, &retoff)) goto no_stackret;
            spv = regs.v[isa->sp_slot] & spmask;
            if (isa->big_endian) for (unsigned i = 0; i < retw; i++) ra = (ra << 8) | v->m->fetch(v->m, isa->sp_base | ((spv + retoff + i) & spmask));
            else for (unsigned i = 0; i < retw; i++) ra |= (uint32_t)v->m->fetch(v->m, isa->sp_base | ((spv + retoff + i) & spmask)) << (8 * i);
            ra += retb;
            rs.v[isa->sp_slot] = (regs.v[isa->sp_slot] & ~spmask) | ((spv + retw) & spmask);
            int64_t kt = cview_index(v, r, ra, bk);
            if (f.kind == DP_FLOW_RET) {
                if (kt < 0) r->closure_unresolved++;   // not a mapped ROM address: counted, never followed
                else if (rw && !rw->cfg->unbounded && rw->ind_vals[(uint32_t)k & rw->mask] >= REG_IND_CAP) r->closure_reg_indirect_sat++;
                else {
                    if (rw) rw->ind_vals[(uint32_t)k & rw->mask]++;
                    r->closure_reg_stackrets++;
                    regs = rs;
                    pc = ra; trust = 0; thru = 1; continue;
                }
            } else if (kt >= 0 && sp < depth) {   // ret cc: the taken side pops and jumps, the walk goes on past it
                if (rw && !rw->cfg->unbounded && rw->ind_vals[(uint32_t)k & rw->mask] >= REG_IND_CAP) r->closure_reg_indirect_sat++;
                else {
                    if (rw) rw->ind_vals[(uint32_t)k & rw->mask]++;
                    rets[sp] = cr; stack[sp][0] = ra; stack[sp][1] = bk; stack[sp][2] = 0; stack[sp][3] = (uint32_t)cur; rg[sp] = rs; sp++;
                    r->closure_reg_stackrets++;
                }
            } else if (kt < 0) r->closure_unresolved++;
            else r->closure_stack_dropped++;
        }
no_stackret:;
        if (have_regs && isa->reg_effect) {   // the constants survive per the ISA's transfer table
            DpRegState nxt;
            isa->reg_effect(b, pc, &regs, &nxt, &rom);
            regs = nxt;
        }
        if (trust && v->m && isa->code_table && (f.kind == DP_FLOW_INDIRECT || f.kind == DP_FLOW_SEQ)) {   // a PC-relative table met by the walk: every entry is a root
            DpCodeTable ct; uint32_t lp = 0;
            if (isa->code_table(v->m, pc, b, &ct, &lp) && code_table_resolve(v->m, isa, &ct, have_regs ? &regs : NULL)) {
                uint32_t tg[128];
                unsigned n = code_table_targets(v->m, isa, &ct, tg, 128);
                for (unsigned i = 0; i < n; i++) {
                    if (cview_index(v, r, tg[i], bk) < 0) continue;
                    if (sp < depth) { rets[sp] = cr; rets[sp].n = 0; stack[sp][0] = tg[i]; stack[sp][1] = bk; stack[sp][2] = trust; stack[sp][3] = (uint32_t)cur; rg[sp] = regs; sp++; }
                    else r->closure_stack_dropped++;
                }
                if (n) { r->closure_jp_tables++; r->closure_jp_entries += n; r->prov_data_bytes += (uint64_t)n * ct.stride; }
            }
        }
        int fall = f.fall || f.kind == DP_FLOW_SEQ || f.kind == DP_FLOW_COND || f.kind == DP_FLOW_CALL;
        if (f.kind != DP_FLOW_RET && (f.target >= 0 || (indirect_ok && f.kind == DP_FLOW_INDIRECT)) &&
            (f.kind == DP_FLOW_JUMP || f.kind == DP_FLOW_COND || f.kind == DP_FLOW_CALL || (f.kind == DP_FLOW_INDIRECT && indirect_ok)) && tgt >= 0) {
            if (cview_index(v, r, (uint32_t)tgt, bk) < 0) r->closure_unresolved++;
            else if (sp < depth) {
                rets[sp] = cr;
                if (r->closure_ret && f.kind == DP_FLOW_CALL && f.cond < 0 && cr.n < CLOSURE_RET && !(__atomic_load_n(&r->flags[cview_index(v, r, (uint32_t)tgt, bk)], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC))) {   // a callee already known is not walked again: its return cannot be followed, the legacy fall-through applies   // the callee's path remembers where it returns to
                    rets[sp].pc[cr.n] = pc + f.length; rets[sp].bk[cr.n] = bk; rets[sp].trust[cr.n] = trust; rets[sp].n = cr.n + 1;
                }
                stack[sp][0] = (uint32_t)tgt; stack[sp][1] = bk; stack[sp][2] = trust; stack[sp][3] = (uint32_t)cur; rg[sp] = regs; sp++;
                if (f.kind == DP_FLOW_CALL && spw && (rg[sp - 1].known[isa->sp_slot] & spknown) == spknown) {   // the callee runs with the return address pushed
                    rg[sp - 1].v[isa->sp_slot] = (rg[sp - 1].v[isa->sp_slot] & ~spmask) | ((rg[sp - 1].v[isa->sp_slot] - spw) & spmask);
                }
            }
            else r->closure_stack_dropped++;   // a root lost to the fixed stack is counted, never silent
        }
        if (f.kind == DP_FLOW_JUMP || f.kind == DP_FLOW_RET || f.kind == DP_FLOW_INDIRECT || f.halt) fall = 0;
        if (r->closure_ret && cr.n && (f.kind == DP_FLOW_RET || (f.kind == DP_FLOW_COND && f.target < 0))) {
            // a return: the caller's continuation, carrying the trust the call site had (the callee proved the address is code)
            uint32_t rpc = cr.pc[cr.n - 1], rbk = cr.bk[cr.n - 1], rtr = cr.trust[cr.n - 1];
            if (f.kind == DP_FLOW_COND) {   // a conditional return: the taken side is queued, the walk goes on past it
                if (sp < depth) { rets[sp] = cr; rets[sp].n--; stack[sp][0] = rpc; stack[sp][1] = rbk; stack[sp][2] = rtr; stack[sp][3] = (uint32_t)cur; rg[sp] = regs; sp++; r->closure_rets++; }
                else r->closure_stack_dropped++;
            } else { cr.n--; pc = rpc; bk = rbk; trust = rtr; thru = 1; r->closure_rets++; continue; }   // the continuation after a proven return walks through what the blind fall-through already marked (the callee's register constants ride on, the Sega closure's own rule)
        }
        if (fall) {
            if (f.kind == DP_FLOW_CALL) {   // point 3: the fall-through continuation of a call (always untrusted, A13) is checked like an entry
                int64_t kx = cview_index(v, r, pc + f.length, bk);
                if (kx >= 0 && closure_entry_is_data(r, isa, v, pc + f.length, kx)) {
                    r->closure_entry_refused++;
                    if (!sp) goto done;
                    CLOSURE_POP();
                    continue;
                }
            }
            pc += f.length;
            if (f.kind == DP_FLOW_CALL) trust = 0;   // A13: no trust after a call unless the callee was observed to return
        }
        else { if (!sp) goto done; CLOSURE_POP(); }
    }
done:
    free(seen); free(heap); free(hret); free(hrg);
    if (snap) { v->m->load(v->m, snap); free(snap); }   // the live machine is left exactly as the walk found it
#undef CLOSURE_POP
}
static void closure_walk(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t root, RegWalk *rw) {
    ClosureView v = { m, NULL, { 0, 0, 0, 0 } };
    closure_run(r, &v, isa, root, 0, 1, NULL, 0, CLOSURE_INSTR, rw);
}
static void closure_walk_untrusted(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t root, RegWalk *rw) {
    ClosureView v = { m, NULL, { 0, 0, 0, 0 } };
    closure_run(r, &v, isa, root, 0, 0, NULL, 0, CLOSURE_INSTR, rw);   // A13: the return address of a call may be inline data
}
// C3: the executed step seeds a walk from the concrete CPU registers — the
// Sega closure's per-step bootstrapping, generic.  Untrusted (the A13 data
// guards apply) and walk-through (it starts inside executed code), like the
// reference.
static void closure_walk_regs(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t root, const DpRegState *regs, RegWalk *rw) {
    ClosureView v = { m, NULL, { 0, 0, 0, 0 } };
    closure_run(r, &v, isa, root, 0, 0, regs, 1, CLOSURE_INSTR, rw);
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
                uint8_t b[10] = {0};
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
static void closure_walk_img(DpCoreResult *r, const DpCoreConfig *cfg, const DpIsa *isa, uint32_t bank, uint32_t root, unsigned win, RegWalk *rw) {
    ClosureView v = { NULL, cfg, img_geo(NULL, cfg, win) };
    closure_run(r, &v, isa, root, bank, 1, NULL, 0, CLOSURE_INSTR, rw);
}

// S4 stride 3: entries are {addr lo, addr hi, bank}; the table sits in the currently mapped window.
static int closure_jp_table3(DpCoreResult *r, DpMachine *m, const DpCoreConfig *cfg, const DpIsa *isa, uint32_t base, int min_entries, RegWalk *rw) {
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
        unsigned topwin = m->rom_top_win ? m->rom_top_win : 2u;   // the machine's ROM window count (P3 hygiene: was the GB 2-window top)
        if (eb >= nbanks || t == 0x0000u || t == 0xFFFFu || t < isa->code_min || t >= topwin * win) break;
        uint32_t ix = (t < win ? 0 : eb) * win + (t & (win - 1u));
        if (ix >= r->rom_size || ix >= cfg->rom_image_size) break;
        uint8_t b[10] = {0};
        for (unsigned i = 0; i < sizeof b && ix + i < cfg->rom_image_size; i++) b[i] = cfg->rom_image[ix + i];
        DpFlowInfo f; memset(&f, 0, sizeof f);
        if (!isa->flow(b, t, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) break;
        tg[cnt][0] = t; tg[cnt][1] = eb; cnt++;
    }
    if (cnt < min_entries) return 0;   // validated first: nothing is marked for a table that does not hold up
    for (int e = 0; e < cnt; e++) {
        r->closure_jp_entries++; r->prov_data_bytes += 3; r->closure_roots++;
        closure_walk_img(r, cfg, isa, tg[e][1], tg[e][0], win, rw);
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
static void closure_abs_table(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t ta, RegWalk *rw) {
    int cnt = 0;
    for (unsigned e = 0; e < 64; e++) {
        uint32_t a = ta + e * 2u;
        uint32_t t = (uint32_t)m->fetch(m, a) | ((uint32_t)m->fetch(m, a + 1) << 8);
        if (m->rom_index(m, t) < 0) break;   // outside the ROM: end of the table
        uint8_t b[10] = {0};
        for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, t + i);
        DpFlowInfo f; memset(&f, 0, sizeof f);
        if (!isa->flow(b, t, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) break;
        cnt++;
        closure_walk(r, m, isa, t, rw);
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

// pair: 0 HL (`jp hl`/`jp (hl)`), 1 IX, 2 IY (`jp (ix)`/`jp (iy)`, P4) — the backward scan
// looks for the matching pair load (`ld hl,nn` / DD|FD `ld ix,nn`).
static void closure_jp_table(DpCoreResult *r, DpMachine *m, const DpCoreConfig *cfg, const DpIsa *isa, uint32_t pc, unsigned pair, const uint32_t *ring, unsigned nring, RegWalk *rw) {
    if (!isa->hl_jp_table) return;
    if (pair == 0) {   // byte-granular: ld b,n / ld c,m then ld h,b / ld l,c (or the reverse pair) before the jump
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
            closure_walk(r, m, isa, t, rw);
            return;
        }
    }
    for (uint32_t d = pair ? 4u : 3u; d <= 24 && d <= pc; d++) {
        uint8_t lop = m->fetch(m, pc - d);
        int base_ok;
        if (pair)   // DD/FD 21 nn nn: the prefix starts the instruction, the operand rides two bytes further
            base_ok = lop == (pair == 1 ? 0xDDu : 0xFDu) && m->fetch(m, pc - d + 1) == 0x21;
        else {
            base_ok = lop == 0x21;
            if (lop == 0x11 || lop == 0x01)   // ld de/bc,nn then add hl,de/bc, or ld a,(de/bc) pointer reads, before the jump
                for (uint32_t q = pc - d + 3; q < pc; q++) { uint8_t o = m->fetch(m, q); if (o == (lop == 0x11 ? 0x19 : 0x09) || o == (lop == 0x11 ? 0x1A : 0x0A)) { base_ok = 1; break; } }
        }
        if (!base_ok) continue;
        if (ring && !ring_has(ring, nring, pc - d)) continue;   // A12: the load must be an executed opcode, not an operand byte that looks like one
        uint32_t base = m->fetch(m, pc - d + (pair ? 2u : 1u)) | (uint32_t)m->fetch(m, pc - d + (pair ? 3u : 2u)) << 8;
        if (base < 0x100 || m->rom_index(m, base) < 0) continue;
        int cnt = 0;
        for (int e = 0; e < 64; e++) {
            uint32_t t = m->fetch(m, base + e * 2) | (uint32_t)m->fetch(m, base + e * 2 + 1) << 8;
            if (!jp_target_ok(m, isa, t)) break;
            uint8_t b[10]; for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, t + i);
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
            closure_walk(r, m, isa, t, rw);
            cnt++;
        }
        if (cnt) { r->closure_jp_tables++; if (cnt < 2) closure_jp_table3(r, m, cfg, isa, base, 2, rw); return; }   // a lone stride-2 word may be the head of a stride-3 table
        if (closure_jp_table3(r, m, cfg, isa, base, 1, rw)) return;   // stride 3: the entries name their own bank
    }
}

// 68000 PC-relative code table: word entries are signed offsets from the base; the table ends where the
// first entry's target begins (the code the offsets point at follows the table).
static void closure_code_table(DpCoreResult *r, DpMachine *m, const DpIsa *isa, const DpCodeTable *t, RegWalk *rw) {
    uint32_t tg[128];
    unsigned n = code_table_targets(m, isa, t, tg, 128);
    for (unsigned i = 0; i < n; i++) closure_walk(r, m, isa, tg[i], rw);
    if (n) { r->closure_jp_tables++; r->closure_jp_entries += n; r->prov_data_bytes += (uint64_t)n * t->stride; }
}

static void mark(Shared *sh, DpCoreResult *r, DpMachine *m, uint32_t pc, unsigned len, uint8_t how, uint16_t origin, uint8_t actor) {
    for (unsigned i = 0; i < len; i++) {
        int64_t k = m->rom_index(m, pc + i);
        if (k >= 0 && (size_t)k < r->rom_size) {
            if (!i && r->hits) __atomic_fetch_add(&r->hits[k], 1, __ATOMIC_RELAXED);   // P13: execution heat of the instruction head
            if (m->page_shift && r->page_win && how & DP_CORE_NATURAL) __atomic_store_n(&r->page_win[(size_t)k >> m->page_shift], (uint8_t)(1u + (((pc + i) >> m->page_shift) & 7u)), __ATOMIC_RELAXED);
            uint32_t srcbit = (how & DP_CORE_NATURAL) ? (origin ? origin : DP_SRC_FORK_NATURAL) : DP_SRC_FORK_FORCED;   // A6: only a natural flow can be fabricated
            uint8_t bits = how | (i == 0 ? ((how & DP_CORE_NATURAL) ? DP_CORE_START_NATURAL : DP_CORE_START_FORCED) : 0);
            // read first: an atomic read-modify-write on an already covered byte only bounces the cache line
            uint8_t cur = __atomic_load_n(&r->flags[k], __ATOMIC_RELAXED);
            if ((cur & bits) == bits && (__atomic_load_n(&r->src_flags[k], __ATOMIC_RELAXED) & srcbit)) continue;
            uint8_t old = __atomic_fetch_or(&r->flags[k], bits, __ATOMIC_RELAXED);
            if (!(old & (DP_CORE_NATURAL | DP_CORE_FORCED))) {
                r->first_seen[k] = __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) + 1;
                r->first_actor[k] = actor;   // P13/A9: the byte's first-cover actor (0 on non-session paths, the src_flags carry the family)
                if (r->in_novelty_path) r->novelty_new_bytes++; else r->plain_new_bytes++;
            }
            __atomic_fetch_or(&r->src_flags[k], srcbit, __ATOMIC_RELAXED);
        }
    }
}

// A ramp byte: pc is in ROM and inside a run of at least DP_RAMP_FF ($FF) or DP_RAMP_00 ($00)
// identical bytes.  $FF chains rst $38 (never the head of real code twice in a row, see
// entry_data_sig); $00 needs a longer run so the real nop delay lines of timing code stay code.
// Only the bytes a path reaches from the ramp byte before (falling through, or an rst $38
// returning to the next byte) are padding: the byte a jump, a call or the reset lands on is an
// entry (the nop at $0100 before jp, a call target nop before ret) and stays code (run_path).
#define DP_RAMP_FF 16
#define DP_RAMP_00 33
static int rom_ramp(DpMachine *m, uint32_t pc, uint8_t v) {
    if (v != 0xFF && v != 0x00) return 0;
    if (m->rom_index(m, pc) < 0) return 0;
    const unsigned need = v == 0xFF ? DP_RAMP_FF : DP_RAMP_00;
    unsigned n = 1;
    for (unsigned i = 1; i < need && n < need; i++) { if (m->rom_index(m, pc - i) < 0 || m->fetch(m, pc - i) != v) break; n++; }
    for (unsigned i = 1; i < need && n < need; i++) { if (m->rom_index(m, pc + i) < 0 || m->fetch(m, pc + i) != v) break; n++; }
    return n >= need;
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

// registry slot + 1 of a memory address (0 = not a register); port tables are reached through obs_io only
static inline unsigned reg_of(const DpCoreResult *r, uint32_t addr) {
    if (!r->reg_plat || r->reg_plat->port_io) return 0;
    if (r->reg_plat->find) { int i = r->reg_plat->find(addr); return i < 0 ? 0u : (unsigned)i + 1u; }
    return addr < 65536 ? r->reg_slot[addr] : 0u;
}

static void obs_read(void *u, uint32_t addr, uint8_t v) {
    CopyObs *o = u;
    if (o->r->reg_plat && !o->forced) {
        unsigned rs = reg_of(o->r, addr);
        if (rs && (o->r->reg_plat->io_regs[rs - 1].rw & 1)) __atomic_fetch_add(&o->r->reg_reads[rs - 1], 1, __ATOMIC_RELAXED);
    }
    int64_t k = o->m->rom_index(o->m, addr);
    int64_t ri = k < 0 && o->ramread ? ridx(o->m, addr) : -1;
    if (ri >= 0 && !o->forced && !o->ramread[ri]) {   // P6: forced paths must not join the semantic cell (same rule as the live bitmap below)
        __atomic_store_n(&o->ramread[ri], 1, __ATOMIC_RELAXED);
        if (o->ctx_epoch) __atomic_fetch_add(o->ctx_epoch, 1, __ATOMIC_RELAXED);   // A2: the semantic cell changed
    }
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
        if (!o->forced && o->run_sweep) __atomic_fetch_or(&o->r->src_flags[k], SRC_SWEEP, __ATOMIC_RELAXED);   // inside a sweep: neither data nor a closure stopper
        else if (!o->forced && !(__atomic_load_n(&o->r->flags[k], __ATOMIC_RELAXED) & DP_CORE_DATA)) {
            __atomic_fetch_or(&o->r->flags[k], DP_CORE_DATA, __ATOMIC_RELAXED);
            __atomic_fetch_or(&o->r->src_flags[k], SRC_RUNFRESH, __ATOMIC_RELAXED);
        }
        if (!o->forced && !o->run_sweep) {   // A13: a byte read as data by a natural path is not static code unless it was ever executed
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
        if (!o->forced && !o->run_sweep && o->run_hi - o->run_lo + 1 >= SWEEP_MIN && (int64_t)o->run_writes * 16 < o->run_hi - o->run_lo + 1) {
            o->run_sweep = 1;   // the run is a sweep: take back the DATA this very run set (bytes read elsewhere keep theirs)
            for (int64_t j = o->run_lo; j <= o->run_hi; j++) {
                if (!(__atomic_load_n(&o->r->src_flags[j], __ATOMIC_RELAXED) & SRC_RUNFRESH)) continue;
                __atomic_fetch_and(&o->r->src_flags[j], ~SRC_RUNFRESH, __ATOMIC_RELAXED);
                __atomic_fetch_or(&o->r->src_flags[j], SRC_SWEEP, __ATOMIC_RELAXED);
                __atomic_fetch_and(&o->r->flags[j], (uint8_t)~DP_CORE_DATA, __ATOMIC_RELAXED);
            }
        }
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

// CPU port I/O: typed against the registry when the platform's registry is a port table
static void obs_io(void *u, uint16_t port, uint8_t v, int write) {
    CopyObs *o = u;
    if (!o->r->reg_slot || !o->r->reg_plat->port_io || o->forced || !o->r->reg_slot[port]) return;
    unsigned rs = o->r->reg_slot[port] - 1u;
    if (write) {
        __atomic_fetch_add(&o->r->reg_writes[rs], 1, __ATOMIC_RELAXED);
        __atomic_fetch_or(&o->r->reg_vals[rs * 8u + (v >> 5)], 1u << (v & 31u), __ATOMIC_RELAXED);
    } else __atomic_fetch_add(&o->r->reg_reads[rs], 1, __ATOMIC_RELAXED);
}

static uint32_t obs_mode(void *u) { return ((CopyObs *)u)->forced ? 1u : 0u; }

static void obs_write(void *u, uint32_t addr, uint8_t v) {
    CopyObs *o = u;
    unsigned rs1 = o->r->reg_plat && !o->forced ? reg_of(o->r, addr) : 0;
    if (rs1 && (o->r->reg_plat->io_regs[rs1 - 1].rw & 2)) {
        unsigned rs = rs1 - 1u;
        __atomic_fetch_add(&o->r->reg_writes[rs], 1, __ATOMIC_RELAXED);
        __atomic_fetch_or(&o->r->reg_vals[rs * 8u + (v >> 5)], 1u << (v & 31u), __ATOMIC_RELAXED);
    }
    const int64_t rik = o->m->rom_index(o->m, addr);   // one lookup: it is the hot call of block-transfer observation
    if (rik >= 0 ||
        (o->m->mapper_reg && o->m->mapper_reg(o->m, addr))) o->mapper_w = 1;
    if (o->run_hi >= 0 && rik < 0) o->run_writes++;
    int64_t wi = rik < 0 ? ridx(o->m, addr) : -1;
    // A later write replaces the previous provenance even if there has been
    // no recent ROM read. Do this before the stale-read early return.
    if (!o->forced && wi >= 0) {
        if (o->fed) __atomic_store_n(&o->fed[wi], 0, __ATOMIC_RELAXED);
        if (o->fedsrc) __atomic_store_n(&o->fedsrc[wi], 0, __ATOMIC_RELAXED);
        if (o->fedprod) __atomic_store_n(&o->fedprod[wi], 0, __ATOMIC_RELAXED);
    }
    if (o->last_rom < 0 || o->age > 4) return;
    if (wi >= 0 && v != o->last_val && o->fedprod && !o->forced) __atomic_store_n(&o->fedprod[wi], (uint32_t)o->last_rom + 1, __ATOMIC_RELAXED);
    if (o->m->data_dest) {   // the write's hardware destination types the data the last ROM read fed
        uint8_t dest = o->m->data_dest(o->m, addr);
        if (dest && o->last_rom >= 0 && (size_t)o->last_rom < o->r->rom_size)
            __atomic_store_n(&o->r->dest[o->last_rom], dest, __ATOMIC_RELAXED);
    }
    if (rik < 0 && v == o->last_val) {
        if (o->forced) o->r->copy_forced++;
        else {
            __atomic_fetch_or(&o->r->flags[o->last_rom], DP_CORE_COPIED, __ATOMIC_RELAXED);
            __atomic_fetch_or(&o->r->src_flags[o->last_rom], DP_SRC_COPY, __ATOMIC_RELAXED);
            o->r->copy_bytes++;
            if (o->fed && wi >= 0) __atomic_store_n(&o->fed[wi], 1, __ATOMIC_RELAXED);
            if (o->fedsrc && wi >= 0) __atomic_store_n(&o->fedsrc[wi], (uint32_t)o->last_rom + 1, __ATOMIC_RELAXED);
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


// C11: the context projection of a fork: the machine's registers and mapper (ctx_reg_hash) plus the RAM pages the code of this ROM
// page and of the target's page has been seen to read (the liveness bitmap `live`), mixed with the target.  Dead registers cannot be
// told apart without operand decoding, so every register stays in the key: the projection only drops the RAM the code never reads.
static uint64_t proj_key(const DpCoreResult *r, DpMachine *m, uint32_t pc, uint32_t other) {
    uint64_t h = 1469598103934665603ull ^ m->ctx_reg_hash(m) ^ ((uint64_t)other << 24);
    int64_t ks[2] = { m->rom_index(m, pc), m->rom_index(m, other) };
    for (int s = 0; s < 2; s++) {
        int64_t k = ks[s];
        if (k < 0 || (size_t)k >= r->rom_size || !r->live) continue;
        const uint32_t *row = r->live + ((size_t)k >> 8) * r->live_w;
        for (uint32_t w = 0; w < r->live_w; w++)
            for (uint32_t bits = row[w]; bits; bits &= bits - 1) {
                uint32_t pg = w * 32u + (uint32_t)__builtin_ctz(bits);
                for (uint32_t a = pg << 8, e = a + 256; a < e && a < rsize(m); a++) { h ^= m->fetch(m, raddr(m, a)) | ((uint64_t)s << 8); h *= 1099511628211ull; }
            }
        h ^= 0x9E3779B97F4A7C15ull;
    }
    return h | 1;
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

// A2: the context re-arm of B1 generalized to every fork technique (index replays, RNG,
// signatures, IRQ): one more fire of a technique already spent at this site, when the
// semantic cell (the RAM the code has been seen to read) differs from every one seen
// before.  The visit heuristic (1, 2, 4, 8) bounds the attempts; probe_ctx_cap bounds
// the productive contexts per site across every technique (saturation counted, never
// silent).  `tag` is the 4-bit channel in the key's top bits (8 index, 10 rng, 12 sig,
// 13 irq, 11 the B1 event probes); `sub` distinguishes sibling keys of one site (the
// IRQ delay).  1 when the (site, cell) pair was fresh and is now spent.
static int ctx_rearm(Worker *w, const DpCoreConfig *cfg, DpCoreResult *loc, DpMachine *m,
                     int64_t ri, uint64_t tag, uint64_t sub, unsigned visit) {
    Shared *sh = w->sh;
    if (!cfg->probe_ctx_cap || cfg->probe_ctx_cap <= 1 || !sh->probectx || ri < 0) return 0;
    if (!visit || visit > 8 || (visit & (visit - 1))) return 0;   // B1's exponential attempt gate
    unsigned pcx = __atomic_load_n(&sh->probectx[ri], __ATOMIC_RELAXED);
    if (!pcx) return 0;                                           // a site that produced nothing stays spent
    if (pcx >= cfg->probe_ctx_cap) { loc->probe_ctx_sat++; return 0; }
    // Recompute the semantic cell at each attempt; a memo of already spent
    // composed keys still skips the shared lock.
    // The read set can remain unchanged while its RAM values change, and
    // loading a snapshot can change them without an observed write.
    uint64_t ck = cell_key(cfg, sh->ramread, m, (uint32_t)ri) ^ 0x5bd1e995u;
    uint64_t pk = (keyhash(ck ^ ((uint64_t)ri << 32) ^ (tag << 24) ^ sub) & ((1ull << 60) - 1)) | (tag << 60);
    if (w->ckspent[keyhash(pk) & 4095] == pk + 1) return 0;   // known spent, the shared set only grows
    pthread_mutex_lock(&sh->mu);
    int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, pk);
    pthread_mutex_unlock(&sh->mu);
    w->ckspent[keyhash(pk) & 4095] = pk + 1;
    return fresh;
}

// one more productive context at the site (shared counter, capped at 255)
static void ctx_note(Shared *sh, int64_t ri) {
    if (!sh->probectx || ri < 0) return;
    unsigned pcx = __atomic_load_n(&sh->probectx[ri], __ATOMIC_RELAXED);
    if (pcx < 255) __atomic_store_n(&sh->probectx[ri], (uint8_t)(pcx + 1), __ATOMIC_RELAXED);
}


// Queue overflow goes to disk, never away: a record is {pc, forced, site, input} + the saved state.
// All called with sh->mu held.  If the disk is unusable the state stays in RAM beyond queue_max.
typedef struct { uint32_t pc, forced, site, input; uint16_t epoch; } SpoolHead;   // forced bit 0, seed bit 1, vector bit 2; B3 epoch (P5: was not serialized)
static void ram_push(Shared *sh, DpMachine *m, Pending p) {
    if (m) { int64_t k = m->rom_index(m, p.pc); p.tri = k >= 0 ? (uint32_t)k + 1u : 0u; }

    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
    sh->q[sh->qn++] = p;
}
static int spool_open(Shared *sh) {
    if (sh->spool_fd >= 0) return 1;
    if (sh->spool_fd == -1) return 0;   // failed once: stay in RAM
    /* The overflow belongs on disk, wherever that is: TMPDIR, then beside the
     * frontier (the state dir — the spool's natural home, it holds what the
     * frontier will persist anyway), then /tmp.  A missing TMPDIR must not
     * pin the whole frontier in RAM (Zelda US: 422 k states, ~9 GiB, the
     * export survived by luck). */
    const char *tmp = getenv("TMPDIR");
    const char *dirs[3] = { (tmp && *tmp) ? tmp : NULL, sh->cfg->state_dir, "/tmp" };
    for (int t = 0; t < 3; t++) {
        if (!dirs[t]) continue;
        char path[512]; snprintf(path, sizeof path, "%s/deepeel-spool-XXXXXX", dirs[t]);
        int fd = mkstemp(path);
        if (fd >= 0) {
            unlink(path);
            sh->spool_fd = fd; sh->spool_rs = (uint32_t)(sizeof(SpoolHead) + sh->sbytes);
            return 1;
        }
    }
    perror("deepeel: kernel spool unavailable (TMPDIR, the state dir and /tmp all failed); retaining states in RAM");
    sh->spool_fd = -1; return 0;
}
// pushes p; when the RAM queue is full, appends it to the spool.  Returns 1 if spilled.
static int queue_push(Shared *sh, DpMachine *m, Pending p) {
    if (m) { int64_t k = m->rom_index(m, p.pc); p.tri = k >= 0 ? (uint32_t)k + 1u : 0u; }   // P2: resolved once, at push, under the state's own mapping

    const DpCoreConfig *cfg = sh->cfg;
    if (cfg->queue_max && sh->qn >= cfg->queue_max && !sh->spool_write_failed && spool_open(sh)) {
        uint8_t *rec = malloc(sh->spool_rs);
        if (rec) {
            SpoolHead h = { p.pc, (uint32_t)!!p.forced | ((uint32_t)!!p.seed << 1) | ((uint32_t)!!p.vector << 2) | (org_enc(p.origin) << 3) | (p.call_depth << 8), p.site, p.input, p.epoch };
            memcpy(rec, &h, sizeof h); memcpy(rec + sizeof h, p.state, sh->sbytes);
            ssize_t w = pwrite(sh->spool_fd, rec, sh->spool_rs, (off_t)((sh->spool_head + sh->spool_n) * sh->spool_rs));
            free(rec);
            if (w == (ssize_t)sh->spool_rs) { sh->spool_n++; sh->spool_total++; free(p.state); return 1; }
        }
        fprintf(stderr, "deepeel: kernel spool write failed; retaining further states in RAM\n");
        sh->spool_write_failed = 1;
    }
    ram_push(sh, m, p);
    return 0;
}
// B6: a path stopped by sat_path / sat_loop reports its state instead of losing it: with a state dir the
// snapshot joins the frontier (resumed later, e.g. under --unbounded) and is counted as deferred.
static void defer_state(Shared *sh, DpMachine *m, DpCoreResult *loc, Pending p) {
    if (!sh->cfg->state_dir || sh->cfg->unbounded) { free(p.state); return; }   // P7: the caller's snapshot (when it passed one) is owned here either way
    { int64_t k = m->rom_index(m, p.pc); p.tri = k >= 0 ? (uint32_t)k + 1u : 0u; }
    if (p.state) {   // P7: the caller already holds the state to defer (a capped fork), do not re-snapshot past it
        pthread_mutex_lock(&sh->mu);
        if (sh->defn == sh->defcap) { sh->defcap = sh->defcap ? sh->defcap * 2 : 64; sh->def = realloc(sh->def, sh->defcap * sizeof *sh->def); }
        sh->def[sh->defn++] = p;
        pthread_mutex_unlock(&sh->mu);
        loc->deferred++;
        return;
    }
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
// Bring back the oldest spilled records. The consumed prefix is discarded
// when the spool becomes empty; appends never overwrite unread records.
static int spool_refill(Shared *sh, size_t limit) {
    size_t want = limit ? limit : sh->cfg->queue_max ? sh->cfg->queue_max / 2 : 64;
    if (want < 1) want = 1;
    uint8_t *rec = malloc(sh->spool_rs);
    if (!rec) { fprintf(stderr, "deepeel: cannot allocate kernel spool read buffer\n"); return 0; }
    int ok = 1;
    while (want-- && sh->spool_n) {
        uint64_t i = sh->spool_head;
        if (pread(sh->spool_fd, rec, sh->spool_rs, (off_t)(i * sh->spool_rs)) != (ssize_t)sh->spool_rs) { ok = 0; break; }
        SpoolHead h; memcpy(&h, rec, sizeof h);
        uint8_t *st = malloc(sh->sbytes);
        if (!st) { ok = 0; break; }
        memcpy(st, rec + sizeof h, sh->sbytes);
        sh->spool_head++; sh->spool_n--;
        ram_push(sh, NULL, (Pending){ st, h.pc, (int)(h.forced & 1), h.site, h.input, (uint8_t)((h.forced >> 1) & 1), (uint8_t)((h.forced >> 2) & 1), org_dec((h.forced >> 3) & 31), h.epoch, 0, h.forced >> 8 });
    }
    free(rec);
    if (!sh->spool_n) { sh->spool_head = 0; if (ftruncate(sh->spool_fd, 0) != 0) { /* later appends overwrite it */ } }
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
        queue_push(sh, NULL, (Pending){ state, h.pc, (int)(h.forced & 1), h.site, h.input, (uint8_t)((h.forced >> 1) & 1), (uint8_t)((h.forced >> 2) & 1), org_dec((h.forced >> 3) & 31), h.epoch, 0, h.forced >> 8 });
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
        SpoolHead h = { sh->q[i].pc, (uint32_t)!!sh->q[i].forced | ((uint32_t)!!sh->q[i].seed << 1) | ((uint32_t)!!sh->q[i].vector << 2) | (org_enc(sh->q[i].origin) << 3) | (sh->q[i].call_depth << 8), sh->q[i].site, sh->q[i].input, sh->q[i].epoch };
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(sh->q[i].state, 1, sh->sbytes, f) == sh->sbytes;
    }
    for (size_t i = 0; ok && i < sh->defn; i++) {
        SpoolHead h = { sh->def[i].pc, (uint32_t)!!sh->def[i].forced | ((uint32_t)!!sh->def[i].seed << 1) | ((uint32_t)!!sh->def[i].vector << 2) | (org_enc(sh->def[i].origin) << 3) | (sh->def[i].call_depth << 8), sh->def[i].site, sh->def[i].input, sh->def[i].epoch };
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(sh->def[i].state, 1, sh->sbytes, f) == sh->sbytes;
    }
    uint8_t *rec = sh->spool_n ? malloc(sh->spool_rs) : NULL;
    if (sh->spool_n && !rec) ok = 0;
    for (uint64_t i = 0; ok && i < sh->spool_n; i++)
        ok = pread(sh->spool_fd, rec, sh->spool_rs, (off_t)((sh->spool_head + i) * sh->spool_rs)) == (ssize_t)sh->spool_rs &&
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
                queue_push(sh, w->m, (Pending){ snap, w->win_pc, 0, 0, input, 0, 0, DP_SRC_SOLVED });
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


// Direct RAM loads into the accumulator/index register (techniques 3, 4, 7 on every ISA):
// the per-ISA decode lives in DpIsa.ram_load (M1); this wrapper adds the shared ROM check.
// Returns the load length and fills the address, the width in bytes (big-endian ISAs: the low-order byte is the last)
// and the destination data register (68000) when `bytes` is such a load of a non-ROM byte, else 0.
static int direct_ram_load(DpMachine *m, const DpIsa *isa, const uint8_t *b, uint32_t *addr, int *idx_reg, int *width, int *dreg) {
    *idx_reg = 0; *width = 1; *dreg = -1;
    if (!isa->ram_load) return 0;
    int len = isa->ram_load(b, addr, idx_reg, width, dreg);
    if (!len || m->rom_index(m, *addr) >= 0) return 0;
    return len;
}

// technique 3: a RAM value compared with a constant, as one site.  Fills the address, value and width.
static int ram_signature(DpMachine *m, const DpIsa *isa, uint32_t pc, const uint8_t *b, int dl_len, int idx_reg, uint32_t dl_addr,
                         int dl_width, int dl_dreg, uint32_t *addr, uint32_t *val, int *width) {
    uint8_t f[10];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = m->fetch(m, pc + (dl_len ? (unsigned)dl_len : 0) + i);
    if (isa->ram_signature_wide &&
        isa->ram_signature_wide(m, pc, b, f, dl_len, dl_width, dl_dreg, dl_addr, addr, val, width))
        return 1;
    if (!dl_len || !isa->cmp_imm || idx_reg || f[0] != isa->cmp_imm) return 0;
    *addr = dl_addr; *val = f[1]; *width = 1;
    return 1;
}

// 1 when a ROM table access indexed by the loaded value follows within 16 bytes (technique 7):
// the per-ISA decode lives in DpIsa.rom_table_follows / DpIsa.table_proven (M1).
// B4: proven length (in entries) of the word table whose base load follows the RAM load; 0 when unproven.
// An entry is proven while its word designates the ROM at or above the ISA's legal-code floor (a legal code target).

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
    uint8_t path_actor = tas ? DP_ACTOR_TAS : play ? DP_ACTOR_SCRIPT : attract ? DP_ACTOR_ATTRACT : 0;   // P13/A9: a session replay discovery carries its play mode
    w->path_actor = path_actor;
    // a session shares half the total budget with its siblings: it must not starve the natural path
    uint64_t session_cap = cfg->max_total / (2 * (cfg->sessions ? (cfg->sessions > 16 ? 16 : cfg->sessions) : 16));
    if (m->set_input && cur_input) m->set_input(m, cur_input - 1);   // input 0: keep the loaded state's held pad (probe forks hold theirs)
    loc->paths++;
    run_flush(&w->cobs);
    w->cobs.forced = p->forced; w->cobs.last_rom = -1; w->cobs.rq_n = 0; w->pcring_n = 0;
    w->win_on = w->cobs.track = 0;
    uint32_t path_id = ++w->path_id, vid = path_id, steps = 0, slice_steps = 0;
    uint32_t ramp_last = UINT32_MAX;   // pc of the last ramp byte this path executed (rom_ramp)
    int cur_forced = p->forced;
    uint16_t cur_origin = p->origin, cur_epoch = p->epoch;
    unsigned fdepth = p->call_depth;   // calls made since the forced fork
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
                    Pending q = { snapshot, m->pc(m), cur_forced, 0, p->input, 0, p->vector, cur_origin, cur_epoch };
                    q.call_depth = fdepth;
                    queue_push(sh, m, q);
                    pthread_mutex_unlock(&sh->mu);
                } else {
                    fprintf(stderr, "deepeel: cannot preserve stopped kernel path\n");
                    __atomic_store_n(&sh->checkpoint_failed, 1, __ATOMIC_RELAXED);
                }
            }
            break;
        }
        if (cfg->unbounded && slice_steps >= (cfg->max_path_steps && cfg->max_path_steps < 65536 ? cfg->max_path_steps : 65536)) {
            // An unlimited path may loop forever. Requeue its machine state
            // behind pending work instead of starving other forks.
            slice_steps = 0;
            pthread_mutex_lock(&sh->mu);
            if (!sh->qn && sh->spool_n && !spool_refill(sh, 0)) {
                sh->spool_read_failed = 1;
                __atomic_store_n(&sh->halt, 3, __ATOMIC_RELAXED);
                pthread_cond_broadcast(&sh->cv);
            }
            if (sh->qn && !__atomic_load_n(&sh->halt, __ATOMIC_RELAXED)) {
                uint8_t *snapshot = malloc(sh->sbytes ? sh->sbytes : 1);
                if (snapshot) {
                    m->save(m, snapshot);
                    Pending q = { snapshot, m->pc(m), cur_forced, 0, p->input, 0, p->vector, cur_origin, cur_epoch };
                    q.call_depth = fdepth;
                    ram_push(sh, m, q);
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
                        unsigned ep_ = __atomic_add_fetch(&sh->ckpts, 1, __ATOMIC_RELAXED);
                        pthread_mutex_lock(&sh->mu);
                        queue_push(sh, m, (Pending){ cp, m->pc(m), 0, ckpt_site(m, loc), 0, 0, 0, DP_SRC_SESSION, (uint16_t)(ep_ < 0x7FFFu ? ep_ : 0x7FFFu) });
                        pthread_cond_signal(&sh->cv);
                        pthread_mutex_unlock(&sh->mu);
                        loc->tas_ckpts++;
                        if (cfg->ckpt_burst_frames && m->set_input && m->frame) {   // B3: movie segment mutation: two bursts of pad input from the live state
                            for (unsigned bi = 0; bi < 2; bi++) {
                                uint8_t *bp = malloc(sh->sbytes ? sh->sbytes : 1);
                                if (!bp) break;
                                m->save(m, bp);
                                pthread_mutex_lock(&sh->mu);
                                queue_push(sh, m, (Pending){ bp, m->pc(m), 0, 0, SESSION_PLAY + 16u + (unsigned)((loc->tas_ckpts * 3u + bi * 5u + 1u) % 10u), 0, 0, DP_SRC_SESSION });
                                pthread_cond_signal(&sh->cv);
                                pthread_mutex_unlock(&sh->mu);
                                loc->ckpt_bursts++;
                            }
                        }
                        // A1: B3 generalized beyond the platform registry: the live checkpoints (a game state, not the
                        // power-on one) seed every entry point the platform queued at boot (cfg->seeds: reset, vectors,
                        // user entries, music rips).  The reset seed replays boot validation (SRAM checks, mapper init)
                        // from a live state.  >= 16 of 256 sampled work-RAM bytes must differ from every chosen state, at most 4.
                        int diverse = 0;
                        if (!cfg->no_vectors && !cfg->no_live_seeds && cfg->seeds && cfg->n_seeds) {
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
                            for (size_t ei = 0; ei < cfg->n_seeds; ei++) {
                                uint32_t vpc = cfg->seeds[ei];
                                int pad = 1;
                                for (unsigned q = 0; q < 8 && pad; q++) { uint8_t bb = m->fetch(m, vpc + q); pad = bb == 0x00 || bb == 0xFF; }
                                uint8_t *lp = pad ? NULL : malloc(sh->sbytes);
                                if (!lp) continue;
                                memcpy(lp, cp, sh->sbytes);
                                pthread_mutex_lock(&sh->mu);
                                queue_push(sh, m, (Pending){ lp, vpc, 1, 0, 0, 1, 1, DP_SRC_SESSION });
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
                                queue_push(sh, m, (Pending){ cp, sh->tas_pc, 0, 0, TAS_INPUT + 3 + order[i], 0, 0, DP_SRC_SESSION });
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
                        unsigned ep_ = __atomic_add_fetch(&sh->sess_ckpts, 1, __ATOMIC_RELAXED);
                        pthread_mutex_lock(&sh->mu);
                        queue_push(sh, m, (Pending){ cp, m->pc(m), 0, ckpt_site(m, loc), 0, 0, 0, DP_SRC_SESSION, (uint16_t)(0x8000u | (ep_ < 0x7FFFu ? ep_ : 0x7FFFu)) });
                        pthread_cond_signal(&sh->cv);
                        pthread_mutex_unlock(&sh->mu);
                        loc->sess_ckpts++;
                        if (cfg->ckpt_burst_frames && m->set_input && m->frame) {   // B3: two bursts of pad input from the live state
                            for (unsigned b = 0; b < 2; b++) {
                                uint8_t *bp = malloc(sh->sbytes ? sh->sbytes : 1);
                                if (!bp) break;
                                m->save(m, bp);
                                pthread_mutex_lock(&sh->mu);
                                queue_push(sh, m, (Pending){ bp, m->pc(m), 0, 0, SESSION_PLAY + 16u + (unsigned)((loc->sess_ckpts * 3u + b * 5u) % 10u), 0, 0, DP_SRC_SESSION });
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
        if (!cfg->unbounded && (attract || play) && steps >= (cfg->max_total ? cfg->max_total / 2 : 400000000u)) { loc->sat_path++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin, cur_epoch }); loc->session_frames += m->frame(m) - sess_f0; break; }   // a long session shares the budget
        if (!cfg->unbounded && !tas && !attract && !play && cfg->max_path_steps && steps >= cfg->max_path_steps) { loc->sat_path++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin, cur_epoch }); break; }
        if (!attract && !play && cur_input && cfg->max_total && steps >= session_cap) { loc->sat_path++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin, cur_epoch }); break; }
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
        if (!tas && cfg->visits_per_pc && w->visits[vi] > cfg->visits_per_pc) { loc->sat_loop++; defer_state(sh, m, loc, (Pending){ NULL, 0, cur_forced, 0, p->input, 0, 0, cur_origin, cur_epoch }); break; }
        if (cfg->unbounded && !tas && m->state_hash && !cfg->no_exact && w->visits[vi] >= 4 && (!(w->visits[vi] & (w->visits[vi] - 1)) || !(w->visits[vi] & 63))) {
            // B5: the pc was revisited 4, 8, 16... and every 64 times on this path (a loop whose period is a frame needs the dense samples); the same machine state at the same pc means
            // the continuation was (or is being) explored already: a spin loop ends here instead of running forever
            // A fabricated branch cannot prove that the same machine state
            // has already been explored by a natural path: the two paths
            // carry different provenance and forced-path cut policies.
            uint64_t ek = (m->state_hash(m) ^ ((uint64_t)pc + 1) * 0x9E3779B97F4A7C15ull ^
                           (cur_forced ? 0xd1b54a32d192ed03ull : 0)) | 1ull;
            pthread_mutex_lock(&sh->mu);
            int fresh = keyset_insert(&sh->exactset, &sh->exactcap, &sh->exactn, ek);
            pthread_mutex_unlock(&sh->mu);
            if (!fresh) { loc->state_repeats++; break; }
        }
        uint8_t bytes[10] = {0};
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
            if (cur_forced && !cfg->no_forced_ram_cut && !(sh->fed && pri >= 0 && __atomic_load_n(&sh->fed[pri], __ATOMIC_RELAXED))) {
                loc->forced_ram_cuts++;
                break;
            }
            loc->exec_outside++;
            if (sh->fedsrc && pri >= 0) {
                uint32_t s0 = __atomic_load_n(&sh->fedsrc[pri], __ATOMIC_RELAXED), p0 = __atomic_load_n(&sh->fedprod[pri], __ATOMIC_RELAXED);
                if (s0 && cfg->rom_image && ((size_t)(s0 - 1) >= cfg->rom_image_size || cfg->rom_image[s0 - 1] != bytes[0])) s0 = 0;
                dp_ramarch_add_p(&loc->ra, (uint16_t)pc, bytes, len > 10 ? 10 : len, s0 ? (int32_t)(s0 - 1) : -1, s0 ? -1 : (p0 ? (int32_t)(p0 - 1) : -1));
            }
            if (!cur_forced && sh->fedsrc) {   // executed RAM bytes that a verbatim ROM copy produced: attribute them to their ROM source
                int any = 0;
                for (int i = 0; i < len; i++) {
                    int64_t si = ridx(m, pc + (uint32_t)i);
                    uint32_t src = si >= 0 ? __atomic_load_n(&sh->fedsrc[si], __ATOMIC_RELAXED) : 0;
                    if (src && src - 1 < loc->rom_size &&
                        (!cfg->rom_image || ((size_t)(src - 1) < cfg->rom_image_size && cfg->rom_image[src - 1] == bytes[i]))) {
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
            if (isa->rom_table_follows && isa->rom_table_follows(isa, m, pc, dl_len)) {
                uint32_t addr = dl_addr;
                pthread_mutex_lock(&sh->mu);
                int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ((uint64_t)addr | ((uint64_t)(m->rom_index(m, pc) + 1) << 24)) | (7ull << 60));   // A7: (site, address)
                pthread_mutex_unlock(&sh->mu);
                if (!fresh) fresh = ctx_rearm(w, cfg, loc, m, (int64_t)m->rom_index(m, pc), 8, 0, w->visits[vi]);   // A2: one more enumeration under a new semantic cell

                uint8_t *tmp = fresh ? malloc(sh->sbytes) : NULL;
                if (tmp) m->save(m, tmp);
                unsigned proven = isa->table_proven ? isa->table_proven(isa, m, pc, dl_len) : 0, kmax = cfg->index_max ? cfg->index_max : cfg->unbounded ? 255 : 15;
                unsigned tew = isa->table_entry_width ? isa->table_entry_width : 2u;   // B4: bytes of one table entry (the 68000 lea tables are longs)
                if (!cfg->index_max && !cfg->unbounded && proven * tew > kmax) kmax = proven * tew > 255 ? 255 : proven * tew;   // B4: the whole proven table, index scaled by the entry size
                for (unsigned k = 0; tmp && k <= kmax; k++) {   // B4: --unbounded enumerates the whole byte
                    uint8_t *snap = malloc(sh->sbytes);
                    if (!snap) break;
                    m->poke(m, addr, (uint8_t)k);
                    m->save(m, snap);
                    pthread_mutex_lock(&sh->mu);
                    queue_push(sh, m, (Pending){ snap, pc, 0, 0, cur_input, 0, 0, DP_SRC_INDEX });
                    pthread_mutex_unlock(&sh->mu);
                    loc->index_replays++;
                    ctx_note(sh, (int64_t)m->rom_index(m, pc));   // A2: the context counts as productive
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
                if (!fresh) fresh = ctx_rearm(w, cfg, loc, m, (int64_t)m->rom_index(m, pc), 10, 0, w->visits[vi]);   // A2: the byte went volatile again under a new semantic cell
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
                    queue_push(sh, m, (Pending){ snap, pc, 0, 0, cur_input, 0, 0, DP_SRC_RNG });
                    pthread_mutex_unlock(&sh->mu);
                    loc->rng_forks++;
                    ctx_note(sh, (int64_t)m->rom_index(m, pc));   // A2: the context counts as productive
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
            if (!fresh) fresh = ctx_rearm(w, cfg, loc, m, (int64_t)m->rom_index(m, pc), 12, 0, w->visits[vi]);   // A2: the same signature test met under a new semantic cell

            if (fresh) {
                uint8_t *tmp = malloc(sh->sbytes), *snap = malloc(sh->sbytes);
                if (tmp && snap) {
                    m->save(m, tmp);
                    poke_wide(m, addr, sg_w, sigv);
                    m->save(m, snap);
                    m->load(m, tmp);
                    pthread_mutex_lock(&sh->mu);
                    queue_push(sh, m, (Pending){ snap, pc, 0, 0, cur_input, 0, 0, DP_SRC_SIG });
                    pthread_mutex_unlock(&sh->mu);
                    loc->learned_sigs++;
                    ctx_note(sh, (int64_t)m->rom_index(m, pc));   // A2: the context counts as productive
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
        // ramp: a 1-byte $FF (rst $38 on z80/sm83) or $00 inside a long run of the same ROM byte is
        // padding a path slid through (an rst $38 returns into the next $FF, a forked state jumps
        // into the filler): the path keeps going (the code after the run may be a real entry) but
        // the run is not marked as code; counted (pad_unmarked).  TAS playback stays an observation.
        int ramp = !tas && len == 1 && !cfg->no_nonsense && rom_ramp(m, pc, bytes[0]);
        int slid = ramp && ramp_last != UINT32_MAX && pc == ramp_last + 1;
        if (ramp) ramp_last = pc;   // kept across the rst $38 handler, whose ret lands on the next byte
        if (slid) loc->pad_unmarked++;
        else mark(sh, loc, m, pc, (unsigned)len, cur_forced ? DP_CORE_FORCED : DP_CORE_NATURAL, cur_origin, w->path_actor);
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
        if (!cfg->no_closure && !cur_forced && have_flow && isa->code_table && (fi.kind == DP_FLOW_INDIRECT || fi.kind == DP_FLOW_SEQ)) {
            DpCodeTable ct; uint32_t lp = 0;
            if (isa->code_table(m, pc, bytes, &ct, &lp) && code_table_resolve(m, isa, &ct, NULL)) {
                // a table is closed once per thread: direct-mapped cache of the bases already walked (objects run the same table every frame)
                static __thread uint32_t seen_ct[256];
                uint32_t *slot = &seen_ct[((ct.base * 0x9e3779b1u) >> 24) & 255u], tag = ct.base | 0x80000000u | (uint32_t)ct.stride << 28;
                if (*slot != tag) { *slot = tag; closure_code_table(loc, m, isa, &ct, w->regwalk); }
            }
        }
        if (!cfg->no_closure && !cur_forced && have_flow && fi.kind != DP_FLOW_SEQ && fi.kind != DP_FLOW_ILLEGAL) {
            if (fi.target >= 0 && (fi.kind == DP_FLOW_JUMP || fi.kind == DP_FLOW_COND || fi.kind == DP_FLOW_CALL)) closure_walk(loc, m, isa, (uint32_t)fi.target, w->regwalk);
            if (fi.kind == DP_FLOW_COND) closure_walk(loc, m, isa, pc + fi.length, w->regwalk);
            else if (fi.kind == DP_FLOW_CALL) closure_walk_untrusted(loc, m, isa, pc + fi.length, w->regwalk);
            uint32_t dispatch_arg = 0;
            int dispatch = fi.kind == DP_FLOW_INDIRECT && isa->table_dispatch ? isa->table_dispatch(bytes, &dispatch_arg) : 0;
            if (dispatch == 1)
                closure_jp_table(loc, m, cfg, isa, pc, dispatch_arg,
                                 w->pcring_n >= 8 ? w->pcring : NULL, w->pcring_n < 32 ? w->pcring_n : 32, w->regwalk);   // a path too young to hold the load is not filtered
            if (dispatch == 2)
                closure_abs_table(loc, m, isa, dispatch_arg, w->regwalk);
            // C3: this executed step seeds a walk carrying the concrete CPU
            // registers (the Sega closure's per-step bootstrapping, generic:
            // DpMachine.get_regs + DpIsa.reg_effect).  Per-site cap on
            // distinct states, global dedup, both counted, never silent.
            if (!cfg->no_closure_regs && m->get_regs && isa->reg_effect) {
                DpRegState cr2;
                if (m->get_regs(m, &cr2)) {
                    RegWalk *rw = w->regwalk;
                    int64_t ri = m->rom_index(m, pc);
                    uint64_t key = ri >= 0 ? (reg_hash(&cr2) ^ ((uint64_t)(uint32_t)ri * 0x9e3779b97f4a7c15ull)) | 1ull : 0;
                    if (ri < 0) { /* RAM-executed code: no site key */ }
                    else if (regwalk_seen(rw, key)) { /* already seeded under this state */ }
                    else if (cfg->closure_seed_cap && !cfg->unbounded && rw->site_seeds[(uint32_t)ri & rw->mask] >= cfg->closure_seed_cap) loc->closure_reg_seed_sat++;
                    else if (!regwalk_insert(rw, key)) loc->closure_reg_set_sat++;
                    else {
                        rw->site_seeds[(uint32_t)ri & rw->mask]++;
                        loc->closure_reg_roots++;
                        closure_walk_regs(loc, m, isa, pc, &cr2, rw);
                    }
                }
            }
        }
        if (!cfg->no_solve && !cur_forced && m->poke && have_flow) {
            // window: the sequential instructions before a conditional; their start state and RAM reads feed solve_cond
            if (fi.kind == DP_FLOW_COND) { /* closes the window below */ }
            else if (fi.kind != DP_FLOW_SEQ) w->win_on = w->cobs.track = 0;
            else if (!w->win_on && cond_ahead(w, isa, pc) && !win_both_natural(w, loc, m, pc)) {
                m->save(m, w->wsnap);
                w->win_on = 1; w->win_pc = pc; w->win_n = 0; w->win_ni = 0; w->cobs.nram = 0; w->cobs.track = 1;
            }
            if (w->win_on) {
                if (++w->win_n > 8) w->win_on = w->cobs.track = 0;
                for (int i = 1; i < len && w->win_ni < sizeof w->win_imm; i++) w->win_imm[w->win_ni++] = bytes[i];
            }
        }
        if (!cfg->no_event_probes && cfg->hooks && cfg->hooks->event_probe && m->rom_index(m, pc) >= 0) {
            uint64_t pkey = (uint64_t)m->rom_index(m, pc) + 1;
            int fresh = 0;
            if (!kc_known(w, pkey)) {
                pthread_mutex_lock(&sh->mu);
                fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, pkey);
                pthread_mutex_unlock(&sh->mu);
                kc_add(w, pkey);
            }
            int rearm = 0;
            int64_t pri = m->rom_index(m, pc);
            if (!fresh && ctx_rearm(w, cfg, loc, m, pri, 11, 0, w->visits[vi])) fresh = rearm = 1;
            // B1: a site that produced probes is re-armed when it is reached under another semantic cell (the RAM the
            // code has been seen to read), up to probe_ctx_cap contexts per site (saturation counted, never silent)
            if (fresh) {
                unsigned k = 0;
                m->pad_full = cfg->pad_full;
                uint8_t *snap = malloc(sh->sbytes);
                int any = 0;
                while (snap && cfg->hooks->event_probe(cfg->hooks->user, m, pc, bytes, k++, snap)) {
                    any = 1;
                    pthread_mutex_lock(&sh->mu);
                    {
                        queue_push(sh, m, (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input, 0, 0, DP_SRC_PROBE });
                        loc->event_probes++;
                        pthread_cond_signal(&sh->cv);
                        snap = malloc(sh->sbytes);
                    }
                    pthread_mutex_unlock(&sh->mu);
                }
                free(snap);
                loc->event_probe_arms++;   // P9: an arm, not a variant — variants are counted per queued state below
                if (any) ctx_note(sh, pri);
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
                int64_t iri = (int64_t)m->rom_index(m, pc);
                uint64_t ikey = ((uint64_t)m->rom_index(m, pc) + 1) | ((uint64_t)d << 40);
                int known = 0, rearmed = 0;   // (site, delay) already forked: skip the costly hook and the 17 KiB snapshot
                int hooked = 0;   // the hook already ran into w->irqscratch (nothing pending => no shared state is touched)
                if (kc_known(w, ikey)) known = 1;
                else {
                    if (!w->irqscratch) w->irqscratch = malloc(sh->sbytes);
                    if (!w->irqscratch || !cfg->hooks->irq_alt(cfg->hooks->user, m, d, w->irqscratch)) break;
                    hooked = 1;
                    pthread_mutex_lock(&sh->mu);
                    if (sh->probecap) { size_t h = keyhash(ikey) & (sh->probecap - 1); while (sh->probeset[h]) { if (sh->probeset[h] == ikey) { known = 1; break; } h = (h + 1) & (sh->probecap - 1); } }
                    pthread_mutex_unlock(&sh->mu);
                    if (known) kc_add(w, ikey);
                }
                if (known) { if (ctx_rearm(w, cfg, loc, m, iri, 13, (uint64_t)d << 40, w->visits[vi])) rearmed = 1; else continue; }   // A2: one more interrupt landing under a new semantic cell
                uint8_t *snap = malloc(sh->sbytes);
                if (!snap) break;
                if (hooked) memcpy(snap, w->irqscratch, sh->sbytes);
                else if (!cfg->hooks->irq_alt(cfg->hooks->user, m, d, snap)) { free(snap); break; }
                pthread_mutex_lock(&sh->mu);
                int fresh = rearmed || keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ikey);
                if (fresh) kc_add(w, ikey);
                if (!fresh) free(snap);
                else {
                    queue_push(sh, m, (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input, 0, 0, DP_SRC_IRQ });
                    if (d == 1) loc->irq_forks++; else loc->jitter_forks++;
                    ctx_note(sh, iri);   // A2: the context counts as productive
                    pthread_cond_signal(&sh->cv);
                }
                pthread_mutex_unlock(&sh->mu);
            }
        }
        int stack_cond = have_flow && fi.cond >= 0 && m->invert_condition &&
                         ((fi.kind == DP_FLOW_CALL && fi.call) ||
                          (fi.kind == DP_FLOW_COND && fi.target < 0));
        int cond = have_flow && fi.kind == DP_FLOW_COND && fi.target >= 0;
        if (cond && cfg->fork_site_cap && !cur_epoch && w->capped[site_slot(m, pc, w->site_n, NULL)] == skey) { loc->sat_site++; cond = 0; }   // site already saturated: no snapshot, no lock
        uint8_t *pre = NULL;
        // both sides already covered by natural paths: the fork is pruned below whatever side is taken, so the
        // full-state snapshot is not made (pruned_covered is counted all the same)
        int pre_skip = cond && !stack_cond && covered_natural(loc, m, pc + (uint32_t)len) && covered_natural(loc, m, (uint32_t)fi.target);
        if ((cond || stack_cond) && !pre_skip) {
            pre = malloc(sh->sbytes);
            if (pre) m->save(m, pre);
            else { cond = 0; stack_cond = 0; }
        }
        if (!m->run(m)) { free(pre); break; }
        if (stack_cond) {
            uint8_t *post = malloc(sh->sbytes);
            if (post) {
                uint32_t actual = m->pc(m);
                m->save(m, post);
                m->load(m, pre);
                CopyObs saved = w->cobs;
                w->cobs.forced = 1;
                if (m->invert_condition(m, fi.cond) && m->run(m)) {
                    uint32_t other = m->pc(m);
                    if (other != actual && !(covered(loc, m, other) && covered(loc, m, actual)) &&
                        (!cfg->hooks || !cfg->hooks->veto_fork || cfg->hooks->veto_fork(cfg->hooks->user, m, pc, other))) {
                        uint8_t *alt = malloc(sh->sbytes);
                        if (alt) {
                            m->save(m, alt);
                            Pending q = { alt, other, 1, skey, cur_input, 0, 0, cur_origin, cur_epoch };
                            q.call_depth = cur_forced ? fdepth : 0;
                            if (other != pc + (uint32_t)len) {
                                if (fi.call) q.call_depth++;
                                else if (q.call_depth) q.call_depth--;
                            }
                            int defer = 0;
                            pthread_mutex_lock(&sh->mu);
                            uint32_t *cnt = site_count(&sh->sites, skey ^ ((uint32_t)cur_epoch * 0x9E3779B1u));
                            if (!cfg->fork_site_cap || *cnt < cfg->fork_site_cap) {
                                queue_push(sh, m, q);
                                (*cnt)++; loc->forks++;
                                pthread_cond_signal(&sh->cv);
                            } else { loc->sat_site++; defer = 1; }
                            pthread_mutex_unlock(&sh->mu);
                            if (defer) defer_state(sh, m, loc, q);
                        }
                    }
                }
                w->cobs = saved;
                m->load(m, post);
                free(post);
            }
            free(pre); pre = NULL;
        }
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
                    uint64_t key = (keyhash(bk) & ((1ull << 60) - 1)) | (13ull << 60);
                    pthread_mutex_lock(&sh->mu);
                    int known = 0;
                    if (sh->probecap) {
                        size_t h = keyhash(key) & (sh->probecap - 1);
                        while (sh->probeset[h]) {
                            if (sh->probeset[h] == key) { known = 1; break; }
                            h = (h + 1) & (sh->probecap - 1);
                        }
                    }
                    pthread_mutex_unlock(&sh->mu);
                    if (known) continue;
                    int over_cap = cfg->bank_fork_cap && made >= cfg->bank_fork_cap;
                    if (over_cap) {
                        loc->bank_fork_sat++;
                        if (!cfg->state_dir) break;
                    }
                    uint8_t *snap = malloc(sh->sbytes);
                    if (!snap) break;
                    int ok = m->bank_view(m, b);
                    if (ok) m->save(m, snap);
                    if (!ok) { m->load(m, cur); free(snap); continue; }
                    if (over_cap) {
                        Pending q = { snap, m->pc(m), 0, (uint32_t)(site + 1), cur_input, 0, 0, DP_SRC_BANK };
                        defer_state(sh, m, loc, q);
                        m->load(m, cur);
                        continue;
                    }
                    pthread_mutex_lock(&sh->mu);
                    int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, key);
                    if (fresh) {
                        queue_push(sh, m, (Pending){ snap, m->pc(m), 0, (uint32_t)(site + 1), cur_input, 0, 0, DP_SRC_BANK });
                        pthread_cond_signal(&sh->cv);
                    } else free(snap);
                    pthread_mutex_unlock(&sh->mu);
                    m->load(m, cur);
                    if (fresh) { loc->bank_forks++; made++; }
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
                uint64_t pk = 0;
                if (!cfg->no_proj && m->ctx_reg_hash && loc->live && w->pcache) {   // A5: the memo before the locks: a (site, projection) key once spent stays spent (the shared set only grows)
                    pk = proj_key(loc, m, pc, other) ^ 0xC11C11C11ull;
                    if (w->pcache[keyhash(pk) & (w->pcap - 1)] == pk + 1) { loc->proj_pruned++; free(pre); continue; }
                }
                pthread_mutex_lock(&sh->mu);
                int64_t sk_ = m->rom_index(m, pc);
                uint32_t *cnt = site_count(&sh->sites, (sk_ >= 0 ? (uint32_t)sk_ + 1u : (pc | 0x80000000u)) ^ ((uint32_t)cur_epoch * 0x9E3779B1u));   // B3: the cap restarts in each checkpoint epoch   // A5: per ROM site (bank-aware), not per pc
                int pushed = 0;
                if (!cfg->no_proj && m->ctx_reg_hash && loc->live && w->pcache) {   // A5: the memo before the locks: a (site, projection) key once spent stays spent (the shared set only grows)
                    uint64_t pk = proj_key(loc, m, pc, other) ^ 0xC11C11C11ull;
                    if (w->pcache[keyhash(pk) & (w->pcap - 1)] == pk + 1) { loc->proj_pruned++; pthread_mutex_unlock(&sh->mu); free(pre); continue; }
                }
                if (cfg->cell_hi > cfg->cell_lo || !cfg->no_cells) {
                    if (cell_insert(sh, cell_key(cfg, sh->ramread, m, other))) loc->cells++;
                    else { loc->cell_pruned++; pthread_mutex_unlock(&sh->mu); free(pre); continue; }
                }
                if (!cfg->no_proj && m->ctx_reg_hash && loc->live) {   // C11: the same target under the same projected context was queued already
                    if (!cell_insert(sh, proj_key(loc, m, pc, other) ^ 0xC11C11C11ull)) { loc->proj_pruned++; pthread_mutex_unlock(&sh->mu); free(pre); continue; }
                    else if (w->pcache && pk) w->pcache[keyhash(pk) & (w->pcap - 1)] = pk + 1;   // A5: remember the freshly spent key
                }
                if (cfg->fork_site_cap && *cnt >= cfg->fork_site_cap) {
                    loc->sat_site++;
                    if (!cur_epoch) {
                        w->capped[site_slot(m, pc, w->site_n, NULL)] = skey;   // the fast path above skips the site from now on
                        pthread_mutex_unlock(&sh->mu);   // defer_state takes the lock itself
                        defer_state(sh, m, loc, (Pending){ pre, other, 1, (uint32_t)(m->rom_index(m, pc) + 1), cur_input, 0, 0, cur_origin, cur_epoch });
                        pre = NULL;   // P7: the capped fork joins the deferred frontier (the defer owns the snapshot; it is kept for --resume, never re-run here)
                        pthread_mutex_lock(&sh->mu);
                    }
                }
                else {
                    queue_push(sh, m, (Pending){ pre, other, 1, (uint32_t)(m->rom_index(m, pc) + 1), cur_input, 0, 0, cur_origin, cur_epoch });
                    (*cnt)++;
                    if (cur_epoch && cfg->fork_site_cap && *site_count(&sh->sites, sk_ >= 0 ? (uint32_t)sk_ + 1u : (pc | 0x80000000u)) >= cfg->fork_site_cap) loc->epoch_forks++;   // B3: a fork the cap would have refused outside any epoch
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
    time_t t0 = time(NULL); long last_snap = 0;
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
        if (sh->cfg->snap_cov && sh->cfg->snap_secs && sh->snap_m && el - last_snap >= (long)sh->cfg->snap_secs) {   // periodic coverage snapshot: a crash or a kill no longer loses the run
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s.tmp", sh->cfg->snap_cov);
            dp_core_write_cov(tmp, "snapshot", sh->snap_m, sh->out);
            int ok = rename(tmp, sh->cfg->snap_cov) == 0;
            last_snap = el;
            fprintf(stderr, "[%ldm%02lds] snapshot %s: %s (natural %zu, forced %zu)\n", el / 60, el % 60, sh->cfg->snap_cov, ok ? "written" : "FAILED", nat, forced);
        }
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
        int ltab = i && r->inferred[i - 1] == DP_MAP_TABLE, rtab = j < n && r->inferred[j] == DP_MAP_TABLE;
        if (len <= 3 && (lf & code) && (rf & code)) c = DP_MAP_INF_CODE;
        else if (len <= 1024 && lpad && (j >= n || rpad || !(rf & any))) c = DP_MAP_INF_PAD;
        else if (len <= 1024 && ((lf & any) || (rf & any))) c = DP_MAP_INF_DATA;
        /* The residual shapes of a long campaign (Zelda US: the last unknown
         * bytes): a small gap inside a run of table entries — the period
         * detection missed an entry — stays table; and a data gap bounded by
         * another inference (inferred-data blob tail, table|padding or
         * table|table boundary) with no kernel-classified code on either side
         * stays inferred data — the same data-continuity shape the report
         * level has always inferred (data neighbours, up to 1024).  All are
         * inferences, never observations. */
        else if (len <= 16 && ltab && rtab) c = DP_MAP_TABLE;
        else if (len <= 1024 && !((lf | rf) & code) &&
                 ((i && (r->inferred[i - 1] == DP_MAP_INF_DATA || r->inferred[i - 1] == DP_MAP_TABLE)) ||
                  (j < n && (r->inferred[j] == DP_MAP_INF_DATA || r->inferred[j] == DP_MAP_TABLE))))
            c = DP_MAP_INF_DATA;
        if (c) {
            for (size_t k = i; k < j; k++) r->inferred[k] = c;
            if (c == DP_MAP_INF_CODE) r->inferred_code += len; else if (c == DP_MAP_INF_PAD) r->inferred_pad += len; else if (c == DP_MAP_TABLE) r->jt_bytes += len; else r->inferred_data += len;
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
    DpObservers obsv = { NULL, obs_read, obs_write, obs_copy, obs_io, obs_mode };
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
            if (!sh->qn && sh->spool_n && !spool_refill(sh, 0)) {
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
        // An unlimited run must not starve disk states while its in-memory
        // queue keeps growing. Periodically run the oldest spilled state
        // directly, even when novelty/session priorities have work ready.
        size_t spool_pick = SIZE_MAX;
        if (cfg->unbounded && sh->spool_n && ++sh->picks % 32 == 0) {
            spool_pick = sh->qn;
            if (!spool_refill(sh, 1)) {
                sh->spool_read_failed = 1;
                __atomic_store_n(&sh->halt, 3, __ATOMIC_RELAXED);
                pthread_cond_broadcast(&sh->cv);
                continue;
            }
        }
        size_t pick = spool_pick != SIZE_MAX ? spool_pick : sh->qn - 1;
        w->loc.in_novelty_path = 0;
        int session_first = 0;
        for (size_t i = spool_pick != SIZE_MAX ? 0 : sh->qn; i-- > 0;)   // the few long sessions (attract / play) start before the fork backlog
            if (sh->q[i].input >= SESSION_ATTRACT) { pick = i; session_first = 1; break; }
        if (spool_pick == SIZE_MAX && !session_first && !cfg->no_novelty_order) {
            double best = -1;
            for (size_t i = sh->qn; i-- > 0;) {
                if (sh->q[i].origin != DP_SRC_SESSION) {   // B3: a live checkpoint resumes from covered code by design; its cell's visit count orders it
                    uint32_t tri = sh->q[i].tri;   // P2/A19: the index stored at push, not the worker's current mapping
                    if (tri && tri - 1 < sh->out->rom_size
                        ? (__atomic_load_n(&sh->out->flags[tri - 1], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED))
                        : covered(&w->loc, w->m, sh->q[i].pc)) continue;
                }
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

// One appended "magic size entries" section of the coverage file: NULL when
// absent or short.  The tail reads strictly in write order — one miss ends it,
// later sections cannot align anymore.
static void *cov_tail_section(FILE *cf, uint32_t magic, size_t n, size_t esz, int *aligned) {
    if (!*aligned) return NULL;
    uint32_t hd[2];
    if (fread(hd, 1, sizeof hd, cf) != sizeof hd || hd[0] != magic || hd[1] != n) { *aligned = 0; return NULL; }
    void *p = malloc(n ? n * esz : 1);
    if (!p || fread(p, esz, n, cf) != n) { free(p); *aligned = 0; return NULL; }
    return p;
}

// Bind future resumes to the ROM and backend that produced the checkpoint.
// Legacy checkpoints without this file remain readable, with a warning.
static int state_identity(DpMachine *m, const DpCoreConfig *cfg) {
    if (!cfg->state_dir || !cfg->rom_image || !cfg->rom_image_size) return 1;
    if (mkdir(cfg->state_dir, 0777) != 0 && errno != EEXIST) return 0;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/identity.bin", cfg->state_dir) >= (int)sizeof path) return 0;
    struct Identity { uint8_t magic[8], sha1[20]; uint64_t size; uint32_t state_size; char machine[32], isa[32]; } id = {0}, old;
    memcpy(id.magic, "DPID0001", 8);
    dp_sha1(cfg->rom_image, cfg->rom_image_size, id.sha1);
    id.size = cfg->rom_image_size;
    id.state_size = m->state_bytes(m);
    snprintf(id.machine, sizeof id.machine, "%s", m->name ? m->name : "");
    snprintf(id.isa, sizeof id.isa, "%s", m->isa->name ? m->isa->name : "");
    FILE *f = fopen(path, "rb");
    if (f) {
        int ok = fread(&old, 1, sizeof old, f) == sizeof old && fgetc(f) == EOF && !ferror(f);
        fclose(f);
        if (!ok || memcmp(&old, &id, sizeof id)) {
            fprintf(stderr, "deepeel: state directory identity does not match ROM or machine: %s\n", path);
            return 0;
        }
        return 1;
    }
    if (errno != ENOENT) { perror("deepeel: cannot read state identity"); return 0; }
    if (cfg->resume) fprintf(stderr, "deepeel: legacy checkpoint has no ROM identity; resume cannot be verified\n");
    char temp[1050];
    if (snprintf(temp, sizeof temp, "%s.tmp-XXXXXX", path) >= (int)sizeof temp) return 0;
    int fd = mkstemp(temp);
    if (fd < 0) return 0;
    f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(temp); return 0; }
    int ok = fwrite(&id, 1, sizeof id, f) == sizeof id;
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(temp, path) != 0) ok = 0;
    if (!ok) { unlink(temp); fprintf(stderr, "deepeel: cannot save state identity\n"); }
    return ok;
}

// RAM the explorer may still take: MemAvailable, lowered to the room left under the cgroup limit
// (systemd scope MemoryMax, container).  Bytes; 0 when it cannot be read.
uint64_t dp_mem_budget(void) {
    uint64_t avail = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    char ln[160];
    while (f && fgets(ln, sizeof ln, f)) { unsigned long kb; if (sscanf(ln, "MemAvailable: %lu kB", &kb) == 1) { avail = (uint64_t)kb * 1024u; break; } }
    if (f) fclose(f);
    char cg[256] = "";
    f = fopen("/proc/self/cgroup", "r");
    while (f && fgets(ln, sizeof ln, f)) if (!strncmp(ln, "0::", 3)) { snprintf(cg, sizeof cg, "%s", ln + 3); cg[strcspn(cg, "\n")] = 0; break; }
    if (f) fclose(f);
    if (cg[0]) {
        char path[512]; unsigned long long lim = 0, cur = 0; int have = 0;
        snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.max", cg);
        f = fopen(path, "r"); if (f) { have = fscanf(f, "%llu", &lim) == 1; fclose(f); }
        snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.current", cg);
        f = fopen(path, "r"); if (f) { if (fscanf(f, "%llu", &cur) != 1) have = 0; fclose(f); }
        if (have && lim > cur && lim - cur < avail) avail = lim - cur;
        else if (have && lim <= cur) avail = 0;
    }
    return avail;
}

int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg_in, DpCoreResult *out) {
    DpCoreConfig cfg_unb = *cfg_in;
    if (cfg_unb.unbounded) {
        cfg_unb.max_total = 0; cfg_unb.fork_site_cap = 0; cfg_unb.bank_fork_cap = 0;   // A8: max_path_steps (the platform path shape) is kept, not lifted
        cfg_unb.visits_per_pc = 0; cfg_unb.stall_secs = 0; cfg_unb.max_secs = 0;   // A8: the platform path shape (max_path_steps) is kept; the only stops stay exhaustion or SIGINT (the owner's contract)
        cfg_unb.no_cells = 1; cfg_unb.cell_lo = cfg_unb.cell_hi = 0;
        cfg_unb.no_proj = 1;  // learned liveness is not proof of future non-dependence
        cfg_unb.no_taint_cut = cfg_unb.no_forced_ram_cut = 1;
    }
    const DpCoreConfig *cfg = &cfg_unb;
    memset(out, 0, sizeof *out);
    // the classification products a previous run earned (execution hits, the
    // inferred classes, the probable-code walks, the typed destinations),
    // reloaded by --resume and merged after the fresh inference: a resumed
    // report never knows less
    uint32_t *sv_hits = NULL; uint8_t *sv_inf = NULL, *sv_prov = NULL, *sv_dest = NULL;
    out->rom_size = rom_size;
    if (!m || !m->isa || !m->run || !m->save || !m->load || !m->fetch || !m->rom_index) return 0;
    if (!m->state_bytes || !state_identity(m, cfg)) return 0;
    if (!m->pc || !m->set_pc) { out->no_pc = 1; return 1; }
    out->flags = calloc(rom_size ? rom_size : 1, 1);
    out->prov = calloc(rom_size ? rom_size : 1, 1);
    out->dest = calloc(rom_size ? rom_size : 1, 1);
    out->spec = calloc(rom_size ? rom_size : 1, 1);
    out->refs = calloc(rom_size ? rom_size : 1, 1);
    out->live_w = ((rsize(m) >> 8) + 31) / 32;
    out->live = calloc(((rom_size >> 8) + 1) * out->live_w, sizeof *out->live);
    if (m->plat && m->plat->io_reg_count > 0) {   // typed register accesses: one slot per registry entry
        out->reg_n = m->plat->io_reg_count; out->reg_plat = m->plat; out->closure_banks = !cfg->no_closure_banks; out->closure_ret = !cfg->no_closure_ret;
        out->closure_regs = !cfg->no_closure_regs;
        out->reg_slot = m->plat->find ? NULL : calloc(65536, sizeof *out->reg_slot);
        out->reg_reads = calloc((size_t)out->reg_n, sizeof *out->reg_reads);
        out->reg_writes = calloc((size_t)out->reg_n, sizeof *out->reg_writes);
        out->reg_vals = calloc((size_t)out->reg_n * 8, sizeof *out->reg_vals);
        if ((!out->reg_slot && !m->plat->find) || !out->reg_reads || !out->reg_writes || !out->reg_vals) { dp_core_result_free(out); return 0; }
        for (int ri = 0; out->reg_slot && ri < out->reg_n; ri++) {
            const DpIoReg *g = &m->plat->io_regs[ri];
            for (uint32_t a = g->lo; a <= g->hi && a < 65536; a++) if (!out->reg_slot[a]) out->reg_slot[a] = (uint16_t)(ri + 1);
        }
    }
    out->page_win = calloc((rom_size >> 8) + 2, 1);   // one byte per 256 bytes: enough for any page_shift >= 8
    out->src_flags = calloc(rom_size ? rom_size : 1, sizeof *out->src_flags);
    out->first_seen = calloc(rom_size ? rom_size : 1, sizeof *out->first_seen);
    out->first_actor = calloc(rom_size ? rom_size : 1, 1);
    out->hits = calloc(rom_size ? rom_size : 1, sizeof *out->hits);
    if (!out->flags || !out->prov || !out->dest || !out->src_flags || !out->first_seen || !out->first_actor || !out->hits) { dp_core_result_free(out); return 0; }
    if (cfg->on_live) cfg->on_live(cfg->on_live_user, out);
    else if (g_live_cb) g_live_cb(g_live_user, out);

    Shared sh;
    memset(&sh, 0, sizeof sh);
    pthread_mutex_init(&sh.mu, NULL);
    pthread_cond_init(&sh.cv, NULL);
    sh.out = out; sh.cfg = cfg; sh.snap_m = m;
    sh.sbytes = m->state_bytes(m);
    if (cfg_unb.queue_auto) {   // the queue holds whole machine snapshots: bound it by what the RAM can carry, the rest spills to the disk spool
        uint64_t budget = dp_mem_budget();
        if (budget) {
            uint64_t per = (uint64_t)sh.sbytes + 128u, cap = budget * 4u / 10u / per;
            if (cap < 256) cap = 256;
            if (!cfg_unb.queue_max || cap < cfg_unb.queue_max) {
                fprintf(stderr, "deepeel: queue bounded to %llu states in RAM (%u KiB each, %llu MiB available); the rest goes to the spool\n",
                        (unsigned long long)cap, (unsigned)(sh.sbytes / 1024u), (unsigned long long)(budget >> 20));
                cfg_unb.queue_max = (size_t)cap;
            }
        }
    }
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
        w->loc.refs = out->refs; w->loc.page_win = out->page_win; w->loc.live = out->live; w->loc.reg_slot = out->reg_slot; w->loc.reg_reads = out->reg_reads; w->loc.reg_writes = out->reg_writes; w->loc.reg_vals = out->reg_vals; w->loc.reg_n = out->reg_n; w->loc.reg_plat = out->reg_plat; w->loc.closure_banks = out->closure_banks; w->loc.closure_ret = out->closure_ret; w->loc.live_w = out->live_w; w->loc.flags = out->flags; w->loc.src_flags = out->src_flags; w->loc.hits = out->hits;
        w->loc.dest = out->dest;
        w->loc.prov = out->prov;
        w->loc.first_seen = out->first_seen; w->loc.first_actor = out->first_actor; w->loc.rom_size = rom_size;
        w->site_n = site_n;
        w->visits = calloc(site_n, sizeof *w->visits);
        w->capped = calloc(site_n, sizeof *w->capped);
        w->stamp = calloc(site_n, sizeof *w->stamp);
        w->wsnap = malloc(sh.sbytes ? sh.sbytes : 1); w->winmemo = calloc(0x30000, sizeof *w->winmemo);
        w->pcache = calloc(4096, sizeof *w->pcache); w->pcap = 4096;   // A5: 32 KiB of spent (site, projection) keys per worker
        w->ckm_ri = calloc(4096, sizeof *w->ckm_ri); w->ckm_ep = calloc(4096, sizeof *w->ckm_ep);   // A2: per-ri cell-key memo
        w->ckm_ck = calloc(4096, sizeof *w->ckm_ck); w->ckspent = calloc(4096, sizeof *w->ckspent);
        w->rng_stamp = calloc(site_n, sizeof *w->rng_stamp); w->rng_last = calloc(site_n, 1);
        // C3: the regstate-aware closure state (the seed set grows by rehashing to REG_SET_CEIL; --unbounded lifts the per-site caps)
        uint32_t rwm = 1; while (rwm < rom_size && rwm < (1u << 23)) rwm <<= 1;   // direct-mapped by rom index, masked
        w->regwalk = calloc(1, sizeof *w->regwalk);
        if (w->regwalk) *w->regwalk = (RegWalk){ .r = &w->loc, .cfg = cfg, .rom_size = rom_size, .mask = rwm - 1,
            .set = calloc(REG_SET_CAP, sizeof *w->regwalk->set), .set_cap = REG_SET_CAP,
            .site_seeds = calloc(rwm, 1), .ind_vals = calloc(rwm, 1), .provd = calloc((rom_size >> 3) + 1, 1) };
        if (!w->visits || !w->stamp || !w->capped || !w->regwalk || !w->regwalk->set ||
            !w->regwalk->site_seeds || !w->regwalk->ind_vals || !w->regwalk->provd ||
            !w->ckm_ri || !w->ckm_ep || !w->ckm_ck || !w->ckspent) { ok = 0; break; }
        w->cobs = (CopyObs){ .m = wm, .r = &w->loc, .last_rom = -1, .fed = sh.fed, .age = 99, .run_lo = -1, .run_hi = -1, .no_decompress = cfg->no_decompress };
        w->cobs.fedsrc = sh.fedsrc; w->cobs.fedprod = sh.fedprod;
        w->cobs.ramread = sh.ramread;
        w->cobs.ctx_epoch = &sh.ctx_epoch;   // A2: the shared cell-context epoch
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
                    // P13/A9: the first-cover actors, an appended section (absent in older files)
                    uint32_t act[2] = {0, 0};
                    if (fread(act, 1, sizeof act, cf) == sizeof act && act[0] == 0x31524F41u && act[1] == rom_size
                        && fread(out->first_actor, 1, rom_size, cf) == rom_size)
                        fprintf(stderr, "deepeel: resumed first-cover actors\n");
                    // the classification products, appended the same way: the
                    // execution hits (the heat), the inferred classes, the
                    // probable-code walks, the typed data destinations
                    int aligned = act[0] == 0x31524F41u;   // the tail follows the actor section
                    sv_hits = cov_tail_section(cf, 0x31544948u, rom_size, sizeof *out->hits, &aligned);
                    sv_inf = cov_tail_section(cf, 0x31464E49u, rom_size, 1, &aligned);
                    sv_prov = cov_tail_section(cf, 0x31565250u, rom_size, 1, &aligned);
                    sv_dest = cov_tail_section(cf, 0x31545344u, rom_size, 1, &aligned);
                    if (sv_hits) fprintf(stderr, "deepeel: resumed execution hits (the heat)\n");
                    if (sv_inf) fprintf(stderr, "deepeel: resumed inferred classes\n");
                    if (sv_prov) fprintf(stderr, "deepeel: resumed probable-code marks\n");
                    if (sv_dest) fprintf(stderr, "deepeel: resumed typed destinations\n");
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
        if (sh.celln && cfg->unbounded)
            fprintf(stderr, "deepeel: resumed %zu archived cells; heuristic pruning remains disabled under --unbounded\n", sh.celln);
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
    if (cfg->cov_hits_path && *cfg->cov_hits_path) {   // --cov-hits: the heat of another campaign (or this one's own export), never lowered
        FILE *hf = fopen(cfg->cov_hits_path, "r");
        if (!hf) perror("deepeel: --cov-hits");
        else {
            char hl[128];
            size_t n = 0;
            while (fgets(hl, sizeof hl, hf)) {
                if (hl[0] == '#' || hl[0] == '\n') continue;
                unsigned bank = 0, off = 0, flags = 0, hits = 0;
                if (sscanf(hl, "%x %x %x %u", &bank, &off, &flags, &hits) < 4 || !hits) continue;
                if ((off >= 0x4000u) != (bank != 0)) continue;   // the v1 geometry
                size_t idx = ((size_t)bank << 14) | (off & 0x3fffu);
                if (idx >= rom_size) continue;
                if (hits > out->hits[idx]) { out->hits[idx] = hits; n++; }
            }
            fclose(hf);
            fprintf(stderr, "deepeel: --cov-hits: %zu execution-hit maxima loaded\n", n);
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
            if (sh.exactn) fprintf(stderr, "deepeel: resumed %zu exact states for the dedup set\n", sh.exactn);   // the acceleration must be visible, never implicit
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
            queue_push(&sh, m, (Pending){ cp, cfg->seeds[i], 1, 0, 0, 1, 0, prep ? DP_SRC_SEEDRAM : 0 });
            out->seeded++;
        }
        if (cfg->cov_transfer_rom && *cfg->cov_transfer_rom && cfg->cov_transfer_cov && *cfg->cov_transfer_cov) {
            // --cov-transfer (generic): the other revision's coverage replayed as
            // forced-path seeds at the same compact index where the four-byte
            // instruction windows match byte for byte.  Nothing is marked known by
            // the transfer itself: this ROM proves its own bytes by running them.
            uint32_t *tpcs = NULL; uint16_t *tbks = NULL;
            uint64_t tlines = 0, tmatched = 0;
            size_t tn = dp_core_transfer_seeds(cfg->rom_image ? cfg->rom_image : NULL, rom_size,
                                               cfg->cov_transfer_rom, cfg->cov_transfer_cov, &tpcs, &tbks, &tlines, &tmatched);
            if (tn && cfg->rom_image) {
                for (size_t i = 0; i < tn; i++) {
                    uint8_t *cp = malloc(sh.sbytes);
                    if (!cp) break;
                    memcpy(cp, start, sh.sbytes);
                    int prep = 0;
                    if (m->bank_view) {
                        if (m->bank_view(m, tbks[i])) prep = 1;
                        else out->seed_banks_absent++;
                    } else if (tbks[i] != 0 && tbks[i] != 0xFFFF) out->seed_banks_absent++;
                    if (prep) { m->save(m, cp); m->load(m, start); }
                    queue_push(&sh, m, (Pending){ cp, tpcs[i], 1, 0, 0, 1, 0, prep ? DP_SRC_SEEDRAM : 0 });
                    out->seeded++;
                }
                fprintf(stderr, "deepeel: --cov-transfer: %llu of %llu coverage lines byte-identical — %zu seeds queued\n",
                        (unsigned long long)tmatched, (unsigned long long)tlines, tn);
            }
            free(tpcs); free(tbks);
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
    if (sv_inf && !out->inferred) out->inferred = calloc(rom_size ? rom_size : 1, 1);
    if (sv_inf || sv_prov || sv_dest || sv_hits) {   // the reloaded products fill what this pass left unknown: never less
        for (size_t i = 0; i < rom_size; i++) {
            if (sv_inf && out->inferred && !out->inferred[i]) out->inferred[i] = sv_inf[i];
            if (sv_prov && !out->prov[i]) out->prov[i] = sv_prov[i];
            if (sv_dest && !out->dest[i]) out->dest[i] = sv_dest[i];
            if (sv_hits && sv_hits[i] > out->hits[i]) out->hits[i] = sv_hits[i];
        }
        free(sv_hits); free(sv_inf); free(sv_prov); free(sv_dest);
        sv_hits = NULL; sv_inf = sv_prov = sv_dest = NULL;
    }
    for (unsigned i = 0; i < started; i++) {
        DpCoreResult *l = &ws[i].loc;
        out->paths += l->paths; out->forks += l->forks;
        out->cooperative_yields += l->cooperative_yields;
        out->sat_total += l->sat_total; out->sat_path += l->sat_path; out->sat_loop += l->sat_loop;
        out->attract_paths += l->attract_paths; out->play_paths += l->play_paths; out->play_epochs += l->play_epochs; out->session_frames += l->session_frames; out->session_steps += l->session_steps; out->session_stalls += l->session_stalls;
        out->sat_site += l->sat_site;
        out->pruned_covered += l->pruned_covered; out->copy_bytes += l->copy_bytes;
        out->copy_forced += l->copy_forced; out->forced_ram_cuts += l->forced_ram_cuts;
        out->nonsense_pc += l->nonsense_pc; out->nonsense_sp += l->nonsense_sp; out->pad_cuts += l->pad_cuts; out->pad_unmarked += l->pad_unmarked;
        out->taint_cut_indirect += l->taint_cut_indirect; out->taint_cut_ret += l->taint_cut_ret;
        out->novelty_picks += l->novelty_picks; out->closure_stopped_data += l->closure_stopped_data; out->closure_entry_refused += l->closure_entry_refused; out->closure_stack_invalidations += l->closure_stack_invalidations; out->closure_unresolved += l->closure_unresolved; out->closure_stack_dropped += l->closure_stack_dropped; out->site_scored_picks += l->site_scored_picks; out->learned_sigs += l->learned_sigs; out->solved_forks += l->solved_forks; out->promoted_forced += l->promoted_forced; out->solve_tries += l->solve_tries; out->rng_forks += l->rng_forks; out->index_replays += l->index_replays; out->compressed_bytes += l->compressed_bytes; out->decompress_runs += l->decompress_runs; out->cells += l->cells; out->closure_roots += l->closure_roots; out->closure_bank_sims += l->closure_bank_sims; out->closure_bytes += l->closure_bytes; out->closure_capped += l->closure_capped; out->closure_stack_dropped += l->closure_stack_dropped; out->pending_dropped += l->pending_dropped; out->deferred += l->deferred; out->state_repeats += l->state_repeats; out->probe_ctx_sat += l->probe_ctx_sat; out->probe_rearms += l->probe_rearms; out->bank_forks += l->bank_forks; out->tas_ckpts += l->tas_ckpts; out->sess_ckpts += l->sess_ckpts; out->ckpt_bursts += l->ckpt_bursts; out->epoch_forks += l->epoch_forks; out->live_seeds += l->live_seeds; out->imm_refs += l->imm_refs; out->held_stops += l->held_stops; out->bank_fork_sat += l->bank_fork_sat; out->closure_jp_tables += l->closure_jp_tables; out->closure_jp_entries += l->closure_jp_entries; out->prov_data_bytes += l->prov_data_bytes; out->event_probes += l->event_probes; out->event_probe_sites += l->event_probe_sites; out->event_probe_arms += l->event_probe_arms; out->irq_forks += l->irq_forks; out->jitter_forks += l->jitter_forks; out->cell_pruned += l->cell_pruned; out->closure_rets += l->closure_rets; out->proj_pruned += l->proj_pruned; out->novelty_new_bytes += l->novelty_new_bytes; out->plain_new_bytes += l->plain_new_bytes; out->hook_vetoed += l->hook_vetoed; out->tas_paths += l->tas_paths;
        out->exec_outside += l->exec_outside; out->exec_wram_rom += l->exec_wram_rom; dp_ramarch_merge(&out->ra, &l->ra);
        out->closure_reg_roots += l->closure_reg_roots; out->closure_reg_seed_sat += l->closure_reg_seed_sat; out->closure_reg_set_sat += l->closure_reg_set_sat;
        out->closure_reg_indirect += l->closure_reg_indirect; out->closure_reg_indirect_sat += l->closure_reg_indirect_sat; out->closure_reg_miss += l->closure_reg_miss;
        out->closure_reg_stackrets += l->closure_reg_stackrets;
        free(l->jp_seen); l->jp_seen = NULL; free(ws[i].visits); free(ws[i].wsnap); free(ws[i].winmemo); free(ws[i].capped); free(ws[i].stamp); free(ws[i].pcache); free(ws[i].ckm_ri); free(ws[i].ckm_ep); free(ws[i].ckm_ck); free(ws[i].ckspent);
        if (ws[i].regwalk) { free(ws[i].regwalk->set); free(ws[i].regwalk->site_seeds); free(ws[i].regwalk->ind_vals); free(ws[i].regwalk->provd); free(ws[i].regwalk); }
        if (i && ws[i].m && ws[i].m->close) ws[i].m->close(ws[i].m);
    }
    out->steps = sh.steps;   // session steps are reported separately
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
            uint32_t act[2] = { 0x31524F41u, (uint32_t)rom_size };   // P13/A9: "AOR1", the first-cover actor per byte
            fwrite(act, sizeof *act, 2, cf); fwrite(out->first_actor, 1, rom_size, cf);   // appended: an older resume still reads the first three sections
            // the classification products of THIS run, appended the same way:
            // the execution hits (the heat), the inferred classes, the
            // probable-code walks, the typed data destinations — a later
            // resume re-exports the same map without re-exploring
            if (!out->inferred) out->inferred = calloc(rom_size ? rom_size : 1, 1);
            uint32_t hs[2] = { 0x31544948u, (uint32_t)rom_size };   // "HIT1"
            fwrite(hs, sizeof *hs, 2, cf); fwrite(out->hits, sizeof *out->hits, rom_size, cf);
            uint32_t isec[2] = { 0x31464E49u, (uint32_t)rom_size };   // "INF1"
            fwrite(isec, sizeof *isec, 2, cf); fwrite(out->inferred, 1, rom_size, cf);
            uint32_t psec[2] = { 0x31565250u, (uint32_t)rom_size };   // "PRV1"
            fwrite(psec, sizeof *psec, 2, cf); fwrite(out->prov, 1, rom_size, cf);
            uint32_t dsec[2] = { 0x31545344u, (uint32_t)rom_size };   // "DST1"
            fwrite(dsec, sizeof *dsec, 2, cf); fwrite(out->dest, 1, rom_size, cf);
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
        if (!frontier_save(&sh, fp, out))   // the map and the coverage are complete without it: only an exact resume of the pending states is lost
            fprintf(stderr, "deepeel: the pending frontier was not saved (disk space?); outputs are kept, a resume restarts from the coverage alone\n");
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
    if (!local.state_dir && (local.unbounded || local.resume || local.campaign)) {
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
    if (local.campaign && !local.max_secs && !local.max_total) local.max_secs = 120;   // the phase-1 bound defaults to two minutes, halt 1
    int ok = dp_core_explore(m, rom_size, &local, out);
    if (ok && local.campaign && out->saved && !core_cli_interrupted) {
        // Phase 2 of the validated recipe (Zelda: -31% unknown, +3 024 naturals, +14 348 copy bytes): the
        // same state dir, --resume --unbounded, to SIGINT or exhaustion.  The coverage arrays come back
        // OR-ed from the state dir, so phase 2's report already reads the whole campaign.
        dp_core_report(stderr, "campaign phase 1 (bounded)", out);   // one report per phase, never silent
        DpCoreConfig p2 = local;
        p2.resume = 1; p2.unbounded = 1; p2.campaign = 0;
        p2.max_secs = 0; p2.stall_secs = 0;   // phase 2 is unbounded: the only stops stay exhaustion or SIGINT (the owner's contract)
        DpCoreResult second;
        int ok2 = dp_core_explore(m, rom_size, &p2, &second);
        dp_core_result_free(out);   // the arrays are reloaded from the state dir by phase 2; phase 1's counters stay in its own report
        *out = second;
        if (!ok2) ok = 0;
    }
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
    fprintf(out, "saturation: total %llu path %llu loop %llu site %llu\n", U(r->sat_total), U(r->sat_path), U(r->sat_loop), U(r->sat_site));
    fprintf(out, "losses: closure_stack_dropped %llu pending_dropped %llu deferred %llu\nexact state: repeats cut %llu, states %llu\n", U(r->closure_stack_dropped), U(r->pending_dropped), U(r->deferred), U(r->state_repeats), U(r->exact_states));
    fprintf(out, "static walks stopped or retracted on data %llu (A13)\n", U(r->closure_stopped_data));
    fprintf(out, "static edges to targets outside the mapped ROM view %llu (C3)\n", U(r->closure_unresolved));
    fprintf(out, "index replays %llu\n", U(r->index_replays));
    fprintf(out, "rng-byte forks %llu\n", U(r->rng_forks));
    fprintf(out, "learned signatures %llu\n", U(r->learned_sigs));
    fprintf(out, "solved forks %llu (tries %llu, promoted from forced %llu)\n", U(r->solved_forks), U(r->solve_tries), U(r->promoted_forced));
    fprintf(out, "tas replay paths %llu\n", U(r->tas_paths));
    fprintf(out, "site-scored picks %llu\n", U(r->site_scored_picks));
    fprintf(out, "novelty picks %llu (new ROM bytes: novelty %llu, plain %llu)\n", U(r->novelty_picks), U(r->novelty_new_bytes), U(r->plain_new_bytes));
    fprintf(out, "workers %llu (short %llu)%s\n", U(r->workers), U(r->workers_short), r->workers_max ? "" : "");
    fprintf(out, "cells %llu, cell-pruned forks %llu, projection-pruned forks %llu\n", U(r->cells), U(r->cell_pruned), U(r->proj_pruned));
    fprintf(out, "deepeel: %llu (target, RAM) cells archived into the checkpoint pool\n", U(r->cells));
    fprintf(out, "decompress runs %llu, compressed ROM bytes %llu\n", U(r->decompress_runs), U(r->compressed_bytes));
    fprintf(out, "seeded RAM states %llu\n", U(r->seeded_ram));
    fprintf(out, "held-pad sessions %llu\n", U(r->sessions));
    fprintf(out, "IRQ forks %llu (timing jitter %llu)\n", U(r->irq_forks), U(r->jitter_forks));
    fprintf(out, "movie checkpoints continued as free paths: %llu (vector seeds from live states: %llu); session checkpoints continued as free paths: %llu, pad bursts from them: %llu\nforks past the per-site cap thanks to a checkpoint epoch %llu (B3)\n", U(r->tas_ckpts), U(r->live_seeds), U(r->sess_ckpts), U(r->ckpt_bursts), U(r->epoch_forks));
    fprintf(out, "held-pad session paths stopped by their own budget: %llu\n", U(r->held_stops));
    fprintf(out, "closure bank writes simulated during walks: %llu\n", U(r->closure_bank_sims));
    fprintf(out, "bank forks: %llu (site cap hit %llu); probe re-arms: %llu (context cap hit %llu)\n", U(r->bank_forks), U(r->bank_fork_sat), U(r->probe_rearms), U(r->probe_ctx_sat));
    fprintf(out, "event probes %llu at %llu sites: %llu forced variants\n", U(r->event_probe_arms), U(r->event_probe_sites), U(r->event_probes));
    fprintf(out, "seeded entry points %llu\n", U(r->seeded));
    if (r->reg_plat && r->reg_n) {   // typed register accesses (natural paths), by registry entry, then by category
        unsigned touched = 0;
        for (int i = 0; i < r->reg_n; i++) touched += r->reg_reads[i] || r->reg_writes[i];
        fprintf(out, "register accesses: %u of %d registry entries touched by natural paths\n", touched, r->reg_n);
        for (int i = 0; i < r->reg_n; i++) {
            if (!r->reg_reads[i] && !r->reg_writes[i]) continue;
            unsigned vals = 0;
            for (int k = 0; k < 8; k++) vals += (unsigned)__builtin_popcount(r->reg_vals[i * 8 + k]);
            const DpIoReg *g = &r->reg_plat->io_regs[i];
            fprintf(out, "  reg $%04X-$%04X %s [%s] reads %llu writes %llu distinct-values %u\n", g->lo, g->hi, g->name, g->category, U(r->reg_reads[i]), U(r->reg_writes[i]), vals);
        }
    }
    if (r->vector_seeds) fprintf(out, "vector seeds (RST/IRQ registry entries) %llu\n", U(r->vector_seeds));
    fprintf(out, "pointer seeds refused by cap %llu\n", U(r->seed_refused_cap));
    if (r->seed_regs_absent) fprintf(out, "register seeds unsupported by the machine: %llu (not seeded)\n", U(r->seed_regs_absent));
    if (r->seed_banks_absent) fprintf(out, "bank seeds the machine cannot show: %llu (not seeded)\n", U(r->seed_banks_absent));
    fprintf(out, "forced RAM cuts %llu\n", U(r->forced_ram_cuts));
    if (r->nonsense_pc || r->nonsense_sp || r->pad_cuts)
        fprintf(out, "nonsense cuts: echo/I-O pc %llu, stack outside RAM %llu, padding slides %llu\n",
                U(r->nonsense_pc), U(r->nonsense_sp), U(r->pad_cuts));
    if (r->pad_unmarked) fprintf(out, "padding ramps executed but not marked as code: %llu instructions (rst $38 / nop runs)\n", U(r->pad_unmarked));
    fprintf(out, "forced taint cuts: indirect %llu, ret %llu\n", U(r->taint_cut_indirect), U(r->taint_cut_ret));
    if (r->exec_wram_rom) fprintf(out, "RAM-executed instructions traced to ROM copy sources: %llu\n", U(r->exec_wram_rom));
    if (r->ra.n) fprintf(out, "runtime code archive: %u distinct instructions executed from RAM/VRAM/HRAM, %u attached to a ROM copy source, %u generated (no ROM source; %u with an approximate ROM producer), %u rewrote an earlier instruction at the same address (self-modifying)\n", r->ra.n, r->ra.attached, r->ra.n - r->ra.attached, r->ra.derived, r->ra.poly);
    fprintf(out, "exec_outside %llu, rom->ram copy bytes %llu (%zu distinct ROM sources), forced %llu\n", U(r->exec_outside), U(r->copy_bytes), copied, U(r->copy_forced));
    DpCoreSection secs[64];
    size_t ns = dp_core_sections(r, secs, 64);
    fprintf(out, "sections %zu\n", ns);
    for (size_t i = 0; i < ns && i < 64; i++)
        fprintf(out, "  section %zu rom-index %zu-%zu natural %zu forced %zu verdict %s live-ram-pages %zu\n", i + 1, secs[i].lo, secs[i].hi, secs[i].natural, secs[i].forced, secs[i].required ? "required" : "speculative", secs[i].live_pages);
    size_t stat_only = 0, prov_only = 0;
    for (size_t i = 0; i < r->rom_size; i++) {
        if ((r->flags[i] & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED)) == DP_CORE_STATIC) stat_only++;
        if (r->prov && r->prov[i] && !(r->flags[i] & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED))) prov_only++;
    }
    fprintf(out, "speculative gaps: %llu runs, %llu bytes (never counted as known); %llu runs reinforced by an immediate reference (%llu references seen)\n", U(r->spec_runs), U(r->spec_bytes), U(r->spec_ref_runs), U(r->imm_refs));
    fprintf(out, "static closure: roots %llu, bytes %llu, static-only %zu, provisional %zu (untrusted walks, code once confirmed), capped walks %llu, returns followed %llu\n", U(r->closure_roots), U(r->closure_bytes), stat_only, prov_only, U(r->closure_capped), U(r->closure_rets));
    if (r->closure_entry_refused) fprintf(out, "untrusted walk entries refused (data plausibility, point 3): %llu\n", U(r->closure_entry_refused));
    if (r->closure_stack_invalidations) fprintf(out, "ghost returns discarded on memory-control OUT (port_effect): %llu\n", U(r->closure_stack_invalidations));
    if (r->closure_regs) fprintf(out, "closure registers (C3): seed walks %llu, indirect resolved %llu, indirect unresolved %llu, stack returns %llu, indirect site cap %llu, seeds refused (site cap %llu, set full %llu)\n",
        U(r->closure_reg_roots), U(r->closure_reg_indirect), U(r->closure_reg_miss), U(r->closure_reg_stackrets), U(r->closure_reg_indirect_sat), U(r->closure_reg_seed_sat), U(r->closure_reg_set_sat));
    if (r->inferred) fprintf(out, "static inference: padding %llu, gap code %llu, gap pad %llu, gap data %llu, copy tables %llu (%llu sibling bytes, %llu refused as code), jump tables %llu (%llu bytes)\n", U(r->padding_bytes), U(r->inferred_code), U(r->inferred_pad), U(r->inferred_data), U(r->sib_tables), U(r->sib_bytes), U(r->sib_refused), U(r->jt_tables), U(r->jt_bytes));
    if (r->closure_jp_entries) fprintf(out, "provenance: %llu jp-hl targets, %llu data bytes static\n", U(r->closure_jp_entries), U(r->prov_data_bytes));
    if (r->closure_jp_tables) fprintf(out, "S4 operand tables of executed JP HL: %llu tables, %llu entries\n", U(r->closure_jp_tables), U(r->closure_jp_entries));
    fprintf(out, "rom bytes: natural %zu, forced %zu (speculative), static-only %zu\n", nat, forced, stat_only);
    #undef U
}

// --cov FILE: coverage in the old engine's line format (bank offset flags hits; flags 1 = natural, 5 = forced)
// The ROM index is the compact bank * $4000 + (addr & $3FFF) on every platform (dp_rom_index), so
// bank = index >> 14 and the offset is the in-bank address — except GB, whose banks 1+ map at
// $4000 and whose old format wrote the mapped address.  The one platform exception lives here,
// named, instead of an inline ternary (P10).
// P10: the cov offset of image byte `index` (16 KiB chunks), from the machine geometry:
// a linear space walks the image at its own addresses (MD, and the MPR-journaled PCE),
// a windowed machine maps bank 0 at code_base and every switchable bank at bank_window
// (GB/Sega/PCE $4000, NES $8000); the bank column already identifies the bank, so a
// wrapped window keeps the byte unambiguous.  0: unknown — image-relative everywhere.
static unsigned cov_bank_offset(const DpMachine *m, size_t index) {
    unsigned bank = (unsigned)(index >> 14), off = (unsigned)(index & 0x3fffu);
    if (m->linear) return off + (bank << 14);
    if (!bank || !m->bank_window) return off;
    return m->bank_window + off;
}

// Replay another revision's coverage as speculative seeds: every executed or
// speculative line of the other campaign whose four-byte instruction window is
// byte-identical between the two images becomes a forced-path seed at the same
// compact index.  Nothing is marked known here — the new ROM proves its own
// bytes by running them; a differing window is never transferred.
size_t dp_core_transfer_seeds(const uint8_t *rom, size_t rom_size,
                              const char *other_rom_path, const char *cov_path,
                              uint32_t **pcs, uint16_t **banks,
                              uint64_t *lines_out, uint64_t *matched_out) {
    *pcs = NULL; *banks = NULL;
    if (lines_out) *lines_out = 0;
    if (matched_out) *matched_out = 0;
    FILE *rf = fopen(other_rom_path, "rb");
    if (!rf) { fprintf(stderr, "deepeel: --cov-transfer: cannot read %s\n", other_rom_path); return 0; }
    if (fseek(rf, 0, SEEK_END) != 0) { fclose(rf); return 0; }
    long rs = ftell(rf);
    if (rs <= 0) { fclose(rf); return 0; }
    if (fseek(rf, 0, SEEK_SET) != 0) { fclose(rf); return 0; }
    size_t on = (size_t)rs, cn = on < rom_size ? on : rom_size;
    if (on != rom_size)
        fprintf(stderr, "deepeel: --cov-transfer: %s is %ld bytes against this ROM's %zu — comparing the common prefix only\n",
                other_rom_path, rs, rom_size);
    uint8_t *other = malloc(cn ? cn : 1);
    if (!other || fread(other, 1, cn, rf) != cn) {
        fclose(rf); free(other);
        fprintf(stderr, "deepeel: --cov-transfer: short read of %s\n", other_rom_path);
        return 0;
    }
    fclose(rf);
    FILE *cf = fopen(cov_path, "r");
    if (!cf) {
        free(other);
        fprintf(stderr, "deepeel: --cov-transfer: cannot read %s\n", cov_path);
        return 0;
    }
    uint8_t *seen = calloc(cn, 1);
    uint32_t *tp = NULL; uint16_t *tb = NULL;
    size_t tn = 0, cap = 0;
    uint64_t lines = 0;
    char line[128];
    while (fgets(line, sizeof line, cf)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned bank = 0, off = 0, flags = 0;
        if (sscanf(line, "%x %x %x", &bank, &off, &flags) < 3) continue;
        if (!(flags & 5)) continue;                                  // executed or speculative code only
        if ((off >= 0x4000u) != (bank != 0)) continue;               // the v1 geometry: bank 0 is 0000-3FFF, others 4000-7FFF
        size_t idx = ((size_t)bank << 14) | (off & 0x3fffu);
        if (idx >= cn) continue;
        lines++;
        if (seen[idx]) continue;
        size_t w = cn - idx < 4 ? cn - idx : 4;
        if (memcmp(rom + idx, other + idx, w) != 0) continue;        // exactly similar, byte for byte
        seen[idx] = 1;
        if (tn == cap) {
            cap = cap ? cap * 2 : 1024;
            uint32_t *np = realloc(tp, cap * sizeof *np);
            uint16_t *nb = realloc(tb, cap * sizeof *nb);
            if (!np || !nb) {
                free(np); free(nb); free(tp); free(tb); free(seen); free(other); fclose(cf);
                fprintf(stderr, "deepeel: --cov-transfer: out of memory\n");
                return 0;
            }
            tp = np; tb = nb;
        }
        tb[tn] = (uint16_t)bank;
        tp[tn++] = off;
    }
    fclose(cf);
    free(seen);
    free(other);
    *pcs = tp; *banks = tb;
    if (lines_out) *lines_out = lines;
    if (matched_out) *matched_out = tn;
    return tn;
}

void dp_core_write_cov(const char *path, const char *id, const DpMachine *m, const DpCoreResult *r) {
    if (!path || !path[0]) return;
    FILE *cf = fopen(path, "w");
    if (!cf) return;
    fprintf(cf, "# deepeel %s coverage v1 (generic kernel): bank offset flags(1=exec,4=speculative,8=data read by a natural path) hits\n", id);
    for (size_t i = 0; i < r->rom_size; i++) {
        uint8_t f = r->flags[i];
        unsigned out = (f & DP_CORE_NATURAL) ? 1 : (f & DP_CORE_FORCED) ? 5 : 0;
        if ((f & DP_CORE_DATA) && !((f & DP_CORE_COPIED) && (f & DP_CORE_NATURAL))) out |= 8;   // a copied byte later executed is code, not plain data
        if (!out) {
            // The suspected code classes go out as flag 4 (speculative, never heat): the
            // static closure's marks, the untrusted probable-code walks, the inferred-code
            // bytes.  A consumer that compiles them acquires the suspect as code — the
            // speculative-gap disassembly stays out, it is never known at all.
            if ((f & DP_CORE_STATIC) || (r->prov && r->prov[i]) ||
                (r->inferred && r->inferred[i] == DP_MAP_INF_CODE)) out = 4;
            else continue;
        }
        // Hits are recorded on instruction heads; retain one for covered operands
        // and data, for which the kernel does not maintain a visit counter.
        uint32_t hits = r->hits && r->hits[i] ? r->hits[i] : 1;
        fprintf(cf, "%02x %04x %x %u\n", (unsigned)(i >> 14), cov_bank_offset(m, i), out, hits);
    }
    fclose(cf);
}
