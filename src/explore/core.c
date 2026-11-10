// core.c: generic exploration kernel over DpMachine + DpIsa (see core.h).
#include "core.h"

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

static void mark(DpCoreResult *r, DpMachine *m, uint32_t pc, unsigned len, uint8_t how) {
    for (unsigned i = 0; i < len; i++) {
        int64_t k = m->rom_index(m, pc + i);
        if (k >= 0 && (size_t)k < r->rom_size) {
            if (!r->flags[k]) r->first_seen[k] = r->steps + 1;
            r->flags[k] |= how;
            r->src_flags[k] |= (how & DP_CORE_NATURAL) ? DP_SRC_FORK_NATURAL : DP_SRC_FORK_FORCED;
        }
    }
}

static int covered(DpCoreResult *r, DpMachine *m, uint32_t pc) {
    int64_t k = m->rom_index(m, pc);
    return k >= 0 && (size_t)k < r->rom_size && r->flags[k];
}

// per-site fork counter, open addressing on the pc
typedef struct { Site *s; size_t cap, n; } SiteTab;
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

typedef struct {
    DpMachine *m;
    DpCoreResult *r;
    int64_t last_rom;     // ROM index of the last ROM byte read, -1 when consumed
    uint8_t last_val;
    uint8_t *fed;         // 64 KiB bitmap: CPU addresses written by a natural verbatim ROM copy
    int forced;           // the current path is a forced fork: speculative state
    int age;              // instructions since that read; a stale read is not a copy
} CopyObs;

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
        else { o->r->flags[o->last_rom] |= DP_CORE_COPIED; o->r->src_flags[o->last_rom] |= DP_SRC_COPY; o->r->copy_bytes++; if (o->fed) o->fed[addr] = 1; }
    }
    o->last_rom = -1;
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
    const DpIsa *isa = m->isa;
    uint8_t *fed = calloc(0x10000, 1);
    CopyObs cobs = { m, out, -1, 0, fed, 0, 99 };
    DpObservers obsv = { NULL, obs_read, obs_write };
    if (!cfg->no_copies && m->observe) m->observe(m, &obsv, &cobs);
    uint32_t sbytes = m->state_bytes(m);
    uint8_t *start = malloc(sbytes);
    m->save(m, start);

    Pending *q = NULL; size_t qn = 0, qcap = 0;
    SiteTab sites = {0};
    uint32_t *visits = calloc(0x10000, sizeof *visits);
    uint32_t *stamp = calloc(0x10000, sizeof *stamp);
    int ok = start && visits && stamp;
    // the first path is the natural one: the machine's current state
    uint8_t *cur_state = start; start = NULL;
    uint32_t cur_pc = m->pc(m);
    int cur_forced = 0, have = ok;
    uint32_t path_id = 0;

    while (ok && have) {
        m->load(m, cur_state);
        free(cur_state); cur_state = NULL;
        if (cur_forced) m->set_pc(m, cur_pc);
        out->paths++;
        cobs.forced = cur_forced; cobs.last_rom = -1;
        path_id++;
        uint32_t steps = 0;
        for (;;) {
            if (cfg->max_total && out->steps >= cfg->max_total) { out->sat_total++; qn = 0; break; }
            if (cfg->max_path_steps && steps >= cfg->max_path_steps) { out->sat_path++; break; }
            uint32_t pc = m->pc(m);
            if (cfg->hooks && cfg->hooks->before_step &&
                !cfg->hooks->before_step(cfg->hooks->user, m, pc)) break;
            uint16_t vi = (uint16_t)pc;
            if (stamp[vi] != path_id) { stamp[vi] = path_id; visits[vi] = 0; }
            if (cfg->visits_per_pc && ++visits[vi] > cfg->visits_per_pc) { out->sat_loop++; break; }
            uint8_t bytes[8] = {0};
            for (unsigned i = 0; i < sizeof bytes; i++) bytes[i] = m->fetch(m, pc + i);
            cobs.age++;
            int len = isa->length(bytes[0]);
            DpFlowInfo fi;
            memset(&fi, 0, sizeof fi);
            int have_flow = isa->flow && isa->flow(bytes, (uint16_t)pc, &fi);
            if (have_flow && fi.length) len = fi.length;
            if (len <= 0) len = 1;
            if (m->rom_index(m, pc) < 0) {
                if (cur_forced && !cfg->no_forced_ram_cut && !(fed && fed[pc & 0xffff])) {
                    out->forced_ram_cuts++;
                    break;
                }
                out->exec_outside++;
            }
            mark(out, m, pc, (unsigned)len, cur_forced ? DP_CORE_FORCED : DP_CORE_NATURAL);
            int cond = have_flow && fi.kind == DP_FLOW_COND && fi.target >= 0;
            uint8_t *pre = NULL;
            if (cond) {
                pre = malloc(sbytes);
                m->save(m, pre);
            }
            if (!m->run(m)) { free(pre); break; }
            steps++; out->steps++;
            if (cfg->hooks && cfg->hooks->after_step) cfg->hooks->after_step(cfg->hooks->user, m, pc);
            if (cond) {
                uint32_t after = m->pc(m);
                uint32_t fall = pc + (uint32_t)len, tgt = (uint32_t)fi.target;
                uint32_t other = after == tgt ? fall : tgt;
                if (after != tgt && after != fall) { free(pre); continue; }   // an IRQ intervened
                if (covered(out, m, other) && covered(out, m, after)) {
                    out->pruned_covered++; free(pre);
                } else if (cfg->hooks && cfg->hooks->veto_fork &&
                           !cfg->hooks->veto_fork(cfg->hooks->user, m, pc, other)) {
                    out->hook_vetoed++; free(pre);
                } else {
                    uint32_t *cnt = site_count(&sites, pc);
                    if (cfg->fork_site_cap && *cnt >= cfg->fork_site_cap) { out->sat_site++; free(pre); }
                    else if (cfg->queue_max && qn >= cfg->queue_max) { out->sat_queue++; free(pre); }
                    else {
                        if (qn == qcap) { qcap = qcap ? qcap * 2 : 64; q = realloc(q, qcap * sizeof *q); }
                        q[qn++] = (Pending){ pre, other, 1 };
                        (*cnt)++;
                        out->forks++;
                    }
                }
            }
        }
        have = 0;
        if (qn) {
            size_t pick = qn - 1;
            if (!cfg->no_novelty_order) {
                for (size_t i = qn; i-- > 0;)
                    if (!covered(out, m, q[i].pc)) { pick = i; break; }
                if (pick != qn - 1) out->novelty_picks++;
            }
            Pending p = q[pick];
            q[pick] = q[qn - 1];
            qn--;
            cur_state = p.state; cur_pc = p.pc; cur_forced = p.forced; have = 1;
        }
    }
    for (size_t i = 0; i < qn; i++) free(q[i].state);
    free(fed);
    free(q); free(sites.s); free(visits); free(stamp); free(cur_state); free(start);
    return ok;
}
