// sections.c: executed-range sections and their per-context bookkeeping.
// A section is a maximal contiguous run of executed addresses within one
// bank; every access an instruction makes is attributed to the section its
// pc belongs to, which is where the necessity statistics live.
#include "explorer.h"

MsSection *ms_section_touch(MsResults *res, uint8_t bank, uint16_t pc, uint16_t hi, int natural,
                            int is_entry, uint32_t cur_sec) {
    uint32_t key = ms_rom_index(bank, pc);
    uint32_t sid1 = res->section_of[key];
    MsSection *s;
    if (sid1) {
        s = &res->sections[sid1 - 1];
        if (s->bank == bank && (uint16_t)(s->hi + 1) >= pc && hi > s->hi) s->hi = hi;
    } else {
        MsSection *cur = cur_sec < (uint32_t)res->section_count ? &res->sections[cur_sec] : NULL;
        if (cur && cur->bank == bank && pc == (uint16_t)(cur->hi + 1)) {
            s = cur; // sequential execution continues the current run
            s->hi = hi;
        } else {
            if (res->section_count == res->section_cap) {
                res->section_cap = res->section_cap ? res->section_cap * 2 : 256;
                res->sections = realloc(res->sections, sizeof(MsSection) * (size_t)res->section_cap);
            }
            s = &res->sections[res->section_count];
            memset(s, 0, sizeof *s);
            s->id = (uint32_t)res->section_count++;
            s->bank = bank;
            s->lo = pc;
            s->hi = hi;
            ms_set_init(&s->ctx_seen, 4096);
        }
        res->section_of[key] = s->id + 1;
    }
    s->hits++;
    if (natural) s->natural_hits++;
    else s->forced_hits++;
    if (is_entry) s->entries++;
    s->kind_bits |= (uint8_t)(natural ? MS_XF_NATURAL : MS_XF_FORCED);
    return s;
}

static void ctx_grew(MsSection *s) {
    // the liveness mask grew: previously evicted contexts may now differ
    ms_set_free(&s->ctx_seen);
    ms_set_init(&s->ctx_seen, 4096);
}

void ms_section_live_reg(MsSection *s, int bit) {
    uint64_t old = s->ctx_mask;
    s->ctx_mask |= 1ull << bit;
    if (s->ctx_mask != old) ctx_grew(s);
}

void ms_section_live_wram(MsSection *s, uint32_t page) {
    if (page > 31) return;
    uint64_t old = s->page_mask_wram;
    s->page_mask_wram |= 1ull << page;
    if (s->page_mask_wram != old) ctx_grew(s);
}

void ms_section_live_vram(MsSection *s, uint32_t bank, uint32_t page) {
    if (page > 31) return;
    bank &= 1;
    uint64_t old = s->page_mask_vram[bank];
    s->page_mask_vram[bank] |= 1ull << page;
    if (s->page_mask_vram[bank] != old) ctx_grew(s);
}

void ms_section_live_sram(MsSection *s, uint32_t page) {
    if (page > 63) return;
    uint64_t old = s->page_mask_sram;
    s->page_mask_sram |= 1ull << page;
    if (s->page_mask_sram != old) ctx_grew(s);
}

void ms_section_live_small(MsSection *s, int what) {
    // what: 0 = hram, 1 = oam, 2 = ie, 3 = romN window, 4 = ramg, 5 = vbk
    switch (what) {
    case 0: if (!s->ctx_hram) { s->ctx_hram = 1; ctx_grew(s); } break;
    case 1: if (!s->ctx_oam) { s->ctx_oam = 1; ctx_grew(s); } break;
    case 2: ms_section_live_reg(s, MS_CTX_IE); break;
    case 3: ms_section_live_reg(s, MS_CTX_ROMN); break;
    case 4: ms_section_live_reg(s, MS_CTX_RAMG); break;
    default: ms_section_live_reg(s, MS_CTX_VBK); break;
    }
}

void ms_section_live_io(MsSection *s, uint16_t io_off) {
    uint32_t old = s->io_read_mask;
    if (io_off < 0x80) s->io_read_mask |= 1u << io_off;
    if (s->io_read_mask != old) ctx_grew(s);
}

void ms_section_io_used(MsSection *s, int slot) {
    if (slot < 0) return;
    for (int i = 0; i < s->io_used_n; i++)
        if (s->io_used[i] == (uint16_t)slot) return;
    if (s->io_used_n < 16) s->io_used[s->io_used_n++] = (uint16_t)slot;
}

void ms_section_banks_seen(MsSection *s, uint32_t bank) {
    for (int i = 0; i < s->banks_seen_n; i++)
        if (s->banks_seen[i] == bank) return;
    if (s->banks_seen_n < 8) s->banks_seen[s->banks_seen_n++] = bank;
}
