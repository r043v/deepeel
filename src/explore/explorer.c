// explorer.c: the forking worklist engine on the m0boy machine.
//
// Every conditional branch forks (both outcomes are always tested), every
// pending enabled IRQ forks a no-IRQ continuation (the machine itself
// dispatches the IRQ naturally).  Dedup evicts exact state images and
// "same context" section re-entries; budgets and backward edge caps keep
// loops finite; the halt watchdog cuts paths nothing wakes.
//
// The same recording runs inside the play-session workers (one DpExplorer
// each, sharing DpResults): their per-byte updates go through __atomic
// builtins, and their section/record updates take the results mutexes while
// res->mt is set.  The fork explorer only runs while the sessions wait at
// the epoch barrier, so its own updates stay lock-free.
#include <zlib.h>
#include <sys/stat.h>
#include "explorer.h"
#include "../cpu/sm83/sm83.h"
#include "tree.h"
#include <time.h>
#include <unistd.h>

#include <stddef.h>

static int is_padding_at(const DpCart *c, uint32_t bank, uint16_t addr);
static int is_illegal_op(uint8_t op);
static void fork_branch(DpExplorer *ex, int cond, uint32_t site, uint16_t pc, uint8_t op);
static void fork_irq(DpExplorer *ex);
static void fork_key(DpExplorer *ex, uint32_t site);
static void fork_p1(DpExplorer *ex, uint32_t site, uint16_t pc, uint8_t op);

// --- shared-map primitives -----------------------------------------------------
static inline void src_or(DpResults *res, uint32_t k, uint32_t bits) {
    dp_cov_mark(&res->first, res->src_flags, k, bits, res->steps_total, 0);
}
static inline uint8_t flags_or(uint8_t *p, uint8_t bit) {
    return __atomic_or_fetch(p, bit, __ATOMIC_RELAXED); // returns the old bits
}
// per-byte hit counter, saturating at UINT32_MAX (a hot loop never wraps to
// "cold"); each held increment is counted
static inline void hits_add(DpResults *res, uint32_t *p) {
    if (__atomic_fetch_add(p, 1, __ATOMIC_RELAXED) == UINT32_MAX) {
        __atomic_store_n(p, UINT32_MAX, __ATOMIC_RELAXED);
        __atomic_add_fetch(&res->hits_saturated, 1, __ATOMIC_RELAXED);
    }
}
static inline void u32_add(uint32_t *p, uint32_t v) { __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
static inline void u64_add(uint64_t *p, uint64_t v) { __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
static inline void u64_or(uint64_t *p, uint64_t v) { __atomic_fetch_or(p, v, __ATOMIC_RELAXED); }

// --- u32 -> payload map ---------------------------------------------------------
static void *map_find(DpMap32 *m, uint32_t key) {
    size_t i = (size_t)(key * 0x9E3779B97F4A7C15ull) & (m->cap - 1);
    while (m->used[i]) {
        if (m->keys[i] == key) return m->vals + i * m->stride;
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}

static int map_grow(DpMap32 *m) {
    if (m->cap >= m->cap_abs) return 0;
    size_t ncap = m->cap * 2;
    uint32_t *nk = calloc(ncap, sizeof(uint32_t));
    uint8_t *nu = calloc(ncap, 1);
    uint8_t *nv = calloc(ncap, m->stride);
    if (!nk || !nu || !nv) { free(nk); free(nu); free(nv); return 0; }
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->used[i]) continue;
        size_t j = (size_t)(m->keys[i] * 0x9E3779B97F4A7C15ull) & (ncap - 1);
        while (nu[j]) j = (j + 1) & (ncap - 1);
        nk[j] = m->keys[i];
        nu[j] = 1;
        memcpy(nv + j * m->stride, m->vals + i * m->stride, m->stride);
    }
    free(m->keys);
    free(m->used);
    free(m->vals);
    m->keys = nk;
    m->used = nu;
    m->vals = nv;
    m->cap = ncap;
    return 1;
}

void dp_map_init(DpMap32 *m, size_t stride, size_t cap_abs) {
    m->stride = stride;
    m->cap_abs = cap_abs ? cap_abs : 1 << 20;
    m->cap = 1024;
    m->keys = calloc(m->cap, sizeof(uint32_t));
    m->used = calloc(m->cap, 1);
    m->vals = calloc(m->cap, stride);
    m->cnt = 0;
}

void dp_map_free(DpMap32 *m) {
    free(m->keys);
    free(m->used);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

void *dp_map_get_or_add(DpMap32 *m, uint32_t key) {
    void *p = map_find(m, key);
    if (p) return p;
    if (m->cnt >= m->cap * 3 / 4) {
        if (!map_grow(m)) return NULL;
    }
    size_t i = (size_t)(key * 0x9E3779B97F4A7C15ull) & (m->cap - 1);
    while (m->used[i]) i = (i + 1) & (m->cap - 1);
    m->keys[i] = key;
    m->used[i] = 1;
    m->cnt++;
    return m->vals + i * m->stride;
}

void *dp_map_find(DpMap32 *m, uint32_t key) {
    return map_find(m, key);
}

// --- config / results ------------------------------------------------------------
void dp_config_defaults(DpConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->max_steps_path = 100000;
    cfg->max_paths = 200000;
    cfg->max_steps_total = 20000000ull;
    cfg->queue_cap = 4096;
    cfg->loop_back_cap = 4096;
    cfg->fork_site_cap = 16;
    cfg->seed_cap = 4096;
    cfg->max_copies = 65536;
    cfg->closure_rounds = 12; // each round exits early once it seeds nothing
    cfg->covered_fork_keep = 2;
    cfg->spool_cap_mb = 4096;
}

uint64_t dp_rss_mb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long rss_pages = 0;
    if (fscanf(f, "%*lu %lu", &rss_pages) != 1) rss_pages = 0;
    fclose(f);
    return rss_pages * (unsigned long)sysconf(_SC_PAGESIZE) / (1024u * 1024u);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n) abort();
    return q;
}

void dp_results_init(DpResults *res, const DpCart *cart, const DpPlatform *plat) {
    memset(res, 0, sizeof *res);
    res->cart = cart;
    res->plat = plat;
    res->banks = cart->rom_banks;
    pthread_mutex_init(&res->sec_mtx, NULL);
    pthread_mutex_init(&res->rec_mtx, NULL);
    size_t n = (size_t)cart->rom_banks * 0x4000;
    res->exec_flags = xrealloc(NULL, n);
    res->exec_hits = xrealloc(NULL, n * sizeof(uint32_t));
    res->data_flags = xrealloc(NULL, n);
    res->data_hits = xrealloc(NULL, n * sizeof(uint32_t));
    res->play_exec_hits = xrealloc(NULL, n * sizeof(uint32_t));
    res->play_data_hits = xrealloc(NULL, n * sizeof(uint32_t));
    res->static_flags = xrealloc(NULL, n);
    res->section_of = xrealloc(NULL, n * sizeof(uint32_t));
    res->src_flags = xrealloc(NULL, n * sizeof(uint32_t));
    memset(res->src_flags, 0, n * sizeof(uint32_t));
    dp_first_init(&res->first, 1u << 20);
    memset(res->play_exec_hits, 0, n * sizeof(uint32_t));
    memset(res->play_data_hits, 0, n * sizeof(uint32_t));
    memset(res->exec_flags, 0, n);
    memset(res->exec_hits, 0, n * sizeof(uint32_t));
    memset(res->data_flags, 0, n);
    memset(res->data_hits, 0, n * sizeof(uint32_t));
    memset(res->static_flags, 0, n);
    memset(res->section_of, 0, n * sizeof(uint32_t));
    res->io_count = plat->io_reg_count;
    res->io = xrealloc(NULL, sizeof(DpIoStat) * (size_t)res->io_count);
    memset(res->io, 0, sizeof(DpIoStat) * (size_t)res->io_count);
    dp_map_init(&res->sites, sizeof(DpSiteStat), 1 << 22);
    dp_map_init(&res->callers, sizeof(DpCallList), 1 << 20);
    dp_map_init(&res->fork_counts, sizeof(uint32_t), 1 << 20);
    dp_map_init(&res->indirect, sizeof(DpIndirect), 1 << 16);
    dp_set_init(&res->bad_seen, 1 << 20);
}

static double disc_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void dp_disc_add(DpResults *res, const char *source, uint64_t credit) {
    if (res->disc_n == res->disc_cap) {
        int cap = res->disc_cap ? res->disc_cap * 2 : 256;
        DpDiscEntry *n = realloc(res->disc, (size_t)cap * sizeof *n);
        if (!n) return;
        res->disc = n;
        res->disc_cap = cap;
    }
    double now = disc_now();
    if (!res->disc_n) res->disc_t0 = now;
    size_t total = (size_t)res->banks * 0x4000;
    uint32_t known = 0;
    for (size_t i = 0; i < total; i++)
        if (res->exec_flags[i] || res->data_flags[i] || res->static_flags[i]) known++;
    DpDiscEntry *e = &res->disc[res->disc_n++];
    e->t = now - res->disc_t0;
    snprintf(e->source, sizeof e->source, "%s", source);
    e->credit = credit;
    e->known = known;
}

void dp_results_free(DpResults *res) {
    free(res->disc);
    free(res->exec_flags);
    free(res->exec_hits);
    free(res->data_flags);
    free(res->data_hits);
    free(res->play_exec_hits);
    free(res->play_data_hits);
    free(res->static_flags);
    free(res->section_of);
    free(res->src_flags);
    dp_first_free(&res->first);
    for (int i = 0; i < res->section_count; i++) dp_set_free(&res->sections[i].ctx_seen);
    free(res->sections);
    free(res->io);
    free(res->copies);
    free(res->bank_events);
    dp_map_free(&res->sites);
    dp_map_free(&res->callers);
    dp_map_free(&res->fork_counts);
    dp_map_free(&res->indirect);
    dp_set_free(&res->bad_seen);
    free(res->clo_entries);
    free(res->clo_bank_entries);
    free(res->bad_ops);
    pthread_mutex_destroy(&res->sec_mtx);
    pthread_mutex_destroy(&res->rec_mtx);
    memset(res, 0, sizeof *res);
}

int dp_io_slot(const DpResults *res, uint16_t addr) {
    for (int i = 0; i < res->io_count; i++) {
        const DpIoReg *r = &res->plat->io_regs[i];
        if (addr >= r->lo && addr <= r->hi) return i;
    }
    return -1;
}

// static classification: padding runs and pointer tables
void dp_static_scan(DpResults *res) {
    const DpCart *c = res->cart;
    size_t n = (size_t)c->rom_banks * 0x4000;
    // padding: runs of 8+ identical 00 or FF bytes, the whole run marked
    // (an unmarked head of 7 would leave a systematic unknown fringe)
    uint32_t run = 0;
    size_t rstart = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = c->rom[i < c->rom_size ? i : c->rom_size - 1];
        if (b == 0x00 || b == 0xFF) {
            if (!run) rstart = i;
            run++;
            if (run == 8)
                for (size_t j = rstart; j <= i; j++) { res->static_flags[j] |= DP_SF_PAD; dp_cov_mark(&res->first, res->src_flags, (uint32_t)j, DP_SRC_STATIC_SCAN, 0, 0); }
            else if (run > 8) { res->static_flags[i] |= DP_SF_PAD; dp_cov_mark(&res->first, res->src_flags, (uint32_t)i, DP_SRC_STATIC_SCAN, 0, 0); }
        } else {
            run = 0;
        }
    }
    // pointer tables: 4+ consecutive words pointing at plausible ROM targets
    // (a zeroed or padding ROM is not a table of null pointers)
    for (int bank = 0; bank < res->banks; bank++) {
        for (int off = 0; off + 7 < 0x4000; off += 2) {
            uint32_t k = dp_rom_index(bank, (uint16_t)off);
            if (res->static_flags[k] & DP_SF_POINTER) continue;
            int ok = 1;
            uint16_t first = 0;
            for (int w = 0; w < 4 && ok; w++) {
                uint32_t a = k + (uint32_t)w * 2;
                uint16_t v;
                if (a + 1 >= n) { ok = 0; break; }
                v = (uint16_t)(c->rom[a] | (c->rom[a + 1] << 8));
                if (v < 0x0100 || v >= 0x8000) ok = 0;
                else if (w == 0) first = v;
                else if (v != first && is_padding_at(c, v < 0x4000 ? 0 : 1, v)) ok = 0;
            }
            if (!ok) continue;
            for (int w = 0; w < 4; w++) {
                uint32_t a = k + (uint32_t)w * 2;
                if (a + 1 < n) {
                    res->static_flags[a] |= DP_SF_POINTER;
                    res->static_flags[a + 1] |= DP_SF_POINTER;
                    dp_cov_mark(&res->first, res->src_flags, (uint32_t)a, DP_SRC_STATIC_SCAN, 0, 0);
                    dp_cov_mark(&res->first, res->src_flags, (uint32_t)a + 1, DP_SRC_STATIC_SCAN, 0, 0);
                }
            }
            off += 6; // skip past the table we just marked
        }
    }
}

// --- explorer ---------------------------------------------------------------------

// --- engine hooks (wired in dp_explorer_new) ----------------------------------------
static void hook_step(DpMach *m, uint16_t pc, uint8_t op, int entry);
static void hook_read(DpMach *m, uint16_t a, uint8_t v);
static void hook_write(DpMach *m, uint16_t a, uint8_t v);

DpExplorer *dp_explorer_new(const DpConfig *cfg, DpResults *res, DpMach *mach) {
    DpExplorer *ex = calloc(1, sizeof *ex);
    ex->cfg = *cfg;
    ex->res = res;
    ex->mach = mach;
    ex->sb = dp_mach_state_bytes(mach);
    ex->q_cap = cfg->queue_cap ? cfg->queue_cap : 2048;
    ex->q_max = cfg->queue_max ? cfg->queue_max : ex->q_cap * 4;
    if (ex->q_max < ex->q_cap) ex->q_max = ex->q_cap;
    ex->queue = xrealloc(NULL, sizeof(DpPathState) * ex->q_cap);
    memset(ex->queue, 0, sizeof(DpPathState) * ex->q_cap);
    // the priority inbox stays small: seeds are few, saturation is counted
    ex->pq_cap = cfg->seed_cap ? (cfg->seed_cap < 1024 ? cfg->seed_cap : 1024) : 512;
    ex->pqueue = xrealloc(NULL, sizeof(DpPathState) * ex->pq_cap);
    memset(ex->pqueue, 0, sizeof(DpPathState) * ex->pq_cap);
    size_t dedup_cap = (size_t)ex->q_cap * 64;
    if (dedup_cap < (1 << 16)) dedup_cap = 1 << 16;
    dp_set_init(&ex->exact, dedup_cap);
    pthread_mutex_init(&ex->tmtx, NULL);
    ex->exactp = &ex->exact;
    ex->exact_mtx = &ex->tmtx; // any mutex works single-threaded; share() retargets
    ex->tree_mtx = &ex->tmtx;
    dp_mach_attach(mach, ex, hook_step, hook_read, hook_write, NULL);
    return ex;
}

