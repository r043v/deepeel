// core_gbseeds.c: entry discovery for the generic kernel on banked Game Boy ROMs.  Every little-endian ROM word that points
// at plausible code becomes a forced-path seed {bank, address}: the same pointer scan the GB explorer does (seeds.c), so the
// kernel can reach the code a natural path never branches to.  A word >= $4000 carries no bank evidence; a word flanked by
// two other $4000-window words is a dispatch-table entry, seeded in every switchable bank; an isolated one in bank 1.
#include "core.h"
#include <stdlib.h>
#include <string.h>

static int pad_at(const uint8_t *rom, size_t size, uint32_t bank, uint16_t addr) {
    size_t base = (size_t)bank * 0x4000 + (addr & 0x3FFF);
    uint8_t b0 = base < size ? rom[base] : 0xFF;
    if (b0 != 0x00 && b0 != 0xFF) return 0;
    for (int i = 1; i < 8; i++) { uint8_t b = base + (size_t)i < size ? rom[base + (size_t)i] : 0xFF; if (b != b0) return 0; }
    return 1;
}

size_t dp_core_gb_pointer_seeds(const uint8_t *rom, size_t size, size_t cap, uint32_t **pcs, uint16_t **banks) {
    unsigned nbanks = (unsigned)(size / 0x4000);
    uint8_t *seen = calloc(size ? size : 1, 1);   // by compact ROM index
    uint32_t *pc = malloc(cap * sizeof *pc); uint16_t *bk = malloc(cap * sizeof *bk);
    size_t n = 0;
    if (!seen || !pc || !bk) { free(seen); free(pc); free(bk); return 0; }
    for (unsigned bank = 0; bank < nbanks; bank++)
        for (uint32_t off = 0; off + 1 < 0x4000; off++) {
            size_t a = (size_t)bank * 0x4000 + off;
            if (a + 1 >= size) break;
            uint16_t v = (uint16_t)(rom[a] | (rom[a + 1] << 8));
            if (v < 0x0100 || v >= 0x8000) continue;
            uint16_t pv = a >= 2 ? (uint16_t)(rom[a - 2] | (rom[a - 1] << 8)) : 0;
            uint16_t nv = a + 3 < size ? (uint16_t)(rom[a + 2] | (rom[a + 3] << 8)) : 0;
            int tab = pv >= 0x4000 && pv < 0x8000 && nv >= 0x4000 && nv < 0x8000;
            unsigned nb = v < 0x4000 ? 1 : (tab ? nbanks : 2);
            for (unsigned tb = v < 0x4000 ? 0 : 1; tb < nb && tb < nbanks; tb++) {
                if (pad_at(rom, size, tb, v)) continue;
                size_t tk = (size_t)tb * 0x4000 + (v & 0x3FFF);
                if (tk >= size || seen[tk]) continue;
                seen[tk] = 1;
                if (n < cap) { pc[n] = v; bk[n] = (uint16_t)tb; n++; }
            }
        }
    free(seen);
    *pcs = pc; *banks = bk;
    return n;
}
