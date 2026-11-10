// core.c: generic exploration kernel over DpMachine + DpIsa (see core.h).
#include "core.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *state;
    uint32_t pc;       // where the forced path resumes
    int forced;
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
    if (!strcmp(a, "--no-copies")) { c->no_copies = 1; return 1; }
    if (!strcmp(a, "--no-forced-ram-cut")) { c->no_forced_ram_cut = 1; return 1; }
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
    int active;                // workers currently running a path
    uint64_t steps;            // global instruction count (atomic)
    DpCoreResult *out;
    const DpCoreConfig *cfg;
    uint8_t *fed;
    uint32_t sbytes;
} Shared;

typedef struct {
    DpMachine *m;
    DpCoreResult *r;
    int64_t last_rom;     // ROM index of the last ROM byte read, -1 when consumed
    uint8_t last_val;
    uint8_t *fed;         // 64 KiB bitmap: CPU addresses written by a natural verbatim ROM copy
    int forced;           // the current path is a forced fork: speculative state
    int age;              // instructions since that read; a stale read is not a copy
} CopyObs;

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
            if (!old) r->first_seen[k] = __atomic_load_n(&sh->steps, __ATOMIC_RELAXED) + 1;
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
    if (k >= 0 && (size_t)k < o->r->rom_size) { o->last_rom = k; o->last_val = v; o->age = 0; }
}

static void obs_write(void *u, uint16_t addr, uint8_t v) {
    CopyObs *o = u;
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

static void run_path(Worker *w, Pending *p) {
    Shared *sh = w->sh;
    const DpCoreConfig *cfg = sh->cfg;
    DpMachine *m = w->m;
    DpCoreResult *loc = &w->loc;
    const DpIsa *isa = m->isa;
    m->load(m, p->state);
    free(p->state);
    if (p->forced) m->set_pc(m, p->pc);
    loc->paths++;
    w->cobs.forced = p->forced; w->cobs.last_rom = -1;
    uint32_t path_id = ++w->path_id, steps = 0;
    int cur_forced = p->forced;
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
        mark(sh, loc, m, pc, (unsigned)len, cur_forced ? DP_CORE_FORCED : DP_CORE_NATURAL);
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
                if (cfg->fork_site_cap && *cnt >= cfg->fork_site_cap) loc->sat_site++;
                else if (cfg->queue_max && sh->qn >= cfg->queue_max) loc->sat_queue++;
                else {
                    if (sh->qn == sh->qcap) { sh->qcap = sh->qcap ? sh->qcap * 2 : 64; sh->q = realloc(sh->q, sh->qcap * sizeof *sh->q); }
                    sh->q[sh->qn++] = (Pending){ pre, other, 1 };
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
        if (!cfg->no_novelty_order) {
            for (size_t i = sh->qn; i-- > 0;)
                if (!covered(&w->loc, w->m, sh->q[i].pc)) { pick = i; break; }
            if (pick != sh->qn - 1) w->loc.novelty_picks++;
        }
        Pending p = sh->q[pick];
        sh->q[pick] = sh->q[sh->qn - 1];
        sh->qn--;
        sh->active++;
        pthread_mutex_unlock(&sh->mu);
        run_path(w, &p);
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
        w->cobs = (CopyObs){ wm, &w->loc, -1, 0, sh.fed, 0, 99 };
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
            sh.q[sh.qn++] = (Pending){ cp, cfg->seeds[i], 1 };
            out->seeded++;
        }
        // the natural path goes last: it is the first one taken
        if (sh.qn == sh.qcap) { sh.qcap = sh.qcap ? sh.qcap * 2 : 64; sh.q = realloc(sh.q, sh.qcap * sizeof *sh.q); }
        sh.q[sh.qn++] = (Pending){ start, start_pc, 0 };
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
        out->novelty_picks += l->novelty_picks; out->hook_vetoed += l->hook_vetoed;
        out->exec_outside += l->exec_outside;
        free(ws[i].visits); free(ws[i].stamp);
        if (i && ws[i].m && ws[i].m->close) ws[i].m->close(ws[i].m);
    }
    out->steps = sh.steps;
    for (size_t i = 0; i < sh.qn; i++) free(sh.q[i].state);
    free(sh.q); free(sh.sites.s); free(sh.fed); free(ws);
    return ok;
}
