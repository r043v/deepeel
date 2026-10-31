// copies.c: ROM→VRAM/OAM copy-loop detection.
// Hardware channels (OAM DMA, HDMA) produce records directly from the
// platform hooks; this tracker catches the CPU loops: a backward edge whose
// body reads a ROM range and writes a VRAM range with drifting pointers.
// While the ranges accumulate, it also hashes the bytes flowing through both
// sides: a record whose read and written bytes hash identically over equal
// counts is marked verbatim — that is the proof the ROM source is pure data
// (tile map, tiles), not the input of some decompression or transformation.
#include "explorer.h"

#define FNV0 0xcbf29ce484222325ull

void ms_copy_reset(MsCopyTracker *ct) {
    memset(ct, 0, sizeof *ct);
}

static void rom_touch(MsCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v) {
    if (!ct->src_valid || ct->src_bank != bank || addr < (uint16_t)(ct->src_lo - 0x80) ||
        addr > (uint16_t)(ct->src_hi + 0x80) || ct->src_lo > (uint16_t)(addr + 0x80)) {
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

static void vram_touch(MsCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v) {
    if (!ct->dst_valid || ct->dst_bank != bank || addr < (uint16_t)(ct->dst_lo - 0x80) ||
        addr > (uint16_t)(ct->dst_hi + 0x80) || ct->dst_lo > (uint16_t)(addr + 0x80)) {
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

void ms_copy_on_rom_read(MsCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v) {
    rom_touch(ct, bank, addr, v);
}

void ms_copy_on_vram_write(MsCopyTracker *ct, uint8_t bank, uint16_t addr, uint8_t v) {
    vram_touch(ct, bank, addr, v);
}

void ms_copy_on_back_edge(MsCopyTracker *ct, uint16_t site, uint32_t site_key) {
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
void ms_record_copy(MsExplorer *ex, const MsCopyRec *rec, uint32_t attrib_key) {
    MsResults *res = ex->res;
    if (res->copy_count >= (int)ex->cfg.max_copies) return;
    if (res->copy_count == res->copy_cap) {
        res->copy_cap = res->copy_cap ? res->copy_cap * 2 : 64;
        res->copies = realloc(res->copies, sizeof(MsCopyRec) * (size_t)res->copy_cap);
    }
    res->copies[res->copy_count++] = *rec;
    ex->credit_copies++;
    if (rec->src_space == MS_SPACE_ROM) {
        uint32_t len = (uint32_t)(rec->src_hi - rec->src_lo + 1);
        uint32_t base = ms_rom_index(rec->src_bank, rec->src_lo);
        uint8_t bit = (uint8_t)(MS_DF_COPIED | (rec->verbatim ? MS_DF_VERBATIM : 0));
        for (uint32_t i = 0; i < len; i++) {
            if (base + i >= (uint32_t)res->banks * 0x4000) break;
            uint8_t old = __atomic_or_fetch(&res->data_flags[base + i], bit, __ATOMIC_RELAXED);
            if (!(old & bit)) ex->credit_data++;
            __atomic_add_fetch(&res->data_hits[base + i], 1, __ATOMIC_RELAXED);
        }
    }
    if (res->mt) pthread_mutex_lock(&res->sec_mtx);
    MsSection *s = NULL;
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
static int tracker_verbatim(const MsCopyTracker *ct) {
    if (ct->src_n != ct->dst_n || !ct->src_n) return 0;
    if ((uint32_t)(ct->src_hi - ct->src_lo + 1) != (uint32_t)(ct->dst_hi - ct->dst_lo + 1)) return 0;
    return ct->src_hash == ct->dst_hash;
}

// emits and clears accumulated ranges once a loop has iterated enough
static void flush(MsExplorer *ex, MsCopyTracker *ct, int force) {
    if (ct->src_valid && ct->dst_valid && (force || ct->iter >= 2)) {
        MsResults *res = ex->res;
        if (res->copy_count < (int)ex->cfg.max_copies) {
            uint8_t verb = (uint8_t)tracker_verbatim(ct);
            uint32_t bytes = (uint32_t)(ct->src_hi - ct->src_lo + 1);
            if (res->mt) pthread_mutex_lock(&res->rec_mtx);
            // coalesce with a contiguous or duplicate previous record
            MsCopyRec *co = NULL;
            int dup = 0;
            for (int i = res->copy_count - 1; i >= 0 && i >= res->copy_count - 16; i--) {
                MsCopyRec *r = &res->copies[i];
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
                if (co->src_space == MS_SPACE_ROM) {
                    uint32_t len = (uint32_t)(co->src_hi - co->src_lo + 1);
                    uint32_t base = ms_rom_index(co->src_bank, co->src_lo);
                    uint8_t bit = (uint8_t)(MS_DF_COPIED | (co->verbatim ? MS_DF_VERBATIM : 0));
                    for (uint32_t i = 0; i < len; i++) {
                        uint32_t k = base + i;
                        if (k >= (uint32_t)res->banks * 0x4000) break;
                        uint8_t old = __atomic_or_fetch(&res->data_flags[k], bit, __ATOMIC_RELAXED);
                        if (!(old & bit)) ex->credit_data++;
                        __atomic_add_fetch(&res->data_hits[k], 1, __ATOMIC_RELAXED);
                    }
                }
            } else {
                if (res->mt) pthread_mutex_unlock(&res->rec_mtx);
                MsCopyRec r = { 0, verb, MS_SPACE_ROM, MS_SPACE_VRAM, ct->src_bank, ct->dst_bank,
                                ct->src_lo, ct->src_hi, ct->dst_lo, ct->dst_hi, bytes, ct->loop_pc };
                ms_record_copy(ex, &r, ct->loop_key);
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

void ms_copy_flush_path(MsExplorer *ex, MsCopyTracker *ct) {
    flush(ex, ct, 1);
}
