// sections.c: executed-range sections and their per-context bookkeeping.
// A section is a maximal contiguous run of executed addresses within one
// bank; every access an instruction makes is attributed to the section its
// pc belongs to, which is where the necessity statistics live.
//
// Section records are shared with the play-session workers: every entry point
// below takes res->sec_mtx when res->mt is set.  The fork explorer runs with
// the sessions paused and skips the lock.
#include "explorer.h"

static DpSection *sec_lock(DpResults *res) {
    if (res->mt) pthread_mutex_lock(&res->sec_mtx);
    return NULL;
}
static void sec_unlock(DpResults *res) {
    if (res->mt) pthread_mutex_unlock(&res->sec_mtx);
}

// creates or extends the section at (bank, pc) and updates its per-run
// counters under the section lock; returns the section id (the pointer would
// not survive the lock: another worker may realloc the array)
// Per-thread hit cache (parallel mode): the counters of a section already known to this
// thread (and wide enough for the executed range) are accumulated here and merged into
// the shared record under sec_mtx when the slot is evicted or by dp_section_flush().  A
// lock per executed instruction made ten session workers wait on one mutex.
#define SEC_TC 256
typedef struct {
    DpResults *res;
    uint32_t sid1, hits, nat, forced, entries, boot;
    uint16_t hi;
    uint8_t bank, kinds;
} SecTc;
static __thread SecTc sec_tc[SEC_TC];
static __thread int sec_tc_on; // only while this thread runs an epoch (flushed at its end)

void dp_section_tc_begin(void) { sec_tc_on = 1; }

// merge one slot into the shared record; caller holds sec_mtx
static void sec_tc_merge(SecTc *t) {
    if (!t->res || !t->sid1) return;
    DpSection *s = &t->res->sections[t->sid1 - 1];
    s->hits += t->hits;
    s->natural_hits += t->nat;
    s->forced_hits += t->forced;
    s->entries += t->entries;
    s->boot_hits += t->boot;
    s->kind_bits |= t->kinds;
    t->hits = t->nat = t->forced = t->entries = t->boot = 0;
    t->kinds = 0;
}

// call from the owning thread when it stops (end of an epoch / of the run)
void dp_section_flush(DpResults *res) {
    sec_tc_on = 0;
    if (!res->mt) return;
    pthread_mutex_lock(&res->sec_mtx);
    for (int i = 0; i < SEC_TC; i++)
        if (sec_tc[i].res == res) { sec_tc_merge(&sec_tc[i]); sec_tc[i].res = NULL; sec_tc[i].sid1 = 0; }
    pthread_mutex_unlock(&res->sec_mtx);
}

uint32_t dp_section_touch(DpResults *res, uint8_t bank, uint16_t pc, uint16_t hi, int natural,
                          int is_entry, uint32_t cur_sec, int from_reset, int irq) {
    uint32_t key0 = dp_rom_index(bank, pc);
    if (res->mt && sec_tc_on) { // lock-free hit on a section this thread already knows
        uint32_t k1 = __atomic_load_n(&res->section_of[key0], __ATOMIC_RELAXED);
        SecTc *t = &sec_tc[k1 & (SEC_TC - 1)];
        if (k1 && t->res == res && t->sid1 == k1 && t->bank == bank && hi <= t->hi) {
            t->hits++;
            if (natural) t->nat++; else t->forced++;
            if (is_entry) t->entries++;
            if (from_reset && natural) t->boot++;
            t->kinds |= (uint8_t)((natural ? DP_XF_NATURAL : DP_XF_FORCED) | (irq ? DP_XF_IRQ : 0));
            return k1 - 1;
        }
    }
    sec_lock(res);
    uint32_t key = dp_rom_index(bank, pc);
    uint32_t sid1 = res->section_of[key];
    DpSection *s;
    if (sid1) {
        s = &res->sections[sid1 - 1];
        if (s->bank == bank && (uint16_t)(s->hi + 1) >= pc && hi > s->hi) s->hi = hi;
    } else {
        DpSection *cur = cur_sec < (uint32_t)res->section_count ? &res->sections[cur_sec] : NULL;
        if (cur && cur->bank == bank && pc == (uint16_t)(cur->hi + 1)) {
            s = cur; // sequential execution continues the current run
            s->hi = hi;
        } else {
            if (res->section_count == res->section_cap) {
                res->section_cap = res->section_cap ? res->section_cap * 2 : 256;
                res->sections = realloc(res->sections, sizeof(DpSection) * (size_t)res->section_cap);
            }
            s = &res->sections[res->section_count];
            memset(s, 0, sizeof *s);
            s->id = (uint32_t)res->section_count++;
            s->bank = bank;
            s->lo = pc;
            s->hi = hi;
            dp_set_init(&s->ctx_seen, 4096);
        }
        res->section_of[key] = s->id + 1;
    }
    s->hits++;
    if (natural) s->natural_hits++;
    else s->forced_hits++;
    if (is_entry) s->entries++;
    s->kind_bits |= (uint8_t)(natural ? DP_XF_NATURAL : DP_XF_FORCED);
    if (from_reset && natural) s->boot_hits++;
    if (irq) s->kind_bits |= DP_XF_IRQ;
    uint32_t id = s->id;
    if (res->mt && sec_tc_on) { // remember this section for the lock-free path (hits above are already counted)
        SecTc *t = &sec_tc[(id + 1) & (SEC_TC - 1)];
        if (t->res != res || t->sid1 != id + 1) {
            sec_tc_merge(t);
            memset(t, 0, sizeof *t);
            t->res = res;
            t->sid1 = id + 1;
        }
        t->bank = s->bank;
        t->hi = s->hi;
    }
    sec_unlock(res);
    return id;
}