void dp_explorer_free(DpExplorer *ex) {
    if (!ex) return;
    for (uint32_t i = 0; i < ex->q_cap; i++) free(ex->queue[i].img);
    free(ex->queue);
    ex->res->defer_pending += ex->defer_len; // left at a wall: counted, never silent
    ex->res->defer_on_disk = dp_explorer_frontier_disk(ex);
    for (uint32_t i = 0; i < ex->defer_len; i++) free(ex->defer[i].p.img);
    free(ex->defer);
    free(ex->pseed);
    free(ex->lots);
    free(ex->fdir);
    for (uint32_t i = 0; i < ex->pq_cap; i++) free(ex->pqueue[i].img);
    free(ex->pqueue);
    free(ex->seed_base);
    free(ex->seed_bank_img);
    dp_set_free(&ex->exact);
    pthread_mutex_destroy(&ex->tmtx);
    if (ex->spool && ex->spool_owned) {
        fclose(ex->spool->f);
        pthread_mutex_destroy(&ex->spool->mtx);
        free(ex->spool->cursor_path);
        free(ex->spool);
    }
    free(ex);
}

void dp_explorer_progress(DpExplorer *ex, DpProgress *out) {
    out->queue_len = ex->q_len;
    out->steps_total = ex->res->steps_total;
    out->paths_run = ex->res->paths_run;
    out->sections = (uint32_t)ex->res->section_count;
}

// --- bank/key helpers -----------------------------------------------------------------
// The runtime masks banks into 512; the result maps are cart-sized: fold a
// wild bank back into the cart (a bank register value beyond the cart reads
// as a mirror on real hardware too).
static uint16_t cur_bank(DpExplorer *ex, uint16_t pc) {
    uint32_t b = pc < 0x4000 ? 0 : ex->mach->api->gbr->rom_bank;
    uint32_t banks = (uint32_t)ex->res->banks;
    if (banks && b >= banks) b %= banks;
    return (uint16_t)b;
}

static uint32_t key_at(DpExplorer *ex, uint16_t pc) {
    return dp_rom_index(cur_bank(ex, pc), pc);
}

// --- queue -----------------------------------------------------------------------
// pc of a state image (gbl_cpu_t rides at offset 0 of the gbl state stream)
static uint16_t image_pc(const uint8_t *img) {
    gbl_cpu_t g;
    memcpy(&g, img, sizeof g);
    return g.pc;
}

static uint64_t image_hash(DpExplorer *ex, const uint8_t *img) {
    // gbr.cyc/next_evt ride at the head of the image and only encode where
    // time stood: skip them so equal machine states dedup across frames
    uint64_t h = 0xcbf29ce484222325ull;
    h = dp_fnv1a(h, img, offsetof(gbl_cpu_t, cyc));
    h = dp_fnv1a(h, img + sizeof(gbl_cpu_t), ex->sb - sizeof(gbl_cpu_t));
    return h;
}

// bounded adaptive growth of the main worklist: a full queue used to drop
// whole paths; doubling up to q_max (and under the memory cap when set)
// keeps them.  Returns 0 when the growth is impossible (ceiling, memory cap).
static int queue_grow(DpExplorer *ex) {
    if (ex->q_cap >= ex->q_max) return 0;
    if (ex->cfg.mem_cap_mb && dp_rss_mb() >= ex->cfg.mem_cap_mb) return 0;
    uint32_t ncap = ex->q_cap * 2;
    if (ncap > ex->q_max) ncap = ex->q_max;
    DpPathState *nq = xrealloc(NULL, sizeof(DpPathState) * (size_t)ncap);
    // logical order preserved, head rebased to 0
    for (uint32_t i = 0; i < ex->q_len; i++)
        nq[i] = ex->queue[(ex->q_head + i) % ex->q_cap];
    memset(nq + ex->q_len, 0, sizeof(DpPathState) * (size_t)(ncap - ex->q_len));
    free(ex->queue);
    ex->queue = nq;
    ex->q_head = 0;
    ex->q_cap = ncap;
    ex->res->queue_growths++;
    return 1;
}

// returns 0 when the state was dropped (dedup, dedup-set full or queue full):
// the caller can then retry it later instead of losing it
static int spool_write_rec(DpExplorer *ex, const uint8_t *img, int forced, int kind,
                           int from_reset, uint32_t choice);
static void tree_close_g(DpExplorer *ex, uint32_t choice, int issue);
static uint32_t tree_add_g(DpExplorer *ex, uint32_t parent, int kind, uint32_t site, int bank,
                           int pc);
static int defer_push(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                      uint32_t choice, int reason);
static int queue_push_pri(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                          uint32_t choice, int pri) {
    uint64_t h = image_hash(ex, img);
    int ins;
    if (ex->sh) { // the exact set is shared by the parallel workers
        pthread_mutex_lock(ex->exact_mtx);
        ins = dp_set_insert(ex->exactp, h);
        pthread_mutex_unlock(ex->exact_mtx);
    } else {
        ins = dp_set_insert(ex->exactp, h);
    }
    if (ins == 0) {
        u32_add(&ex->res->evicted_exact, 1);
        tree_close_g(ex, choice, DP_ISSUE_MERGED);
        free(img);
        return 0;
    }
    if (ins < 0) { // dedup set full: surfaced; the state waits in the frontier (not for the shared pool)
        u32_add(&ex->res->queue_overflows, 1);
        if (!ex->sh && defer_push(ex, img, forced, kind, from_reset, choice, DP_R_DEDUP_FULL)) {
            tree_close_g(ex, choice, DP_ISSUE_SPOOLED);
            return 0;
        }
        tree_close_g(ex, choice, DP_ISSUE_BUDGET);
        free(img);
        return 0;
    }
    if (ex->sh && !pri) { // the parallel drain: everything through the pool
        DpSharedPool *sh = ex->sh;
        pthread_mutex_lock(&sh->mtx);
        if (sh->len >= sh->cap) {
            uint32_t ncap = sh->cap * 2;
            if (ncap > sh->max) ncap = sh->max;
            if (sh->cap < sh->max &&
                !(ex->cfg.mem_cap_mb && dp_rss_mb() >= ex->cfg.mem_cap_mb)) {
                sh->q = xrealloc(sh->q, sizeof(DpPathState) * (size_t)ncap);
                sh->cap = ncap;
                u32_add(&ex->res->queue_growths, 1);
            } else if (spool_write_rec(ex, img, forced, kind, from_reset, choice)) {
                pthread_mutex_unlock(&sh->mtx);
                tree_close_g(ex, choice, DP_ISSUE_SPOOLED);
                free(img);
                return 0;
            } else {
                u32_add(&ex->res->queue_overflows, 1);
                pthread_mutex_unlock(&sh->mtx);
                // the pool and the spool are both at their wall: the worker keeps the
                // state in its own holding area (merged into the leader's at the join)
                if (defer_push(ex, img, forced, kind, from_reset, choice, DP_R_PATHS)) {
                    tree_close_g(ex, choice, DP_ISSUE_SPOOLED);
                    return 0;
                }
                tree_close_g(ex, choice, DP_ISSUE_BUDGET);
                free(img);
                return 0;
            }
        }
        sh->q[(sh->head + sh->len) % sh->cap] =
            (DpPathState){ img, (uint8_t)(forced ? 1 : 0), (uint8_t)kind,
                           (uint8_t)(from_reset ? 1 : 0), choice };
        sh->len++;
        pthread_mutex_unlock(&sh->mtx);
        return 1;
    }
    DpPathState *q = pri ? ex->pqueue : ex->queue;
    uint32_t cap = pri ? ex->pq_cap : ex->q_cap;
    uint32_t len = pri ? ex->pq_len : ex->q_len;
    uint32_t head = pri ? ex->pq_head : ex->q_head;
    if (len >= cap) {
        if (!pri && queue_grow(ex)) { // bounded adaptive growth, counted
            q = ex->queue;
            cap = ex->q_cap;
            head = ex->q_head;
        } else if (!pri && ex->spool &&
                   spool_write_rec(ex, img, forced, kind, from_reset, choice)) {
            // the RAM worklist is at its wall: the state waits on disk instead
            // of dying — never a silent loss
            if (ex->tree) dp_tree_close(ex->tree, choice, DP_ISSUE_SPOOLED);
            free(img);
            return 0;
        } else {
            if (pri) ex->res->closure_seeds_refused++; // saturation surfaced
            else ex->res->queue_overflows++;
            // no room and no spool: the state waits in the frontier instead of dying
            if (defer_push(ex, img, forced, kind, from_reset, choice, pri ? DP_R_PRIORITY : DP_R_PATHS)) {
                if (ex->tree) dp_tree_close(ex->tree, choice, DP_ISSUE_SPOOLED);
                return 0;
            }
            if (ex->tree) dp_tree_close(ex->tree, choice, DP_ISSUE_BUDGET);
            free(img);
            return 0;
        }
    }
    DpPathState *p = &q[(head + len) % cap];
    p->img = img;
    p->forced = (uint8_t)(forced ? 1 : 0);
    p->kind = (uint8_t)kind;
    p->from_reset = (uint8_t)(from_reset ? 1 : 0);
    p->choice = choice;
    if (pri) ex->pq_len++;
    else ex->q_len++;
    return 1;
}

static int queue_push(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                      uint32_t choice) {
    return queue_push_pri(ex, img, forced, kind, from_reset, choice, 0);
}

// a tree choice for a new queued path (when the tree is tracked)
static uint32_t choice_new(DpExplorer *ex, uint32_t parent, int kind, uint32_t site, int bank,
                           int pc) {
    return tree_add_g(ex, parent, kind, site, bank, pc);
}

// tree bookkeeping under the shared lock when the drain is parallel
static void tree_close_g(DpExplorer *ex, uint32_t choice, int issue) {
    if (!ex->tree) return;
    if (ex->sh) pthread_mutex_lock(ex->tree_mtx);
    dp_tree_close(ex->tree, choice, issue);
    if (ex->sh) pthread_mutex_unlock(ex->tree_mtx);
}

static uint32_t tree_add_g(DpExplorer *ex, uint32_t parent, int kind, uint32_t site, int bank,
                           int pc) {
    if (!ex->tree) return 0;
    if (ex->sh) pthread_mutex_lock(ex->tree_mtx);
    uint32_t c = dp_tree_add(ex->tree, parent, kind, site, bank, pc);
    if (ex->sh) pthread_mutex_unlock(ex->tree_mtx);
    return c;
}

int dp_explorer_seed(DpExplorer *ex, uint8_t *img, int kind, int forced) {
    return dp_explorer_seed_pri(ex, img, kind, forced, 0);
}

// priority seeds (closure entries, indirect targets, TAS snapshots handed in
// after the play) are served before the main worklist; ordinary seeds join it
int dp_explorer_seed_pri(DpExplorer *ex, uint8_t *img, int kind, int forced, int pri) {
    uint16_t pc = image_pc(img);
    uint16_t bank = (uint16_t)(pc < 0x4000 ? 0 : ((const gbl_cpu_t *)img)->rom_bank);
    if (!queue_push_pri(ex, img, forced, kind, kind == DP_SEED_RESET,
                        choice_new(ex, 0, kind, dp_rom_index(bank, pc), bank, pc), pri))
        return 0; // dropped: the seed counters only count seeds that will run
    if (forced) ex->res->seeds_spec++;
    else ex->res->seeds_natural++;
    return 1;
}

// reset state with the bank window applied and pc moved: the shared seed
// images every entry point starts from
static uint8_t *seed_base(DpExplorer *ex) {
    if (ex->seed_base) return ex->seed_base;
    uint8_t *img = xrealloc(NULL, ex->sb);
    dp_mach_reset(ex->mach);
    dp_mach_save(ex->mach, img);
    ex->seed_base = img;
    return img;
}

uint8_t *dp_explorer_seed_base(DpExplorer *ex) { return seed_base(ex); }

static uint8_t *seed_bank_img(DpExplorer *ex, int bank) {
    if (ex->seed_bank_img && ex->seed_bank == bank) return ex->seed_bank_img;
    if (!ex->seed_bank_img) ex->seed_bank_img = xrealloc(NULL, ex->sb);
    dp_mach_load(ex->mach, seed_base(ex));
    const DpGblApi *api = ex->mach->api;
    api->wr(0x2000, (uint8_t)(bank & 0xFF)); // mapper registers, unobserved
    if (bank > 0xFF) api->wr(0x3000, (uint8_t)(bank >> 8));
    dp_mach_save(ex->mach, ex->seed_bank_img);
    ex->seed_bank = bank;
    return ex->seed_bank_img;
}

int dp_explorer_seed_pc(DpExplorer *ex, int bank, uint16_t pc, int kind, int forced) {
    return dp_explorer_seed_pc_pri(ex, bank, pc, kind, forced, 0);
}

int dp_explorer_seed_pc_pri(DpExplorer *ex, int bank, uint16_t pc, int kind, int forced, int pri) {
    uint8_t *img = xrealloc(NULL, ex->sb);
    memcpy(img, seed_bank_img(ex, bank), ex->sb);
    gbl_cpu_t g = *ex->mach->api->gbr; // the bank write leaves the regs alone
    g.pc = pc;
    memcpy(img, &g, sizeof g);
    return dp_explorer_seed_pri(ex, img, kind, forced, pri);
}

// --- read/write hooks ---------------------------------------------------------------
static void section_live_access(DpExplorer *ex, uint16_t a) {
    if (ex->session) return;
    DpSection *s = ex->cur_sec ? &ex->res->sections[ex->cur_sec] : NULL;
    if (!s) return;
    if (a >= 0x4000 && a < 0x8000) dp_section_live_small(s, 3); // upper window bank matters
    else if (a >= 0x8000 && a < 0xA000) {
        dp_section_live_vram(s, ex->mach->api->io[0x4F] & 1, (a - 0x8000) >> 8);
        dp_section_live_small(s, 5); // vbk matters
    } else if (a >= 0xA000 && a < 0xC000) {
        dp_section_live_sram(s, (a - 0xA000) >> 8); // bank folds into the ctx hash
        dp_section_live_small(s, 4); // ramg matters
    } else if (a >= 0xC000 && a < 0xFE00) {
        dp_section_live_wram(s, ((a - 0xC000) & 0x1FFF) >> 8);
    } else if (a >= 0xFF80 && a < 0xFFFF) dp_section_live_small(s, 0);
    else if (a >= 0xFE00 && a < 0xFEA0) dp_section_live_small(s, 1);
    else if (a == 0xFFFF) dp_section_live_small(s, 2);
}

