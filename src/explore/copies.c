// copies.c: ROM→VRAM/OAM copy-loop detection.
// Hardware channels (OAM DMA, HDMA) produce records directly from the
// platform hooks; this tracker catches the CPU loops: a backward edge whose
// body reads a ROM range and writes a VRAM range with drifting pointers.
#include "explorer.h"

void ms_copy_reset(MsCopyTracker *ct) {
    memset(ct, 0, sizeof *ct);
}

static void rom_touch(MsCopyTracker *ct, uint8_t bank, uint16_t addr) {
    if (!ct->src_valid || ct->src_bank != bank || addr < (uint16_t)(ct->src_lo - 0x80) ||
        addr > (uint16_t)(ct->src_hi + 0x80) || ct->src_lo > (uint16_t)(addr + 0x80)) {
        ct->src_valid = 1;
        ct->src_bank = bank;
        ct->src_lo = ct->src_hi = addr;
    } else {
        if (addr < ct->src_lo) ct->src_lo = addr;
        if (addr > ct->src_hi) ct->src_hi = addr;
    }
}

static void vram_touch(MsCopyTracker *ct, uint8_t bank, uint16_t addr) {
    if (!ct->dst_valid || ct->dst_bank != bank || addr < (uint16_t)(ct->dst_lo - 0x80) ||
        addr > (uint16_t)(ct->dst_hi + 0x80) || ct->dst_lo > (uint16_t)(addr + 0x80)) {
        ct->dst_valid = 1;
        ct->dst_bank = bank;
        ct->dst_lo = ct->dst_hi = addr;
    } else {
        if (addr < ct->dst_lo) ct->dst_lo = addr;
        if (addr > ct->dst_hi) ct->dst_hi = addr;
    }
}

void ms_copy_on_rom_read(MsCopyTracker *ct, uint8_t bank, uint16_t addr) {
    rom_touch(ct, bank, addr);
}

void ms_copy_on_vram_write(MsCopyTracker *ct, uint8_t bank, uint16_t addr) {
    vram_touch(ct, bank, addr);
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
// appends, marks the ROM source as read+copied, attributes to the current
// section and to the IO register that triggered hardware channels
void ms_record_copy(MsExplorer *ex, const MsCopyRec *rec, uint32_t attrib_key) {
    MsResults *res = ex->res;
    if (res->copy_count >= (int)ex->cfg.max_copies) return;
    if (res->copy_count == res->copy_cap) {
        res->copy_cap = res->copy_cap ? res->copy_cap * 2 : 64;
        res->copies = realloc(res->copies, sizeof(MsCopyRec) * (size_t)res->copy_cap);
    }
    res->copies[res->copy_count++] = *rec;
    if (rec->src_space == MS_SPACE_ROM) {
        uint32_t len = (uint32_t)(rec->src_hi - rec->src_lo + 1);
        uint32_t base = ms_rom_index(rec->src_bank, rec->src_lo);
        for (uint32_t i = 0; i < len; i++) {
            if (base + i >= (uint32_t)res->banks * 0x4000) break;
            res->data_flags[base + i] |= MS_DF_COPIED;
            res->data_hits[base + i]++;
        }
    }
    MsSection *s = NULL;
    if (attrib_key && attrib_key < (uint32_t)res->banks * 0x4000 && res->section_of[attrib_key])
        s = &res->sections[res->section_of[attrib_key] - 1]; // the loop's own section
    else if (ex->cur_sec < (uint32_t)res->section_count)
        s = &res->sections[ex->cur_sec]; // hardware channel: the writing section
    if (s) s->copies++;
    if (rec->channel != 0 && ex->last_io_slot >= 0) res->io[ex->last_io_slot].copies++;
}

// emits and clears accumulated ranges once a loop has iterated enough
static void flush(MsExplorer *ex, MsCopyTracker *ct, int force) {
    if (ct->src_valid && ct->dst_valid && (force || ct->iter >= 2)) {
        MsResults *res = ex->res;
        if (res->copy_count < (int)ex->cfg.max_copies) {
            // coalesce with a contiguous previous record
            MsCopyRec *co = NULL;
            for (int i = res->copy_count - 1; i >= 0 && i >= res->copy_count - 16; i--) {
                MsCopyRec *r = &res->copies[i];
                if (r->channel != 0 || r->src_bank != ct->src_bank || r->dst_bank != ct->dst_bank) continue;
                if ((uint16_t)(r->src_hi + 1) == ct->src_lo && (uint16_t)(r->dst_hi + 1) == ct->dst_lo) {
                    co = r;
                    break;
                }
                if (r->src_lo == ct->src_lo && r->src_hi == ct->src_hi && r->dst_lo == ct->dst_lo &&
                    r->dst_hi == ct->dst_hi) {
                    co = r; // same ranges again: nothing new to record
                    break;
                }
            }
            if (co && co->src_lo == ct->src_lo && co->src_hi == ct->src_hi && co->dst_lo == ct->dst_lo &&
                co->dst_hi == ct->dst_hi)
                co = (MsCopyRec *)-1; // duplicate marker
            uint32_t bytes = (uint32_t)(ct->src_hi - ct->src_lo + 1);
            if (co == (MsCopyRec *)-1) {
                // duplicate: drop
            } else if (co) {
                co->src_hi = ct->src_hi;
                co->dst_hi = ct->dst_hi;
                co->bytes += bytes;
            } else {
                MsCopyRec r = { 0, MS_SPACE_ROM, MS_SPACE_VRAM, ct->src_bank, ct->dst_bank,
                                ct->src_lo, ct->src_hi, ct->dst_lo, ct->dst_hi, bytes, ct->loop_pc };
                ms_record_copy(ex, &r, ct->loop_key);
            }
        }
    }
    if (force || ct->iter >= 2) {
        // ranges consumed; keep the loop site so a bigger loop re-accumulates
        ct->src_valid = ct->dst_valid = 0;
        ct->iter = 0;
    }
}

void ms_copy_flush_path(MsExplorer *ex, MsCopyTracker *ct) {
    flush(ex, ct, 1);
}