static pthread_mutex_t live_mtx = PTHREAD_MUTEX_INITIALIZER;

static void ctx_grew(DpSection *s) {
    // the liveness mask grew: previously evicted contexts may now differ
    dp_set_free(&s->ctx_seen);
    dp_set_init(&s->ctx_seen, 4096);
}

// The live_* entry points run per memory access, from several drain workers or
// sessions at once.  The common case (bit already set) takes no lock; a change
// takes live_mtx, rechecks, and rebuilds ctx_seen once, never twice.
static void live_set(DpSection *s, uint64_t *mask, uint64_t bit) {
    if (__atomic_load_n(mask, __ATOMIC_RELAXED) & bit) return;
    pthread_mutex_lock(&live_mtx);
    if (!(*mask & bit)) {
        __atomic_store_n(mask, *mask | bit, __ATOMIC_RELAXED);
        ctx_grew(s);
    }
    pthread_mutex_unlock(&live_mtx);
}

void dp_section_live_reg(DpSection *s, int bit) { live_set(s, &s->ctx_mask, 1ull << bit); }

void dp_section_live_wram(DpSection *s, uint32_t page) {
    if (page > 31) return;
    live_set(s, &s->page_mask_wram, 1ull << page);
}

void dp_section_live_vram(DpSection *s, uint32_t bank, uint32_t page) {
    if (page > 31) return;
    live_set(s, &s->page_mask_vram[bank & 1], 1ull << page);
}

void dp_section_live_sram(DpSection *s, uint32_t page) {
    if (page > 63) return;
    live_set(s, &s->page_mask_sram, 1ull << page);
}

void dp_section_live_small(DpSection *s, int what) {
    // what: 0 = hram, 1 = oam, 2 = ie, 3 = romN window, 4 = ramg, 5 = vbk
    switch (what) {
    case 0: case 1: {
        uint8_t *f = what ? &s->ctx_oam : &s->ctx_hram;
        if (__atomic_load_n(f, __ATOMIC_RELAXED)) break;
        pthread_mutex_lock(&live_mtx);
        if (!*f) { __atomic_store_n(f, 1, __ATOMIC_RELAXED); ctx_grew(s); }
        pthread_mutex_unlock(&live_mtx);
        break;
    }
    case 2: dp_section_live_reg(s, DP_CTX_IE); break;
    case 3: dp_section_live_reg(s, DP_CTX_ROMN); break;
    case 4: dp_section_live_reg(s, DP_CTX_RAMG); break;
    default: dp_section_live_reg(s, DP_CTX_VBK); break;
    }
}

void dp_section_live_io(DpSection *s, uint16_t io_off) {
    if (io_off >= 0x80) return;
    uint32_t bit = 1u << (io_off & 31); // the shift wrapped mod 32 on the targets in use; kept explicit
    if (__atomic_load_n(&s->io_read_mask, __ATOMIC_RELAXED) & bit) return;
    pthread_mutex_lock(&live_mtx);
    if (!(s->io_read_mask & bit)) {
        __atomic_store_n(&s->io_read_mask, s->io_read_mask | bit, __ATOMIC_RELAXED);
        ctx_grew(s);
    }
    pthread_mutex_unlock(&live_mtx);
}

void dp_section_io_used(DpSection *s, int slot) {
    if (slot < 0) return;
    for (int i = 0; i < s->io_used_n; i++)
        if (s->io_used[i] == (uint16_t)slot) return;
    if (s->io_used_n < 16) s->io_used[s->io_used_n++] = (uint16_t)slot;
}

void dp_section_banks_seen(DpSection *s, uint32_t bank) {
    for (int i = 0; i < s->banks_seen_n; i++)
        if (s->banks_seen[i] == bank) return;
    if (s->banks_seen_n < 8) s->banks_seen[s->banks_seen_n++] = bank;
}