static void hook_read(DpMach *m, uint16_t a, uint8_t v) {
    DpExplorer *ex = m->owner;
    DpResults *res = ex->res;
    if (a < 0x8000) { // ROM data read (opcode fetches never take this path)
        uint32_t key = key_at(ex, a);
        uint8_t old = flags_or(&res->data_flags[key], DP_DF_READ);
        src_or(res, key, ex->session ? DP_SRC_SESSION : (ex->cur_forced ? DP_SRC_FORK_FORCED : DP_SRC_FORK_NATURAL));
        if (!(old & DP_DF_READ)) ex->credit_data++;
        hits_add(res, &res->data_hits[key]);
        if (ex->session) hits_add(res, &res->play_data_hits[key]);
        dp_copy_on_rom_read(ex, (uint8_t)cur_bank(ex, a), a, v);
    }
    section_live_access(ex, a);
    if (a >= 0xFF00 && a < 0xFF80) {
        int slot = dp_io_slot(res, a);
        if (slot >= 0) {
            DpIoStat *st = &res->io[slot];
            uint32_t was = __atomic_fetch_add(&st->rd, 1, __ATOMIC_RELAXED);
            if (!was && !__atomic_load_n(&st->wr, __ATOMIC_RELAXED)) ex->credit_io++;
            if (!ex->session) {
                DpSection *s = ex->cur_sec ? &res->sections[ex->cur_sec] : NULL;
                if (s) {
                    dp_section_live_io(s, (uint16_t)(a - 0xFF00));
                    dp_section_io_used(s, slot);
                }
            }
        }
    }
}

static void hook_write(DpMach *m, uint16_t a, uint8_t v) {
    DpExplorer *ex = m->owner;
    DpResults *res = ex->res;
    if (a < 0x8000) { // mapper write: the bank flip resolves on the next step
        ex->pend_map = 1;
        ex->pend_pc = m->api->gbr->pc;
        ex->pend_addr = a;
        ex->pend_window = a < 0x2000 ? 2 : (a < 0x4000 ? 1 : (a < 0x6000 ? 2 : 1));
        ex->pend_old = m->api->gbr->rom_bank;
        return;
    }
    if (a < 0xA000) { // VRAM
        int vbk = m->api->io[0x4F] & 1;
        u64_or(&res->vram_write_pages[vbk], 1ull << ((a - 0x8000) >> 8));
        dp_copy_on_vram_write(ex, (uint8_t)vbk, a, v);
        return;
    }
    if (a < 0xC000) { // cartridge RAM
        u64_or(&res->sram_write_pages,
               1ull << (((a - 0xA000) & (DP_CRAM_PAGES * 256 - 1)) >> 8));
        return;
    }
    if (a < 0xFE00) return; // WRAM and its echo: no per-byte map
    if (a < 0xFF00) { // OAM
        __atomic_store_n(&res->oam_written, 1, __ATOMIC_RELAXED);
        return;
    }
    if (a < 0xFF80) { // IO
        int slot = dp_io_slot(res, a);
        if (slot >= 0) {
            ex->last_io_slot = slot;
            DpIoStat *st = &res->io[slot];
            uint32_t was = __atomic_fetch_add(&st->wr, 1, __ATOMIC_RELAXED);
            if (!was && !__atomic_load_n(&st->rd, __ATOMIC_RELAXED)) ex->credit_io++;
            if (res->mt) pthread_mutex_lock(&res->rec_mtx);
            int have = 0;
            for (int i = 0; i < st->nvalues; i++)
                if (st->values[i] == v) have = 1;
            if (!have && st->nvalues < 8) st->values[st->nvalues++] = v;
            uint16_t pc = m->api->gbr->pc;
            if (st->npcs < 4) st->pcs[st->npcs++] = pc;
            else {
                memmove(st->pcs, st->pcs + 1, 3 * sizeof(uint16_t));
                st->pcs[3] = pc;
            }
            if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
        }
        if (a == 0xFF46 && v * 0x100u < 0x8000u) {
            // OAM DMA: 160 bytes land verbatim in the OAM
            DpCopyRec rec;
            memset(&rec, 0, sizeof rec);
            rec.channel = 1;
            rec.verbatim = 1;
            rec.src_space = DP_SPACE_ROM;
            rec.dst_space = DP_SPACE_OAM;
            rec.src_bank = v >= 0x40 ? cur_bank(ex, 0x4000) : 0;
            rec.src_lo = (uint16_t)((v * 0x100u) & 0x3FFF);
            rec.src_hi = (uint16_t)(rec.src_lo + 159);
            rec.dst_lo = 0xFE00;
            rec.dst_hi = 0xFE9F;
            rec.bytes = 160;
            rec.pc = m->api->gbr->pc;
            dp_record_copy(ex, &rec, 0);
        }
    }
}

// --- the per-instruction hook ---------------------------------------------------------
static void emit_pend_map(DpExplorer *ex) {
    if (!ex->pend_map) return;
    ex->pend_map = 0;
    DpResults *res = ex->res;
    uint32_t new_bank = ex->mach->api->gbr->rom_bank;
    if (res->mt) pthread_mutex_lock(&res->rec_mtx);
    if (res->bank_count == res->bank_cap) {
        res->bank_cap = res->bank_cap ? res->bank_cap * 2 : 64;
        res->bank_events = xrealloc(res->bank_events, sizeof(DpBankEvent) * (size_t)res->bank_cap);
    }
    DpBankEvent *e = &res->bank_events[res->bank_count++];
    e->seq = (uint32_t)res->bank_count;
    e->pc = ex->pend_pc;
    e->window = ex->pend_window;
    e->old_bank = ex->pend_old;
    e->new_bank = new_bank;
    if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
    int slot = dp_io_slot(res, ex->pend_addr);
    if (slot >= 0) {
        u32_add(&res->io[slot].wr, 1);
        u32_add(&res->io[slot].bank_events, 1);
    }
    ex->credit_banks++;
    if (!ex->session) {
        DpSection *s = ex->cur_sec ? &res->sections[ex->cur_sec] : NULL;
        if (s) {
            dp_section_banks_seen(s, new_bank);
            dp_section_io_used(s, slot);
        }
    }
}

const char *dp_reason_name(int r) {
    static const char *n[DP_R_COUNT] = { "none", "priority", "dedup-full", "paths", "fork-cap",
                                         "steps", "loop", "wall", "covered" };
    return r >= 0 && r < DP_R_COUNT ? n[r] : "?";
}

// Every state a budget would have thrown away is kept here instead: a path
// cut by its step budget, a fork past the per-site cap, a full dedup set, ...
// Each deferral raises the tier by one (the resumed path gets a bigger budget);
// past DP_TIER_MAX, or when the holding area is full, the loss stays final
// and is counted per reason — never silent.  The noisy reasons (fork-cap,
// covered) hold at most a quarter of the area each so they cannot starve the rest.
static int defer_room(DpExplorer *ex, int reason) {
    DpResults *res = ex->res;
    if (ex->cur_tier >= DP_TIER_MAX) { res->defer_final[reason]++; return 0; }
    uint32_t lim = (reason == DP_R_FORK_CAP || reason == DP_R_COVERED) ? DP_DEFER_CAP / 4 : DP_DEFER_CAP;
    if (ex->defer_len >= DP_DEFER_CAP || ex->defer_by_reason[reason] >= lim) {
        res->defer_final[reason]++;
        res->defer_dropped++;
        return 0;
    }
    return 1;
}

// takes ownership of img on success; 0 = counted final loss (img untouched)
static int defer_push(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                      uint32_t choice, int reason) {
    if (!defer_room(ex, reason)) return 0;
    if (ex->defer_len >= ex->defer_cap) {
        uint32_t nc = ex->defer_cap ? ex->defer_cap * 2 : 64;
        ex->defer = xrealloc(ex->defer, sizeof *ex->defer * nc);
        ex->defer_cap = nc;
    }
    ex->defer[ex->defer_len++] = (DpDeferred){
        { img, (uint8_t)forced, (uint8_t)kind, (uint8_t)from_reset, choice },
        (uint8_t)(ex->cur_tier + 1), (uint8_t)reason };
    ex->defer_by_reason[reason]++;
    ex->res->defer_by[reason]++;
    if (reason == DP_R_STEPS) ex->res->steps_deferred++;
    else if (reason == DP_R_LOOP) ex->res->loop_deferred++;
    return 1;
}

// the running path, cut by a budget: snapshot it here and resume it later
static void defer_cut_reason(DpExplorer *ex, DpMach *m, int reason) {
    DpResults *res = ex->res;
    if (!defer_room(ex, reason)) {
        if (reason == DP_R_LOOP) res->loop_final++;
        else if (reason == DP_R_STEPS) res->steps_final++;
        return;
    }
    uint8_t *img = xrealloc(NULL, ex->sb);
    dp_mach_save(m, img);
    uint16_t pc = image_pc(img);
    uint16_t bank = (uint16_t)(pc < 0x4000 ? 0 : ((const gbl_cpu_t *)img)->rom_bank);
    uint32_t choice = choice_new(ex, ex->cur_choice, ex->cur_kind, dp_rom_index(bank, pc), bank, pc);
    defer_push(ex, img, ex->cur_forced, ex->cur_kind, ex->cur_from_reset, choice, reason);
}

static void defer_cut(DpExplorer *ex, DpMach *m, int loop) {
    defer_cut_reason(ex, m, loop ? DP_R_LOOP : DP_R_STEPS);
}

// a fork variant that the per-site cap / covered prune would have dropped
static void fork_deferred(DpExplorer *ex, uint8_t *fimg, int reason, uint32_t site, uint16_t tpc) {
    if (!defer_room(ex, reason)) { free(fimg); return; }
    uint32_t choice = choice_new(ex, ex->cur_choice, DP_PATH_FORK, site, (int)cur_bank(ex, tpc), tpc);
    if (ex->tree) dp_tree_close(ex->tree, choice, DP_ISSUE_SPOOLED);
    defer_push(ex, fimg, 1, DP_PATH_FORK, ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset, choice, reason);
}

// a seed the caps held back; the list is bounded and its overflow counted
static void pseed_add(DpExplorer *ex, int bank, uint16_t pc, int kind, int forced) {
    if (ex->pseed_len >= 65536) { ex->res->seeds_lost++; return; }
    if (ex->pseed_len >= ex->pseed_cap) {
        ex->pseed_cap = ex->pseed_cap ? ex->pseed_cap * 2 : 256;
        ex->pseed = xrealloc(ex->pseed, sizeof *ex->pseed * ex->pseed_cap);
    }
    ex->pseed[ex->pseed_len++] = (DpPendingSeed){ (uint16_t)bank, pc, (uint8_t)kind, (uint8_t)forced };
    ex->res->seeds_deferred++;
}

