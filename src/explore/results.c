// results.c: the DpResults model (per-byte coverage, maps, discovery shares) and the static scan
#include <zlib.h>

#include <signal.h>
#include <sys/stat.h>
#include "results.h"
#include "coverage_defaults.h"
#include "../core/dpmachine.h"
#include <time.h>
#include <unistd.h>

#include <stddef.h>


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
    void **r = realloc(m->retired, (m->nretired + 3) * sizeof *r);
    if (!r) { free(nk); free(nu); free(nv); return 0; }
    m->retired = r;
    m->retired[m->nretired++] = m->keys;
    m->retired[m->nretired++] = m->used;
    m->retired[m->nretired++] = m->vals;
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
    m->retired = NULL;
    m->nretired = 0;
    pthread_mutex_init(&m->mtx, NULL);
}

void dp_map_free(DpMap32 *m) {
    for (size_t i = 0; i < m->nretired; i++) free(m->retired[i]);
    free(m->retired);
    pthread_mutex_destroy(&m->mtx);
    free(m->keys);
    free(m->used);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

static void *map_get_or_add_locked(DpMap32 *m, uint32_t key);
void *dp_map_get_or_add(DpMap32 *m, uint32_t key) {
    pthread_mutex_lock(&m->mtx);
    void *r = map_get_or_add_locked(m, key);
    pthread_mutex_unlock(&m->mtx);
    return r;
}

static void *map_get_or_add_locked(DpMap32 *m, uint32_t key) {
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
    pthread_mutex_lock(&m->mtx);
    void *r = map_find(m, key);
    pthread_mutex_unlock(&m->mtx);
    return r;
}

// --- config / results ------------------------------------------------------------
void dp_config_defaults(DpConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->max_steps_path = 100000;
    cfg->nocredit_cut = 0;
    cfg->max_paths = 200000;
    cfg->max_steps_total = 20000000ull;
    cfg->queue_cap = 4096;
    cfg->loop_back_cap = 4096;
    cfg->fork_site_cap = DP_DEFAULT_BRANCH_FORK_CAP;
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
    res->map_sticky = calloc(n ? n : 1, 1);
    memset(res->section_of, 0, n * sizeof(uint32_t));
    res->io_count = plat->io_reg_count;
    res->io = xrealloc(NULL, sizeof(DpIoStat) * (size_t)res->io_count);
    memset(res->io, 0, sizeof(DpIoStat) * (size_t)res->io_count);
    dp_map_init(&res->sites, sizeof(DpSiteStat), 1 << 22);
    dp_map_init(&res->callers, sizeof(DpCallList), 1 << 20);
    dp_map_init(&res->fork_counts, sizeof(uint32_t), 1 << 20);
    dp_map_init(&res->indirect, sizeof(DpIndirect), 1 << 16);
    dp_set_init(&res->bad_seen, 1 << 20);
    dp_set_init(&res->residual_tried, 1 << 12);
}

static double disc_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void dp_disc_add(DpResults *res, const char *source, uint64_t credit) {
    if (res->disc_n == res->disc_cap) {
        if (res->disc_cap >= DP_DISC_CAP_MAX) {
            // log compaction at the absolute cap: keep every other entry (the
            // curve keeps its shape, the time resolution halves); the folded
            // count is reported
            int keep = 0;
            for (int i = 0; i < res->disc_n; i += 2) res->disc[keep++] = res->disc[i];
            res->disc_dropped += (uint32_t)(res->disc_n - keep);
            res->disc_n = keep;
        }
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
    e->frame = 0;
}


// --- coverage map persistence (--resume must not forget what a previous pass proved) ---
// Flags only ever gain bits within a run; a resumed run used to start from an empty map and
// deduplicated states then never re-proved their bytes, so earlier knowledge was lost.
// Format: "DPCOV1\0\0", u32 banks, then exec_flags, data_flags, static_flags (1 byte per ROM
// byte) and src_flags (u32 per ROM byte).
int dp_results_cov_save(const DpResults *res, const char *path) {
    size_t n = (size_t)res->banks * 0x4000;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    uint32_t banks = (uint32_t)res->banks;
    int ok = fwrite("DPCOV1\0\0", 1, 8, f) == 8 && fwrite(&banks, 4, 1, f) == 1 &&
             fwrite(res->exec_flags, 1, n, f) == n && fwrite(res->data_flags, 1, n, f) == n &&
             fwrite(res->static_flags, 1, n, f) == n && fwrite(res->src_flags, 4, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
    return rename(tmp, path) == 0;
}

// OR the saved flags into the map; returns the bytes whose exec|data|static state became non-empty, -1 if absent
long dp_results_cov_load(DpResults *res, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = (size_t)res->banks * 0x4000;
    char magic[8];
    uint32_t banks = 0;
    long gained = -1;
    uint8_t *e = malloc(n), *d = malloc(n), *st = malloc(n);
    uint32_t *sr = malloc(n * sizeof(uint32_t));
    if (e && d && st && sr && fread(magic, 1, 8, f) == 8 && !memcmp(magic, "DPCOV1\0\0", 8) &&
        fread(&banks, 4, 1, f) == 1 && banks == (uint32_t)res->banks &&
        fread(e, 1, n, f) == n && fread(d, 1, n, f) == n && fread(st, 1, n, f) == n &&
        fread(sr, 4, n, f) == n) {
        gained = 0;
        for (size_t i = 0; i < n; i++) {
            if (!(res->exec_flags[i] | res->data_flags[i] | res->static_flags[i]) && (e[i] | d[i] | st[i])) gained++;
            res->exec_flags[i] |= e[i];
            res->data_flags[i] |= d[i];
            res->static_flags[i] |= st[i];
            res->src_flags[i] |= sr[i];
        }
    }
    free(e); free(d); free(st); free(sr);
    fclose(f);
    return gained;
}

// The discovery log rides next to the coverage map so a resumed pass keeps crediting the
// earlier passes' bytes to the methods that found them (methods_pct) instead of to "static".
// Format: "DPDSC1\0\0", u32 entry size, u32 count, raw entries.
int dp_results_disc_save(const DpResults *res, const char *path) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    uint32_t esz = (uint32_t)sizeof(DpDiscEntry), cnt = (uint32_t)res->disc_n;
    int ok = fwrite("DPDSC1\0\0", 1, 8, f) == 8 && fwrite(&esz, 4, 1, f) == 1 && fwrite(&cnt, 4, 1, f) == 1 &&
             (cnt == 0 || fwrite(res->disc, sizeof(DpDiscEntry), cnt, f) == cnt);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
    return rename(tmp, path) == 0;
}

// Prepend the saved log to the entries already present; returns the entries loaded, -1 if absent
long dp_results_disc_load(DpResults *res, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[8];
    uint32_t esz = 0, cnt = 0;
    long got = -1;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "DPDSC1\0\0", 8) &&
        fread(&esz, 4, 1, f) == 1 && fread(&cnt, 4, 1, f) == 1 && esz == sizeof(DpDiscEntry) && cnt < (1u << 26)) {
        size_t have = (size_t)res->disc_n;
        DpDiscEntry *n = malloc((cnt + have + 1) * sizeof *n);
        if (n && fread(n, sizeof *n, cnt, f) == cnt) {
            if (have) memcpy(n + cnt, res->disc, have * sizeof *n);
            free(res->disc);
            res->disc = n;
            res->disc_n = res->disc_cap = (int)(cnt + have);
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            // the clock of the new pass continues after the last saved entry
            double last = cnt ? n[cnt - 1].t : 0.0;
            res->disc_t0 = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 - last;
            for (size_t i = 0; i < have; i++) n[cnt + i].t += last;
            got = (long)cnt;
        } else free(n);
    }
    fclose(f);
    return got;
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
    free(res->map_sticky);
    free(res->section_of);
    free(res->src_flags);
    dp_first_free(&res->first);
    for (int i = 0; i < res->section_count; i++) dp_set_free(&res->sections[i].ctx_seen);
    free(res->sections);
    free(res->io);
    free(res->copies);
    free(res->bank_events);
    free(res->bank_seen);
    dp_map_free(&res->sites);
    dp_map_free(&res->callers);
    dp_map_free(&res->fork_counts);
    dp_map_free(&res->indirect);
    dp_set_free(&res->bad_seen);
    dp_set_free(&res->residual_tried);
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
int is_padding_at(const DpCart *c, uint32_t bank, uint16_t addr) {
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

void dp_static_scan(DpResults *res) {
    const DpCart *c = res->cart;
    size_t n = (size_t)c->rom_banks * 0x4000;
    // the Nintendo logo at $0104-$0133: hardware-mandated data the boot ROM
    // verifiably reads on every power-on.  Only when the cartridge carries
    // the canonical logo (a hardware-bootable ROM); a mismatched blob at the
    // same offset stays untouched — the rule proves position, never content
    {
        static const uint8_t logo_pre[16] = { 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
                                              0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D };
        if (c->rom_size >= 0x134 && !memcmp(c->rom + 0x104, logo_pre, sizeof logo_pre))
            for (uint32_t a = 0x104; a <= 0x133; a++)
                if (!(res->data_flags[a] & DP_DF_STATIC)) {
                    res->data_flags[a] |= DP_DF_STATIC;
                    res->prov_data++;
                    res->logo_data++;
                    dp_cov_mark(&res->first, res->src_flags, a, DP_SRC_STATIC_SCAN, 0, 0);
                }
    }
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


