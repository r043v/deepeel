// core.c: generic exploration kernel over DpMachine + DpIsa (see core.h).
#include <unistd.h>
#include "core.h"

#include <pthread.h>
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
} Pending;

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
    if (!strcmp(a, "--no-cells")) { c->no_cells = 1; return 1; }
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
    if (!strcmp(a, "--sessions") && i + 1 < argc) { c->sessions = (uint32_t)strtoul(argv[i + 1], NULL, 0); c->no_sessions = 0; return 2; }
    if (!strcmp(a, "--no-irq-jitter")) { c->no_irq_jitter = 1; return 1; }
    if (!strcmp(a, "--no-decompress")) { c->no_decompress = 1; return 1; }
    if (!strcmp(a, "--no-copies")) { c->no_copies = 1; return 1; }
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
        DpCoreSection sec = { i, i, 0, 0, 0 };
        for (; i < r->rom_size && (r->flags[i] & (DP_CORE_NATURAL | DP_CORE_FORCED)); i++) {
            if (r->flags[i] & DP_CORE_NATURAL) sec.natural++;
            else sec.forced++;
            sec.hi = i;
        }
        sec.required = sec.natural > 0;
        if (n < cap) out[n] = sec;
        n++;
    }
    return n;
}

void dp_core_result_free(DpCoreResult *r) {
    dp_ramarch_free(&r->ra);
    free(r->flags);
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
    SiteTab sites;
    uint32_t *site_picks, *site_gain;   // per fork site (rom index + 1): paths run / new ROM bytes they found
    int active;                // workers currently running a path
    uint64_t steps;            // global instruction count (atomic)
    uint64_t paths_done;       // finished paths (atomic), for the progress line
    int stop_monitor;
    int halt;                  // 0 running, 1 max-time reached, 2 stagnation (atomic): workers wind down
    int spool_fd; uint64_t spool_n, spool_total; uint32_t spool_rs;   // disk overflow of the pending queue: fixed-size records, LIFO
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
    uint8_t *tas_start; uint32_t tas_pc; int tas_shifts_left;   // power-on state kept to queue the shifted movie replays once the unshifted one ends
} Shared;

typedef struct {
    DpMachine *m;
    DpCoreResult *r;
    int64_t last_rom;     // ROM index of the last ROM byte read, -1 when consumed
    uint8_t last_val;
    uint8_t *fed;         // 64 KiB bitmap: CPU addresses written by a natural verbatim ROM copy
    int forced;           // the current path is a forced fork: speculative state
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
    uint32_t *visits, *stamp;
    uint32_t *rng_stamp;   // per ld a,(nn) site: path id of the last visit (technique 4)
    uint8_t *rng_last;
    uint32_t path_id;
    int ok;
    unsigned idx;
    uint32_t *capped;      // direct-mapped cache of conditional sites that reached fork_site_cap (pc + 1)
    unsigned step_batch;   // steps not yet added to the shared counter (flushed every 256)
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
static void closure_walk(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t root) {
    uint32_t stack[256]; unsigned sp = 0;
    unsigned budget = CLOSURE_INSTR;
    uint32_t pc = root;
    int64_t k0 = m->rom_index(m, pc);
    if (k0 < 0 || (size_t)k0 >= r->rom_size || (__atomic_load_n(&r->flags[k0], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC))) return;
    r->closure_roots++;
    for (;;) {
        int64_t k = m->rom_index(m, pc);
        if (k < 0 || (size_t)k >= r->rom_size || !budget) {
            if (k >= 0 && !budget) r->closure_capped++;
            if (!sp) return;
            pc = stack[--sp]; if (!budget) budget = 1;
            continue;
        }
        if (__atomic_load_n(&r->flags[k], __ATOMIC_RELAXED) & (DP_CORE_NATURAL | DP_CORE_FORCED | DP_CORE_STATIC)) {
            if (!sp) return;
            pc = stack[--sp];
            continue;
        }
        uint8_t b[8];
        for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, pc + i);
        DpFlowInfo f; memset(&f, 0, sizeof f);
        if (!isa->flow(b, pc, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) {
            if (!sp) return;
            pc = stack[--sp];
            continue;
        }
        budget--;
        for (unsigned i = 0; i < f.length; i++) {
            int64_t q = m->rom_index(m, pc + i);
            if (q >= 0 && (size_t)q < r->rom_size) {
                uint8_t old = __atomic_fetch_or(&r->flags[q], DP_CORE_STATIC, __ATOMIC_RELAXED);
                if (!(old & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED))) r->closure_bytes++;
            }
        }
        int fall = f.fall || f.kind == DP_FLOW_SEQ || f.kind == DP_FLOW_COND || f.kind == DP_FLOW_CALL;
        if (f.kind != DP_FLOW_RET && f.kind != DP_FLOW_INDIRECT && f.target >= 0 && sp < 256 &&
            (f.kind == DP_FLOW_JUMP || f.kind == DP_FLOW_COND || f.kind == DP_FLOW_CALL))
            stack[sp++] = (uint32_t)f.target;
        if (f.kind == DP_FLOW_JUMP || f.kind == DP_FLOW_RET || f.kind == DP_FLOW_INDIRECT || f.halt) fall = 0;
        if (fall) pc += f.length;
        else {
            if (!sp) return;
            pc = stack[--sp];
        }
    }
}

