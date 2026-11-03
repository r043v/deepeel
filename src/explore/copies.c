// copies.c: ROM→VRAM/OAM copy-loop detection.
// Hardware channels (OAM DMA; HDMA once a CGB runtime exists) produce records directly from the
// platform hooks; this tracker catches the CPU loops: a backward edge whose
// body reads a ROM range and writes a VRAM range with drifting pointers.
// While the ranges accumulate, it also hashes the bytes flowing through both
// sides: a record whose read and written bytes hash identically over equal
// counts is marked verbatim — that is the proof the ROM source is pure data
// (tile map, tiles), not the input of some decompression or transformation.
#include "explorer.h"

#define FNV0 0xcbf29ce484222325ull

void dp_copy_reset(DpCopyTracker *ct) {
    memset(ct, 0, sizeof *ct);
}

static void flush(DpExplorer *ex, DpCopyTracker *ct, int force);

// a touch outside the accumulation window restarts it; what the loop had
// accumulated so far is emitted first (same >=2-iteration rule as the flush at
// path end) — a long session (a whole TAS) used to drop every copy that an IRQ
// or the next routine interrupted, keeping only the copies of short fork paths
static void window_break(DpExplorer *ex, DpCopyTracker *ct) {
    if (ct->src_valid && ct->dst_valid && ct->iter >= 2) {
        flush(ex, ct, 0);
        __atomic_add_fetch(&ex->res->copy_window_breaks, 1, __ATOMIC_RELAXED);
    }
}

static void rom_touch(DpExplorer *ex, DpCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v) {
    if (!ct->src_valid || ct->src_bank != bank || addr < (uint16_t)(ct->src_lo - 0x80) ||
        addr > (uint16_t)(ct->src_hi + 0x80) || ct->src_lo > (uint16_t)(addr + 0x80)) {
        if (ct->src_valid) window_break(ex, ct);
        ct->src_valid = 1;
        ct->src_bank = bank;
        ct->src_lo = ct->src_hi = addr;
        ct->src_hash = FNV0;
        ct->src_n = 0;
    } else {
        if (addr < ct->src_lo) ct->src_lo = addr;
        if (addr > ct->src_hi) ct->src_hi = addr;
    }
    ct->src_hash = (ct->src_hash ^ v) * 0x100000001b3ull;
    ct->src_n++;
}

static void vram_touch(DpExplorer *ex, DpCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v) {
    if (!ct->dst_valid || ct->dst_bank != bank || addr < (uint16_t)(ct->dst_lo - 0x80) ||
        addr > (uint16_t)(ct->dst_hi + 0x80) || ct->dst_lo > (uint16_t)(addr + 0x80)) {
        if (ct->dst_valid) window_break(ex, ct);
        ct->dst_valid = 1;
        ct->dst_bank = bank;
        ct->dst_lo = ct->dst_hi = addr;
        ct->dst_hash = FNV0;
        ct->dst_n = 0;
    } else {
        if (addr < ct->dst_lo) ct->dst_lo = addr;
        if (addr > ct->dst_hi) ct->dst_hi = addr;
    }
    ct->dst_hash = (ct->dst_hash ^ v) * 0x100000001b3ull;
    ct->dst_n++;
}

void dp_copy_on_rom_read(DpExplorer *ex, uint8_t bank, uint16_t addr, uint8_t v) {
    rom_touch(ex, &ex->ct, bank, addr, v);
}

void dp_copy_on_vram_write(DpExplorer *ex, uint8_t bank, uint16_t addr, uint8_t v) {
    vram_touch(ex, &ex->ct, bank, addr, v);
}

void dp_copy_on_back_edge(DpCopyTracker *ct, uint16_t site, uint32_t site_key) {
    if (ct->loop_active && ct->loop_pc == site) {
        ct->iter++;
    } else {
        ct->loop_active = 1;
        ct->loop_pc = site;
        ct->loop_key = site_key;
        ct->iter = 1;
    }
}

