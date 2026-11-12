// lr_dsy: per-frame DSY1 producer (src/core/desync.h) for ANY libretro core: the reference side of `deepeel desync`
// for GB (gambatte), NES (mesen/fceumm), PCE (beetle-pce) ...  Emits "wram" (RETRO_MEMORY_SYSTEM_RAM), "vram" (id 3)
// and "sram" (id 0) when the core exposes them.
// usage: lr_dsy CORE.so ROM masks.bin out.dsy [FRAMES]
//   masks: one byte per frame, bits 0 Up 1 Down 2 Left 3 Right 4 B 5 A 6 Select 7 Start (libretro joypad ids B0 Select2 Start3 U4 D5 L6 R7 A8)
#include "mach/libretro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t to_retro(uint8_t m) {
    static const int id[8] = { 4, 5, 6, 7, 0, 8, 2, 3 };
    uint16_t r = 0;
    for (int i = 0; i < 8; i++) if (m & (1u << i)) r |= (uint16_t)(1u << id[i]);
    return r;
}
int main(int c, char **v) {
    if (c < 5) { fprintf(stderr, "usage: lr_dsy CORE.so ROM masks.bin out.dsy [FRAMES]\n"); return 2; }
    char err[256] = "";
    DpLibretro *lr = dp_lr_open(v[1], err, sizeof err);
    if (!lr) { fprintf(stderr, "open: %s\n", err); return 1; }
    if (!dp_lr_load_rom(lr, v[2], err, sizeof err)) { fprintf(stderr, "rom: %s\n", err); return 1; }
    FILE *f = fopen(v[3], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *masks = malloc((size_t)n + 1); if (fread(masks, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    if (c > 5 && atol(v[5]) > 0) n = atol(v[5]) < n ? atol(v[5]) : n;
    FILE *o = fopen(v[4], "wb"); if (!o) return 1;
    fwrite("DSY1", 1, 4, o);
    static const struct { int id; const char *name; } blk[3] = { { 2, "wram" }, { 3, "vram" }, { 0, "sram" } };
    for (long fr = 0; fr < n; fr++) {
        dp_lr_set_input(lr, 0, to_retro(masks[fr]));
        dp_lr_advance(lr, 1);
        const uint8_t *d[3]; size_t sz[3]; uint16_t nb = 0;
        for (int i = 0; i < 3; i++) { d[i] = dp_lr_memory(lr, blk[i].id, &sz[i]); if (d[i] && sz[i]) nb++; else d[i] = NULL; }
        uint32_t fn = (uint32_t)(fr + 1);
        fwrite(&fn, 4, 1, o); fwrite(&nb, 2, 1, o);
        for (int i = 0; i < 3; i++) if (d[i]) {
            uint8_t nl = (uint8_t)strlen(blk[i].name); uint32_t len = (uint32_t)sz[i];
            fwrite(&nl, 1, 1, o); fwrite(blk[i].name, 1, nl, o); fwrite(&len, 4, 1, o); fwrite(d[i], 1, len, o);
        }
    }
    fclose(o); dp_lr_close(lr);
    return 0;
}