static void hook_step(DpMach *m, uint16_t pc, uint8_t op, int entry) {
    (void)entry;
    DpExplorer *ex = m->owner;
    DpResults *res = ex->res;
    const DpGblApi *api = m->api;

    // a mapper write from the previous step resolved: record the switch
    emit_pend_map(ex);

    // the fork explorer's walls do not apply to session workers: their play
    // is bounded by frames and segments, and they share res->steps_total
    if (!ex->session) {
        // the wall counts the fork explorer's own steps: the sessions' play
        // steps share res->steps_total and must not starve the total search
        ex->steps_own++;
        if (ex->steps_own >= ex->cfg.max_steps_total) {
            res->budget_steps++;
            ex->stop = 1;
            defer_cut_reason(ex, m, DP_R_WALL); // the running path waits for the next run
            dp_mach_cut(m, DP_CUT_QUIT);
        }
        uint64_t cap = (uint64_t)ex->cfg.max_steps_path << (2 * ex->cur_tier);
        if (++ex->steps_this_path > cap) {
            res->step_cuts++;
            defer_cut(ex, m, 0);
            dp_mach_cut(m, DP_CUT_STEPS);
        }
    }
    u64_add(&res->steps_total, 1);

    // a RETI from the previous step ends its IRQ handler
    if (ex->reti_pending) {
        ex->reti_pending = 0;
        if (ex->irq_depth) ex->irq_depth--;
    }
    if (op == 0xD9) ex->reti_pending = 1;
    uint16_t bank = cur_bank(ex, pc);
    int is_entry = !ex->have_last || ex->last_bank != bank ||
                   (uint16_t)(ex->last_pc + ex->last_len) != pc;
    if (is_entry && (pc == 0x40 || pc == 0x48 || pc == 0x50 || pc == 0x58 || pc == 0x60))
        ex->irq_depth++;

    // illegal opcodes: recorded with their true banked key (the machine
    // itself keeps running whatever its decoder does with them)
    if (is_illegal_op(op)) {
        if (!ex->session) ex->path_dead = 1;
        if (res->mt) pthread_mutex_lock(&res->rec_mtx);
        // one record per site: a hot illegal opcode used to append every hit
        if (dp_set_insert(&res->bad_seen, key_at(ex, pc)) != 1) {
            if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
            goto bad_done;
        }
        if (res->bad_op_count == res->bad_op_cap) {
            res->bad_op_cap = res->bad_op_cap ? res->bad_op_cap * 2 : 16;
            res->bad_ops = xrealloc(res->bad_ops, sizeof(*res->bad_ops) * (size_t)res->bad_op_cap);
        }
        res->bad_ops[res->bad_op_count].key = key_at(ex, pc);
        res->bad_ops[res->bad_op_count].op = op;
        res->bad_op_count++;
        if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
    }
bad_done:

    // nonsense eviction (fork explorer only): the path proved senseless —
    // execution in echo RAM or OAM, execution inside verbatim-proven data,
    // or a RET whose stack pointer left WRAM/HRAM.  The path is cut at once
    // (the nonsense bytes never become code evidence); the reason is counted
    // and the tree choice closes with the nonsense issue.
    if (!ex->session && !ex->path_nonsense) {
        uint32_t pk = key_at(ex, pc);
        int ns = DP_NS_NONE;
        if (pc >= 0xE000 && pc < 0xFE00) ns = DP_NS_ECHO; // echo RAM
        else if (pc >= 0xFE00 && pc < 0xFF00) ns = DP_NS_ECHO; // OAM
        else if ((res->data_flags[pk] & DP_DF_VERBATIM) &&
                 !(res->exec_flags[pk] & DP_XF_NATURAL)) ns = DP_NS_DATA;
        else if ((op == 0xC9 || op == 0xD9) &&
                 (api->gbr->sp < 0xC000 || api->gbr->sp > 0xFFFE)) ns = DP_NS_STACK;
        if (ns != DP_NS_NONE) {
            ex->path_nonsense = (uint8_t)ns;
            if (ns == DP_NS_ECHO) res->nonsense_echo++;
            else if (ns == DP_NS_DATA) res->nonsense_data++;
            else res->nonsense_stack++;
            dp_mach_cut(m, DP_CUT_HALT);
        }
    }

    // execution back at the entry point: the game reset itself
    if (!ex->session && pc == 0x0100 && ex->steps_this_path > 64 &&
        !(ex->cur_kind == DP_SEED_RESET && ex->steps_no_credit == 0))
        ex->path_reset = 1;

    // execution sliding through a padding run: keep going (the code after
    // the run is often the point of a forced entry) but never call the
    // padding itself code; bound the slide so garbage loops stay bounded
    uint32_t pk = key_at(ex, pc);
    if ((res->static_flags[pk] & DP_SF_PAD) && (op == 0x00 || op == 0xFF)) {
        if (++ex->pad_slide > 4096) {
            res->pad_cuts++;
            dp_mach_cut(m, DP_CUT_PAD);
        }
        ex->cur_pad = 1;
    } else {
        ex->pad_slide = 0;
        ex->cur_pad = 0;
    }

    // forking: both sides of every conditional, and the road not taken when
    // an IRQ is about to dispatch (the machine takes the IRQ naturally)
    if (!ex->session) {
        int cond = sm83_cond_of(op);
        if (cond >= 0) fork_branch(ex, cond, pk, pc, op);
        if (!ex->cfg.no_irq_fork && api->gbr->ime && (*api->ie & api->io[0x0F] & 0x1F))
            fork_irq(ex);
        // keys that only the joypad IRQ can deliver (IE bit 4 armed, nothing
        // pending): the variant presses one key the machine is not pressing
        if (!ex->cfg.no_key_fork && !ex->cfg.no_irq_fork && (*api->ie & 0x10))
            fork_key(ex, pk);
        // keys a polling game reads straight out of $FF00 (no IRQ needed):
        // propose each pressed key as its own variant
        if (!ex->cfg.no_p1_fork && (op == 0xF0 || op == 0xF2))
            fork_p1(ex, pk, pc, op);
    }

    // backward edges and call sites (operand bytes read through the map)
    uint16_t target = 0;
    int is_back = 0, is_call = 0;
    switch (op) {
    case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t e8 = (int8_t)dp_mach_op(m, (uint16_t)(pc + 1));
        target = (uint16_t)(pc + 2 + e8);
        is_back = target <= pc;
        break;
    }
    case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA:
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xCD: {
        target = (uint16_t)(dp_mach_op(m, (uint16_t)(pc + 1)) |
                            (dp_mach_op(m, (uint16_t)(pc + 2)) << 8));
        is_back = target <= pc && (op == 0xC3 || op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA);
        is_call = op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC;
        break;
    }
    case 0xE9: { // JP HL: the observed target feeds the indirect-site table
        if (!ex->session) {
            DpIndirect *ind = dp_map_get_or_add(&res->indirect, pk);
            uint16_t t = api->gbr->hl;
            if (ind) {
                ind->hits++;
                uint8_t tb = (uint8_t)(t < 0x4000 ? 0 : cur_bank(ex, t));
                int have = 0;
                for (int i = 0; i < ind->n; i++)
                    if (ind->target[i] == t && ind->tbank[i] == tb) have = 1;
                if (!have) {
                    if (ind->n < 8) { ind->target[ind->n] = t; ind->tbank[ind->n++] = tb; }
                    else ind->overflow = 1;
                }
            }
        }
        break;
    }
    default:
        if ((op & 0xC7) == 0xC7) { // RST
            target = (uint16_t)(op & 0x38);
            is_call = 1;
        }
        break;
    }
    if (is_back && ex->cfg.loop_back_cap) {
        int found = -1;
        for (int i = 0; i < 8; i++)
            if (ex->back_ring[i].pc == target && ex->back_ring[i].count) { found = i; break; }
        if (found < 0) {
            found = ex->back_idx;
            ex->back_idx = (ex->back_idx + 1) & 7;
            ex->back_ring[found].pc = target;
            ex->back_ring[found].count = 0;
        }
        ex->back_ring[found].count++;
        dp_copy_on_back_edge(&ex->ct, target, pk);
        if (ex->back_ring[found].count > (uint64_t)ex->cfg.loop_back_cap << (2 * ex->cur_tier) &&
            !ex->session) {
            res->cycle_cuts++;
            defer_cut(ex, m, 1);
            dp_mach_cut(m, DP_CUT_CYCLES);
        }
    }
    if (is_call && !ex->session) {
        uint32_t tkey = dp_rom_index(target < 0x4000 ? 0 : (uint32_t)cur_bank(ex, target), target);
        DpCallList *cl = dp_map_get_or_add(&res->callers, tkey);
        if (cl && cl->n < 8) {
            int dup = 0;
            for (int i = 0; i < cl->n; i++)
                if (cl->site[i] == pk) dup = 1;
            if (!dup) cl->site[cl->n++] = pk;
        }
    }
    // branch-site outcome: the previous conditional's natural outcome shows now
    if (!ex->session && ex->prev_was_cond) {
        ex->prev_was_cond = 0;
        uint32_t skey = dp_rom_index(ex->prev_site_bank, ex->prev_site_pc);
        DpSiteStat *st = dp_map_get_or_add(&res->sites, skey);
        if (st) {
            if (pc == ex->prev_target) st->taken++;
            else st->nottaken++;
        }
    }

    // exec marking + section touch (padding slides are never classified)
    if (!ex->cur_pad) {
        if (pc >= 0x8000) {
            // executing VRAM/WRAM/HRAM code: real hardware allows it, but it
            // is not ROM map information — count it and move on
            u32_add(&res->exec_outside, 1);
        } else {
            uint8_t nf = (uint8_t)(ex->cur_forced ? DP_XF_FORCED : DP_XF_NATURAL);
            uint8_t old = flags_or(&res->exec_flags[pk], nf);
            uint32_t xs = ex->session ? DP_SRC_SESSION : (ex->cur_forced ? DP_SRC_FORK_FORCED : DP_SRC_FORK_NATURAL);
            src_or(res, pk, xs);
            if (!(old & nf)) ex->credit_exec++;
            if (!(old & DP_XF_START)) flags_or(&res->exec_flags[pk], DP_XF_START);
            hits_add(res, &res->exec_hits[pk]);
            if (ex->session) hits_add(res, &res->play_exec_hits[pk]);
            if (is_entry) flags_or(&res->exec_flags[pk], DP_XF_ENTRY);
            if (ex->irq_depth) flags_or(&res->exec_flags[pk], DP_XF_IRQ);
            int len = sm83_len(op);
            uint16_t hi = pc;
            for (int i = 1; i < len; i++) { // operand bytes count as executed code
                uint16_t a = (uint16_t)(pc + i);
                if (pc < 0x4000 && a >= 0x4000) break; // crossed the window edge
                uint32_t k = dp_rom_index(bank, a);
                uint8_t o2 = flags_or(&res->exec_flags[k], nf);
                src_or(res, k, xs);
                if (!(o2 & nf)) ex->credit_exec++;
                hi = a;
            }
            uint32_t id = dp_section_touch(res, (uint8_t)bank, pc, hi, ex->cur_forced ? 0 : 1,
                                           is_entry, ex->cur_sec,
                                           ex->cur_from_reset && !ex->cur_forced,
                                           ex->irq_depth > 0);
            ex->cur_sec = id;
        }
    }
    ex->last_pc = pc;
    ex->last_bank = bank;
    ex->last_len = (uint8_t)sm83_len(op);
    ex->have_last = 1;
    ex->prev_was_cond = (uint8_t)(sm83_cond_of(op) >= 0 && !ex->session);
    ex->prev_target = target;
    ex->prev_site_bank = bank;
    ex->prev_site_pc = pc;

    // merged detection: steps since the last first discovery on this path
    if (!ex->session && ex->tree) {
        uint64_t cur = ex->credit_exec + ex->credit_data + ex->credit_copies + ex->credit_io +
                       ex->credit_banks;
        if (cur != ex->credit_shown) {
            ex->credit_shown = cur;
            ex->steps_no_credit = 0;
        } else {
            ex->steps_no_credit++;
        }
    }
}

// --- forking -------------------------------------------------------------------------
// The variant is captured by mutating the live machine through the public
// api (the flag bit, the IF byte), saving, then restoring the natural state:
// three state images per fork, no knowledge of the image layout needed.
// true when both successors of the conditional at pc were already executed:
// the fork can then only add state diversity, never a new byte of code
static int both_sides_covered(DpExplorer *ex, uint16_t pc, uint8_t op) {
    DpResults *res = ex->res;
    uint16_t bank = cur_bank(ex, pc);
    uint16_t taken;
    switch (op) {
    case 0x20: case 0x28: case 0x30: case 0x38:
        taken = (uint16_t)(pc + 2 + (int8_t)dp_mach_op(ex->mach, (uint16_t)(pc + 1)));
        break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        taken = (uint16_t)(dp_mach_op(ex->mach, (uint16_t)(pc + 1)) |
                           (dp_mach_op(ex->mach, (uint16_t)(pc + 2)) << 8));
        break;
    default:
        return 0; // RET cc: the taken side depends on the stack
    }
    uint16_t fall = (uint16_t)(pc + sm83_len(op));
    uint16_t sides[2] = { taken, fall };
    for (int i = 0; i < 2; i++) {
        uint16_t a = sides[i];
        if (a >= 0x8000) return 0;
        uint32_t k = dp_rom_index(a < 0x4000 ? 0 : (a >= 0x4000 && pc < 0x4000 ? ex->mach->api->gbr->rom_bank
                                                                              : bank), a);
        if (k >= (uint32_t)res->banks * 0x4000) return 0;
        uint8_t f = res->exec_flags[k];
        if (!(f & DP_XF_START) || !(f & (DP_XF_NATURAL | DP_XF_FORCED))) return 0;
    }
    return 1;
}

/* per-site fork counter; a full counter table must not lift the cap (the
 * NULL slot used to skip it): the fork is refused and the saturation counted */
static uint32_t *fork_slot(DpExplorer *ex, uint32_t key) {
    uint32_t *cnt = dp_map_get_or_add(&ex->res->fork_counts, key);
    if (!cnt) {
        ex->res->forks_table_full++;
        ex->res->forks_capped++;
    }
    return cnt;
}

static void fork_branch(DpExplorer *ex, int cond, uint32_t site, uint16_t pc, uint8_t op) {
    // per-site cap: loop bodies reach their conditional every iteration and
    // the extra variants are data drift, not new map information
    uint32_t *cnt = fork_slot(ex, site);
    if (!cnt) return;
    int defer = 0; // the variant is built anyway and parked under its reason
    if (*cnt >= ex->cfg.fork_site_cap) {
        ex->res->forks_capped++;
        defer = DP_R_FORK_CAP;
    } else if (!ex->cfg.no_covered_prune && *cnt >= ex->cfg.covered_fork_keep &&
               both_sides_covered(ex, pc, op)) {
        ex->res->forks_covered_pruned++;
        defer = DP_R_COVERED;
    }
    // a deferred variant spends the site's counter too: each site parks at most
    // as many variants as its cap again (a loop body cannot flood the frontier)
    if (defer && (*cnt >= 2 * ex->cfg.fork_site_cap || !defer_room(ex, defer))) {
        if (*cnt >= 2 * ex->cfg.fork_site_cap) ex->res->defer_final[defer]++;
        return; // counted final, no snapshot paid
    }
    (*cnt)++;
    DpMach *m = ex->mach;
    uint8_t flag = sm83_cond_flag(cond); // 0x80 = Z, 0x10 = C
    uint8_t *img = xrealloc(NULL, ex->sb);
    dp_mach_save(m, img);
    if (flag == 0x80) m->api->gbr->zr = m->api->gbr->zr ? 0 : 1; // Z set <=> zr == 0
    else m->api->gbr->cf ^= 1;
    uint8_t *fimg = xrealloc(NULL, ex->sb);
    dp_mach_save(m, fimg);
    dp_mach_load(m, img); // the natural side keeps running
    free(img);
    // the fork keeps the reset lineage of its parent path
    uint16_t tpc = ex->mach->api->gbr->pc;
    if (defer) { fork_deferred(ex, fimg, defer, site, tpc); return; }
    queue_push(ex, fimg, 1, DP_PATH_FORK,
               ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
               choice_new(ex, ex->cur_choice, DP_PATH_FORK, site, (int)cur_bank(ex, tpc), tpc));
    ex->res->forks_branch++;
}

static void fork_irq(DpExplorer *ex) {
    const DpGblApi *api = ex->mach->api;
    uint8_t pending = (uint8_t)(*api->ie & api->io[0x0F] & 0x1F);
    if (!pending) return;
    DpMach *m = ex->mach;
    uint8_t *img = xrealloc(NULL, ex->sb);
    dp_mach_save(m, img);
    api->io[0x0F] &= (uint8_t)~pending; // the variant: the IRQ never fires
    uint8_t *fimg = xrealloc(NULL, ex->sb);
    dp_mach_save(m, fimg);
    dp_mach_load(m, img);
    free(img);
    uint16_t tpc = m->api->gbr->pc;
    queue_push(ex, fimg, 1, DP_PATH_FORK,
               ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
               choice_new(ex, ex->cur_choice, DP_PATH_FORK, 0, (int)cur_bank(ex, tpc), tpc));
    ex->res->forks_irq++;
}


/* the key-IRQ variant: the console raises the joypad IRQ when a NEW key is
 * pressed; games that poll keys only through that IRQ see the press through
 * the handler, which the natural path here cannot produce without a player.
 * Each pressed variant is one key the machine currently does not hold,
 * capped per site like branch forks. */
