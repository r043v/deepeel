// core.c: generic exploration kernel over DpMachine + DpIsa (see core.h).
#include "core.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *state;
    uint32_t pc;       // where the forced path resumes
    int forced;
    uint32_t site;     // fork site rom index + 1 (0: none), for the per-site novelty score
    uint32_t input;    // held pad mask + 1 (0: none); inherited by every fork of the path
} Pending;

typedef struct {
    uint32_t pc;
    uint32_t count;
} Site;

void dp_core_config_default(DpCoreConfig *c) {
    memset(c, 0, sizeof *c);
    c->max_total = 5000000;
    c->max_path_steps = 100000;
    c->fork_site_cap = 4;      // DP_DEFAULT_BRANCH_FORK_CAP
    c->visits_per_pc = 64;
    c->queue_max = 65536;
}

int dp_core_parse_option(DpCoreConfig *c, int argc, char **argv, int i) {
    const char *a = argv[i];
    if (!strcmp(a, "--cells") && i + 1 < argc) {   // LO-HI: RAM range hashed into a cell
        char *e; unsigned long lo = strtoul(argv[i + 1], &e, 0);
        if (*e == '-') { c->cell_lo = (uint32_t)lo; c->cell_hi = (uint32_t)strtoul(e + 1, NULL, 0); }
        return 2;
    }
    if (!strcmp(a, "--no-event-probes")) { c->no_event_probes = 1; return 1; }
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
    if (!strcmp(a, "--no-sessions")) { c->no_sessions = 1; return 1; }
    if (!strcmp(a, "--sessions") && i + 1 < argc) { c->sessions = (uint32_t)strtoul(argv[i + 1], NULL, 0); c->no_sessions = 0; return 2; }
    if (!strcmp(a, "--no-irq-jitter")) { c->no_irq_jitter = 1; return 1; }
    if (!strcmp(a, "--no-decompress")) { c->no_decompress = 1; return 1; }
    if (!strcmp(a, "--no-copies")) { c->no_copies = 1; return 1; }
    if (!strcmp(a, "--no-learn-sig")) { c->no_learn_sig = 1; return 1; }
    if (!strcmp(a, "--no-taint-cut")) { c->no_taint_cut = 1; return 1; }
    if (!strcmp(a, "--no-forced-ram-cut")) { c->no_forced_ram_cut = 1; return 1; }
    if (!strcmp(a, "--no-site-score")) { c->no_site_score = 1; return 1; }
    if (!strcmp(a, "--no-novelty-order")) { c->no_novelty_order = 1; return 1; }
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
    DpCoreResult *out;
    const DpCoreConfig *cfg;
    uint8_t *fed;
    uint32_t sbytes;
    uint64_t *cellset;         // technique 8: open-addressing set of (target, RAM cell) keys
    size_t cellcap, celln;
    uint64_t *probeset;        // technique 2: sites already probed
    size_t probecap, proben;
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
    uint32_t path_id;
    int ok;
    pthread_t th;
} Worker;

static void mark(Shared *sh, DpCoreResult *r, DpMachine *m, uint32_t pc, unsigned len, uint8_t how) {
    for (unsigned i = 0; i < len; i++) {
        int64_t k = m->rom_index(m, pc + i);
        if (k >= 0 && (size_t)k < r->rom_size) {
            uint8_t old = __atomic_fetch_or(&r->flags[k], how, __ATOMIC_RELAXED);
            if (!old) {
                r->first_seen[k] = __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) + 1;
                if (r->in_novelty_path) r->novelty_new_bytes++; else r->plain_new_bytes++;
            }
            __atomic_fetch_or(&r->src_flags[k], (how & DP_CORE_NATURAL) ? DP_SRC_FORK_NATURAL : DP_SRC_FORK_FORCED, __ATOMIC_RELAXED);
        }
    }
}