// single entry point for every copy record (hardware channels and CPU loops):
// appends, marks the ROM source as read+copied (verbatim records additionally
// as proven data), attributes to the current section and to the IO register
// that triggered hardware channels
void dp_record_copy(DpExplorer *ex, const DpCopyRec *rec, uint32_t attrib_key) {
    DpResults *res = ex->res;
    // the append+grow is one critical section: two session workers hitting the
    // growth window together used to swap reallocs and corrupt the heap
    if (res->mt) pthread_mutex_lock(&res->rec_mtx);
    if (res->copy_count >= (int)ex->cfg.max_copies) {
        res->copies_dropped++; // saturation: counted, never silent
        if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
        return;
    }
    if (res->copy_count == res->copy_cap) {
        res->copy_cap = res->copy_cap ? res->copy_cap * 2 : 64;
        res->copies = realloc(res->copies, sizeof(DpCopyRec) * (size_t)res->copy_cap);
    }
    res->copies[res->copy_count++] = *rec;
    if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
    ex->credit_copies++;
    if (rec->src_space == DP_SPACE_ROM) {
        uint32_t len = (uint32_t)(rec->src_hi - rec->src_lo + 1);
        uint32_t base = dp_rom_index(rec->src_bank, rec->src_lo);
        uint8_t bit = (uint8_t)(DP_DF_COPIED | (rec->verbatim ? DP_DF_VERBATIM : 0));
        for (uint32_t i = 0; i < len; i++) {
            if (base + i >= (uint32_t)res->banks * 0x4000) break;
            uint8_t old = __atomic_or_fetch(&res->data_flags[base + i], bit, __ATOMIC_RELAXED);
            if (!(old & bit)) ex->credit_data++;
            dp_cov_mark(&res->first, res->src_flags, base + i, DP_SRC_COPY, res->steps_total, 0);
            __atomic_add_fetch(&res->data_hits[base + i], 1, __ATOMIC_RELAXED);
        }
    }
    if (res->mt) pthread_mutex_lock(&res->sec_mtx);
    DpSection *s = NULL;
    if (attrib_key && attrib_key < (uint32_t)res->banks * 0x4000 && res->section_of[attrib_key])
        s = &res->sections[res->section_of[attrib_key] - 1]; // the loop's own section
    else if (ex->cur_sec < (uint32_t)res->section_count)
        s = &res->sections[ex->cur_sec]; // hardware channel: the writing section
    if (s) s->copies++;
    if (res->mt) pthread_mutex_unlock(&res->sec_mtx);
    if (rec->channel != 0 && ex->last_io_slot >= 0) __atomic_add_fetch(&res->io[ex->last_io_slot].copies, 1, __ATOMIC_RELAXED);
}

// verbatim verdict for the accumulated window: same byte count on both sides,
// equal range lengths, identical content hashes
static int tracker_verbatim(const DpCopyTracker *ct) {
    if (ct->src_n != ct->dst_n || !ct->src_n) return 0;
    if ((uint32_t)(ct->src_hi - ct->src_lo + 1) != (uint32_t)(ct->dst_hi - ct->dst_lo + 1)) return 0;
    return ct->src_hash == ct->dst_hash;
}