static void fork_key(DpExplorer *ex, uint32_t site) {
    const DpGblApi *api = ex->mach->api;
    if (api->io[0x0F] & 0x10) return; // already pending: the natural IRQ fires
    uint32_t *cnt = fork_slot(ex, site ^ 0x80000000u);
    if (!cnt) return;
    // once the joypad handler ($60) has run, more key variants at a site only
    // re-run known code: one round of presses per site is enough
    uint32_t kcap = (ex->res->exec_flags[0x60] & DP_XF_START) ? 1 : ex->cfg.fork_site_cap;
    if (ex->cfg.no_covered_prune) kcap = ex->cfg.fork_site_cap;
    int kdefer = 0;
    if (*cnt >= kcap) {
        ex->res->forks_capped++;
        if (*cnt >= 2 * kcap) { ex->res->defer_final[DP_R_FORK_CAP]++; return; }
        if (!defer_room(ex, DP_R_FORK_CAP)) return;
        kdefer = DP_R_FORK_CAP;
    }
    (*cnt)++;
    static const uint8_t keys[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    // the keys the machine already holds, read back from P1 (bits low = pressed)
    DpMach *m = ex->mach;
    uint8_t *img = xrealloc(NULL, ex->sb);
    dp_mach_save(m, img);
    // the fork explorer never holds keys, so every key press is "new":
    // set_buttons raises IF bit 4, which is exactly the console behaviour
    for (int k = 0; k < 8; k++) {
        if (k) { dp_mach_load(m, img); }
        m->api->set_buttons(keys[k]);
        if (api->io[0x0F] & 0x10) { // this key raised the joypad IRQ
            uint8_t *fimg = xrealloc(NULL, ex->sb);
            dp_mach_save(m, fimg);
            uint16_t tpc = m->api->gbr->pc;
            if (kdefer) { fork_deferred(ex, fimg, kdefer, site, tpc); continue; }
            queue_push(ex, fimg, 1, DP_PATH_FORK,
                       ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
                       choice_new(ex, ex->cur_choice, DP_PATH_FORK, site,
                                  (int)cur_bank(ex, tpc), tpc));
            ex->res->forks_key++;
        }
    }
    dp_mach_load(m, img); // the natural side continues, untouched
    m->api->set_buttons(0); // recompute the derived P1 register (not in the image)
    free(img);
}

/* the P1-poll variant: games that read $FF00 directly (polling, no joypad
 * IRQ armed) never see a key pressed on this explorer — the buttons stay
 * released for the whole run.  Each variant holds one key the machine is
 * not pressing; the joypad IRQ the console would raise at the press is
 * cleared in the variant so only the polled VALUE differs (the IRQ path
 * already has its own forks).  Capped per site like the branch forks. */
static void fork_p1(DpExplorer *ex, uint32_t site, uint16_t pc, uint8_t op) {
    const DpGblApi *api = ex->mach->api;
    if (op == 0xF0) { // ldh a,(n): only $FF00 is the joypad
        if (dp_mach_op(ex->mach, (uint16_t)(pc + 1)) != 0x00) return;
    } else if (api->gbr->c != 0x00) { // ld a,(C): P1 only when C == $FF00
        return;
    }
    uint32_t *cnt = fork_slot(ex, site ^ 0xC0000000u);
    if (!cnt) return;
    int pdefer = 0;
    if (*cnt >= ex->cfg.fork_site_cap) {
        ex->res->forks_capped++;
        if (*cnt >= 2 * ex->cfg.fork_site_cap) { ex->res->defer_final[DP_R_FORK_CAP]++; return; }
        if (!defer_room(ex, DP_R_FORK_CAP)) return;
        pdefer = DP_R_FORK_CAP;
    }
    (*cnt)++;
    static const uint8_t keys[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    DpMach *m = ex->mach;
    uint8_t *img = xrealloc(NULL, ex->sb);
    dp_mach_save(m, img);
    for (int k = 0; k < 8; k++) {
        m->api->set_buttons(keys[k]);
        api->io[0x0F] &= (uint8_t)~0x10; // the value fork: no IRQ side effect
        uint8_t *fimg = xrealloc(NULL, ex->sb);
        dp_mach_save(m, fimg);
        uint16_t tpc = m->api->gbr->pc;
        if (pdefer) { fork_deferred(ex, fimg, pdefer, site, tpc); continue; }
        queue_push(ex, fimg, 1, DP_PATH_FORK,
                   ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset,
                   choice_new(ex, ex->cur_choice, DP_PATH_FORK, site,
                              (int)cur_bank(ex, tpc), tpc));
        ex->res->forks_p1++;
    }
    dp_mach_load(m, img); // the natural side continues, untouched
    m->api->set_buttons(0); // recompute the derived P1 register (not in the image)
    free(img);
}
// --- run loop ---------------------------------------------------------------------------
// context projection: hash of the incoming state components this section reads
// (the live machine holds the state being entered).  Cartridge RAM pages are
// hashed raw: that folds the RAM bank and the RAMG gate into one proof.
static uint64_t project_ctx(DpExplorer *ex, const DpSection *s) {
    const DpGblApi *api = ex->mach->api;
    gbl_cpu_t *g = api->gbr;
    uint64_t h = 0xcbf29ce484222325ull;
    h = dp_fnv1a(h, &g->a, 5); // a, zr, cf, hf, nf
    h = dp_fnv1a(h, &g->bc, 2);
    h = dp_fnv1a(h, &g->de, 2);
    h = dp_fnv1a(h, &g->hl, 2);
    h = dp_fnv1a_u32(h, g->sp);
    if (s->ctx_mask & (1ull << DP_CTX_IME)) h = dp_fnv1a(h, &g->ime, 1);
    if (s->ctx_mask & (1ull << DP_CTX_IE)) h = dp_fnv1a(h, api->ie, 1);
    if (s->ctx_mask & (1ull << DP_CTX_ROMN)) h = dp_fnv1a_u32(h, g->rom_bank);
    if (s->ctx_mask & (1ull << DP_CTX_VBK)) h = dp_fnv1a(h, &api->io[0x4F], 1);
    for (int pg = 0; pg < 32; pg++) {
        if (s->page_mask_wram & (1ull << pg)) h = dp_fnv1a(h, api->wram + pg * 256, 256);
        // the runtime holds a single VRAM bank (gbl_vram[0x2000]): reads under
        // VBK=1 land in it too, so both page masks hash the same 8 KiB
        // (reading vram + 0x2000 ran past the array)
        if ((s->page_mask_vram[0] | s->page_mask_vram[1]) & (1ull << pg))
            h = dp_fnv1a(h, api->vram + pg * 256, 256);
    }
    uint32_t eram_bytes = dp_gbl_variant_eram[ex->mach->vi];
    for (int pg = 0; pg < 64 && (uint32_t)(pg * 256) < eram_bytes; pg++)
        if (s->page_mask_sram & (1ull << pg)) h = dp_fnv1a(h, api->eram + pg * 256, 256);
    if (s->ctx_hram) h = dp_fnv1a(h, api->hram, 0x7F);
    if (s->ctx_oam) h = dp_fnv1a(h, api->oam, 0xA0);
    for (int i = 0; i < 0x80; i++)
        if (s->io_read_mask & (1u << i)) h = dp_fnv1a(h, &api->io[i], 1);
    return h;
}

// --- disk spool -------------------------------------------------------------------
// Overflow states survive: a full worklist under the mem-cap/q_max writes them
// to <state-dir>/spool.bin instead of dropping them, dp_explore_step feeds them
// back while the worklist runs low, and a flush before exit lets --resume
// continue exactly where the run stopped.  Record: magic 'DPSR', flags byte,
// u32 choice, u32 img length, image bytes.  One file, two cursors: records are
// appended at spool_wr and read back from spool_rd.
#define DP_SPOOL_REC 0x52535044ull // 'DPSR' little-endian

static int spool_write_rec(DpExplorer *ex, const uint8_t *img, int forced, int kind,
                           int from_reset, uint32_t choice) {
    DpSpool *sp = ex->spool;
    if (!sp) return 0;
    pthread_mutex_lock(&sp->mtx);
    if ((ex->cfg.spool_cap_bytes && sp->wr - sp->rd >= ex->cfg.spool_cap_bytes) ||
        (ex->cfg.spool_cap_mb && (sp->wr - sp->rd) / (1024 * 1024) >= ex->cfg.spool_cap_mb)) {
        pthread_mutex_unlock(&sp->mtx);
        u32_add(&ex->res->spool_refused, 1);
        return 0;
    }
    uint64_t hdr[3] = { DP_SPOOL_REC,
                        (uint64_t)(forced ? 1 : 0) | ((uint64_t)kind << 8) |
                            ((uint64_t)(from_reset ? 1 : 0) << 16) | ((uint64_t)choice << 32),
                        ex->sb };
    int ok = !fseek(sp->f, (long)sp->wr, SEEK_SET) &&
             fwrite(hdr, sizeof hdr, 1, sp->f) == 1 && fwrite(img, 1, ex->sb, sp->f) == ex->sb;
    if (ok) {
        sp->wr += sizeof hdr + ex->sb;
        u32_add(&ex->res->spool_writes, 1);
        ex->res->spool_bytes = sp->wr - sp->rd;
    } else {
        perror("deepeel spool");
    }
    pthread_mutex_unlock(&sp->mtx);
    return ok;
}

// feeds pending spool records back into the worklist (never the priority
// inbox: its seeds are retried by their own phases).  Stops when the spool is
// drained or the target holds no more room.  The exact-state dedup already
// contains these states (inserted at push time), so nothing is re-hashed.
static void spool_feed(DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp) return;
    for (;;) {
        pthread_mutex_lock(&sp->mtx);
        if (sp->rd >= sp->wr) { pthread_mutex_unlock(&sp->mtx); return; }
        // room checked under the pool lock when shared (the push must not overflow)
        if (ex->sh) pthread_mutex_lock(&ex->sh->mtx);
        int room = ex->sh ? (ex->sh->len < ex->sh->cap) : (ex->q_len < ex->q_cap);
        if (!room) {
            if (ex->sh) pthread_mutex_unlock(&ex->sh->mtx);
            pthread_mutex_unlock(&sp->mtx);
            return;
        }
        uint64_t hdr[3];
        if (fseek(sp->f, (long)sp->rd, SEEK_SET) ||
            fread(hdr, sizeof hdr, 1, sp->f) != 1 || hdr[0] != DP_SPOOL_REC ||
            hdr[2] != ex->sb) {
            fprintf(stderr, "deepeel: spool corrupt at %llu, dropping the rest\n",
                    (unsigned long long)sp->rd);
            sp->rd = sp->wr;
            pthread_mutex_unlock(&sp->mtx);
            return;
        }
        uint8_t *img = xrealloc(NULL, ex->sb);
        if (!img || fread(img, 1, ex->sb, sp->f) != ex->sb) {
            free(img);
            pthread_mutex_unlock(&sp->mtx);
            return;
        }
        sp->rd += sizeof hdr + ex->sb;
        u32_add(&ex->res->spool_reads, 1);
        ex->res->spool_bytes = sp->wr - sp->rd;
        DpPathState p;
        memset(&p, 0, sizeof p);
        p.img = img;
        p.forced = (uint8_t)(hdr[1] & 1);
        p.kind = (uint8_t)(hdr[1] >> 8);
        p.from_reset = (uint8_t)((hdr[1] >> 16) & 1);
        p.choice = (uint32_t)(hdr[1] >> 32);
        if (ex->sh) { // the parallel drain: everything goes through the pool
            ex->sh->q[(ex->sh->head + ex->sh->len) % ex->sh->cap] = p;
            ex->sh->len++;
            pthread_mutex_unlock(&ex->sh->mtx);
            pthread_mutex_unlock(&sp->mtx);
        } else {
            pthread_mutex_unlock(&sp->mtx);
            ex->queue[(ex->q_head + ex->q_len) % ex->q_cap] = p;
            ex->q_len++;
        }
    }
}

// read-cursor sidecar: 'DPSC', image size, rd, wr (the wr it was taken at)
#define DP_SPOOL_CURSOR 0x43535044ull // 'DPSC' little-endian

static void spool_save_cursor(DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp || !sp->cursor_path) return;
    pthread_mutex_lock(&sp->mtx);
    uint64_t c[4] = { DP_SPOOL_CURSOR, ex->sb, sp->rd, sp->wr };
    pthread_mutex_unlock(&sp->mtx);
    FILE *f = fopen(sp->cursor_path, "wb");
    if (!f || fwrite(c, sizeof c, 1, f) != 1) perror("deepeel spool cursor");
    if (f) fclose(f);
}

int dp_explorer_spool_attach(DpExplorer *ex, const char *path, int resume) {
    DpSpool *sp = calloc(1, sizeof *sp);
    if (!sp) return 0;
    sp->f = fopen(path, resume ? "r+b" : "wb+");
    // --resume without a previous spool starts an empty one
    if (!sp->f && resume) sp->f = fopen(path, "wb+");
    if (!sp->f) { free(sp); return 0; }
    pthread_mutex_init(&sp->mtx, NULL);
    size_t pl = strlen(path);
    sp->cursor_path = xrealloc(NULL, pl + 4);
    memcpy(sp->cursor_path, path, pl);
    memcpy(sp->cursor_path + pl, ".rd", 4);
    ex->spool = sp;
    ex->spool_owned = 1;
    if (resume && fseek(sp->f, 0, SEEK_END) == 0) {
        long sz = ftell(sp->f);
        if (sz >= 0) {
            // only whole records count: a killed process may have truncated one
            size_t rec = sizeof(uint64_t) * 3 + ex->sb;
            sp->wr = (uint64_t)((long)sz / (long)rec * (long)rec);
            // pending records start at the persisted read cursor; without one
            // (older spool, or a kill before the first flush) every record is
            // fed back again: re-running a state costs time, skipping one
            // loses it
            sp->rd = 0;
            uint64_t c[4];
            FILE *cf = fopen(sp->cursor_path, "rb");
            if (cf && fread(c, sizeof c, 1, cf) == 1 && c[0] == DP_SPOOL_CURSOR &&
                c[1] == ex->sb && c[2] <= sp->wr && c[2] % rec == 0)
                sp->rd = c[2];
            else if (sp->wr)
                fprintf(stderr, "deepeel: spool read cursor missing, feeding back all %llu records\n",
                        (unsigned long long)(sp->wr / rec));
            if (cf) fclose(cf);
            ex->res->spool_bytes = sp->wr - sp->rd;
        }
    } else {
        unlink(sp->cursor_path); // a fresh spool: a stale cursor would skip records
    }
    return 1;
}

// parallel drain: the worker borrows the leader's spool, exact dedup and tree
void dp_explorer_share(DpExplorer *worker, DpExplorer *leader) {
    worker->spool = leader->spool;
    worker->spool_owned = 0;
    worker->exactp = &leader->exact;
    worker->exact_mtx = leader->exact_mtx;
    worker->tree_mtx = &leader->tmtx;
    worker->sh = leader->sh;
}

uint64_t dp_explorer_spool_pending(const DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp) return 0;
    pthread_mutex_lock(&sp->mtx);
    uint64_t p = sp->wr - sp->rd;
    pthread_mutex_unlock(&sp->mtx);
    return p;
}

