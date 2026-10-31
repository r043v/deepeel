// explorer.c: the forking worklist engine.
//
// Every conditional branch forks (both outcomes are always tested), every
// pending IRQ at an interrupt-enabled point forks a dispatch.  Dedup evicts
// exact states and "same context" section re-entries; budgets and backward
// edge caps keep loops finite.  Everything the machine does is recorded into
// the results maps through the trace hooks.
//
// The same recording runs inside the play-session workers (one MsExplorer
// each, sharing MsResults): their per-byte updates go through __atomic
// builtins, and their section/record updates take the results mutexes while
// res->mt is set.  The fork explorer itself only runs while the sessions are
// paused, so its updates stay lock-free.
#include "explorer.h"
#include "../cpu/sm83/sm83.h"
#include "../cpu/sm83/sm83_cycles.h"
#include <time.h>

static int is_padding_at(const MsCart *c, uint32_t bank, uint16_t addr);

// --- shared-map primitives -----------------------------------------------------
static inline uint8_t flags_or(uint8_t *p, uint8_t bit) {
    return __atomic_or_fetch(p, bit, __ATOMIC_RELAXED); // returns the old bits
}
static inline void hits_add(uint32_t *p, uint32_t v) { __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
static inline void u32_add(uint32_t *p, uint32_t v) { __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
static inline void u64_add(uint64_t *p, uint64_t v) { __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
static inline void u64_or(uint64_t *p, uint64_t v) { __atomic_fetch_or(p, v, __ATOMIC_RELAXED); }
static inline uint64_t u64_get(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

// --- u64 set ------------------------------------------------------------------
static size_t set_probe(const MsHashSet *s, uint64_t key) {
    size_t i = (size_t)(key * 0x9E3779B97F4A7C15ull) & (s->cap - 1);
    while (s->used[i]) {
        if (s->keys[i] == key) return i;
        i = (i + 1) & (s->cap - 1);
    }
    return i;
}

static int set_grow(MsHashSet *s) {
    if (s->cap >= s->cap_abs) return 0;
    size_t ncap = s->cap * 2;
    uint64_t *nk = calloc(ncap, sizeof(uint64_t));
    uint8_t *nu = calloc(ncap, 1);
    if (!nk || !nu) { free(nk); free(nu); return 0; }
    for (size_t i = 0; i < s->cap; i++) {
        if (!s->used[i]) continue;
        size_t j = (size_t)(s->keys[i] * 0x9E3779B97F4A7C15ull) & (ncap - 1);
        while (nu[j]) j = (j + 1) & (ncap - 1);
        nk[j] = s->keys[i];
        nu[j] = 1;
    }
    free(s->keys);
    free(s->used);
    s->keys = nk;
    s->used = nu;
    s->cap = ncap;
    return 1;
}

void ms_set_init(MsHashSet *s, size_t cap_abs) {
    s->cap_abs = cap_abs ? cap_abs : 1 << 20;
    s->cap = 1024;
    s->keys = calloc(s->cap, sizeof(uint64_t));
    s->used = calloc(s->cap, 1);
    s->cnt = 0;
}

void ms_set_free(MsHashSet *s) {
    free(s->keys);
    free(s->used);
    s->keys = NULL;
    s->used = NULL;
    s->cap = s->cnt = s->cap_abs = 0;
}

int ms_set_insert(MsHashSet *s, uint64_t key) {
    if (s->cnt >= s->cap * 3 / 4) {
        if (!set_grow(s)) return -1;
    }
    size_t i = set_probe(s, key);
    if (s->used[i]) return 0;
    s->keys[i] = key;
    s->used[i] = 1;
    s->cnt++;
    return 1;
}

// --- u32 -> payload map ---------------------------------------------------------
static void *map_find(MsMap32 *m, uint32_t key) {
    size_t i = (size_t)(key * 0x9E3779B97F4A7C15ull) & (m->cap - 1);
    while (m->used[i]) {
        if (m->keys[i] == key) return m->vals + i * m->stride;
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}

static int map_grow(MsMap32 *m) {
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

void ms_map_init(MsMap32 *m, size_t stride, size_t cap_abs) {
    m->stride = stride;
    m->cap_abs = cap_abs ? cap_abs : 1 << 20;
    m->cap = 1024;
    m->keys = calloc(m->cap, sizeof(uint32_t));
    m->used = calloc(m->cap, 1);
    m->vals = calloc(m->cap, stride);
    m->cnt = 0;
}

void ms_map_free(MsMap32 *m) {
    free(m->keys);
    free(m->used);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

void *ms_map_get_or_add(MsMap32 *m, uint32_t key) {
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

void *ms_map_find(MsMap32 *m, uint32_t key) {
    return map_find(m, key);
}

// --- config / results ------------------------------------------------------------
void ms_config_defaults(MsConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->max_steps_path = 100000;
    cfg->max_paths = 200000;
    cfg->max_steps_total = 20000000ull;
    cfg->queue_cap = 4096;
    cfg->loop_back_cap = 4096;
    cfg->fork_site_cap = 16;
    cfg->seed_cap = 4096;
    cfg->max_copies = 65536;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n) abort();
    return q;
}

void ms_results_init(MsResults *res, const MsCart *cart, const MsPlatform *plat) {
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
    res->static_flags = xrealloc(NULL, n);
    res->section_of = xrealloc(NULL, n * sizeof(uint32_t));
    memset(res->exec_flags, 0, n);
    memset(res->exec_hits, 0, n * sizeof(uint32_t));
    memset(res->data_flags, 0, n);
    memset(res->data_hits, 0, n * sizeof(uint32_t));
    memset(res->static_flags, 0, n);
    memset(res->section_of, 0, n * sizeof(uint32_t));
    res->io_count = plat->io_reg_count;
    res->io = xrealloc(NULL, sizeof(MsIoStat) * (size_t)res->io_count);
    memset(res->io, 0, sizeof(MsIoStat) * (size_t)res->io_count);
    ms_map_init(&res->sites, sizeof(MsSiteStat), 1 << 22);
    ms_map_init(&res->callers, sizeof(MsCallList), 1 << 20);
    ms_map_init(&res->fork_counts, sizeof(uint32_t), 1 << 20);
}

void ms_results_free(MsResults *res) {
    free(res->exec_flags);
    free(res->exec_hits);
    free(res->data_flags);
    free(res->data_hits);
    free(res->static_flags);
    free(res->section_of);
    for (int i = 0; i < res->section_count; i++) ms_set_free(&res->sections[i].ctx_seen);
    free(res->sections);
    free(res->io);
    free(res->copies);
    free(res->bank_events);
    ms_map_free(&res->sites);
    ms_map_free(&res->callers);
    ms_map_free(&res->fork_counts);
    free(res->bad_ops);
    pthread_mutex_destroy(&res->sec_mtx);
    pthread_mutex_destroy(&res->rec_mtx);
    memset(res, 0, sizeof *res);
}

int ms_io_slot(const MsResults *res, uint16_t addr) {
    for (int i = 0; i < res->io_count; i++) {
        const MsIoReg *r = &res->plat->io_regs[i];
        if (addr >= r->lo && addr <= r->hi) return i;
    }
    return -1;
}

// static classification: padding runs and pointer tables
void ms_static_scan(MsResults *res) {
    const MsCart *c = res->cart;
    size_t n = (size_t)c->rom_banks * 0x4000;
    // padding: runs of 8+ identical 00 or FF bytes
    uint32_t run = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = c->rom[i < c->rom_size ? i : c->rom_size - 1];
        if (b == 0x00 || b == 0xFF) {
            run++;
            if (run >= 8) res->static_flags[i] |= MS_SF_PAD;
        } else {
            run = 0;
        }
    }
    // pointer tables: 4+ consecutive words pointing at plausible ROM targets
    // (a zeroed or padding ROM is not a table of null pointers)
    for (int bank = 0; bank < res->banks; bank++) {
        for (int off = 0; off + 7 < 0x4000; off += 2) {
            uint32_t k = ms_rom_index(bank, (uint16_t)off);
            if (res->static_flags[k] & MS_SF_POINTER) continue;
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
                    res->static_flags[a] |= MS_SF_POINTER;
                    res->static_flags[a + 1] |= MS_SF_POINTER;
                }
            }
            off += 6; // skip past the table we just marked
        }
    }
}

// --- explorer ---------------------------------------------------------------------

struct MsExplorer; // defined in explorer.h

// --- trace handlers (forward declarations, wired in ms_explorer_new) -----------
static void on_mem_read(void *ctx, MsSpace sp, uint16_t addr, uint8_t v, MsAccKind kind, uint32_t bank);
static void on_mem_write(void *ctx, MsSpace sp, uint16_t addr, uint8_t v, MsAccKind kind, uint32_t bank);
static void on_mapper_write(void *ctx, uint16_t addr, uint8_t v, uint8_t window, uint32_t old, uint32_t new);
static void on_io_access(void *ctx, uint16_t addr, uint8_t v, int is_write);
static void on_copy(void *ctx, const MsCopyRec *rec);
static void on_step(void *ctx, const MsStepInfo *info);
static void on_event(void *ctx, int kind, uint32_t a, uint32_t b);

MsExplorer *ms_explorer_new(const MsConfig *cfg, MsResults *res) {
    MsExplorer *ex = calloc(1, sizeof *ex);
    ex->cfg = *cfg;
    ex->res = res;
    ex->q_cap = cfg->queue_cap ? cfg->queue_cap : 2048;
    ex->queue = xrealloc(NULL, sizeof(MsPathState) * ex->q_cap);
    size_t dedup_cap = (size_t)ex->q_cap * 64;
    if (dedup_cap < (1 << 16)) dedup_cap = 1 << 16;
    ms_set_init(&ex->exact, dedup_cap);
    ex->vt.mem_read = on_mem_read;
    ex->vt.mem_write = on_mem_write;
    ex->vt.mapper_write = on_mapper_write;
    ex->vt.io_access = on_io_access;
    ex->vt.copy = on_copy;
    ex->vt.step = on_step;
    ex->vt.event = on_event;
    return ex;
}

void ms_explorer_free(MsExplorer *ex) {
    if (!ex) return;
    free(ex->queue);
    ms_set_free(&ex->exact);
    free(ex);
}

void ms_explorer_progress(MsExplorer *ex, MsProgress *out) {
    out->queue_len = ex->q_len;
    out->steps_total = ex->res->steps_total;
    out->paths_run = ex->res->paths_run;
    out->sections = (uint32_t)ex->res->section_count;
}

// --- queue -----------------------------------------------------------------------
static void queue_push(MsExplorer *ex, const MsMachine *m, uint32_t remaining, int forced, int kind,
                       int from_reset) {
    uint64_t h = ms_state_hash(m);
    int ins = ms_set_insert(&ex->exact, h);
    if (ins == 0) {
        ex->res->evicted_exact++;
        return;
    }
    if (ins < 0) return; // dedup set full
    if (ex->q_len >= ex->q_cap) {
        ex->res->queue_overflows++;
        return;
    }
    MsPathState *p = &ex->queue[(ex->q_head + ex->q_len) % ex->q_cap];
    p->m = *m;
    p->m.trace = &ex->vt;
    p->m.trace_ctx = ex;
    p->remaining = remaining;
    p->forced = (uint8_t)(forced ? 1 : 0);
    p->kind = (uint8_t)kind;
    p->from_reset = (uint8_t)(from_reset ? 1 : 0);
    ex->q_len++;
}

int ms_explorer_seed(MsExplorer *ex, const MsMachine *m, int kind, int forced) {
    queue_push(ex, m, ex->cfg.max_steps_path, forced, kind, kind == MS_SEED_RESET);
    if (forced) ex->res->seeds_spec++;
    else ex->res->seeds_natural++;
    return 1;
}

// --- trace handlers ----------------------------------------------------------------
static void cur_section_live_from_access(MsExplorer *ex, MsSection *s, MsSpace sp, uint16_t addr, uint32_t bank) {
    switch (sp) {
    case MS_SPACE_ROM:
        if (addr >= 0x4000) ms_section_live_small(s, 3); // upper window bank matters
        break;
    case MS_SPACE_VRAM:
        ms_section_live_vram(s, bank, (addr - 0x8000) >> 8);
        ms_section_live_small(s, 5); // vbk matters
        break;
    case MS_SPACE_SRAM:
        ms_section_live_sram(s, ((addr - 0xA000) + ex->cur->win_ram * 0x2000) >> 8);
        ms_section_live_small(s, 4); // ramg matters
        break;
    case MS_SPACE_WRAM:
        ms_section_live_wram(s, (addr - 0xC000) >> 8);
        break;
    case MS_SPACE_ECHO:
        ms_section_live_wram(s, (addr - 0xE000) >> 8);
        break;
    case MS_SPACE_HRAM: ms_section_live_small(s, 0); break;
    case MS_SPACE_OAM: ms_section_live_small(s, 1); break;
    case MS_SPACE_IE: ms_section_live_small(s, 2); break;
    default: break;
    }
}

static void on_mem_read(void *ctx, MsSpace sp, uint16_t addr, uint8_t v, MsAccKind kind, uint32_t bank) {
    (void)v;
    MsExplorer *ex = ctx;
    MsResults *res = ex->res;
    MsSection *s = ex->cur_sec ? &res->sections[ex->cur_sec] : NULL;
    switch (sp) {
    case MS_SPACE_ROM: {
        uint32_t key = ms_rom_index(bank, addr);
        if (kind == MS_ACC_DATA) {
            uint8_t old = flags_or(&res->data_flags[key], MS_DF_READ);
            if (!(old & MS_DF_READ)) ex->credit_data++;
            hits_add(&res->data_hits[key], 1);
            ms_copy_on_rom_read(&ex->ct, (uint8_t)bank, addr, v);
        }
        break;
    }
    case MS_SPACE_VRAM:
        break;
    default:
        break;
    }
    if (s && !ex->session) cur_section_live_from_access(ex, s, sp, addr, bank);
    if (sp == MS_SPACE_IO && kind == MS_ACC_IO) {
        int slot = ms_io_slot(res, addr);
        if (slot >= 0) {
            MsIoStat *st = &res->io[slot];
            uint32_t was = __atomic_fetch_add(&st->rd, 1, __ATOMIC_RELAXED);
            if (!was && !__atomic_load_n(&st->wr, __ATOMIC_RELAXED)) ex->credit_io++;
        }
        if (s && !ex->session && addr >= 0xFF00 && addr < 0xFF80) {
            ms_section_live_io(s, (uint16_t)(addr - 0xFF00));
            ms_section_io_used(s, slot);
        }
    }
}

static void on_mem_write(void *ctx, MsSpace sp, uint16_t addr, uint8_t v, MsAccKind kind, uint32_t bank) {
    MsExplorer *ex = ctx;
    MsResults *res = ex->res;
    MsSection *s = ex->cur_sec ? &res->sections[ex->cur_sec] : NULL;
    switch (sp) {
    case MS_SPACE_VRAM:
        u64_or(&res->vram_write_pages[bank & 1], 1ull << ((addr - 0x8000) >> 8));
        ms_copy_on_vram_write(&ex->ct, (uint8_t)(bank & 1), addr, v);
        break;
    case MS_SPACE_SRAM:
        u64_or(&res->sram_write_pages,
               1ull << ((((addr - 0xA000) + ex->cur->win_ram * 0x2000) & (MS_CRAM_PAGES * 256 - 1)) >> 8));
        break;
    case MS_SPACE_OAM:
        __atomic_store_n(&res->oam_written, 1, __ATOMIC_RELAXED);
        break;
    case MS_SPACE_IO: {
        int slot = ms_io_slot(res, addr);
        if (slot >= 0) {
            ex->last_io_slot = slot;
            MsIoStat *st = &res->io[slot];
            uint32_t was = __atomic_fetch_add(&st->wr, 1, __ATOMIC_RELAXED);
            if (!was && !__atomic_load_n(&st->rd, __ATOMIC_RELAXED)) ex->credit_io++;
            if (res->mt) pthread_mutex_lock(&res->rec_mtx);
            int have = 0;
            for (int i = 0; i < st->nvalues; i++)
                if (st->values[i] == v) have = 1;
            if (!have && st->nvalues < 8) st->values[st->nvalues++] = v;
            if (st->npcs < 4)
                st->pcs[st->npcs++] = ex->cur->pc;
            else {
                memmove(st->pcs, st->pcs + 1, 3 * sizeof(uint16_t));
                st->pcs[3] = ex->cur->pc;
            }
            if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
        }
        break;
    }
    default:
        break;
    }
    if (s && !ex->session && kind == MS_ACC_IO) ms_section_io_used(s, ms_io_slot(res, addr));
    (void)bank;
}

static void on_mapper_write(void *ctx, uint16_t addr, uint8_t v, uint8_t window, uint32_t old, uint32_t new) {
    MsExplorer *ex = ctx;
    MsResults *res = ex->res;
    if (res->mt) pthread_mutex_lock(&res->rec_mtx);
    if (res->bank_count == res->bank_cap) {
        res->bank_cap = res->bank_cap ? res->bank_cap * 2 : 64;
        res->bank_events = xrealloc(res->bank_events, sizeof(MsBankEvent) * (size_t)res->bank_cap);
    }
    MsBankEvent *e = &res->bank_events[res->bank_count++];
    e->seq = (uint32_t)res->bank_count;
    e->pc = ex->cur->pc;
    e->window = window;
    e->old_bank = old;
    e->new_bank = new;
    if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
    int slot = ms_io_slot(res, addr);
    if (slot >= 0) {
        u32_add(&res->io[slot].wr, 1);
        u32_add(&res->io[slot].bank_events, 1);
    }
    ex->credit_banks++;
    MsSection *s = ex->cur_sec && !ex->session ? &res->sections[ex->cur_sec] : NULL;
    if (s) {
        ms_section_banks_seen(s, new);
        ms_section_io_used(s, slot);
    }
    (void)v;
}

static void on_io_access(void *ctx, uint16_t addr, uint8_t v, int is_write) {
    MsExplorer *ex = ctx;
    MsResults *res = ex->res;
    int slot = ms_io_slot(res, addr);
    if (slot >= 0 && is_write) {
        MsIoStat *st = &res->io[slot];
        u32_add(&st->wr, 1);
        if (res->mt) pthread_mutex_lock(&res->rec_mtx);
        int have = 0;
        for (int i = 0; i < st->nvalues; i++)
            if (st->values[i] == v) have = 1;
        if (!have && st->nvalues < 8) st->values[st->nvalues++] = v;
        if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
    }
}

static void on_copy(void *ctx, const MsCopyRec *rec) {
    ms_record_copy((MsExplorer *)ctx, rec, 0);
}

// per-step recording shared by both engines (see the header).  The fork mode
// keeps the branch-site and caller maps; the session mode adds the RETI
// bookkeeping of the natural IRQ depth and the cycle count of the step.
void ms_step_record(MsExplorer *ex, const MsStepInfo *info) {
    MsResults *res = ex->res;
    ex->last_len = info->len;
    ex->have_last = 1;
    ex->last_ctrl = (int)info->ctrl;
    switch (info->ctrl) {
    case MS_CTRL_JP:
    case MS_CTRL_JR:
    case MS_CTRL_JP_CC:
    case MS_CTRL_JR_CC:
    case MS_CTRL_JP_HL:
        if (info->target <= info->pc) { // backward edge: loop guard
            int found = -1;
            for (int i = 0; i < 8; i++)
                if (ex->back_ring[i].pc == info->target && ex->back_ring[i].count) { found = i; break; }
            if (found < 0) {
                found = ex->back_idx;
                ex->back_idx = (ex->back_idx + 1) & 7;
                ex->back_ring[found].pc = info->target;
                ex->back_ring[found].count = 0;
            }
            ex->back_ring[found].count++;
            ms_copy_on_back_edge(&ex->ct, info->target, ms_rom_key(ex->cur, info->pc));
        }
        break;
    case MS_CTRL_CALL:
    case MS_CTRL_CALL_CC:
    case MS_CTRL_RST: {
        if (ex->session) break; // the fork explorer owns the caller map
        uint32_t tkey = ms_rom_index(info->target < 0x4000 ? 0 : ex->cur_bankN, info->target);
        MsCallList *cl = ms_map_get_or_add(&res->callers, tkey);
        if (cl && cl->n < 8) {
            int dup = 0;
            uint32_t skey = ms_rom_key(ex->cur, info->pc);
            for (int i = 0; i < cl->n; i++)
                if (cl->site[i] == skey) dup = 1;
            if (!dup) cl->site[cl->n++] = skey;
        }
        break;
    }
    case MS_CTRL_RETI:
        if (ex->session && ex->irq_depth) ex->irq_depth--;
        break;
    default:
        break;
    }
    if (info->ctrl == MS_CTRL_JP_CC || info->ctrl == MS_CTRL_JR_CC || info->ctrl == MS_CTRL_CALL_CC ||
        info->ctrl == MS_CTRL_RET_CC) {
        if (!ex->session) { // branch-site outcomes are the fork explorer's map
            uint32_t skey = ms_rom_key(ex->cur, info->pc);
            MsSiteStat *st = ms_map_get_or_add(&res->sites, skey);
            if (st) {
                if (info->cond_taken) st->taken++;
                else st->nottaken++;
            }
        }
    }
    if (ex->session)
        ex->last_cycles = sm83_step_cycles(info, info->opcode == 0xCB ? ms_peek(ex->cur, (uint16_t)(info->pc + 1)) : 0);
}

static void on_step(void *ctx, const MsStepInfo *info) { ms_step_record((MsExplorer *)ctx, info); }

static void on_event(void *ctx, int kind, uint32_t a, uint32_t b) {
    MsExplorer *ex = ctx;
    MsResults *res = ex->res;
    if (kind == MS_EV_BADOP) {
        if (res->mt) pthread_mutex_lock(&res->rec_mtx);
        if (res->bad_op_count == res->bad_op_cap) {
            res->bad_op_cap = res->bad_op_cap ? res->bad_op_cap * 2 : 16;
            res->bad_ops = xrealloc(res->bad_ops, sizeof(*res->bad_ops) * (size_t)res->bad_op_cap);
        }
        res->bad_ops[res->bad_op_count].key = a;
        res->bad_ops[res->bad_op_count].op = (uint8_t)b;
        res->bad_op_count++;
        if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
    }
}

// --- forking -------------------------------------------------------------------------
static void fork_branch(MsExplorer *ex, const MsMachine *m, int cond) {
    // per-site cap: loop bodies reach their conditional every iteration and
    // the extra variants are data drift, not new map information
    uint32_t site = ms_rom_key(m, m->pc);
    uint32_t *cnt = ms_map_get_or_add(&ex->res->fork_counts, site);
    if (cnt && *cnt >= ex->cfg.fork_site_cap) {
        ex->res->forks_capped++;
        return;
    }
    if (cnt) (*cnt)++;
    MsMachine f = *m;
    f.f ^= sm83_cond_flag(cond); // the other outcome of this branch
    // the fork keeps the reset lineage of its parent path
    MsPathState *parent = NULL; // lineage passed through the machine copy below
    (void)parent;
    queue_push(ex, &f, ex->cfg.max_steps_path, 1, MS_PATH_FORK, ex->cur_kind == MS_SEED_RESET || ex->cur_from_reset);
    ex->res->forks_branch++;
}

static void fork_irq(MsExplorer *ex, const MsMachine *m) {
    uint8_t pending = (uint8_t)(m->ie & m->io[0x0F] & 0x1F);
    if (!pending) return;
    int bit = 0;
    while (!(pending & (1 << bit))) bit++;
    const MsPlatform *plat = m->plat;
    uint16_t vector = 0;
    for (int i = 0; i < plat->irq_count; i++)
        if (plat->irqs[i].bit == bit) vector = plat->irqs[i].vector;
    if (!vector) return;
    MsMachine f = *m;
    const MsTraceVt *t = f.trace;
    void *tc = f.trace_ctx;
    f.trace = NULL;
    f.trace_ctx = NULL;
    f.sp = (uint16_t)(f.sp - 2);
    ms_write(&f, f.sp, (uint8_t)f.pc, MS_ACC_STACK);
    ms_write(&f, (uint16_t)(f.sp + 1), (uint8_t)(f.pc >> 8), MS_ACC_STACK);
    f.pc = vector;
    f.ime = 0;
    f.ime_delay = 0;
    f.io[0x0F] &= (uint8_t)~(1 << bit);
    f.trace = t;
    f.trace_ctx = tc;
    queue_push(ex, &f, ex->cfg.max_steps_path, 0, 2, 0);
    ex->res->forks_irq++;
}

// --- per-step bookkeeping -------------------------------------------------------------
void ms_pre_step(MsExplorer *ex, MsMachine *m, uint8_t op) {
    MsResults *res = ex->res;
    if (ex->cur_pad) return; // padding slide: executed, but never classified code
    if (m->pc >= 0x8000) {
        // executing VRAM/WRAM/HRAM code: real hardware allows it, but it is
        // not ROM map information — count it and move on
        u32_add(&res->exec_outside, 1);
        return;
    }
    uint32_t key = ms_rom_key(m, m->pc);
    uint8_t bank = m->pc < 0x4000 ? (uint8_t)m->win_rom0 : (uint8_t)m->win_romN;
    int is_entry = !ex->have_last || ex->last_bank != bank ||
                   (uint16_t)(ex->last_pc + ex->last_len) != m->pc;
    uint8_t nf = (uint8_t)(ex->cur_forced ? MS_XF_FORCED : MS_XF_NATURAL);
    uint8_t old = flags_or(&res->exec_flags[key], nf);
    if (!(old & nf)) ex->credit_exec++;
    hits_add(&res->exec_hits[key], 1);
    if (is_entry) flags_or(&res->exec_flags[key], MS_XF_ENTRY);
    if (ex->cur_irq) flags_or(&res->exec_flags[key], MS_XF_IRQ);
    int len = sm83_len(op);
    uint16_t hi = m->pc;
    for (int i = 1; i < len; i++) { // operand bytes count as executed code
        uint16_t a = (uint16_t)(m->pc + i);
        if (m->pc < 0x4000 && a >= 0x4000) break; // crossed the window edge
        uint32_t k = ms_rom_key(m, a);
        uint8_t o2 = flags_or(&res->exec_flags[k], nf);
        if (!(o2 & nf)) ex->credit_exec++;
        hi = a;
    }
    uint32_t id = ms_section_touch(res, bank, m->pc, hi, ex->cur_forced ? 0 : 1, is_entry, ex->cur_sec,
                                   ex->cur_from_reset && !ex->cur_forced, ex->cur_irq);
    ex->cur_sec = id;
    ex->last_pc = m->pc;
    ex->last_bank = bank;
    // last_len is refreshed by the step trace after the instruction executes
}

// natural IRQ dispatch (play sessions): highest-priority pending bit, push PC,
// jump to the vector, clear IME and the IF bit; the handler is marked MS_XF_IRQ
// until its RETI
int ms_service_irq(MsExplorer *ex, MsMachine *m) {
    uint8_t pending = (uint8_t)(m->ime & m->ie & m->io[0x0F] & 0x1F);
    if (!pending) return 0;
    int bit = 0;
    while (!(pending & (1 << bit))) bit++;
    const MsPlatform *plat = m->plat;
    uint16_t vector = 0;
    for (int i = 0; i < plat->irq_count; i++)
        if (plat->irqs[i].bit == bit) vector = plat->irqs[i].vector;
    if (!vector) return 0;
    m->halted = 0;
    const MsTraceVt *t = m->trace;
    void *tc = m->trace_ctx;
    m->trace = NULL; // dispatch bookkeeping is not ROM behaviour
    m->trace_ctx = NULL;
    m->sp = (uint16_t)(m->sp - 2);
    ms_write(m, m->sp, (uint8_t)m->pc, MS_ACC_STACK);
    ms_write(m, (uint16_t)(m->sp + 1), (uint8_t)(m->pc >> 8), MS_ACC_STACK);
    m->pc = vector;
    m->ime = 0;
    m->ime_delay = 0;
    m->io[0x0F] &= (uint8_t)~(1 << bit);
    m->trace = t;
    m->trace_ctx = tc;
    ex->irq_depth++;
    if (m->trace && m->trace->event) m->trace->event(m->trace_ctx, MS_EV_IRQ, vector, (uint32_t)bit);
    return bit + 1;
}

// --- run loop ---------------------------------------------------------------------------
static void run_path(MsExplorer *ex, MsPathState *p) {
    MsResults *res = ex->res;
    MsMachine *m = &p->m;
    ex->cur = m;
    ex->cur_forced = p->forced;
    ex->cur_kind = p->kind;
    ex->cur_from_reset = p->from_reset;
    ex->cur_irq = (uint8_t)(p->kind == 2 ? 1 : 0);
    ex->have_last = 0;
    ex->cur_sec = 0;
    ex->steps_this_path = 0;
    memset(ex->back_ring, 0, sizeof ex->back_ring);
    ex->back_idx = 0;
    ms_copy_reset(&ex->ct);
    while (ex->steps_this_path < p->remaining) {
        if (res->steps_total >= ex->cfg.max_steps_total) {
            res->budget_steps++;
            ex->stop = 1;
            break;
        }
        if (m->halted) break;
        uint8_t op = ms_peek(m, m->pc);
        // execution sliding through a padding run: keep going (the code after
        // the run is often the point of a forced entry) but never call the
        // padding itself code; bound the slide so garbage loops stay bounded
        uint32_t pk = ms_rom_key(m, m->pc);
        if ((res->static_flags[pk] & MS_SF_PAD) && (op == 0x00 || op == 0xFF)) {
            if (++ex->pad_slide > 4096) { res->pad_cuts++; break; }
            ex->cur_pad = 1;
        } else {
            ex->pad_slide = 0;
            ex->cur_pad = 0;
        }
        int cond = sm83_cond_of(op);
        if (cond >= 0) fork_branch(ex, m, cond);
        if (!ex->cfg.no_irq_fork && m->ime && (m->ie & m->io[0x0F] & 0x1F)) fork_irq(ex, m);
        ex->cur_bankN = m->win_romN;
        ms_pre_step(ex, m, op);
        int r = sm83_step(m);
        ex->steps_this_path++;
        u64_add(&res->steps_total, 1);
        if (r != 0) break; // halt/stop/undefined opcode
        if (ex->cfg.loop_back_cap) {
            int cut = 0;
            for (int i = 0; i < 8; i++)
                if (ex->back_ring[i].count > ex->cfg.loop_back_cap) cut = 1;
            if (cut) {
                res->cycle_cuts++;
                break;
            }
        }
    }
    if (ex->steps_this_path >= p->remaining) res->step_cuts++;
    ms_copy_flush_path(ex, &ex->ct);
    res->paths_run++;
}

// context projection: hash of the incoming state components this section reads
static uint64_t project_ctx(MsExplorer *ex, const MsSection *s, const MsMachine *m) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t regs[8] = { m->a, m->f, m->b, m->c, m->d, m->e, m->h, m->l };
    h = ms_fnv1a(h, regs, 8);
    h = ms_fnv1a_u32(h, m->sp);
    if (s->ctx_mask & (1ull << MS_CTX_IME)) h = ms_fnv1a(h, &m->ime, 1);
    if (s->ctx_mask & (1ull << MS_CTX_IE)) h = ms_fnv1a(h, &m->ie, 1);
    if (s->ctx_mask & (1ull << MS_CTX_ROMN)) h = ms_fnv1a_u32(h, m->win_romN);
    if (s->ctx_mask & (1ull << MS_CTX_RAMG)) h = ms_fnv1a(h, &m->ramg, 1);
    if (s->ctx_mask & (1ull << MS_CTX_VBK)) h = ms_fnv1a(h, &m->vram_bank, 1);
    for (int pg = 0; pg < 32; pg++) {
        if (s->page_mask_wram & (1ull << pg)) h = ms_fnv1a_u64(h, m->ph_wram[pg]);
        if (s->page_mask_vram[0] & (1ull << pg)) h = ms_fnv1a_u64(h, m->ph_vram[0][pg]);
        if (s->page_mask_vram[1] & (1ull << pg)) h = ms_fnv1a_u64(h, m->ph_vram[1][pg]);
    }
    for (int pg = 0; pg < 64; pg++)
        if (s->page_mask_sram & (1ull << pg)) h = ms_fnv1a_u64(h, m->ph_cram[pg]);
    if (s->ctx_hram) h = ms_fnv1a_u64(h, m->ph_hram);
    if (s->ctx_oam) h = ms_fnv1a_u64(h, m->ph_oam);
    for (int i = 0; i < 0x80; i++)
        if (s->io_read_mask & (1u << i)) h = ms_fnv1a(h, &m->io[i], 1);
    (void)ex;
    return h;
}

int ms_explore_step(MsExplorer *ex) {
    MsResults *res = ex->res;
    if (!ex->q_len) return 0;
    if (ex->cfg.max_paths && res->paths_run >= ex->cfg.max_paths) {
        res->budget_paths++;
        return 0;
    }
    MsPathState *p = &ex->queue[ex->q_head % ex->q_cap];
    MsPathState cur = *p;
    ex->q_head = (ex->q_head + 1) % ex->q_cap;
    ex->q_len--;
    // "same context" eviction: entering a section start with a context the
    // section cannot distinguish from one already explored
    if (!ex->cfg.no_context_evict) {
        uint32_t key = ms_rom_key(&cur.m, cur.m.pc);
        uint32_t sid1 = res->section_of[key];
        if (sid1) {
            MsSection *s = &res->sections[sid1 - 1];
            if (key == ms_rom_index(s->bank, s->lo)) {
                uint64_t h = project_ctx(ex, s, &cur.m);
                int ins = ms_set_insert(&s->ctx_seen, h);
                if (ins == 0) {
                    res->evicted_ctx++;
                    return ex->q_len ? 1 : 0;
                }
                if (ins > 0) s->ctx_count++;
            }
        }
    }
    run_path(ex, &cur);
    return ex->q_len ? 1 : 0;
}

int ms_explore_slice(MsExplorer *ex, int ms) {
    uint64_t deadline = (uint64_t)ms * 1000 + (uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC;
    int more = 1;
    while (more) {
        more = ms_explore_step(ex);
        if (ex->stop) return 0;
        if ((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC >= deadline) break;
    }
    return more;
}

void ms_explore_finalize(MsExplorer *ex) {
    MsResults *res = ex->res;
    for (size_t i = 0; i < res->sites.cap; i++) {
        if (!res->sites.used[i]) continue;
        uint32_t key = res->sites.keys[i];
        uint32_t sid1 = key < (uint32_t)res->banks * 0x4000 ? res->section_of[key] : 0;
        if (!sid1) continue;
        MsSection *s = &res->sections[sid1 - 1];
        MsSiteStat *st = (MsSiteStat *)((uint8_t *)res->sites.vals + i * res->sites.stride);
        s->cond_sites++;
        if (st->taken && st->nottaken) s->cond_both++;
    }
    for (size_t i = 0; i < res->callers.cap; i++) {
        if (!res->callers.used[i]) continue;
        uint32_t key = res->callers.keys[i];
        uint32_t sid1 = key < (uint32_t)res->banks * 0x4000 ? res->section_of[key] : 0;
        if (!sid1) continue;
        MsSection *s = &res->sections[sid1 - 1];
        MsCallList *cl = (MsCallList *)((uint8_t *)res->callers.vals + i * res->callers.stride);
        for (int j = 0; j < cl->n && s->callers_n < 8; j++) s->callers[s->callers_n++] = cl->site[j];
    }
    for (int i = 0; i < res->section_count; i++) {
        MsSection *s = &res->sections[i];
        if (s->boot_hits > 0 || (s->kind_bits & MS_XF_IRQ) || s->copies > 0)
            s->verdict = MS_V_REQUIRED;
        else if (s->natural_hits > 0 && s->forced_hits == 0)
            s->verdict = MS_V_PLAYED; // only ever seen in a real play session
        else
            s->verdict = MS_V_FORCED;
    }
    // the m0boy bridge: count bytes that are both executed and proven data —
    // execution wins there, the data mark is dropped at export
    {
        uint32_t contra = 0;
        uint64_t verb = 0;
        size_t n = (size_t)res->banks * 0x4000;
        for (size_t k = 0; k < n; k++) {
            if (res->data_flags[k] & MS_DF_VERBATIM) {
                verb++;
                if (res->exec_flags[k] & (MS_XF_NATURAL | MS_XF_FORCED)) contra++;
            }
        }
        res->data_verbatim_bytes = verb;
        res->data_contradictions = contra;
    }
}

int ms_explore(MsExplorer *ex) {
    while (ms_explore_slice(ex, 50))
        ;
    ms_explore_finalize(ex);
    return 0;
}

// --- GB seeding ------------------------------------------------------------------------
uint64_t ms_results_unexplained(const MsResults *res) {
    size_t n = (size_t)res->banks * 0x4000;
    uint64_t cnt = 0;
    for (size_t k = 0; k < n; k++) {
        if (res->exec_flags[k] & (MS_XF_NATURAL | MS_XF_FORCED)) continue;
        if (res->data_flags[k] & (MS_DF_READ | MS_DF_COPIED | MS_DF_VERBATIM)) continue;
        if (res->static_flags[k] & (MS_SF_POINTER | MS_SF_PAD)) continue;
        cnt++;
    }
    return cnt;
}

static int is_padding_at(const MsCart *c, uint32_t bank, uint16_t addr) {
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

void ms_seed_gb(MsExplorer *ex) {
    MsResults *res = ex->res;
    const MsCart *c = res->cart;
    const MsPlatform *plat = res->plat;
    MsMachine m;
    ms_machine_init(&m, c, plat);
    // reset entry
    ms_explorer_seed(ex, &m, MS_SEED_RESET, 0);
    // RST and IRQ vectors when they hold something that is not padding
    for (int i = 0; i < plat->entry_count; i++) {
        const MsEntry *e = &plat->entries[i];
        if (e->kind == MS_SEED_RESET) continue;
        if (is_padding_at(c, 0, e->addr)) continue;
        MsMachine v = m;
        v.pc = e->addr;
        ms_explorer_seed(ex, &v, e->kind, e->kind == MS_SEED_RST ? 1 : 0);
    }
    // pointer-table scan: every ROM word pointing at plausible code
    if (!ex->cfg.no_pointer_scan) {
        int seeded = 0;
        MsHashSet seen;
        ms_set_init(&seen, 65536);
        for (int bank = 0; bank < res->banks && seeded < (int)ex->cfg.seed_cap; bank++) {
            for (int off = 0; off + 1 < 0x4000 && seeded < (int)ex->cfg.seed_cap; off++) {
                uint32_t a = (uint32_t)bank * 0x4000 + (uint32_t)off;
                if (a + 1 >= c->rom_size) break;
                uint16_t v = (uint16_t)(c->rom[a] | (c->rom[a + 1] << 8));
                if (v < 0x0100 || v >= 0x8000) continue;
                uint32_t tbank = v < 0x4000 ? 0 : 1;
                if (is_padding_at(c, tbank, v)) continue;
                uint64_t ckey = (uint64_t)ms_rom_index((uint32_t)tbank, v);
                if (ms_set_insert(&seen, ckey) == 0) continue;
                MsMachine s = m;
                s.pc = v;
                if (v >= 0x4000) {
                    s.rom_lo = 1;
                    ms_mapper_update_windows(&s);
                }
                ms_explorer_seed(ex, &s, MS_SEED_POINTER, 1);
                seeded++;
            }
        }
        ms_set_free(&seen);
    }
    // bank sweep seeding: every switchable bank entered at $4000
    if (ex->cfg.seed_banks) {
        for (int bank = 1; bank < res->banks; bank++) {
            if (is_padding_at(c, (uint32_t)bank, 0x4000)) continue;
            MsMachine s = m;
            s.rom_lo = (uint8_t)bank;
            ms_mapper_update_windows(&s);
            s.pc = 0x4000;
            ms_explorer_seed(ex, &s, MS_SEED_POINTER, 1);
        }
    }
}