// S4 (sm83/z80): an executed `jp hl` preceded (within 24 bytes) by `ld hl,nn` reads a table of code
// pointers at nn; each entry that starts a legal instruction is a static root.  Stride 2, up to 64 words.
static void closure_jp_table(DpCoreResult *r, DpMachine *m, const DpIsa *isa, uint32_t pc) {
    if (!isa->name || (strcmp(isa->name, "sm83") && strcmp(isa->name, "z80"))) return;
    {   // byte-granular: ld b,n / ld c,m then ld h,b / ld l,c (or the reverse pair) before the jump
        int hb = -1, lc = -1, mv = 0;
        for (uint32_t d = 1; d <= 24 && d <= pc; d++) {
            uint8_t o = m->fetch(m, pc - d);
            if (o == 0x60 || o == 0x69) mv |= o == 0x60 ? 1 : 2;
            if (d >= 2 && hb < 0 && m->fetch(m, pc - d) == 0x06) hb = m->fetch(m, pc - d + 1);
            if (d >= 2 && lc < 0 && m->fetch(m, pc - d) == 0x0E) lc = m->fetch(m, pc - d + 1);
        }
        if (mv == 3 && hb >= 0 && lc >= 0 && (hb << 8 | lc) >= 0x150 && (hb << 8 | lc) < 0x8000) {
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
        uint32_t base = m->fetch(m, pc - d + 1) | (uint32_t)m->fetch(m, pc - d + 2) << 8;
        if (base < 0x100 || base >= 0x8000) continue;
        int cnt = 0;
        for (int e = 0; e < 64; e++) {
            uint32_t t = m->fetch(m, base + e * 2) | (uint32_t)m->fetch(m, base + e * 2 + 1) << 8;
            if (t < 0x150 || t >= 0x8000) break;
            uint8_t b[8]; for (unsigned i = 0; i < sizeof b; i++) b[i] = m->fetch(m, t + i);
            DpFlowInfo f; memset(&f, 0, sizeof f);
            if (!isa->flow(b, t, &f) || !f.length || f.kind == DP_FLOW_ILLEGAL) break;
            uint32_t ea = base + (uint32_t)e * 2; int fresh = 1;
            for (unsigned z = 0; z < r->jp_seen_n; z++) if (r->jp_seen[z] == ea) { fresh = 0; break; }
            if (fresh && r->jp_seen_n < 256) r->jp_seen[r->jp_seen_n++] = ea;
            if (fresh) {
                r->closure_jp_entries++; r->prov_data_bytes += 2;
            }
            closure_walk(r, m, isa, t);
            cnt++;
        }
        if (cnt) { r->closure_jp_tables++; return; }
    }
}

static void mark(Shared *sh, DpCoreResult *r, DpMachine *m, uint32_t pc, unsigned len, uint8_t how) {
    for (unsigned i = 0; i < len; i++) {
        int64_t k = m->rom_index(m, pc + i);
        if (k >= 0 && (size_t)k < r->rom_size) {
            uint32_t srcbit = (how & DP_CORE_NATURAL) ? DP_SRC_FORK_NATURAL : DP_SRC_FORK_FORCED;
            // read first: an atomic read-modify-write on an already covered byte only bounces the cache line
            uint8_t cur = __atomic_load_n(&r->flags[k], __ATOMIC_RELAXED);
            if ((cur & how) == how && (__atomic_load_n(&r->src_flags[k], __ATOMIC_RELAXED) & srcbit)) continue;
            uint8_t old = __atomic_fetch_or(&r->flags[k], how, __ATOMIC_RELAXED);
            if (!(old & (DP_CORE_NATURAL | DP_CORE_FORCED))) {
                r->first_seen[k] = __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) + 1;
                if (r->in_novelty_path) r->novelty_new_bytes++; else r->plain_new_bytes++;
            }
            __atomic_fetch_or(&r->src_flags[k], srcbit, __ATOMIC_RELAXED);
        }
    }
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
    if (k < 0 && o->ramread && !o->ramread[addr & 0xFFFF]) __atomic_store_n(&o->ramread[addr & 0xFFFF], 1, __ATOMIC_RELAXED);
    if (k < 0 && o->track && o->nram < 4) {
        unsigned i = 0;
        while (i < o->nram && o->ram[i] != addr) i++;
        if (i == o->nram) o->ram[o->nram++] = addr;
    }
    if (k >= 0 && (size_t)k < o->r->rom_size) {
        if (!o->forced && !(__atomic_load_n(&o->r->flags[k], __ATOMIC_RELAXED) & DP_CORE_DATA)) __atomic_fetch_or(&o->r->flags[k], DP_CORE_DATA, __ATOMIC_RELAXED);
        o->last_rom = k; o->last_val = v; o->age = 0;
        if (o->run_hi >= 0 && k >= o->run_hi && k <= o->run_hi + 2) o->run_hi = k;
        else { run_flush(o); o->run_lo = o->run_hi = k; }
    }
}

static void obs_write(void *u, uint32_t addr, uint8_t v) {
    CopyObs *o = u;
    if (o->run_hi >= 0 && o->m->rom_index(o->m, addr) < 0) o->run_writes++;
    if (o->last_rom < 0 || o->age > 4) return;
    if (o->m->rom_index(o->m, addr) < 0 && v != o->last_val && o->fedprod && !o->forced) o->fedprod[addr & 0xFFFF] = (uint32_t)o->last_rom + 1;
    if (o->m->rom_index(o->m, addr) < 0 && v == o->last_val) {
        if (o->forced) o->r->copy_forced++;
        else {
            __atomic_fetch_or(&o->r->flags[o->last_rom], DP_CORE_COPIED, __ATOMIC_RELAXED);
            __atomic_fetch_or(&o->r->src_flags[o->last_rom], DP_SRC_COPY, __ATOMIC_RELAXED);
            o->r->copy_bytes++;
            if (o->fed) o->fed[addr & 0xFFFF] = 1;
            if (o->fedsrc) o->fedsrc[addr & 0xFFFF] = (uint32_t)o->last_rom + 1;
        }
    }
    o->last_rom = -1;
}

// FNV-1a over the configured RAM range, mixed with the fork target
static uint64_t cell_key(const DpCoreConfig *cfg, const uint8_t *ramread, DpMachine *m, uint32_t other) {
    uint64_t h = 1469598103934665603ull ^ other;
    if (!(cfg->cell_hi > cfg->cell_lo)) {   // semantic cell: the RAM bytes the code has been seen to read (at most 4096), no game knowledge
        unsigned n = 0;
        for (uint32_t a = 0; a < 0x10000 && n < 4096; a++)
            if (ramread[a]) { h ^= m->fetch(m, a) | ((uint64_t)n << 8); h *= 1099511628211ull; n++; }
        return h | 1;
    }
    for (uint32_t a = cfg->cell_lo; a <= cfg->cell_hi; a++) { h ^= m->fetch(m, a); h *= 1099511628211ull; }
    return h | 1;   // never 0: 0 marks an empty slot
}