// pours the remaining worklist into the spool (a stop or a normal exit): the
// next --resume picks the states up; returns states still pending
uint64_t dp_explorer_spool_flush(DpExplorer *ex) {
    if (!ex->spool) return 0;
    // a parallel drain may have left states in the shared pool
    if (ex->sh) {
        pthread_mutex_lock(&ex->sh->mtx);
        for (uint32_t i = 0; i < ex->sh->len; i++) {
            DpPathState *p = &ex->sh->q[(ex->sh->head + i) % ex->sh->cap];
            if (p->img) {
                if (!spool_write_rec(ex, p->img, p->forced, p->kind, p->from_reset, p->choice)) {
                if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, DP_R_PATHS)) {
                    memset(p, 0, sizeof *p);
                    continue;
                }
                ex->res->queue_overflows++; // frontier full too: final and counted
            }
                free(p->img);
                memset(p, 0, sizeof *p);
            }
        }
        ex->sh->len = 0;
        ex->sh->head = 0;
        pthread_mutex_unlock(&ex->sh->mtx);
    }
    for (uint32_t i = 0; i < ex->q_len; i++) {
        DpPathState *p = &ex->queue[(ex->q_head + i) % ex->q_cap];
        if (p->img) {
            if (!spool_write_rec(ex, p->img, p->forced, p->kind, p->from_reset, p->choice)) {
                if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, DP_R_PATHS)) {
                    memset(p, 0, sizeof *p);
                    continue;
                }
                ex->res->queue_overflows++; // frontier full too: final and counted
            }
            free(p->img);
            memset(p, 0, sizeof *p);
        }
    }
    ex->q_len = 0;
    ex->q_head = 0;
    for (uint32_t i = 0; i < ex->pq_len; i++) {
        DpPathState *p = &ex->pqueue[(ex->pq_head + i) % ex->pq_cap];
        if (p->img) {
            if (!spool_write_rec(ex, p->img, p->forced, p->kind, p->from_reset, p->choice)) {
                if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, DP_R_PATHS)) {
                    memset(p, 0, sizeof *p);
                    continue;
                }
                ex->res->queue_overflows++; // frontier full too: final and counted
            }
            free(p->img);
            memset(p, 0, sizeof *p);
        }
    }
    ex->pq_len = 0;
    ex->pq_head = 0;
    fflush(ex->spool->f);
    spool_save_cursor(ex);
    return dp_explorer_spool_pending(ex);
}

// --- exact-set persistence -----------------------------------------------------
int dp_explorer_exact_save(const DpExplorer *ex, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const DpHashSet *s = ex->exactp;
    const uint32_t ver = 1;
    fwrite("DPEX", 1, 4, f);
    fwrite(&ver, sizeof ver, 1, f);
    fwrite(&s->cnt, sizeof s->cnt, 1, f);
    for (size_t i = 0; i < s->cap; i++)
        if (s->used[i]) fwrite(&s->keys[i], sizeof(uint64_t), 1, f);
    int ok = !ferror(f);
    fclose(f);
    return ok;
}

int dp_explorer_exact_load(DpExplorer *ex, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    uint32_t ver;
    size_t cnt = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "DPEX", 4) || fread(&ver, sizeof ver, 1, f) != 1 ||
        ver != 1 || fread(&cnt, sizeof cnt, 1, f) != 1) {
        fclose(f);
        return 0;
    }
    // room for the saved keys plus the run's own discoveries
    DpHashSet *s = ex->exactp;
    size_t need = cnt * 2 + 65536;
    if (need > s->cap_abs) {
        dp_set_free(s);
        dp_set_init(s, need);
    }
    size_t n = 0;
    for (uint64_t k; n < cnt && fread(&k, sizeof k, 1, f) == 1; n++)
        if (dp_set_insert(s, k) != 1) break;
    fclose(f);
    fprintf(stderr, "deepeel: resumed dedup (%zu states known)\n", n);
    return n == cnt;
}


// ---- on-disk frontier (docs/COUVERTURE.fr.md §6.3) ---------------------------
// <dir>/frontier/frontier.idx                      text index, one line per lot
// <dir>/frontier/b<bank>-s<sec>-<reason>-<seq>.dpf one lot: 'DPFB', version,
//   platform, image size, count, base image (zlib), then per record 8 header
//   bytes (tier, forced, kind, from_reset, reason) + zlib length + the zlib XOR
//   delta against the base.  A lot is read sequentially; the byte offset read so
//   far is persisted in the index, so a resume never replays nor skips a record.
// <dir>/frontier/seeds.bin                         seeds held back by the caps
#define DP_LOT_MAX_BYTES (64u << 20)
#define DP_LOT_MAGIC 0x42465044u /* 'DPFB' */

static void frontier_path(const DpExplorer *ex, char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/frontier/%s", ex->fdir, name);
}

static void idx_write(DpExplorer *ex) {
    char p[700];
    frontier_path(ex, p, sizeof p, "frontier.idx");
    if (!ex->nlots) {
        unlink(p);
        return;
    }
    FILE *f = fopen(p, "w");
    if (!f) { perror("deepeel frontier.idx"); return; }
    fprintf(f, "deepeel-frontier 1\nplatform gb\nimage_bytes %u\n", (unsigned)ex->sb);
    for (uint32_t i = 0; i < ex->nlots; i++) {
        const DpLot *l = &ex->lots[i];
        fprintf(f, "lot %s count=%u off=%llu reason=%s bank=%d sec=%d t1=%u t2=%u t3=%u read=%u\n", l->name,
                l->count, (unsigned long long)l->off, dp_reason_name(l->reason), l->bank, l->sec,
                l->tier_n[1], l->tier_n[2], l->tier_n[3], l->read);
    }
    fclose(f);
}

static int reason_of_name(const char *s) {
    for (int r = 1; r < DP_R_COUNT; r++) if (!strcmp(s, dp_reason_name(r))) return r;
    return 0;
}

static void idx_read(DpExplorer *ex) {
    char p[700], line[400];
    frontier_path(ex, p, sizeof p, "frontier.idx");
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        DpLot l;
        memset(&l, 0, sizeof l);
        char name[96], rs[32];
        unsigned long long off;
        if (sscanf(line, "lot %95s count=%u off=%llu reason=%31s bank=%d sec=%d t1=%u t2=%u t3=%u read=%u", name,
                   &l.count, &off, rs, &l.bank, &l.sec, &l.tier_n[1], &l.tier_n[2], &l.tier_n[3], &l.read) == 10) {
            snprintf(l.name, sizeof l.name, "%s", name);
            l.off = off;
            l.reason = reason_of_name(rs);
            if (!l.reason) continue;
            if (ex->nlots >= ex->lotcap) {
                ex->lotcap = ex->lotcap ? ex->lotcap * 2 : 16;
                ex->lots = xrealloc(ex->lots, sizeof *ex->lots * ex->lotcap);
            }
            ex->lots[ex->nlots++] = l;
            unsigned sq = 0;
            const char *dash = strrchr(name, '-');
            if (dash && sscanf(dash + 1, "%u", &sq) == 1 && sq >= ex->lot_seq) ex->lot_seq = sq + 1;
        }
    }
    fclose(f);
}

static void lot_remove(DpExplorer *ex, uint32_t i) {
    char p[700];
    frontier_path(ex, p, sizeof p, ex->lots[i].name);
    unlink(p);
    memmove(&ex->lots[i], &ex->lots[i + 1], sizeof *ex->lots * (ex->nlots - i - 1));
    ex->nlots--;
}

// records still on disk (unread)
uint64_t dp_explorer_frontier_disk(const DpExplorer *ex) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < ex->nlots; i++) n += ex->lots[i].count - ex->lots[i].read;
    return n;
}

static int reason_eligible(const DpExplorer *ex, int reason) {
    return reason < DP_R_COVERED || ex->defer_all;
}

// reads the next records of the cheapest eligible lot into the holding area
static int frontier_refill(DpExplorer *ex) {
    if (!ex->fdir || !ex->nlots) return 0;
    int best = -1;
    for (uint32_t i = 0; i < ex->nlots; i++) {
        const DpLot *l = &ex->lots[i];
        if (l->read >= l->count || !reason_eligible(ex, l->reason)) continue;
        if (best < 0 || l->reason < ex->lots[best].reason) best = (int)i;
    }
    if (best < 0) return 0;
    DpLot *l = &ex->lots[best];
    char p[700];
    frontier_path(ex, p, sizeof p, l->name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "deepeel: frontier lot %s missing, dropped\n", l->name); lot_remove(ex, (uint32_t)best); return 1; }
    uint32_t h[5];
    uint8_t *base = xrealloc(NULL, ex->sb);
    int ok = fread(h, sizeof h, 1, f) == 1 && h[0] == DP_LOT_MAGIC && h[1] == 1 && h[3] == (uint32_t)ex->sb;
    uint32_t bl = 0;
    uint8_t *z = NULL;
    if (ok) {
        ok = fread(&bl, 4, 1, f) == 1 && bl <= 4 * (uint32_t)ex->sb + 1024;
        if (ok) { z = xrealloc(NULL, bl); uLongf o = (uLongf)ex->sb;
                  ok = fread(z, 1, bl, f) == bl && uncompress(base, &o, z, bl) == Z_OK && o == (uLongf)ex->sb; }
        free(z);
    }
    if (!ok) {
        fprintf(stderr, "deepeel: frontier lot %s unusable, dropped (%u records)\n", l->name, l->count - l->read);
        ex->res->defer_dropped += l->count - l->read;
        fclose(f); free(base); lot_remove(ex, (uint32_t)best);
        return 1;
    }
    if (l->off) fseek(f, (long)l->off, SEEK_SET);
    int got = 0;
    while (l->read < l->count && ex->defer_len < DP_DEFER_CAP) {
        uint8_t hd[8];
        uint32_t zl;
        if (fread(hd, 8, 1, f) != 1 || fread(&zl, 4, 1, f) != 1 || zl > 4 * (uint32_t)ex->sb + 1024) { l->read = l->count; break; }
        z = xrealloc(NULL, zl);
        uint8_t *img = xrealloc(NULL, ex->sb);
        uLongf o = (uLongf)ex->sb;
        if (fread(z, 1, zl, f) != zl || uncompress(img, &o, z, zl) != Z_OK || o != (uLongf)ex->sb) {
            free(z); free(img); l->read = l->count; break;
        }
        free(z);
        for (uint32_t i = 0; i < (uint32_t)ex->sb; i++) img[i] ^= base[i];
        if (ex->defer_len >= ex->defer_cap) {
            uint32_t nc = ex->defer_cap ? ex->defer_cap * 2 : 64;
            ex->defer = xrealloc(ex->defer, sizeof *ex->defer * nc);
            ex->defer_cap = nc;
        }
        ex->defer[ex->defer_len++] = (DpDeferred){ { img, hd[1], hd[2], hd[3], 0 }, hd[0], hd[4] };
        ex->defer_by_reason[hd[4] < DP_R_COUNT ? hd[4] : 0]++;
        l->read++;
        l->off = (uint64_t)ftell(f);
        got++;
    }
    fclose(f);
    free(base);
    if (l->read >= l->count) lot_remove(ex, (uint32_t)best);
    idx_write(ex);
    return got ? got : 1;
}

typedef struct { uint32_t i; int reason, bank, sec; } LotKey;
static int lotkey_cmp(const void *a, const void *b) {
    const LotKey *x = a, *y = b;
    if (x->reason != y->reason) return x->reason - y->reason;
    if (x->bank != y->bank) return x->bank - y->bank;
    if (x->sec != y->sec) return x->sec - y->sec;
    return (int)x->i - (int)y->i;
}

static int section_of(const DpResults *res, int bank, uint16_t pc) {
    for (int i = 0; i < res->section_count; i++) {
        const DpSection *s = &res->sections[i];
        if (s->bank == bank && s->lo <= pc && pc <= s->hi) return (int)s->id;
    }
    return 0;
}

// the states still in the worklists at a stop (a path wall) join the frontier
static void adopt_queues(DpExplorer *ex) {
    DpPathState *q[2] = { ex->queue, ex->pqueue };
    uint32_t cap[2] = { ex->q_cap, ex->pq_cap }, len[2] = { ex->q_len, ex->pq_len }, head[2] = { ex->q_head, ex->pq_head };
    for (int w = 0; w < 2; w++) {
        for (uint32_t i = 0; i < len[w]; i++) {
            DpPathState *p = &q[w][(head[w] + i) % cap[w]];
            if (!p->img) continue;
            int save_tier = ex->cur_tier;
            ex->cur_tier = 0; // never refused by the tier cap: a parked path, not a retry
            if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, w ? DP_R_PRIORITY : DP_R_PATHS))
                p->img = NULL;
            ex->cur_tier = (uint8_t)save_tier;
            if (p->img) { ex->res->defer_dropped++; free(p->img); p->img = NULL; }
        }
        if (w) ex->pq_len = 0; else ex->q_len = 0;
    }
}

// writes the whole holding area as lots (one per bank x section x reason, closed at 64 MiB)
// Converts the records of an old state spool ('DPSR') into frontier lots
// (reason paths): the spool is drained into the holding area, which is flushed
// to .dpf lots every DP_DEFER_CAP records. Returns the number converted.
uint64_t dp_explorer_spool_convert(DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp || !ex->fdir) return 0;
    uint64_t n = 0;
    for (;;) {
        while (sp->rd < sp->wr && ex->defer_len < DP_DEFER_CAP) {
            uint64_t hdr[3];
            if (fseek(sp->f, (long)sp->rd, SEEK_SET) || fread(hdr, sizeof hdr, 1, sp->f) != 1 ||
                hdr[0] != DP_SPOOL_REC || hdr[2] != ex->sb) {
                fprintf(stderr, "deepeel: spool corrupt at %llu, conversion stops\n", (unsigned long long)sp->rd);
                sp->rd = sp->wr;
                break;
            }
            uint8_t *img = xrealloc(NULL, ex->sb);
            if (fread(img, 1, ex->sb, sp->f) != ex->sb) { free(img); sp->rd = sp->wr; break; }
            sp->rd += sizeof hdr + ex->sb;
            if (ex->defer_len >= ex->defer_cap) {
                uint32_t nc = ex->defer_cap ? ex->defer_cap * 2 : 64;
                ex->defer = xrealloc(ex->defer, sizeof *ex->defer * nc);
                ex->defer_cap = nc;
            }
            ex->defer[ex->defer_len++] = (DpDeferred){
                { img, (uint8_t)(hdr[1] & 1), (uint8_t)(hdr[1] >> 8), (uint8_t)((hdr[1] >> 16) & 1),
                  (uint32_t)(hdr[1] >> 32) },
                1, DP_R_PATHS };
            n++;
        }
        if (!ex->defer_len) break;
        dp_explorer_frontier_save(ex, ex->fdir);
    }
    ex->res->spool_bytes = 0;
    spool_save_cursor(ex);
    return n;
}