// emits and clears accumulated ranges once a loop has iterated enough
static void flush(DpExplorer *ex, DpCopyTracker *ct, int force) {
    if (ct->src_valid && ct->dst_valid && (force || ct->iter >= 2)) {
        DpResults *res = ex->res;
        if (res->copy_count >= (int)ex->cfg.max_copies) {
            __atomic_add_fetch(&res->copies_dropped, 1, __ATOMIC_RELAXED);
        } else {
            uint8_t verb = (uint8_t)tracker_verbatim(ct);
            uint32_t bytes = (uint32_t)(ct->src_hi - ct->src_lo + 1);
            if (res->mt) pthread_mutex_lock(&res->rec_mtx);
            // coalesce with a contiguous or duplicate previous record
            DpCopyRec *co = NULL;
            int dup = 0;
            for (int i = res->copy_count - 1; i >= 0 && i >= res->copy_count - 16; i--) {
                DpCopyRec *r = &res->copies[i];
                if (r->channel != 0 || r->src_bank != ct->src_bank || r->dst_bank != ct->dst_bank) continue;
                if ((uint16_t)(r->src_hi + 1) == ct->src_lo && (uint16_t)(r->dst_hi + 1) == ct->dst_lo) {
                    co = r;
                    break;
                }
                if (r->src_lo == ct->src_lo && r->src_hi == ct->src_hi && r->dst_lo == ct->dst_lo &&
                    r->dst_hi == ct->dst_hi) {
                    dup = 1; // same ranges again: nothing new to record
                    break;
                }
            }
            if (dup) {
                if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
            } else if (co) {
                co->src_hi = ct->src_hi;
                co->dst_hi = ct->dst_hi;
                co->bytes += bytes;
                co->verbatim &= verb; // extended only if every part is verbatim
                if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
                // mark the whole extended source range (first-flip credit
                // only counts bytes that were not marked before)
                if (co->src_space == DP_SPACE_ROM) {
                    uint32_t len = (uint32_t)(co->src_hi - co->src_lo + 1);
                    uint32_t base = dp_rom_index(co->src_bank, co->src_lo);
                    uint8_t bit = (uint8_t)(DP_DF_COPIED | (co->verbatim ? DP_DF_VERBATIM : 0));
                    for (uint32_t i = 0; i < len; i++) {
                        uint32_t k = base + i;
                        if (k >= (uint32_t)res->banks * 0x4000) break;
                        uint8_t old = __atomic_or_fetch(&res->data_flags[k], bit, __ATOMIC_RELAXED);
                        if (!(old & bit)) ex->credit_data++;
                        dp_cov_mark(&res->first, res->src_flags, k, DP_SRC_COPY, res->steps_total, 0);
                        __atomic_add_fetch(&res->data_hits[k], 1, __ATOMIC_RELAXED);
                    }
                }
            } else {
                if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
                DpCopyRec r = { 0, verb, DP_SPACE_ROM, DP_SPACE_VRAM, ct->src_bank, ct->dst_bank,
                                ct->src_lo, ct->src_hi, ct->dst_lo, ct->dst_hi, bytes, ct->loop_pc };
                dp_record_copy(ex, &r, ct->loop_key);
            }
        }
    }
    if (force || ct->iter >= 2) {
        // ranges consumed; keep the loop site so a bigger loop re-accumulates
        ct->src_valid = ct->dst_valid = 0;
        ct->iter = 0;
        ct->src_hash = ct->dst_hash = FNV0;
        ct->src_n = ct->dst_n = 0;
    }
}

void dp_copy_flush_path(DpExplorer *ex, DpCopyTracker *ct) {
    flush(ex, ct, 1);
}

// --- copy-sibling inference ----------------------------------------------------
// A verbatim ROM->VRAM copy proves its source range is pure data.  When the
// copy's source address is itself stored as a word in a ROM table that the
// game read as data, the loader picked that entry out of the table: the
// table's other entries are the same loader's other sources (another level's
// tiles, another screen's map).  Each sibling entry gets the observed length,
// marked DP_DF_SIBLING — an inference, shown as inferred-data, and it never
// overrides an observation (the map gives every observed class priority).
// Bounded: tables of at most SIB_MAX_ENTRIES words, ranges clipped to the
// bank window, refusals (a sibling range crossing naturally executed code)
// and caps counted.
#define SIB_MAX_ENTRIES 64
#define SIB_MIN_LEN 16

static int sib_word_ok(const DpResults *res, uint32_t k, uint32_t n) {
    if (k + 1 >= n) return 0;
    for (int i = 0; i < 2; i++) {
        if (res->exec_flags[k + i] & (DP_XF_NATURAL | DP_XF_FORCED | DP_XF_STATIC)) return 0;
        if (!(res->data_flags[k + i] & (DP_DF_READ | DP_DF_STATIC)) &&
            !(res->static_flags[k + i] & DP_SF_POINTER))
            return 0;
    }
    return 1;
}

// window of a CPU address: 0 = fixed bank $0000-$3FFF, 1 = switchable $4000-$7FFF
static int sib_window(uint16_t a) { return a < 0x4000 ? 0 : (a < 0x8000 ? 1 : -1); }

