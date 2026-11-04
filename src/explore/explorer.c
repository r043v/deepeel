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

#include <signal.h>
volatile sig_atomic_t dp_explore_interrupt;
#include <sys/stat.h>
#include "explorer.h"
#include "../core/dpmachine.h"
#include "tree.h"
#include "explorer_priv.h"
#include <time.h>
#include <unistd.h>

#include <stddef.h>


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

void *xrealloc(void *p, size_t n) {
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
    free(res->rt);
    free(res->rt_pcseen);
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
static void probe_site(DpResults *res, int which, uint16_t pc);
static void hook_read(DpMach *m, uint16_t a, uint8_t v);
// count a probe once per code site (cap 16 sites per probe, parallel-safe)
static void probe_site(DpResults *res, int which, uint16_t pc) {
    if (res->mt) pthread_mutex_lock(&res->rec_mtx);
    uint32_t *cnt = which == 1 ? &res->rtc_selects : which == 2 ? &res->div_reads
                  : which == 3 ? &res->stat_reads : which == 4 ? &res->ly_reads : &res->serial_starts;
    uint32_t i;
    for (i = 0; i < *cnt; i++)
        if (res->probe_pc[which][i] == pc) break;
    if (i == *cnt && *cnt < 16) res->probe_pc[which][(*cnt)++] = pc;
    if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
}

static void hook_write(DpMach *m, uint16_t a, uint8_t v);

DpExplorer *dp_explorer_new(const DpConfig *cfg, DpResults *res, DpMach *mach) {
    DpExplorer *ex = calloc(1, sizeof *ex);
    if (ex) ex->sgb_bit = -1;
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
    stream_free(ex);
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
uint16_t cur_bank(DpExplorer *ex, uint16_t pc) {
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
uint16_t image_pc(const uint8_t *img) {
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
static void tree_close_g(DpExplorer *ex, uint32_t choice, int issue);
static uint32_t tree_add_g(DpExplorer *ex, uint32_t parent, int kind, uint32_t site, int bank,
                           int pc);
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
                // ring order must survive the growth: rebase the head to 0 (a plain
                // realloc scrambles a wrapped ring and leaves the new slots unzeroed)
                DpPathState *nq = xrealloc(NULL, sizeof(DpPathState) * (size_t)ncap);
                for (uint32_t i = 0; i < sh->len; i++) nq[i] = sh->q[(sh->head + i) % sh->cap];
                memset(nq + sh->len, 0, sizeof(DpPathState) * (size_t)(ncap - sh->len));
                free(sh->q);
                sh->q = nq;
                sh->head = 0;
                sh->cap = ncap;
                u32_add(&ex->res->queue_growths, 1);
            } else {
                // the pool mutex is released before the spool write: zlib and the disk
                // write must not serialize every worker behind the pool
                pthread_mutex_unlock(&sh->mtx);
                if (spool_write_rec(ex, img, forced, kind, from_reset, choice)) {
                    tree_close_g(ex, choice, DP_ISSUE_SPOOLED);
                    free(img);
                    return 0;
                }
                u32_add(&ex->res->queue_overflows, 1);
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

int queue_push(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                      uint32_t choice) {
    return queue_push_pri(ex, img, forced, kind, from_reset, choice, 0);
}

// a tree choice for a new queued path (when the tree is tracked)
uint32_t choice_new(DpExplorer *ex, uint32_t parent, int kind, uint32_t site, int bank,
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
uint8_t *seed_base(DpExplorer *ex) {
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

// dpIx: record a ROM data read per reading site. Lock-free: slots are claimed by CAS on site1, the range
// is widened by CAS loops, the count is atomic. A full table is counted (ix_full), never silent.
static void ix_record(DpResults *res, uint32_t site, uint32_t key) {
    uint32_t h = (site * 2654435761u) % DP_IX_CAP;
    for (uint32_t probe = 0; probe < 32; probe++, h = (h + 1) % DP_IX_CAP) {
        uint32_t cur = __atomic_load_n(&res->ix[h].site1, __ATOMIC_ACQUIRE);
        if (!cur) {
            uint32_t zero = 0;
            if (__atomic_compare_exchange_n(&res->ix[h].site1, &zero, site + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                uint32_t mn = 0;   // first claimant seeds the range; later ones only ever narrow/widen it
                __atomic_compare_exchange_n(&res->ix[h].kmin, &mn, key + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
                cur = site + 1;
            } else cur = zero;
        }
        if (cur != site + 1) continue;
        uint32_t lo = __atomic_load_n(&res->ix[h].kmin, __ATOMIC_RELAXED);
        while ((!lo || key + 1 < lo) && !__atomic_compare_exchange_n(&res->ix[h].kmin, &lo, key + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        uint32_t hi = __atomic_load_n(&res->ix[h].kmax, __ATOMIC_RELAXED);
        while (key + 1 > hi && !__atomic_compare_exchange_n(&res->ix[h].kmax, &hi, key + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        __atomic_fetch_add(&res->ix[h].n, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&res->ix_full, 1, __ATOMIC_RELAXED);
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
        ix_record(res, key_at(ex, m->api->gbr->pc), key);
        if (ex->session) hits_add(res, &res->play_data_hits[key]);
        dp_copy_on_rom_read(ex, (uint8_t)cur_bank(ex, a), a, v);
        ex->last_rom_key = key; ex->last_rom_val = v; ex->last_rom_valid = 1;
        if (!ex->session) stream_read(ex, a);
    }
    section_live_access(ex, a);
    if (a == 0xFF04 || a == 0xFF41 || a == 0xFF44)
        probe_site(res, a == 0xFF04 ? 2 : a == 0xFF41 ? 3 : 4, m->api->gbr->pc);
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

// Super Game Boy command packets travel on P1 ($FF00): a 00 pulse starts a packet, then 128 bits
// LSB first (10 = 1, 20 = 0, each followed by 30), then a stop bit. Observation only: no SGB
// border or palette is rendered.
static void sgb_feed(DpExplorer *ex, DpResults *res, uint8_t v) {
    v &= 0x30;
    if (v == 0x00) { ex->sgb_bit = 0; memset(ex->sgb_buf, 0, sizeof ex->sgb_buf); return; }
    if (ex->sgb_bit < 0 || v == 0x30) return;
    if (ex->sgb_bit == 128) {   // the stop bit closes the packet
        __atomic_fetch_add(&res->sgb_packets, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&res->sgb_cmd[ex->sgb_buf[0] >> 3 & 31], 1, __ATOMIC_RELAXED);
        ex->sgb_bit = -1;
        return;
    }
    if (v == 0x10) ex->sgb_buf[ex->sgb_bit >> 3] |= (uint8_t)(1u << (ex->sgb_bit & 7));
    ex->sgb_bit++;
}

static void hook_write(DpMach *m, uint16_t a, uint8_t v) {
    DpExplorer *ex = m->owner;
    DpResults *res = ex->res;
    if (a == 0xFF00) sgb_feed(ex, res, v);
    if (a < 0x8000) { // mapper write: the bank flip resolves on the next step
        if (a >= 0x4000 && a < 0x6000 && v >= 8 && v <= 0x0C)
            probe_site(res, 1, m->api->gbr->pc);
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
    if (a < 0xE000) {
        ex->wram_taint[a - 0xC000] = ex->last_rom_valid ? ex->last_rom_key + 1 : 0;
        if (ex->wram_exec[a - 0xC000]) {   // a byte that ran is rewritten: a code version change
            ex->wram_exec[a - 0xC000] = 0;
            if (!__atomic_fetch_add(&res->smc_writes, 1, __ATOMIC_RELAXED))
                __atomic_store_n(&res->smc_first_pc, m->api->gbr->pc, __ATOMIC_RELAXED);
        }
        dp_copy_on_wram_write(ex, a, v);
        return;
    }   // WRAM: copy destination (code run from WRAM)
    if (a < 0xFE00) return; // echo: no per-byte map
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
        if (ex->last_rom_valid && ex->last_rom_val == v && ((a >= 0xFF30 && a <= 0xFF3F) ||
                                                           (a >= 0xFF10 && a <= 0xFF25)))
            flags_or(&res->data_flags[ex->last_rom_key], a >= 0xFF30 ? DP_DF_SAMPLE : DP_DF_MUSIC);
        if (a == 0xFF02 && (v & 0x80)) probe_site(res, 0, m->api->gbr->pc);
        if (ex->last_rom_valid && ex->last_rom_val == v && (a == 0xFF69 || a == 0xFF6B))
            flags_or(&res->data_flags[ex->last_rom_key], DP_DF_PALETTE);
        if (a >= 0xFF51 && a <= 0xFF54) ex->hdma[a - 0xFF51] = v;
        if (a == 0xFF55) {
            // CGB HDMA: source/destination come from HDMA1-4, length from the
            // written value (bit 7 selects H-blank mode: same source range)
            unsigned src = (((unsigned)ex->hdma[0] << 8) | ex->hdma[1]) & 0xFFF0u;
            unsigned dst = 0x8000u | ((((unsigned)ex->hdma[2] << 8) | ex->hdma[3]) & 0x1FF0u);
            unsigned len = ((v & 0x7Fu) + 1u) * 16u;
            if (src < 0x8000u) {
                DpCopyRec rec;
                memset(&rec, 0, sizeof rec);
                rec.channel = 2;
                rec.verbatim = 1;
                rec.src_space = DP_SPACE_ROM;
                rec.dst_space = DP_SPACE_VRAM;
                rec.src_bank = src >= 0x4000 ? cur_bank(ex, 0x4000) : 0;
                // one record per bank window: a transfer crossing it is clipped
                unsigned room = 0x4000u - (src & 0x3FFFu);
                unsigned n = len < room ? len : room;
                rec.src_lo = (uint16_t)src;
                rec.src_hi = (uint16_t)(src + n - 1);
                rec.dst_lo = (uint16_t)dst;
                rec.dst_hi = (uint16_t)(dst + n - 1);
                rec.bytes = n;
                rec.pc = m->api->gbr->pc;
                dp_record_copy(ex, &rec, 0);
            }
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

// ROM index whose byte a verbatim copy loop wrote at WRAM address pc, or -1.
// The newest matching record wins; the answer is cached until a new record arrives.
static int32_t wram_rom_source(DpExplorer *ex, uint16_t pc) {
    if (pc < 0xC000 || pc >= 0xE000) return -1;
    DpResults *res = ex->res;
    uint32_t gen = (uint32_t)res->copy_count + 1;
    if (ex->wram_src[pc - 0xC000].gen == gen) return ex->wram_src[pc - 0xC000].rk;
    int32_t rk = -1;
    dp_copy_flush_for_exec(ex, pc);
    gen = (uint32_t)res->copy_count + 1;
    if (res->mt) pthread_mutex_lock(&res->rec_mtx);
    for (int i = res->copy_count - 1; i >= 0; i--) {
        const DpCopyRec *c = &res->copies[i];
        if (c->channel != 0 || !c->verbatim || c->src_space != DP_SPACE_ROM) continue;
        if (pc < c->dst_lo || pc > c->dst_hi) continue;
        uint32_t sa = (uint32_t)c->src_lo + (uint32_t)(pc - c->dst_lo);
        if (sa > c->src_hi || sa >= 0x8000) continue;
        uint32_t k = dp_rom_index(sa < 0x4000 ? 0 : c->src_bank, (uint16_t)sa);
        if (k < (uint32_t)res->banks * 0x4000) { rk = (int32_t)k; break; }
    }
    if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
    ex->wram_src[pc - 0xC000].gen = gen;
    ex->wram_src[pc - 0xC000].rk = rk;
    return rk;
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

// Every state a budget would have thrown away is kept here instead: a path
// cut by its step budget, a fork past the per-site cap, a full dedup set, ...
// The policy (tiers, caps, final losses) is in frontier.c.
int defer_room(DpExplorer *ex, int reason) {
    DpResults *res = ex->res;
    if (ex->cfg.unbounded) return 1;   // never refused: defer_push parks it in memory or on the disk spool
    switch (dp_frontier_room(ex->cur_tier, ex->defer_len, ex->defer_by_reason[reason], reason)) {
    case DP_ROOM_TIER_FINAL: res->defer_final[reason]++; return 0;
    case DP_ROOM_FULL: res->defer_final[reason]++; res->defer_dropped++; return 0;
    default: return 1;
    }
}

// takes ownership of img on success; 0 = counted final loss (img untouched)
int defer_push(DpExplorer *ex, uint8_t *img, int forced, int kind, int from_reset,
                      uint32_t choice, int reason) {
    if (!defer_room(ex, reason)) return 0;
    if (ex->cfg.unbounded && ex->spool &&
        (ex->cur_tier >= DP_TIER_MAX || ex->defer_len >= DP_DEFER_CAP ||
         ex->defer_by_reason[reason] >= DP_DEFER_CAP)) {
        // holding area or tier ladder exhausted: the state waits on the disk spool
        // (it resumes at the base budget; it has still advanced, so no loop)
        if (spool_write_rec(ex, img, forced, kind, from_reset, choice)) {
            ex->res->defer_by[reason]++;
            free(img);
            return 1;
        }
    }
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
void fork_deferred(DpExplorer *ex, uint8_t *fimg, int reason, uint32_t site, uint16_t tpc) {
    if (!defer_room(ex, reason)) { free(fimg); return; }
    uint32_t choice = choice_new(ex, ex->cur_choice, DP_PATH_FORK, site, (int)cur_bank(ex, tpc), tpc);
    if (ex->tree) dp_tree_close(ex->tree, choice, DP_ISSUE_SPOOLED);
    defer_push(ex, fimg, 1, DP_PATH_FORK, ex->cur_kind == DP_SEED_RESET || ex->cur_from_reset, choice, reason);
}

// a seed the caps held back; the list is bounded and its overflow counted
void pseed_add(DpExplorer *ex, int bank, uint16_t pc, int kind, int forced) {
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
        int cond = dp_isa_sm83()->cond_of(op);
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
        if (!ex->cfg.no_event_fork && (op == 0xF0 || op == 0xF2 || op == 0xFA))
            fork_event(ex, pk, pc, op);
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
    if (!ex->session) stream_step(ex);
    if (op == 0xCD && !ex->session)
        stream_call(ex, dp_rom_index(target < 0x4000 ? 0 : (uint32_t)cur_bank(ex, target), target));
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
            // executing VRAM/WRAM/HRAM code: counted, and when a verbatim copy loop
            // put these bytes in WRAM from the ROM, the ROM source is executed code
            u32_add(&res->exec_outside, 1);
            int32_t rk = wram_rom_source(ex, pc);
            {   // archive the executed bytes themselves (generated code has no ROM source)
                int ilen = dp_isa_sm83()->length(op);
                uint8_t ib[4];
                const DpGblApi *ga = ex->mach->api;
                for (int i = 0; i < ilen && i < 4; i++) {
                    uint16_t a = (uint16_t)(pc + i);
                    if (a >= 0xE000 && a < 0xFE00) a -= 0x2000;
                    ib[i] = a >= 0xC000 && a < 0xE000 ? ga->wram[a - 0xC000]
                          : a >= 0x8000 && a < 0xA000 ? ga->vram[a - 0x8000]
                          : a >= 0xFF80 && a < 0xFFFF ? ga->hram[a - 0xFF80] : 0xFF;
                }
                int32_t dv = -1;
                if (pc >= 0xC000 && pc < 0xE000 && ex->wram_taint[pc - 0xC000]) dv = (int32_t)(ex->wram_taint[pc - 0xC000] - 1);
                dp_rtcode_add(res, pc, ib, ilen, rk, dv, !ex->cur_forced);
                if (pc >= 0xC000 && pc < 0xE000)
                    for (int i = 0; i < ilen && pc + i < 0xE000; i++) ex->wram_exec[pc + i - 0xC000] = 1;
            }
            if (rk >= 0) {
                uint8_t nf = (uint8_t)(ex->cur_forced ? DP_XF_FORCED : DP_XF_NATURAL);
                uint32_t xs = ex->session ? DP_SRC_SESSION : (ex->cur_forced ? DP_SRC_FORK_FORCED : DP_SRC_FORK_NATURAL);
                uint32_t nbytes = (uint32_t)res->banks * 0x4000;
                int len = dp_isa_sm83()->length(op);
                for (int i = 0; i < len && (uint32_t)rk + (uint32_t)i < nbytes; i++) {
                    uint32_t k = (uint32_t)rk + (uint32_t)i;
                    uint8_t o2 = flags_or(&res->exec_flags[k], i ? nf : (uint8_t)(nf | DP_XF_START));
                    src_or(res, k, xs);
                    if (!(o2 & nf)) ex->credit_exec++;
                }
                u32_add(&res->exec_wram_rom, 1);
            }
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
            int len = dp_isa_sm83()->length(op);
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
    ex->last_len = (uint8_t)dp_isa_sm83()->length(op);
    ex->have_last = 1;
    ex->prev_was_cond = (uint8_t)(dp_isa_sm83()->cond_of(op) >= 0 && !ex->session);
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

// --- run loop ---------------------------------------------------------------------------

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

static int explore_step_inner(DpExplorer *ex);
// In a parallel drain a momentarily empty pool is not the end: another thread may be
// mid-path and about to push, or the spool may still hold records (a batch that dedup
// rejected entirely leaves the pool empty). The drain ends only when every thread is idle,
// the pool is empty and nothing is left on disk.
int dp_explore_step(DpExplorer *ex) {
    int r = explore_step_inner(ex);
    if (r || !ex->sh) return r;
    DpSharedPool *sh = ex->sh;
    pthread_mutex_lock(&sh->mtx);
    sh->idle++;
    pthread_mutex_unlock(&sh->mtx);
    for (;;) {
        pthread_mutex_lock(&sh->mtx);
        int work = sh->len != 0;
        int all_idle = sh->idle >= sh->nthreads;
        pthread_mutex_unlock(&sh->mtx);
        if (ex->stop || dp_explore_interrupt) { r = 0; break; }
        if (work || dp_explorer_spool_pending(ex)) { r = 1; break; }
        if (all_idle) { r = 0; break; }
        usleep(1000);
    }
    if (r) { // a thread that ends stays counted idle, or its siblings would wait for it forever
        pthread_mutex_lock(&sh->mtx);
        sh->idle--;
        pthread_mutex_unlock(&sh->mtx);
    }
    return r;
}

static int explore_step_inner(DpExplorer *ex) {
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
    stream_poll(ex);
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
        if (!p->img && !defer_pop(ex, &tmp)) return stream_replay(ex) ? 1 : 0;
    } else {
        if (!ex->q_len && !ex->pq_len && !defer_pop(ex, &tmp)) {
            // nothing resumable: held seeds come back before the run ends
            if (pseed_feed(ex) && (ex->q_len || ex->pq_len)) return 1;
            if (stream_replay(ex)) return 1;
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
    memset(ex->wram_taint, 0, sizeof ex->wram_taint); // producer tracking restarts per path
    dp_mach_load(ex->mach, img); // execution starts at the image's pc
    free(img);
    if (p != &tmp && !from_pq && ex->sh) { // trap: a NULL written into a live ring slot?
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
    if (ex->q_len || ex->pq_len || ex->sh || frontier_has_work(ex) || dp_explorer_spool_pending(ex)) return 1;
    // the last path closed: decoders seen with several starts replay over their tables
    return stream_replay(ex) ? 1 : 0;
}

int dp_explore_slice(DpExplorer *ex, int ms) {
    uint64_t deadline = (uint64_t)ms * 1000 + (uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC;
    int more = 1;
    while (more) {
        more = dp_explore_step(ex);
        if (ex->stop || dp_explore_interrupt) return 0;
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
        uint64_t verb = 0, smp = 0, mus = 0, pal = 0;
        size_t n = (size_t)res->banks * 0x4000;
        for (size_t k = 0; k < n; k++) {
            smp += (res->data_flags[k] & DP_DF_SAMPLE) != 0;
            mus += (res->data_flags[k] & DP_DF_MUSIC) != 0;
            pal += (res->data_flags[k] & DP_DF_PALETTE) != 0;
            if (res->data_flags[k] & DP_DF_VERBATIM) {
                verb++;
                if (res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED)) contra++;
            }
        }
        res->data_verbatim_bytes = verb;
        res->apu_sample_bytes = smp;
        res->apu_music_bytes = mus;
        res->palette_bytes = pal;
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
    sh.nthreads = 1; // the leader; the workers add themselves below
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
        sh.nthreads++;
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