int dp_explorer_frontier_save(DpExplorer *ex, const char *dir) {
    if (!ex->fdir) { ex->fdir = strdup(dir); }
    char p[700];
    mkdir(dir, 0777); // the state directory itself may not exist yet
    snprintf(p, sizeof p, "%s/frontier", dir);
    mkdir(p, 0777);
    adopt_queues(ex);
    int saved = 0;
    if (ex->defer_len) {
        LotKey *k = xrealloc(NULL, sizeof *k * ex->defer_len);
        for (uint32_t i = 0; i < ex->defer_len; i++) {
            uint16_t pc = image_pc(ex->defer[i].p.img);
            int bank = pc < 0x4000 ? 0 : (int)((const gbl_cpu_t *)ex->defer[i].p.img)->rom_bank;
            k[i] = (LotKey){ i, ex->defer[i].reason, bank, section_of(ex->res, bank, pc) };
        }
        qsort(k, ex->defer_len, sizeof *k, lotkey_cmp);
        uLongf cap = compressBound((uLong)ex->sb);
        uint8_t *z = xrealloc(NULL, cap), *d = xrealloc(NULL, ex->sb);
        uint32_t a = 0;
        while (a < ex->defer_len) {
            uint32_t b = a;
            while (b < ex->defer_len && k[b].reason == k[a].reason && k[b].bank == k[a].bank && k[b].sec == k[a].sec) b++;
            // [a,b) is one group: cut it into lots
            for (uint32_t s = a; s < b;) {
                DpLot l;
                memset(&l, 0, sizeof l);
                l.reason = k[a].reason; l.bank = k[a].bank; l.sec = k[a].sec;
                snprintf(l.name, sizeof l.name, "b%d-s%d-%s-%u.dpf", l.bank, l.sec, dp_reason_name(l.reason), ex->lot_seq++);
                char lp[700];
                frontier_path(ex, lp, sizeof lp, l.name);
                FILE *f = fopen(lp, "wb");
                if (!f) { perror("deepeel frontier"); ex->res->defer_dropped += b - s; break; }
                const uint8_t *base = ex->defer[k[s].i].p.img;
                uLongf zl = cap;
                compress2(z, &zl, base, (uLong)ex->sb, 6);
                uint32_t hdr[5] = { DP_LOT_MAGIC, 1, 1 /* gb */, (uint32_t)ex->sb, 0 };
                fwrite(hdr, sizeof hdr, 1, f);
                uint32_t bl = (uint32_t)zl;
                fwrite(&bl, 4, 1, f); fwrite(z, 1, zl, f);
                uint64_t bytes = zl;
                uint32_t n = 0;
                while (s < b && bytes < DP_LOT_MAX_BYTES) {
                    DpDeferred *r = &ex->defer[k[s].i];
                    if (ex->cfg.frontier_cap_mb && ex->fbytes >> 20 >= ex->cfg.frontier_cap_mb) {
                        // --frontier-cap-gb reached: the loss is final, counted per reason
                        ex->res->defer_final[r->reason]++;
                        ex->res->defer_dropped++;
                        s++;
                        continue;
                    }
                    for (uint32_t j = 0; j < (uint32_t)ex->sb; j++) d[j] = r->p.img[j] ^ base[j];
                    zl = cap;
                    if (compress2(z, &zl, d, (uLong)ex->sb, 6) == Z_OK) {
                        uint8_t hd[8] = { r->tier, r->p.forced, r->p.kind, r->p.from_reset, r->reason, 0, 0, 0 };
                        uint32_t l32 = (uint32_t)zl;
                        fwrite(hd, 8, 1, f); fwrite(&l32, 4, 1, f); fwrite(z, 1, zl, f);
                        bytes += 12 + zl;
                        ex->fbytes += 12 + zl;
                        l.tier_n[r->tier <= DP_TIER_MAX ? r->tier : DP_TIER_MAX]++;
                        n++;
                    } else {
                        ex->res->defer_dropped++;
                    }
                    s++;
                }
                fseek(f, 16, SEEK_SET);
                fwrite(&n, 4, 1, f);
                fclose(f);
                // the offset of the first record (after header + base)
                l.count = n;
                l.off = 0; // 0 = start at the first record
                if (ex->nlots >= ex->lotcap) {
                    ex->lotcap = ex->lotcap ? ex->lotcap * 2 : 16;
                    ex->lots = xrealloc(ex->lots, sizeof *ex->lots * ex->lotcap);
                }
                ex->lots[ex->nlots++] = l;
                saved += (int)n;
            }
            a = b;
        }
        free(k); free(z); free(d);
        for (uint32_t i = 0; i < ex->defer_len; i++) free(ex->defer[i].p.img);
        ex->defer_len = 0;
        memset(ex->defer_by_reason, 0, sizeof ex->defer_by_reason);
    }
    idx_write(ex);
    // seeds held back by the caps
    char sp[700];
    frontier_path(ex, sp, sizeof sp, "seeds.bin");
    if (ex->pseed_len) {
        FILE *f = fopen(sp, "wb");
        if (f) { fwrite(ex->pseed, sizeof *ex->pseed, ex->pseed_len, f); fclose(f); }
        saved += (int)ex->pseed_len;
        ex->res->seeds_pending_disk = ex->pseed_len;
        ex->pseed_len = 0;
    } else {
        unlink(sp);
    }
    ex->res->defer_on_disk = dp_explorer_frontier_disk(ex);
    return saved;
}

// attaches the frontier directory: reads the index and loads the first records
int dp_explorer_frontier_load(DpExplorer *ex, const char *dir) {
    free(ex->fdir);
    ex->fdir = strdup(dir);
    ex->nlots = 0;
    idx_read(ex);
    char sp[700];
    frontier_path(ex, sp, sizeof sp, "seeds.bin");
    FILE *f = fopen(sp, "rb");
    if (f) {
        DpPendingSeed s;
        while (fread(&s, sizeof s, 1, f) == 1) pseed_add(ex, s.bank, s.pc, s.kind, s.forced);
        fclose(f);
        unlink(sp);
        ex->res->seeds_deferred -= ex->pseed_len; // reloaded, not new
    }
    uint32_t before = ex->defer_len;
    while (ex->defer_len < DP_DEFER_CAP && frontier_refill(ex)) {
        if (!dp_explorer_frontier_disk(ex)) break;
        int more = 0;
        for (uint32_t i = 0; i < ex->nlots; i++)
            if (ex->lots[i].read < ex->lots[i].count && reason_eligible(ex, ex->lots[i].reason)) more = 1;
        if (!more) break;
    }
    return (int)(ex->defer_len - before) + (int)ex->pseed_len;
}

static int defer_eligible(const DpExplorer *ex) {
    for (uint32_t i = 0; i < ex->defer_len; i++)
        if (reason_eligible(ex, ex->defer[i].reason) && (!ex->defer_tier_limit || ex->defer[i].tier <= ex->defer_tier_limit))
            return 1;
    return 0;
}

// anything left that a pop could still resume (memory, disk lots, held seeds)
static int frontier_has_work(DpExplorer *ex) {
    if (defer_eligible(ex) || ex->pseed_len) return 1;
    for (uint32_t i = 0; i < ex->nlots; i++)
        if (ex->lots[i].read < ex->lots[i].count && reason_eligible(ex, ex->lots[i].reason)) return 1;
    return 0;
}

// next deferred continuation into *out (its tier goes to ex->defer_tier_next); 0 when none.
// Order: lowest reason first (cheapest), newest first within it.
static int defer_pop(DpExplorer *ex, DpPathState *out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        int at = -1;
        for (int i = (int)ex->defer_len - 1; i >= 0; i--) {
            const DpDeferred *d = &ex->defer[i];
            if (!reason_eligible(ex, d->reason)) continue;
            if (ex->defer_tier_limit && d->tier > ex->defer_tier_limit) continue;
            if (at < 0 || d->reason < ex->defer[at].reason) at = i;
        }
        if (at >= 0) {
            DpDeferred dd = ex->defer[at];
            memmove(&ex->defer[at], &ex->defer[at + 1], sizeof dd * (ex->defer_len - at - 1));
            ex->defer_len--;
            if (dd.reason < DP_R_COUNT && ex->defer_by_reason[dd.reason]) ex->defer_by_reason[dd.reason]--;
            *out = dd.p;
            ex->defer_tier_next = dd.tier;
            ex->res->defer_resumed++;
            return 1;
        }
        if (attempt || !frontier_refill(ex)) break;
    }
    return 0;
}

// seeds the caps held back come back in batches of seed_cap once the worklists are empty
static int pseed_feed(DpExplorer *ex) {
    if (!ex->pseed_len) return 0;
    uint32_t batch = ex->cfg.seed_cap ? ex->cfg.seed_cap : 4096, n = 0;
    while (ex->pseed_len && n < batch) {
        DpPendingSeed s = ex->pseed[--ex->pseed_len];
        if (dp_explorer_seed_pc(ex, s.bank, s.pc, s.kind, s.forced)) n++;
        ex->res->seeds_resumed++;
    }
    return n != 0 || ex->pseed_len != 0;
}

int dp_explore_step(DpExplorer *ex) {
    DpResults *res = ex->res;
    // any free worklist slot: pull the spooled overflow back in (also covers
    // the everything-drained case, so a spool keeps dp_explore going)
    if (ex->q_len < ex->q_cap && dp_explorer_spool_pending(ex)) spool_feed(ex);
    // the path wall stops before popping: the pending state stays in the
    // worklist (a flush spools it for --resume) instead of being freed unrun
    if (ex->cfg.max_paths && res->paths_run >= ex->cfg.max_paths) {
        u32_add(&res->budget_paths, 1);
        return 0;
    }
    DpPathState tmp;
    memset(&tmp, 0, sizeof tmp); // the pool may stay empty: img must start NULL
    ex->defer_tier_next = 0;
    DpPathState *p = &tmp;
    int from_pq = ex->pq_len != 0;
    if (ex->sh && !from_pq) { // the parallel drain: pops come from the pool
        pthread_mutex_lock(&ex->sh->mtx);
        if (ex->sh->len) {
            *p = ex->sh->q[ex->sh->head % ex->sh->cap];
            ex->sh->head = (ex->sh->head + 1) % ex->sh->cap;
            ex->sh->len--;
        } else {
            p->img = NULL;
        }
        pthread_mutex_unlock(&ex->sh->mtx);
        if (!p->img && !defer_pop(ex, &tmp)) return 0;
    } else {
        if (!ex->q_len && !ex->pq_len && !defer_pop(ex, &tmp)) {
            // nothing resumable: held seeds come back before the run ends
            if (pseed_feed(ex) && (ex->q_len || ex->pq_len)) return 1;
            return 0;
        }
        if (tmp.img) { /* resumed from the deferred frontier */ }
        else if (from_pq) { // priority seeds first, FIFO: they must not starve
            p = &ex->pqueue[ex->pq_head % ex->pq_cap];
            ex->pq_head = (ex->pq_head + 1) % ex->pq_cap;
            ex->pq_len--;
        } else if (ex->cfg.dfs) { // total search: newest path first, deepest first
            ex->q_len--;
            p = &ex->queue[(ex->q_head + ex->q_len) % ex->q_cap];
        } else {
            p = &ex->queue[ex->q_head % ex->q_cap];
            ex->q_head = (ex->q_head + 1) % ex->q_cap;
            ex->q_len--;
        }
    }
    uint8_t *img = p->img;
    int forced = p->forced, kind = p->kind, from_reset = p->from_reset;
    uint32_t choice = p->choice;
    uint8_t from_defer = p == &tmp ? ex->defer_tier_next : 0;
    ex->cur_forced = forced;
    ex->cur_kind = (uint8_t)kind;
    ex->cur_from_reset = (uint8_t)from_reset;
    ex->cur_choice = choice;
    ex->cur_tier = from_defer;
    ex->path_dead = 0;
    ex->path_reset = 0;
    ex->steps_no_credit = 0;
    ex->credit_shown = ex->credit_exec + ex->credit_data + ex->credit_copies + ex->credit_io +
                       ex->credit_banks;
    ex->have_last = 0;
    ex->cur_sec = 0;
    ex->steps_this_path = 0;
    ex->pad_slide = 0;
    ex->cur_pad = 0;
    ex->irq_depth = 0;
    ex->reti_pending = 0;
    ex->prev_was_cond = 0;
    ex->path_nonsense = 0;
    ex->pend_map = 0;
    memset(ex->back_ring, 0, sizeof ex->back_ring);
    ex->back_idx = 0;
    dp_copy_reset(&ex->ct);
    dp_mach_attach(ex->mach, ex, hook_step, hook_read, hook_write, NULL);
    dp_mach_load(ex->mach, img); // execution starts at the image's pc
    free(img);
    if (p != &tmp && ex->sh) { // trap: a NULL written into a live ring slot?
        long idx = p - ex->queue;
        long rel = (idx - (long)ex->q_head + (long)ex->q_cap) % (long)ex->q_cap;
        if (rel < (long)ex->q_len)
            fprintf(stderr, "deepeel: BUG NULL write in ring window (idx=%ld q_head=%u q_len=%u)\n",
                    idx, ex->q_head, ex->q_len);
    }
    p->img = NULL;
    // "same context" eviction: entering a section start with a context the
    // section cannot distinguish from one already explored.  A priority seed
    // bypasses it: it is a deliberate probe of a specific entry, few by cap.
    // (a deferred continuation is the same path, mid-section: never evicted)
    if (!ex->cfg.no_context_evict && !from_pq && !from_defer) {
        const DpGblApi *api = ex->mach->api;
        uint32_t key = key_at(ex, api->gbr->pc);
        uint32_t sid1 = res->section_of[key];
        if (sid1) {
            DpSection *s = &res->sections[sid1 - 1];
            if (key == dp_rom_index(s->bank, s->lo)) {
                uint64_t h = project_ctx(ex, s);
                if (ex->sh) pthread_mutex_lock(&res->sec_mtx);
                int ins = dp_set_insert(&s->ctx_seen, h);
                if (ex->sh) pthread_mutex_unlock(&res->sec_mtx);
                if (ins == 0) {
                    u32_add(&res->evicted_ctx, 1);
                    if (ex->tree) dp_tree_close(ex->tree, choice, DP_ISSUE_MERGED);
                    return (ex->q_len || ex->pq_len) ? 1 : 0;
                }
                if (ins > 0) s->ctx_count++;
            }
        }
    }
    int cut = dp_mach_run(ex->mach);
    if (cut == DP_CUT_HALT) res->halt_cuts++;
    dp_copy_flush_path(ex, &ex->ct);
    u64_add(&res->paths_run, 1);
    // the tree choice of this path closes with its outcome
    {
        int issue = DP_ISSUE_DONE;
        if (ex->path_dead) issue = DP_ISSUE_DEAD;
        else if (ex->path_reset) issue = DP_ISSUE_RESET;
        else if (ex->path_nonsense) issue = DP_ISSUE_NONSENSE;
        else if (cut == DP_CUT_HALT || ex->steps_no_credit > ex->cfg.max_steps_path / 4)
            issue = DP_ISSUE_MERGED; // a known loop, no way out found
        else if (cut == DP_CUT_CYCLES) issue = DP_ISSUE_CAPPED;
        else if (cut != DP_CUT_NONE) issue = DP_ISSUE_BUDGET;
        tree_close_g(ex, choice, issue);
    }
    return (ex->q_len || ex->pq_len || ex->sh || frontier_has_work(ex) || dp_explorer_spool_pending(ex)) ? 1 : 0;
}

