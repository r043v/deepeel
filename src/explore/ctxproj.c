// ctxproj.c: the context projection of a section (G/C1: which incoming state
// components its behaviour depends on).
#include "explorer.h"
#include "explorer_priv.h"
#include "../core/dpmachine.h"
#include "tree.h"
#include <string.h>
#include <stdlib.h>

// context projection: hash of the incoming state components this section reads
// (the live machine holds the state being entered).  Cartridge RAM pages are
// hashed raw: that folds the RAM bank and the RAMG gate into one proof.
uint64_t project_ctx(DpExplorer *ex, const DpSection *s) {
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