// 1 when the key is new (inserted), 0 when already present; caller holds sh->mu
static int keyset_insert(uint64_t **set, size_t *cap, size_t *n, uint64_t key) {
    if (*n * 2 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        uint64_t *ns = calloc(nc, sizeof *ns);
        if (!ns) return 1;
        for (size_t i = 0; i < *cap; i++)
            if ((*set)[i]) { size_t h = (*set)[i] & (nc - 1); while (ns[h]) h = (h + 1) & (nc - 1); ns[h] = (*set)[i]; }
        free(*set); *set = ns; *cap = nc;
    }
    size_t h = key & (*cap - 1);
    while ((*set)[h]) { if ((*set)[h] == key) return 0; h = (h + 1) & (*cap - 1); }
    (*set)[h] = key; (*n)++;
    return 1;
}
static int cell_insert(Shared *sh, uint64_t key) { return keyset_insert(&sh->cellset, &sh->cellcap, &sh->celln, key); }


// Queue overflow goes to disk, never away: a record is {pc, forced, site, input} + the saved state.
// All called with sh->mu held.  If the disk is unusable the state stays in RAM beyond queue_max.
typedef struct { uint32_t pc, forced, site, input; } SpoolHead;
static void ram_push(Shared *sh, Pending p) {
    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
    sh->q[sh->qn++] = p;
}
static int spool_open(Shared *sh) {
    if (sh->spool_fd > 0) return 1;
    if (sh->spool_fd < 0) return 0;   // failed once: stay in RAM
    const char *dir = getenv("TMPDIR"); if (!dir || !*dir) dir = "/tmp";
    char path[512]; snprintf(path, sizeof path, "%s/deepeel-spool-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) { sh->spool_fd = -1; return 0; }
    unlink(path);
    sh->spool_fd = fd; sh->spool_rs = (uint32_t)(sizeof(SpoolHead) + sh->sbytes);
    return 1;
}
// pushes p; when the RAM queue is full, appends it to the spool.  Returns 1 if spilled.
static int queue_push(Shared *sh, Pending p) {
    const DpCoreConfig *cfg = sh->cfg;
    if (cfg->queue_max && sh->qn >= cfg->queue_max && spool_open(sh)) {
        uint8_t *rec = malloc(sh->spool_rs);
        if (rec) {
            SpoolHead h = { p.pc, (uint32_t)p.forced, p.site, p.input };
            memcpy(rec, &h, sizeof h); memcpy(rec + sizeof h, p.state, sh->sbytes);
            ssize_t w = pwrite(sh->spool_fd, rec, sh->spool_rs, (off_t)(sh->spool_n * sh->spool_rs));
            free(rec);
            if (w == (ssize_t)sh->spool_rs) { sh->spool_n++; sh->spool_total++; free(p.state); return 1; }
        }
    }
    ram_push(sh, p);
    return 0;
}
// the queue is empty: bring back up to queue_max/2 of the newest spilled records
static void spool_refill(Shared *sh) {
    size_t want = sh->cfg->queue_max ? sh->cfg->queue_max / 2 : 64;
    if (want < 1) want = 1;
    uint8_t *rec = malloc(sh->spool_rs);
    if (!rec) return;
    while (want-- && sh->spool_n) {
        uint64_t i = sh->spool_n - 1;
        if (pread(sh->spool_fd, rec, sh->spool_rs, (off_t)(i * sh->spool_rs)) != (ssize_t)sh->spool_rs) break;
        SpoolHead h; memcpy(&h, rec, sizeof h);
        uint8_t *st = malloc(sh->sbytes);
        if (!st) break;
        memcpy(st, rec + sizeof h, sh->sbytes);
        sh->spool_n = i;
        ram_push(sh, (Pending){ st, h.pc, (int)h.forced, h.site, h.input });
    }
    free(rec);
    if (ftruncate(sh->spool_fd, (off_t)(sh->spool_n * sh->spool_rs)) != 0) { /* the next appends overwrite it anyway */ }
}

enum { TAS_INPUT = 0x40000000, SESSION_ATTRACT = 0x20000000, SESSION_PLAY = 0x20001000 };
// pad masks cycled by play sessions (kernel mask bits: up down left right button1 button2 start)
static const uint32_t play_catalog[8] = {0, 0x10, 0x40, 0x20, 0x01, 0x02, 0x04, 0x08};
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
    unsigned queued = 0;
    uint8_t *cur = malloc(sh->sbytes), *snap = malloc(sh->sbytes);
    if (!cur || !snap) { free(cur); free(snap); return 0; }
    m->save(m, cur);
    int saved_forced = w->cobs.forced, saved_track = w->cobs.track;
    w->cobs.forced = 1; w->cobs.track = 0;
    for (unsigned a = 0; a < w->cobs.nram; a++) {
        uint32_t addr = w->cobs.ram[a];
        pthread_mutex_lock(&sh->mu);
        int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, (((uint64_t)m->rom_index(m, condpc) + 1) << 20 ^ (uint64_t)addr << 4 ^ (other & 15)) | (9ull << 60));
        pthread_mutex_unlock(&sh->mu);
        if (!fresh) continue;
        loc->solve_tries++;
        for (unsigned c = 0; c < nc; c++) {
            m->load(m, w->wsnap);
            m->poke(m, addr, cand[c]);
            m->save(m, snap);
            int reached = 0;
            for (int st = 0; st < 16 && m->pc(m) != condpc; st++) if (!m->run(m)) break;
            if (m->pc(m) == condpc) { m->run(m); reached = 1; }
            if (reached && m->pc(m) == other) {
                pthread_mutex_lock(&sh->mu);
                queue_push(sh, (Pending){ snap, w->win_pc, 0, 0, input });
                pthread_cond_signal(&sh->cv);
                pthread_mutex_unlock(&sh->mu);
                snap = malloc(sh->sbytes);
                if (!snap) goto out;
                queued++; loc->solved_forks++;
                break;
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
static void core_dsy_frame(const DpCoreConfig *cfg, DpMachine *m, uint64_t f) {
    uint32_t base = 0xC000, len = 0x2000;   // gb, sms, gg, sg: $C000-$DFFF
    if (!strcmp(m->name, "nes")) { base = 0; len = 0x800; }
    else if (!strcmp(m->name, "pce")) { base = 0x2000; len = 0x2000; }
    pthread_mutex_lock(&dsy_mu);
    if (!dsy_file) { dsy_file = fopen(cfg->dsy_path, "wb"); if (dsy_file) fwrite("DSY1", 1, 4, dsy_file); }
    if (dsy_file && f > dsy_last) {
        uint8_t buf[0x2000];
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
    if (p->seed && covered(loc, m, p->pc)) return;   // a pointer seed whose target the exploration already reached adds nothing
    if (p->forced) m->set_pc(m, p->pc);
    uint32_t cur_input = p->input;
    // TAS replay: input >= TAS_INPUT encodes the frame shift (value - TAS_INPUT - 3, -3..+3)
    int tas = cur_input >= TAS_INPUT && cfg->tas_masks && m->frame && m->set_input;
    int tas_shift = tas ? (int)(cur_input - TAS_INPUT) - 3 : 0;
    uint64_t tas_last = ~0ull; int tas_ended = 0;
    if (cur_input >= TAS_INPUT) cur_input = 0;
    // generic long sessions (every platform with a frame counter): attract = no input for attract_frames frames;
    // play = a pad epoch schedule (nine epochs over play_frames frames, each session starting at another catalog entry)
    int attract = cur_input == SESSION_ATTRACT && m->frame;
    int play = cur_input >= SESSION_PLAY && cur_input < TAS_INPUT && m->frame && m->set_input;
    unsigned play_k = play ? cur_input - SESSION_PLAY : 0;
    if (cur_input == SESSION_ATTRACT || cur_input >= SESSION_PLAY) cur_input = 0;
    uint64_t sess_limit = attract ? cfg->attract_frames : play ? cfg->play_frames : 0;
    uint64_t sess_f0 = (attract || play) ? m->frame(m) : 0, sess_last = ~0ull;
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
    w->cobs.forced = p->forced; w->cobs.last_rom = -1;
    w->win_on = w->cobs.track = 0;
    uint32_t path_id = ++w->path_id, vid = path_id, steps = 0;
    int cur_forced = p->forced;
    unsigned fdepth = 0;   // calls made since the forced fork: a ret below that pops state the path did not build
    for (;;) {
        if (__atomic_load_n(&sh->halt, __ATOMIC_RELAXED)) break;   // time limit / stagnation: this path ends, the queue is dropped by the worker loop
        if (!(attract || play) && cfg->max_total && __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) + w->step_batch >= cfg->max_total) {
            loc->sat_total++;
            pthread_mutex_lock(&sh->mu);
            if (!cfg->state_dir) {
                for (size_t i = 0; i < sh->qn; i++) free(sh->q[i].state);
                sh->qn = 0;
            } else {   // with --state-dir the backlog stays for the frontier file and the run winds down
                __atomic_store_n(&sh->halt, 1, __ATOMIC_RELAXED);
                pthread_cond_broadcast(&sh->cv);
            }
            pthread_mutex_unlock(&sh->mu);
            break;
        }
        if (tas) {
            uint64_t f = m->frame(m);
            if (f != tas_last) {
                tas_last = f;
                if (tas_shift == 0 && cfg->dsy_path && f > 0) core_dsy_frame(cfg, m, f);
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
                if (k >= (int64_t)cfg->tas_frames) {
                    m->set_input(m, 0); tas = 0;
                    tas_ended = 1; if (tas_shift == 0) fprintf(stderr, "deepeel: TAS movie played to its end (%llu frames) ffac=%02x ffb4=%02x\n", (unsigned long long)cfg->tas_frames, m->fetch(m,0xFFAC), m->fetch(m,0xFFB4));
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
                                if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                                sh->q[sh->qn++] = (Pending){ cp, sh->tas_pc, 0, 0, TAS_INPUT + 3 + order[i] };
                            }
                        }
                        pthread_mutex_unlock(&sh->mu);
                    }
                }
                else m->set_input(m, k < 0 ? 0 : cfg->tas_masks[k]);
            }
        }
        if (tas_ended) break;   // the movie is over: its forks (queued along the way) carry the exploration on, the path does not eat the budget
        if (framed) {
            uint64_t f = m->frame(m);
            if (f != sess_last) {
                sess_last = f;
                vid = ++w->path_id;
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
                        m->set_input(m, play_catalog[(ep + play_k) % 8]);
                        loc->play_epochs++;
                    }
                }
            }
        }
        if ((attract || play) && steps >= (cfg->max_total ? cfg->max_total / 2 : 400000000u)) { loc->sat_path++; loc->session_frames += m->frame(m) - sess_f0; break; }   // a long session shares the budget
        if (!tas && !attract && !play && cfg->max_path_steps && steps >= cfg->max_path_steps) { loc->sat_path++; break; }
        if (!attract && !play && cur_input && cfg->max_total && steps >= session_cap) { loc->sat_path++; break; }
        uint32_t pc = m->pc(m);
        if (cfg->hooks && cfg->hooks->before_step &&
            !cfg->hooks->before_step(cfg->hooks->user, m, pc)) break;
        if (sess_seen) {
            int64_t sk = m->rom_index(m, pc);
            if (sk >= 0 && (size_t)sk < loc->rom_size && !(sess_seen[sk >> 3] & (1u << (sk & 7)))) { sess_seen[sk >> 3] |= (uint8_t)(1u << (sk & 7)); sess_cov++; }
        }
        uint16_t vi = (uint16_t)pc;
        if (w->stamp[vi] != vid) { w->stamp[vi] = vid; w->visits[vi] = 0; }
        if (!tas && cfg->visits_per_pc && ++w->visits[vi] > cfg->visits_per_pc) { loc->sat_loop++; break; }
        uint8_t bytes[8] = {0};
        for (unsigned i = 0; i < sizeof bytes; i++) bytes[i] = m->fetch(m, pc + i);
        w->cobs.age++;
        int len = isa->length(bytes[0]);
        DpFlowInfo fi;
        memset(&fi, 0, sizeof fi);
        int have_flow = isa->flow && isa->flow(bytes, pc, &fi);
        if (have_flow && fi.length) len = fi.length;
        if (len <= 0) len = 1;
        if (m->rom_index(m, pc) < 0) {
            if (cur_forced && !cfg->no_forced_ram_cut && !(sh->fed && sh->fed[pc & 0xffff])) {
                loc->forced_ram_cuts++;
                break;
            }
            loc->exec_outside++;
            if (sh->fedsrc) {
                uint32_t s0 = sh->fedsrc[pc & 0xFFFF], p0 = sh->fedprod[pc & 0xFFFF];
                dp_ramarch_add_p(&loc->ra, (uint16_t)pc, bytes, len > 10 ? 10 : len, s0 ? (int32_t)(s0 - 1) : -1, s0 ? -1 : (p0 ? (int32_t)(p0 - 1) : -1));
            }
            if (!cur_forced && sh->fedsrc) {   // executed RAM bytes that a verbatim ROM copy produced: attribute them to their ROM source
                int any = 0;
                for (int i = 0; i < len; i++) {
                    uint32_t src = sh->fedsrc[(pc + (uint32_t)i) & 0xFFFF];
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
                int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ((uint64_t)addr) | (7ull << 60));
                pthread_mutex_unlock(&sh->mu);
                uint8_t *tmp = fresh ? malloc(sh->sbytes) : NULL;
                if (tmp) m->save(m, tmp);
                for (unsigned k = 0; tmp && k <= (cfg->index_max ? cfg->index_max : 15); k++) {
                    uint8_t *snap = malloc(sh->sbytes);
                    if (!snap) break;
                    m->poke(m, addr, (uint8_t)k);
                    m->save(m, snap);
                    pthread_mutex_lock(&sh->mu);
                    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                    sh->q[sh->qn++] = (Pending){ snap, pc, 0, 0, cur_input };
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
            int volatile_byte = w->rng_stamp[(uint16_t)pc] == path_id && w->rng_last[(uint16_t)pc] != v;
            w->rng_stamp[(uint16_t)pc] = path_id; w->rng_last[(uint16_t)pc] = v;
            int fresh = 0;
            if (volatile_byte) {
                pthread_mutex_lock(&sh->mu);
                fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ((uint64_t)addr) | (5ull << 60));
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
                    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                    sh->q[sh->qn++] = (Pending){ snap, pc, 0, 0, cur_input };
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
                    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                    sh->q[sh->qn++] = (Pending){ snap, pc, 0, 0, cur_input };
                    pthread_mutex_unlock(&sh->mu);
                    loc->learned_sigs++;
                    snap = NULL;
                }
                free(tmp); free(snap);
            }
        }
        if (cur_forced && !cfg->no_taint_cut && have_flow) {
            // technique 1.b: the inverted flag may have corrupted registers/stack; a transfer whose target comes from them is untrusted
            if (fi.kind == DP_FLOW_INDIRECT) { loc->taint_cut_indirect++; break; }
            if (fi.kind == DP_FLOW_RET) { if (!fdepth) { loc->taint_cut_ret++; break; } fdepth--; }
            else if (fi.call) fdepth++;
        }
        mark(sh, loc, m, pc, (unsigned)len, cur_forced ? DP_CORE_FORCED : DP_CORE_NATURAL);
        if (!cfg->no_closure && !cur_forced && have_flow && fi.kind != DP_FLOW_SEQ && fi.kind != DP_FLOW_ILLEGAL) {
            if (fi.target >= 0 && (fi.kind == DP_FLOW_JUMP || fi.kind == DP_FLOW_COND || fi.kind == DP_FLOW_CALL)) closure_walk(loc, m, isa, (uint32_t)fi.target);
            if (fi.kind == DP_FLOW_COND || fi.kind == DP_FLOW_CALL) closure_walk(loc, m, isa, pc + fi.length);
            if (fi.kind == DP_FLOW_INDIRECT && bytes[0] == 0xE9) closure_jp_table(loc, m, isa, pc);
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
            if (fresh) {
                unsigned k = 0;
                uint8_t *snap = malloc(sh->sbytes);
                int any = 0;
                while (snap && cfg->hooks->event_probe(cfg->hooks->user, m, pc, bytes, k++, snap)) {
                    any = 1;
                    pthread_mutex_lock(&sh->mu);
                    {
                        queue_push(sh, (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input });
                        loc->event_probes++;
                        pthread_cond_signal(&sh->cv);
                        snap = malloc(sh->sbytes);
                    }
                    pthread_mutex_unlock(&sh->mu);
                }
                free(snap);
                if (any) loc->event_probe_sites++;
            }
        }
        if (cfg->hooks && cfg->hooks->irq_alt && m->rom_index(m, pc) >= 0) {
            // delay d = 1 is the plain IRQ fork; d = 2..3 is technique 9 (temporal
            // perturbation: the interrupt lands a few instructions later).  The hook
            // answers 0 unless an interrupt is pending, so the per-(site, delay) key is
            // spent only when a fork is really produced.
            for (unsigned d = 1; d <= (cfg->no_irq_jitter ? 1u : 3u); d++) {
                uint8_t *snap = malloc(sh->sbytes);
                if (!snap) break;
                if (!cfg->hooks->irq_alt(cfg->hooks->user, m, d, snap)) { free(snap); break; }
                pthread_mutex_lock(&sh->mu);
                int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben, ((uint64_t)m->rom_index(m, pc) + 1) | ((uint64_t)d << 40));
                if (!fresh) free(snap);
                else {
                    queue_push(sh, (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input });
                    if (d == 1) loc->irq_forks++; else loc->jitter_forks++;
                    pthread_cond_signal(&sh->cv);
                }
                pthread_mutex_unlock(&sh->mu);
            }
        }
        int cond = have_flow && fi.kind == DP_FLOW_COND && fi.target >= 0;
        if (cond && cfg->fork_site_cap && w->capped[pc & 0xFFFF] == pc + 1) { loc->sat_site++; cond = 0; }   // site already saturated: no snapshot, no lock
        uint8_t *pre = NULL;
        if (cond) {
            pre = malloc(sh->sbytes);
            m->save(m, pre);
        }
        if (!m->run(m)) { free(pre); break; }
        steps++;
        if (attract || play) loc->session_steps++;   // a long session has its own budget: it neither spends nor waits for the shared one
        else if (++w->step_batch == 256) { __atomic_fetch_add(&sh->steps, 256, __ATOMIC_RELAXED); w->step_batch = 0; }
        if (cfg->hooks && cfg->hooks->after_step) cfg->hooks->after_step(cfg->hooks->user, m, pc);
        if (cond) {
            uint32_t after = m->pc(m);
            uint32_t fall = pc + (uint32_t)len, tgt = (uint32_t)fi.target;
            uint32_t other = after == tgt ? fall : tgt;
            if (after != tgt && after != fall) { free(pre); continue; }   // an IRQ intervened
            if (covered(loc, m, other) && covered(loc, m, after)) {
                loc->pruned_covered++; free(pre);
            } else if (cfg->hooks && cfg->hooks->veto_fork &&
                       !cfg->hooks->veto_fork(cfg->hooks->user, m, pc, other)) {
                loc->hook_vetoed++; free(pre);
            } else if (w->win_on && w->cobs.nram && !covered(loc, m, other) && solve_cond(w, cfg, pc, other, cur_input)) {
                free(pre);   // proved a legal way to the other side: no forced inversion
            } else {
                pthread_mutex_lock(&sh->mu);
                uint32_t *cnt = site_count(&sh->sites, pc);
                int pushed = 0;
                if (cfg->cell_hi > cfg->cell_lo || !cfg->no_cells) {
                    if (cell_insert(sh, cell_key(cfg, sh->ramread, m, other))) loc->cells++;
                    else { loc->cell_pruned++; pthread_mutex_unlock(&sh->mu); free(pre); continue; }
                }
                if (cfg->fork_site_cap && *cnt >= cfg->fork_site_cap) { loc->sat_site++; w->capped[pc & 0xFFFF] = pc + 1; }
                else {
                    queue_push(sh, (Pending){ pre, other, 1, (uint32_t)(m->rom_index(m, pc) + 1), cur_input });
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

static void *worker_main(void *arg) {
    Worker *w = arg;
    Shared *sh = w->sh;
    const DpCoreConfig *cfg = sh->cfg;
    if (w->m->attach && !w->m->attach(w->m)) {
        fprintf(stderr, "core: worker could not build its machine, retired\n");
        return NULL;
    }
    // each worker hooks its own machine from its own thread (platform state may be thread-local)
    DpObservers obsv = { NULL, obs_read, obs_write };
    if (!cfg->no_copies && w->m->observe) w->m->observe(w->m, &obsv, &w->cobs);
    pthread_mutex_lock(&sh->mu);
    for (;;) {
        for (;;) {
            if (__atomic_load_n(&sh->halt, __ATOMIC_RELAXED)) {   // drop the backlog (the disk spool stays reported as left)
                if (!cfg->state_dir) {   // with --state-dir the backlog is kept for the frontier file
                    for (size_t i = 0; i < sh->qn; i++) free(sh->q[i].state);
                    sh->qn = 0;
                }
                break;
            }
            if (!sh->qn && sh->spool_n) spool_refill(sh);
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
                if (covered(&w->loc, w->m, sh->q[i].pc)) continue;
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

int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg_in, DpCoreResult *out) {
    DpCoreConfig cfg_unb = *cfg_in;
    if (cfg_unb.unbounded) { cfg_unb.max_total = 0; cfg_unb.max_path_steps = 0; cfg_unb.fork_site_cap = 0; }   // --unbounded: the one stop is exhaustion or SIGINT
    const DpCoreConfig *cfg = &cfg_unb;
    memset(out, 0, sizeof *out);
    out->rom_size = rom_size;
    if (!m || !m->isa || !m->run || !m->save || !m->load || !m->fetch || !m->rom_index) return 0;
    if (!m->pc || !m->set_pc) { out->no_pc = 1; return 1; }
    out->flags = calloc(rom_size ? rom_size : 1, 1);
    out->src_flags = calloc(rom_size ? rom_size : 1, sizeof *out->src_flags);
    out->first_seen = calloc(rom_size ? rom_size : 1, sizeof *out->first_seen);
    if (!out->flags || !out->src_flags || !out->first_seen) { dp_core_result_free(out); return 0; }

    Shared sh;
    memset(&sh, 0, sizeof sh);
    pthread_mutex_init(&sh.mu, NULL);
    pthread_cond_init(&sh.cv, NULL);
    sh.out = out; sh.cfg = cfg;
    sh.sbytes = m->state_bytes(m);
    sh.spool_fd = 0;
    sh.fed = calloc(0x10000, 1);
    sh.fedsrc = calloc(0x10000, sizeof(uint32_t));
    sh.fedprod = calloc(0x10000, sizeof(uint32_t));
    sh.ramread = calloc(0x10000, 1);
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
        w->loc.flags = out->flags; w->loc.src_flags = out->src_flags;
        w->loc.first_seen = out->first_seen; w->loc.rom_size = rom_size;
        w->visits = calloc(0x10000, sizeof *w->visits);
        w->capped = calloc(0x10000, sizeof *w->capped);
        w->stamp = calloc(0x10000, sizeof *w->stamp);
        w->wsnap = malloc(sh.sbytes ? sh.sbytes : 1); w->winmemo = calloc(0x10000, sizeof *w->winmemo);
        w->rng_stamp = calloc(0x10000, sizeof *w->rng_stamp); w->rng_last = calloc(0x10000, 1);
        if (!w->visits || !w->stamp || !w->capped) { ok = 0; break; }
        w->cobs = (CopyObs){ wm, &w->loc, -1, 0, sh.fed, 0, 99, -1, -1, 0, cfg->no_decompress };
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
    if (cfg->state_dir && cfg->resume) {   // states left pending by a previous run (same machine state size)
        char fp[600]; snprintf(fp, sizeof fp, "%s/frontier.bin", cfg->state_dir);
        FILE *ff = fopen(fp, "rb");
        if (ff) {
            uint32_t hd[3];
            if (fread(hd, 4, 3, ff) == 3 && hd[0] == 0x31524644u && hd[1] == sh.sbytes) {
                for (uint32_t i = 0; i < hd[2]; i++) {
                    SpoolHead h; uint8_t *st = malloc(sh.sbytes);
                    if (!st || fread(&h, sizeof h, 1, ff) != 1 || fread(st, 1, sh.sbytes, ff) != sh.sbytes) { free(st); break; }
                    ram_push(&sh, (Pending){ st, h.pc, (int)h.forced, h.site, h.input });
                    out->resumed++;
                }
            }
            fclose(ff);
        }
    }
        for (size_t i = 0; i < cfg->n_seeds; i++) {
            uint8_t *cp = malloc(sh.sbytes);
            if (!cp) break;
            memcpy(cp, start, sh.sbytes);
            if (cfg->seed_banks && cfg->seed_banks[i] > 1 && m->poke) {   // bank-carrying seed: switch the mapper, save that state
                m->load(m, start); m->poke(m, 0x2000, (uint8_t)cfg->seed_banks[i]); m->save(m, cp); m->load(m, start);
            }
            if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
            sh.q[sh.qn++] = (Pending){ cp, cfg->seeds[i], 1, 0, 0, 1 };
            out->seeded++;
        }
        // the natural path goes last: it is the first one taken
        if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
        sh.q[sh.qn++] = (Pending){ start, start_pc, 0, 0, 0 };
        // technique 3: start states whose RAM range already holds a plausible valid save
        if (m->poke && cfg->seed_n && cfg->seed_hi >= cfg->seed_lo) {
            for (unsigned v = 0; v < cfg->seed_n; v++) {
                m->load(m, start);
                for (uint32_t a = cfg->seed_lo; a <= cfg->seed_hi; a++) m->poke(m, a, cfg->seed_vals[v]);
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
                sh.q[sh.qn++] = (Pending){ cp, start_pc, 0, 0, i < na ? SESSION_ATTRACT : SESSION_PLAY + (i - na) };
            }
        }
        // technique 9 (TAS form): the movie from power-on, unshifted and shifted by 1..3 frames
        if (m->frame && m->set_input && cfg->tas_masks && cfg->tas_frames) {
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
        if (m->frame && m->set_input && cfg->tas_masks && cfg->tas_frames && !cfg->no_tas_jitter) {
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

    free(sh.tas_start);
    for (unsigned i = 0; i < started; i++) {
        DpCoreResult *l = &ws[i].loc;
        out->paths += l->paths; out->forks += l->forks;
        out->sat_total += l->sat_total; out->sat_path += l->sat_path; out->sat_loop += l->sat_loop;
        out->attract_paths += l->attract_paths; out->play_paths += l->play_paths; out->play_epochs += l->play_epochs; out->session_frames += l->session_frames; out->session_steps += l->session_steps; out->session_stalls += l->session_stalls;
        out->sat_site += l->sat_site; out->sat_queue += l->sat_queue;
        out->pruned_covered += l->pruned_covered; out->copy_bytes += l->copy_bytes;
        out->copy_forced += l->copy_forced; out->forced_ram_cuts += l->forced_ram_cuts;
        out->taint_cut_indirect += l->taint_cut_indirect; out->taint_cut_ret += l->taint_cut_ret;
        out->novelty_picks += l->novelty_picks; out->site_scored_picks += l->site_scored_picks; out->learned_sigs += l->learned_sigs; out->solved_forks += l->solved_forks; out->solve_tries += l->solve_tries; out->rng_forks += l->rng_forks; out->index_replays += l->index_replays; out->compressed_bytes += l->compressed_bytes; out->decompress_runs += l->decompress_runs; out->cells += l->cells; out->closure_roots += l->closure_roots; out->closure_bytes += l->closure_bytes; out->closure_capped += l->closure_capped; out->closure_jp_tables += l->closure_jp_tables; out->closure_jp_entries += l->closure_jp_entries; out->prov_data_bytes += l->prov_data_bytes; out->event_probes += l->event_probes; out->event_probe_sites += l->event_probe_sites; out->irq_forks += l->irq_forks; out->jitter_forks += l->jitter_forks; out->cell_pruned += l->cell_pruned; out->novelty_new_bytes += l->novelty_new_bytes; out->plain_new_bytes += l->plain_new_bytes; out->hook_vetoed += l->hook_vetoed; out->tas_paths += l->tas_paths;
        out->exec_outside += l->exec_outside; out->exec_wram_rom += l->exec_wram_rom; dp_ramarch_merge(&out->ra, &l->ra);
        free(ws[i].visits); free(ws[i].wsnap); free(ws[i].winmemo); free(ws[i].capped); free(ws[i].stamp);
        if (i && ws[i].m && ws[i].m->close) ws[i].m->close(ws[i].m);
    }
    out->steps = sh.steps;
    for (unsigned i = 0; i < started; i++) out->steps += 0;   /* session steps are reported separately */
    out->spool_left = sh.spool_n; out->spooled = sh.spool_total;
    if (cfg->state_dir) {   // the pending frontier (RAM queue, then the disk spool) for --resume
        char fp[600]; snprintf(fp, sizeof fp, "%s/frontier.bin", cfg->state_dir);
        if (sh.qn + sh.spool_n == 0) remove(fp);
        else {
            FILE *ff = fopen(fp, "wb");
            if (ff) {
                uint32_t hd[3] = { 0x31524644u, sh.sbytes, (uint32_t)(sh.qn + sh.spool_n) };
                fwrite(hd, 4, 3, ff);
                for (size_t i = 0; i < sh.qn; i++) {
                    SpoolHead h = { sh.q[i].pc, (uint32_t)sh.q[i].forced, sh.q[i].site, sh.q[i].input };
                    fwrite(&h, sizeof h, 1, ff); fwrite(sh.q[i].state, 1, sh.sbytes, ff);
                }
                uint8_t *rec = malloc(sh.spool_rs ? sh.spool_rs : 1);
                for (uint64_t i = 0; rec && i < sh.spool_n; i++)
                    if (pread(sh.spool_fd, rec, sh.spool_rs, (off_t)(i * sh.spool_rs)) == (ssize_t)sh.spool_rs) fwrite(rec, 1, sh.spool_rs, ff);
                free(rec); fclose(ff);
                out->saved = (uint64_t)(sh.qn + sh.spool_n);
            }
        }
    }
    if (sh.spool_fd > 0) close(sh.spool_fd);
    for (size_t i = 0; i < sh.qn; i++) free(sh.q[i].state);
    free(sh.probeset); free(sh.cellset); free(sh.q); free(sh.sites.s); free(sh.fed); free(sh.fedsrc); free(sh.fedprod); free(sh.ramread); free(sh.site_picks); free(sh.site_gain); free(ws);
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
    if (r->halted) fprintf(out, "halted: %s\n", r->halted == 1 ? "--max-time reached" : "--stall-time: no new ROM byte");
    if (r->attract_paths || r->play_paths) fprintf(out, "sessions: %llu attract, %llu play (%llu pad epochs), %llu frames played, %llu steps, %llu stopped by stagnation\n", U(r->attract_paths), U(r->play_paths), U(r->play_epochs), U(r->session_frames), U(r->session_steps), U(r->session_stalls));
    if (r->resumed || r->saved) fprintf(out, "frontier: %llu states resumed, %llu saved\n", U(r->resumed), U(r->saved));
    fprintf(out, "spool: %llu states spilled to disk, %llu left unexplored\n", U(r->spooled), U(r->spool_left));
    fprintf(out, "saturation: total %llu path %llu loop %llu site %llu queue %llu\n", U(r->sat_total), U(r->sat_path), U(r->sat_loop), U(r->sat_site), U(r->sat_queue));
    fprintf(out, "index replays %llu\n", U(r->index_replays));
    fprintf(out, "rng-byte forks %llu\n", U(r->rng_forks));
    fprintf(out, "learned signatures %llu\n", U(r->learned_sigs));
    fprintf(out, "solved forks %llu (tries %llu)\n", U(r->solved_forks), U(r->solve_tries));
    fprintf(out, "tas replay paths %llu\n", U(r->tas_paths));
    fprintf(out, "site-scored picks %llu\n", U(r->site_scored_picks));
    fprintf(out, "novelty picks %llu (new ROM bytes: novelty %llu, plain %llu)\n", U(r->novelty_picks), U(r->novelty_new_bytes), U(r->plain_new_bytes));
    fprintf(out, "workers %llu (short %llu)%s\n", U(r->workers), U(r->workers_short), r->workers_max ? "" : "");
    fprintf(out, "cells %llu, cell-pruned forks %llu\n", U(r->cells), U(r->cell_pruned));
    fprintf(out, "decompress runs %llu, compressed ROM bytes %llu\n", U(r->decompress_runs), U(r->compressed_bytes));
    fprintf(out, "seeded RAM states %llu\n", U(r->seeded_ram));
    fprintf(out, "held-pad sessions %llu\n", U(r->sessions));
    fprintf(out, "IRQ forks %llu (timing jitter %llu)\n", U(r->irq_forks), U(r->jitter_forks));
    fprintf(out, "event probes %llu at %llu sites: %llu forced variants\n", U(r->event_probes), U(r->event_probe_sites), U(r->event_probes));
    fprintf(out, "seeded entry points %llu\n", U(r->seeded));
    fprintf(out, "forced RAM cuts %llu\n", U(r->forced_ram_cuts));
    fprintf(out, "forced taint cuts: indirect %llu, ret %llu\n", U(r->taint_cut_indirect), U(r->taint_cut_ret));
    if (r->exec_wram_rom) fprintf(out, "RAM-executed instructions traced to ROM copy sources: %llu\n", U(r->exec_wram_rom));
    if (r->ra.n) fprintf(out, "runtime code archive: %u distinct instructions executed from RAM/VRAM/HRAM, %u attached to a ROM copy source, %u generated (no ROM source; %u with an approximate ROM producer), %u rewrote an earlier instruction at the same address (self-modifying)\n", r->ra.n, r->ra.attached, r->ra.n - r->ra.attached, r->ra.derived, r->ra.poly);
    fprintf(out, "exec_outside %llu, rom->ram copy bytes %llu (%zu distinct ROM sources), forced %llu\n", U(r->exec_outside), U(r->copy_bytes), copied, U(r->copy_forced));
    DpCoreSection secs[64];
    size_t ns = dp_core_sections(r, secs, 64);
    fprintf(out, "sections %zu\n", ns);
    for (size_t i = 0; i < ns && i < 64; i++)
        fprintf(out, "  section %zu rom-index %zu-%zu natural %zu forced %zu verdict %s\n", i + 1, secs[i].lo, secs[i].hi, secs[i].natural, secs[i].forced, secs[i].required ? "required" : "speculative");
    size_t stat_only = 0;
    for (size_t i = 0; i < r->rom_size; i++) if ((r->flags[i] & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED)) == DP_CORE_STATIC) stat_only++;
    fprintf(out, "static closure: roots %llu, bytes %llu, static-only %zu, capped walks %llu\n", U(r->closure_roots), U(r->closure_bytes), stat_only, U(r->closure_capped));
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