int dp_explore_slice(DpExplorer *ex, int ms) {
    uint64_t deadline = (uint64_t)ms * 1000 + (uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC;
    int more = 1;
    while (more) {
        more = dp_explore_step(ex);
        if (ex->stop) return 0;
        if ((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC >= deadline) break;
    }
    return more;
}

void dp_explore_finalize(DpExplorer *ex) {
    DpResults *res = ex->res;
    // finalize may run once per closure round: the derived counters restart
    for (int i = 0; i < res->section_count; i++) {
        res->sections[i].cond_sites = res->sections[i].cond_both = 0;
        res->sections[i].callers_n = 0;
    }
    for (size_t i = 0; i < res->sites.cap; i++) {
        if (!res->sites.used[i]) continue;
        uint32_t key = res->sites.keys[i];
        uint32_t sid1 = key < (uint32_t)res->banks * 0x4000 ? res->section_of[key] : 0;
        if (!sid1) continue;
        DpSection *s = &res->sections[sid1 - 1];
        DpSiteStat *st = (DpSiteStat *)((uint8_t *)res->sites.vals + i * res->sites.stride);
        s->cond_sites++;
        if (st->taken && st->nottaken) s->cond_both++;
    }
    for (size_t i = 0; i < res->callers.cap; i++) {
        if (!res->callers.used[i]) continue;
        uint32_t key = res->callers.keys[i];
        uint32_t sid1 = key < (uint32_t)res->banks * 0x4000 ? res->section_of[key] : 0;
        if (!sid1) continue;
        DpSection *s = &res->sections[sid1 - 1];
        DpCallList *cl = (DpCallList *)((uint8_t *)res->callers.vals + i * res->callers.stride);
        for (int j = 0; j < cl->n && s->callers_n < 8; j++) s->callers[s->callers_n++] = cl->site[j];
    }
    for (int i = 0; i < res->section_count; i++) {
        DpSection *s = &res->sections[i];
        if (s->boot_hits > 0 || (s->kind_bits & DP_XF_IRQ) || s->copies > 0)
            s->verdict = DP_V_REQUIRED;
        else if (s->natural_hits > 0 && s->forced_hits == 0)
            s->verdict = DP_V_PLAYED; // only ever seen in a real play session
        else
            s->verdict = DP_V_FORCED;
    }
    // the m0boy bridge: count bytes that are both executed and proven data —
    // execution wins there, the data mark is dropped at export
    {
        uint32_t contra = 0;
        uint64_t verb = 0;
        size_t n = (size_t)res->banks * 0x4000;
        for (size_t k = 0; k < n; k++) {
            if (res->data_flags[k] & DP_DF_VERBATIM) {
                verb++;
                if (res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED)) contra++;
            }
        }
        res->data_verbatim_bytes = verb;
        res->data_contradictions = contra;
    }
}

// one parallel drain worker: same slice loop, pops from the shared pool
static void *drain_thread(void *arg) {
    DpExplorer *w = arg;
    while (dp_explore_slice(w, 50))
        ;
    return NULL;
}

int dp_explore(DpExplorer *ex) {
    if (ex->cfg.jobs <= 1 || ex->sh) {
        while (dp_explore_slice(ex, 50))
            ;
        dp_explore_finalize(ex);
        return 0;
    }
    // parallel drain: N explorers (one machine each) share one work pool,
    // exact dedup, spool and tree.  The pool and the spool ARE the frontier:
    // a full RAM pool spills to disk, nothing is dropped while disk lasts.
    DpSharedPool sh;
    memset(&sh, 0, sizeof sh);
    sh.cap = 1024;
    sh.max = ex->q_max;
    sh.q = xrealloc(NULL, sizeof(DpPathState) * sh.cap);
    pthread_mutex_init(&sh.mtx, NULL);
    ex->sh = &sh;
    ex->res->mt = 1; // the record lists are shared from now on: take the mutexes
    // the seeds queued before the drain sit in the leader's local worklist:
    // hand them to the pool, the workers cannot see the local ring
    for (uint32_t i = 0; i < ex->q_len; i++) {
        DpPathState *p = &ex->queue[(ex->q_head + i) % ex->q_cap];
        if (!p->img) continue;
        pthread_mutex_lock(&sh.mtx);
        if (sh.len >= sh.cap) {
            sh.q = xrealloc(sh.q, sizeof(DpPathState) * (size_t)sh.cap * 2);
            sh.cap *= 2;
        }
        sh.q[(sh.head + sh.len) % sh.cap] = *p;
        sh.len++;
        pthread_mutex_unlock(&sh.mtx);
        p->img = NULL;
    }
    ex->q_len = 0;
    ex->q_head = 0;
    // one worker per free machine instance (the sessions and the preview keep
    // theirs): fewer free slots, fewer cores — the cap is DP_GBL_MACHINES
    int n = ex->cfg.jobs - 1;
    if (n > DP_GBL_MACHINES - 1) n = DP_GBL_MACHINES - 1;
    DpExplorer **w = calloc((size_t)n, sizeof *w);
    pthread_t *th = calloc((size_t)n, sizeof *th);
    int nw = 0;
    for (int i = 0; i < n; i++) {
        DpMach *m = dp_mach_open_exclusive(ex->mach->vi, i + 1);
        if (!m) continue; // the slot is taken (sessions, preview)
        dp_mach_set_rom(m, ex->res->cart->rom, ex->res->cart->rom_size);
        w[nw] = dp_explorer_new(&ex->cfg, ex->res, m);
        w[nw]->tree = ex->tree;
        dp_explorer_share(w[nw], ex);
        pthread_create(&th[nw], NULL, drain_thread, w[nw]);
        nw++;
    }
    if (!nw) fprintf(stderr, "deepeel: --jobs: no free machine instance, single core\n");
    while (dp_explore_slice(ex, 50))
        ;
    for (int i = 0; i < nw; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < nw; i++) {
        DpMach *m = w[i]->mach;
        // states a worker still held aside move to the leader (never silently freed)
        for (uint32_t k = 0; k < w[i]->defer_len; k++) {
            DpDeferred *d = &w[i]->defer[k];
            if (ex->defer_len < DP_DEFER_CAP) {
                if (ex->defer_len >= ex->defer_cap) {
                    uint32_t nc = ex->defer_cap ? ex->defer_cap * 2 : 64;
                    ex->defer = xrealloc(ex->defer, sizeof *ex->defer * nc);
                    ex->defer_cap = nc;
                }
                ex->defer[ex->defer_len++] = *d;
                ex->defer_by_reason[d->reason]++;
            } else {
                ex->res->defer_final[d->reason]++;
                ex->res->defer_dropped++;
                free(d->p.img);
            }
        }
        w[i]->defer_len = 0;
        dp_explorer_free(w[i]);
        dp_mach_close(m);
    }
    free(w);
    free(th);
    ex->sh = NULL;
    pthread_mutex_destroy(&sh.mtx);
    free(sh.q);
    dp_explore_finalize(ex);
    return 0;
}

// --- GB seeding ------------------------------------------------------------------------
// Mutually exclusive per-byte classification, first match wins.
void dp_results_breakdown(const DpResults *res, uint64_t out[DP_CLS_COUNT]) {
    size_t n = (size_t)res->banks * 0x4000;
    for (int i = 0; i < DP_CLS_COUNT; i++) out[i] = 0;
    for (size_t k = 0; k < n; k++) {
        uint8_t x = res->exec_flags[k], d = res->data_flags[k], s = res->static_flags[k];
        int c;
        if (x & DP_XF_NATURAL) c = DP_CLS_EXEC_NATURAL;
        else if (x & DP_XF_FORCED) c = DP_CLS_EXEC_FORCED;
        else if (x & DP_XF_STATIC) c = DP_CLS_STATIC_FLOW;
        else if (d & (DP_DF_COPIED | DP_DF_VERBATIM)) c = DP_CLS_DATA_COPIED;
        else if (d & DP_DF_READ) c = DP_CLS_DATA_READ;
        else if (s & (DP_SF_POINTER | DP_SF_PAD)) c = DP_CLS_POINTER_PAD;
        else c = DP_CLS_UNEXPLAINED;
        out[c]++;
    }
}

uint64_t dp_results_unexplained(const DpResults *res) {
    uint64_t b[DP_CLS_COUNT];
    dp_results_breakdown(res, b);
    return b[DP_CLS_UNEXPLAINED];
}

static int is_padding_at(const DpCart *c, uint32_t bank, uint16_t addr) {
    uint32_t base = bank * 0x4000ull + (addr & 0x3FFF);
    uint8_t b0 = base < c->rom_size ? c->rom[base] : 0xFF;
    if (b0 != 0x00 && b0 != 0xFF) return 0;
    for (int i = 1; i < 8; i++) {
        uint32_t a = base + (uint32_t)i;
        uint8_t b = a < c->rom_size ? c->rom[a] : 0xFF;
        if (b != b0) return 0;
    }
    return 1;
}

static int is_illegal_op(uint8_t op) {
    switch (op) {
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4:
    case 0xEB: case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
        return 1;
    default:
        return 0;
    }
}

void dp_seed_gb(DpExplorer *ex) {
    DpResults *res = ex->res;
    const DpCart *c = res->cart;
    const DpPlatform *plat = res->plat;
    // reset entry
    {
        uint8_t *img = xrealloc(NULL, ex->sb);
        memcpy(img, seed_base(ex), ex->sb);
        dp_explorer_seed(ex, img, DP_SEED_RESET, 0);
    }
    // RST and IRQ vectors when they hold something that is not padding.  Both
    // are entered cold, from a synthetic state no natural flow produced: the
    // code they reach is forced until the reset flow (or a session) gets
    // there for real
    for (int i = 0; i < plat->entry_count; i++) {
        const DpEntry *e = &plat->entries[i];
        if (e->kind == DP_SEED_RESET) continue;
        if (is_padding_at(c, 0, e->addr)) continue;
        dp_explorer_seed_pc(ex, 0, e->addr, e->kind, 1);
    }
    // pointer-table scan: every ROM word pointing at plausible code; targets
    // inside padding or pointer tables are evicted (static view — the
    // dynamic data proof only exists once coverage ran, see --sweep).
    // A word >= $4000 carries no bank evidence: every switchable bank's view
    // of the target is seeded (bounded by the seed cap, dedup by rom key).
    if (!ex->cfg.no_pointer_scan) {
        int seeded = 0;
        DpHashSet seen;
        dp_set_init(&seen, 65536);
        for (int bank = 0; bank < res->banks; bank++) {
            for (int off = 0; off + 1 < 0x4000; off++) {
                uint32_t a = (uint32_t)bank * 0x4000 + (uint32_t)off;
                if (a + 1 >= c->rom_size) break;
                uint16_t v = (uint16_t)(c->rom[a] | (c->rom[a + 1] << 8));
                if (v < 0x0100 || v >= 0x8000) continue;
                // a word flanked by two other $4000-window words is a
                // dispatch-table entry: its true bank is unknown, so every
                // switchable bank's view is seeded.  An isolated word is a
                // plain pointer: bank 1, as before.
                uint16_t pv = a >= 2 ? (uint16_t)(c->rom[a - 2] | (c->rom[a - 1] << 8)) : 0;
                uint16_t nv = a + 3 < c->rom_size
                                  ? (uint16_t)(c->rom[a + 2] | (c->rom[a + 3] << 8))
                                  : 0;
                int tab = pv >= 0x4000 && pv < 0x8000 && nv >= 0x4000 && nv < 0x8000;
                int nb = v < 0x4000 ? 1 : (tab ? res->banks : 2);
                for (int tbank = v < 0x4000 ? 0 : 1; tbank < nb; tbank++) {
                    if (is_padding_at(c, (uint32_t)tbank, v)) continue;
                    uint32_t tk = dp_rom_index(tbank, v);
                    if (res->static_flags[tk] & (DP_SF_PAD | DP_SF_POINTER)) {
                        res->seeds_skipped_data++;
                        continue;
                    }
                    if (dp_set_insert(&seen, tk) == 0) continue;
                    if (seeded >= (int)ex->cfg.seed_cap) { // the cap holds the seed back, never drops it
                        pseed_add(ex, tbank, v, DP_SEED_POINTER, 1);
                        continue;
                    }
                    dp_explorer_seed_pc(ex, tbank, v, DP_SEED_POINTER, 1);
                    seeded++;
                }
            }
        }
        dp_set_free(&seen);
    }
    // bank sweep seeding: every switchable bank entered at $4000
    if (ex->cfg.seed_banks) {
        for (int bank = 1; bank < res->banks; bank++) {
            if (is_padding_at(c, (uint32_t)bank, 0x4000)) continue;
            dp_explorer_seed_pc(ex, bank, 0x4000, DP_SEED_POINTER, 1);
        }
    }
}

// the JP HL sites recorded the targets the dispatch actually took; some of
// those continuations were dropped by a budget wall, so seed every target
// whose rom key carries no execution evidence yet (bounded: sites x 8, seed cap)
int dp_seed_indirect(DpExplorer *ex, uint32_t cap) {
    DpResults *res = ex->res;
    int seeded = 0;
    for (size_t i = 0; i < res->indirect.cap; i++) {
        if (!res->indirect.used[i]) continue;
        const DpIndirect *ind = (const DpIndirect *)(res->indirect.vals + i * res->indirect.stride);
        for (int t = 0; t < ind->n; t++) {
            uint32_t k = dp_rom_index(ind->tbank[t], ind->target[t]);
            if (res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED)) continue;
            if (res->static_flags[k] & (DP_SF_PAD | DP_SF_POINTER)) continue;
            if (seeded >= (int)cap) { // the phase's solde is spent: counted, held for later
                res->seeds_refused_budget++;
                pseed_add(ex, ind->tbank[t], ind->target[t], DP_SEED_POINTER, 1);
                continue;
            }
            if (!dp_explorer_seed_pc_pri(ex, ind->tbank[t], ind->target[t], DP_SEED_POINTER, 1, 1))
                continue;
            seeded++;
            res->indirect_seeds++;
        }
    }
    return seeded;
}