uint32_t dp_copy_siblings(DpResults *res) {
    const DpCart *c = res->cart;
    uint32_t n = (uint32_t)res->banks * 0x4000;
    uint32_t marked = 0;
    if (!c || !c->rom || c->rom_size < n) return 0;
    // per-pass counters (the pass runs after every closure series): the
    // report shows the last pass, sib_bytes stays the cumulative credit
    res->sib_tables = res->sib_ranges = res->sib_refused = res->sib_caps = 0;
    DpHashSet seen; // (table start, length) pairs already applied
    dp_set_init(&seen, 1 << 12);
    for (int r = 0; r < res->copy_count; r++) {
        const DpCopyRec *rec = &res->copies[r];
        if (rec->src_space != DP_SPACE_ROM || !rec->verbatim) continue;
        uint32_t len = (uint32_t)(rec->src_hi - rec->src_lo + 1);
        int win = sib_window(rec->src_lo);
        if (len < SIB_MIN_LEN || win < 0) continue;
        uint8_t lo = (uint8_t)rec->src_lo, hi = (uint8_t)(rec->src_lo >> 8);
        // the tables that may hold the word: the fixed bank and the copy's own bank
        for (int tb = 0; tb < 2; tb++) {
            uint32_t tbank = tb ? rec->src_bank : 0;
            if (tb && tbank == 0) break;
            if (tbank >= (uint32_t)res->banks) break;
            uint32_t bs = tbank * 0x4000, be = bs + 0x4000;
            for (uint32_t k = bs; k + 1 < be; k++) {
                if (c->rom[k] != lo || c->rom[k + 1] != hi || !sib_word_ok(res, k, n)) continue;
                // the table: contiguous words, same window, read or pointer-flagged
                uint32_t t0 = k, t1 = k + 2;
                int entries = 1;
                while (t0 >= bs + 2 && sib_word_ok(res, t0 - 2, n) &&
                       sib_window((uint16_t)(c->rom[t0 - 2] | c->rom[t0 - 1] << 8)) == win &&
                       entries < SIB_MAX_ENTRIES) {
                    t0 -= 2;
                    entries++;
                }
                while (t1 + 1 < be && sib_word_ok(res, t1, n) &&
                       sib_window((uint16_t)(c->rom[t1] | c->rom[t1 + 1] << 8)) == win &&
                       entries < SIB_MAX_ENTRIES) {
                    t1 += 2;
                    entries++;
                }
                if (entries >= SIB_MAX_ENTRIES) res->sib_caps++;
                if (entries < 2) continue; // a lone word is not a table
                uint64_t key = ((uint64_t)t0 << 20) ^ ((uint64_t)len << 4) ^ rec->src_bank;
                if (dp_set_insert(&seen, key) <= 0) continue;
                res->sib_tables++;
                for (uint32_t e = t0; e < t1; e += 2) {
                    uint16_t v = (uint16_t)(c->rom[e] | c->rom[e + 1] << 8);
                    if (v == rec->src_lo) continue;
                    uint32_t vbank = win ? rec->src_bank : 0;
                    uint32_t base = dp_rom_index((int)vbank, v);
                    uint32_t wend = vbank * 0x4000 + 0x4000;
                    uint32_t end = base + len > wend ? wend : base + len;
                    // a sibling range over naturally executed code is no data source
                    int natural = 0;
                    for (uint32_t i = base; i < end && !natural; i++)
                        if (res->exec_flags[i] & DP_XF_NATURAL) natural = 1;
                    if (natural) { res->sib_refused++; continue; }
                    res->sib_ranges++;
                    for (uint32_t i = base; i < end; i++) {
                        if (!(res->data_flags[i] & DP_DF_SIBLING)) marked++;
                        res->data_flags[i] |= DP_DF_SIBLING;
                        dp_cov_mark(&res->first, res->src_flags, i, DP_SRC_SIBLING, res->steps_total, 0);
                    }
                }
            }
        }
    }
    dp_set_free(&seen);
    res->sib_bytes += marked;
    return marked;
}