static int covered(DpCoreResult *r, DpMachine *m, uint32_t pc) {
    int64_t k = m->rom_index(m, pc);
    return k >= 0 && (size_t)k < r->rom_size && __atomic_load_n(&r->flags[k], __ATOMIC_RELAXED);
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

static void obs_read(void *u, uint16_t addr, uint8_t v) {
    CopyObs *o = u;
    int64_t k = o->m->rom_index(o->m, addr);
    if (k >= 0 && (size_t)k < o->r->rom_size) {
        o->last_rom = k; o->last_val = v; o->age = 0;
        if (o->run_hi >= 0 && k >= o->run_hi && k <= o->run_hi + 2) o->run_hi = k;
        else { run_flush(o); o->run_lo = o->run_hi = k; }
    }
}

static void obs_write(void *u, uint16_t addr, uint8_t v) {
    CopyObs *o = u;
    if (o->run_hi >= 0 && o->m->rom_index(o->m, addr) < 0) o->run_writes++;
    if (o->last_rom < 0 || o->age > 4) return;
    if (o->m->rom_index(o->m, addr) < 0 && v == o->last_val) {
        if (o->forced) o->r->copy_forced++;
        else {
            __atomic_fetch_or(&o->r->flags[o->last_rom], DP_CORE_COPIED, __ATOMIC_RELAXED);
            __atomic_fetch_or(&o->r->src_flags[o->last_rom], DP_SRC_COPY, __ATOMIC_RELAXED);
            o->r->copy_bytes++;
            if (o->fed) o->fed[addr] = 1;
        }
    }
    o->last_rom = -1;
}

// FNV-1a over the configured RAM range, mixed with the fork target
static uint64_t cell_key(const DpCoreConfig *cfg, DpMachine *m, uint32_t other) {
    uint64_t h = 1469598103934665603ull ^ other;
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

static void run_path(Worker *w, Pending *p) {
    Shared *sh = w->sh;
    const DpCoreConfig *cfg = sh->cfg;
    DpMachine *m = w->m;
    DpCoreResult *loc = &w->loc;
    const DpIsa *isa = m->isa;
    m->load(m, p->state);
    free(p->state);
    if (p->forced) m->set_pc(m, p->pc);
    uint32_t cur_input = p->input;
    // a session shares half the total budget with its siblings: it must not starve the natural path
    uint64_t session_cap = cfg->max_total / (2 * (cfg->sessions ? (cfg->sessions > 16 ? 16 : cfg->sessions) : 16));
    if (m->set_input) m->set_input(m, cur_input ? cur_input - 1 : 0);
    loc->paths++;
    run_flush(&w->cobs);
    w->cobs.forced = p->forced; w->cobs.last_rom = -1;
    uint32_t path_id = ++w->path_id, steps = 0;
    int cur_forced = p->forced;
    unsigned fdepth = 0;   // calls made since the forced fork: a ret below that pops state the path did not build
    for (;;) {
        if (cfg->max_total && __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) >= cfg->max_total) {
            loc->sat_total++;
            pthread_mutex_lock(&sh->mu);
            for (size_t i = 0; i < sh->qn; i++) free(sh->q[i].state);
            sh->qn = 0;
            pthread_mutex_unlock(&sh->mu);
            break;
        }
        if (cfg->max_path_steps && steps >= cfg->max_path_steps) { loc->sat_path++; break; }
        if (cur_input && cfg->max_total && steps >= session_cap) { loc->sat_path++; break; }
        uint32_t pc = m->pc(m);
        if (cfg->hooks && cfg->hooks->before_step &&
            !cfg->hooks->before_step(cfg->hooks->user, m, pc)) break;
        uint16_t vi = (uint16_t)pc;
        if (w->stamp[vi] != path_id) { w->stamp[vi] = path_id; w->visits[vi] = 0; }
        if (cfg->visits_per_pc && ++w->visits[vi] > cfg->visits_per_pc) { loc->sat_loop++; break; }
        uint8_t bytes[8] = {0};
        for (unsigned i = 0; i < sizeof bytes; i++) bytes[i] = m->fetch(m, pc + i);
        w->cobs.age++;
        int len = isa->length(bytes[0]);
        DpFlowInfo fi;
        memset(&fi, 0, sizeof fi);
        int have_flow = isa->flow && isa->flow(bytes, (uint16_t)pc, &fi);
        if (have_flow && fi.length) len = fi.length;
        if (len <= 0) len = 1;
        if (m->rom_index(m, pc) < 0) {
            if (cur_forced && !cfg->no_forced_ram_cut && !(sh->fed && sh->fed[pc & 0xffff])) {
                loc->forced_ram_cuts++;
                break;
            }
            loc->exec_outside++;
        }
        // technique 3 (learned form): z80 `ld a,(nn)` of RAM straight followed by `cp n` is a signature test;
        // queue a state whose byte already holds n so the matching side runs as natural
        if (!cfg->no_learn_sig && !cur_forced && m->poke && bytes[0] == 0x3A && bytes[3] == 0xFE &&
            m->rom_index(m, bytes[1] | (bytes[2] << 8)) < 0 && isa->name && !strcmp(isa->name, "z80")) {
            uint32_t addr = bytes[1] | (bytes[2] << 8);
            pthread_mutex_lock(&sh->mu);
            int fresh = keyset_insert(&sh->probeset, &sh->probecap, &sh->proben,
                                      ((uint64_t)addr << 8 | bytes[4]) | (3ull << 60));
            pthread_mutex_unlock(&sh->mu);
            if (fresh) {
                uint8_t *tmp = malloc(sh->sbytes), *snap = malloc(sh->sbytes);
                if (tmp && snap) {
                    m->save(m, tmp);
                    m->poke(m, addr, bytes[4]);
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
                    if (cfg->queue_max && sh->qn >= cfg->queue_max) loc->sat_queue++;
                    else {
                        if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                        sh->q[sh->qn++] = (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input };
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
                if (!fresh || (cfg->queue_max && sh->qn >= cfg->queue_max)) { if (fresh) loc->sat_queue++; free(snap); }
                else {
                    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                    sh->q[sh->qn++] = (Pending){ snap, pc, 0, (uint32_t)(m->rom_index(m, pc) + 1), cur_input };
                    if (d == 1) loc->irq_forks++; else loc->jitter_forks++;
                    pthread_cond_signal(&sh->cv);
                }
                pthread_mutex_unlock(&sh->mu);
            }
        }
        int cond = have_flow && fi.kind == DP_FLOW_COND && fi.target >= 0;
        uint8_t *pre = NULL;
        if (cond) {
            pre = malloc(sh->sbytes);
            m->save(m, pre);
        }
        if (!m->run(m)) { free(pre); break; }
        steps++;
        __atomic_fetch_add(&sh->steps, 1, __ATOMIC_RELAXED);
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
            } else {
                pthread_mutex_lock(&sh->mu);
                uint32_t *cnt = site_count(&sh->sites, pc);
                int pushed = 0;
                if (cfg->cell_hi > cfg->cell_lo) {
                    if (cell_insert(sh, cell_key(cfg, m, other))) loc->cells++;
                    else { loc->cell_pruned++; pthread_mutex_unlock(&sh->mu); free(pre); continue; }
                }
                if (cfg->fork_site_cap && *cnt >= cfg->fork_site_cap) loc->sat_site++;
                else if (cfg->queue_max && sh->qn >= cfg->queue_max) loc->sat_queue++;
                else {
                    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                    sh->q[sh->qn++] = (Pending){ pre, other, 1, (uint32_t)(m->rom_index(m, pc) + 1), cur_input };
                    (*cnt)++;
                    loc->forks++;
                    pushed = 1;
                    pthread_cond_signal(&sh->cv);
                }
                pthread_mutex_unlock(&sh->mu);
                if (!pushed) free(pre);
            }
        }
    }
}

static void *worker_main(void *arg) {
    Worker *w = arg;
    Shared *sh = w->sh;
    const DpCoreConfig *cfg = sh->cfg;
    pthread_mutex_lock(&sh->mu);
    for (;;) {
        while (!sh->qn && sh->active > 0) pthread_cond_wait(&sh->cv, &sh->mu);
        if (!sh->qn) break;
        size_t pick = sh->qn - 1;
        w->loc.in_novelty_path = 0;
        if (!cfg->no_novelty_order) {
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
    return NULL;
}

int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg, DpCoreResult *out) {
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
    sh.fed = calloc(0x10000, 1);
    sh.site_picks = calloc(rom_size + 1, sizeof *sh.site_picks);
    sh.site_gain = calloc(rom_size + 1, sizeof *sh.site_gain);

    unsigned nw = cfg->jobs > 1 ? cfg->jobs : 1;
    Worker *ws = calloc(nw, sizeof *ws);
    int ok = sh.fed && ws;
    unsigned started = 0;
    for (unsigned i = 0; ok && i < nw; i++) {
        Worker *w = &ws[i];
        DpMachine *wm = i == 0 ? m : (m->clone ? m->clone(m, i) : NULL);
        if (!wm) break;                       // fewer workers than asked: reported
        w->sh = &sh; w->m = wm;
        w->loc.flags = out->flags; w->loc.src_flags = out->src_flags;
        w->loc.first_seen = out->first_seen; w->loc.rom_size = rom_size;
        w->visits = calloc(0x10000, sizeof *w->visits);
        w->stamp = calloc(0x10000, sizeof *w->stamp);
        if (!w->visits || !w->stamp) { ok = 0; break; }
        w->cobs = (CopyObs){ wm, &w->loc, -1, 0, sh.fed, 0, 99, -1, -1, 0, cfg->no_decompress };
        started++;
    }
    out->workers = started;
    if (started < nw && ok) out->workers_short = nw - started;
    ok = ok && started > 0;

    if (ok) {
        // every worker observes its own machine; the start state is the main one's
        for (unsigned i = 0; i < started; i++) {
            DpObservers obsv = { NULL, obs_read, obs_write };
            if (!cfg->no_copies && ws[i].m->observe) ws[i].m->observe(ws[i].m, &obsv, &ws[i].cobs);
        }
        uint8_t *start = malloc(sh.sbytes);
        m->save(m, start);
        uint32_t start_pc = m->pc(m);
        sh.q = NULL;
        for (size_t i = 0; i < cfg->n_seeds; i++) {
            uint8_t *cp = malloc(sh.sbytes);
            if (!cp) break;
            memcpy(cp, start, sh.sbytes);
            if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
            sh.q[sh.qn++] = (Pending){ cp, cfg->seeds[i], 1, 0, 0 };
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
        // clones start from power-on, the same state the main machine saved
        for (unsigned i = 1; i < started; i++) pthread_create(&ws[i].th, NULL, worker_main, &ws[i]);
        worker_main(&ws[0]);
        for (unsigned i = 1; i < started; i++) pthread_join(ws[i].th, NULL);
    }

    for (unsigned i = 0; i < started; i++) {
        DpCoreResult *l = &ws[i].loc;
        out->paths += l->paths; out->forks += l->forks;
        out->sat_total += l->sat_total; out->sat_path += l->sat_path; out->sat_loop += l->sat_loop;
        out->sat_site += l->sat_site; out->sat_queue += l->sat_queue;
        out->pruned_covered += l->pruned_covered; out->copy_bytes += l->copy_bytes;
        out->copy_forced += l->copy_forced; out->forced_ram_cuts += l->forced_ram_cuts;
        out->taint_cut_indirect += l->taint_cut_indirect; out->taint_cut_ret += l->taint_cut_ret;
        out->novelty_picks += l->novelty_picks; out->site_scored_picks += l->site_scored_picks; out->learned_sigs += l->learned_sigs; out->compressed_bytes += l->compressed_bytes; out->decompress_runs += l->decompress_runs; out->cells += l->cells; out->event_probes += l->event_probes; out->event_probe_sites += l->event_probe_sites; out->irq_forks += l->irq_forks; out->jitter_forks += l->jitter_forks; out->cell_pruned += l->cell_pruned; out->novelty_new_bytes += l->novelty_new_bytes; out->plain_new_bytes += l->plain_new_bytes; out->hook_vetoed += l->hook_vetoed;
        out->exec_outside += l->exec_outside;
        free(ws[i].visits); free(ws[i].stamp);
        if (i && ws[i].m && ws[i].m->close) ws[i].m->close(ws[i].m);
    }
    out->steps = sh.steps;
    for (size_t i = 0; i < sh.qn; i++) free(sh.q[i].state);
    free(sh.probeset); free(sh.cellset); free(sh.q); free(sh.sites.s); free(sh.fed); free(sh.site_picks); free(sh.site_gain); free(ws);
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
    fprintf(out, "saturation: total %llu path %llu loop %llu site %llu queue %llu\n", U(r->sat_total), U(r->sat_path), U(r->sat_loop), U(r->sat_site), U(r->sat_queue));
    fprintf(out, "learned signatures %llu\n", U(r->learned_sigs));
    fprintf(out, "site-scored picks %llu\n", U(r->site_scored_picks));
    fprintf(out, "novelty picks %llu (new ROM bytes: novelty %llu, plain %llu)\n", U(r->novelty_picks), U(r->novelty_new_bytes), U(r->plain_new_bytes));
    fprintf(out, "workers %llu (short %llu)\n", U(r->workers), U(r->workers_short));
    fprintf(out, "cells %llu, cell-pruned forks %llu\n", U(r->cells), U(r->cell_pruned));
    fprintf(out, "decompress runs %llu, compressed ROM bytes %llu\n", U(r->decompress_runs), U(r->compressed_bytes));
    fprintf(out, "seeded RAM states %llu\n", U(r->seeded_ram));
    fprintf(out, "held-pad sessions %llu\n", U(r->sessions));
    fprintf(out, "IRQ forks %llu (timing jitter %llu)\n", U(r->irq_forks), U(r->jitter_forks));
    fprintf(out, "event probes %llu at %llu sites\n", U(r->event_probes), U(r->event_probe_sites));
    fprintf(out, "seeded entry points %llu\n", U(r->seeded));
    fprintf(out, "forced RAM cuts %llu\n", U(r->forced_ram_cuts));
    fprintf(out, "forced taint cuts: indirect %llu, ret %llu\n", U(r->taint_cut_indirect), U(r->taint_cut_ret));
    fprintf(out, "exec_outside %llu, rom->ram copy bytes %llu (%zu distinct ROM sources), forced %llu\n", U(r->exec_outside), U(r->copy_bytes), copied, U(r->copy_forced));
    DpCoreSection secs[64];
    size_t ns = dp_core_sections(r, secs, 64);
    fprintf(out, "sections %zu\n", ns);
    for (size_t i = 0; i < ns && i < 64; i++)
        fprintf(out, "  section %zu rom-index %zu-%zu natural %zu forced %zu verdict %s\n", i + 1, secs[i].lo, secs[i].hi, secs[i].natural, secs[i].forced, secs[i].required ? "required" : "speculative");
    fprintf(out, "rom bytes: natural %zu, forced %zu (speculative)\n", nat, forced);
    #undef U
}
