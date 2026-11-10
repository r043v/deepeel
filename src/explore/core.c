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

void dp_core_result_free(DpCoreResult *r) {
    free(r->flags);
    memset(r, 0, sizeof *r);
}

static void mark(DpCoreResult *r, DpMachine *m, uint32_t pc, unsigned len, uint8_t how) {
    for (unsigned i = 0; i < len; i++) {
        int64_t k = m->rom_index(m, pc + i);
        if (k >= 0 && (size_t)k < r->rom_size) r->flags[k] |= how;
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

int dp_core_explore(DpMachine *m, size_t rom_size, const DpCoreConfig *cfg, DpCoreResult *out) {
    memset(out, 0, sizeof *out);
    out->rom_size = rom_size;
    if (!m || !m->isa || !m->run || !m->save || !m->load || !m->fetch || !m->rom_index) return 0;
    if (!m->pc || !m->set_pc) { out->no_pc = 1; return 1; }
    out->flags = calloc(rom_size ? rom_size : 1, 1);
    if (!out->flags) return 0;
    const DpIsa *isa = m->isa;
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
            int len = isa->length(bytes[0]);
            DpFlowInfo fi;
            memset(&fi, 0, sizeof fi);
            int have_flow = isa->flow && isa->flow(bytes, (uint16_t)pc, &fi);
            if (have_flow && fi.length) len = fi.length;
            if (len <= 0) len = 1;
            mark(out, m, pc, (unsigned)len, cur_forced ? DP_CORE_FORCED : DP_CORE_NATURAL);
            int cond = have_flow && fi.kind == DP_FLOW_COND && fi.target >= 0;
            uint8_t *pre = NULL;
            if (cond) {
                pre = malloc(sbytes);
                m->save(m, pre);
            }
            if (!m->run(m)) { free(pre); break; }
            steps++; out->steps++;
            if (cond) {
                uint32_t after = m->pc(m);
                uint32_t fall = pc + (uint32_t)len, tgt = (uint32_t)fi.target;
                uint32_t other = after == tgt ? fall : tgt;
                if (after != tgt && after != fall) { free(pre); continue; }   // an IRQ intervened
                if (covered(out, m, other) && covered(out, m, after)) {
                    out->pruned_covered++; free(pre);
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
            Pending p = q[--qn];
            cur_state = p.state; cur_pc = p.pc; cur_forced = p.forced; have = 1;
        }
    }
    for (size_t i = 0; i < qn; i++) free(q[i].state);
    free(q); free(sites.s); free(visits); free(stamp); free(cur_state); free(start);
    return ok;
}
