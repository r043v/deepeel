// gpgx_offset_probe: does a movie offset (blank frames prepended or frames skipped) change how far Sonic 1 gets in Genesis Plus GX?
// usage: gpgx_offset_probe CORE.so ROM masks OFFSET FRAMES   (prints zone/act/lives/score every 1000 frames)
#include "mach/libretro.h"
#include <stdio.h>
#include <stdlib.h>
static uint16_t to_retro(uint8_t m) { static const int id[8] = {4,5,6,7,0,8,1,3}; uint16_t r = 0; for (int i = 0; i < 8; i++) if (m & (1u << i)) r |= (uint16_t)(1u << id[i]); return r; }
int main(int c, char **v) {
    char err[256]; DpLibretro *lr = dp_lr_open(v[1], err, sizeof err); if (!lr || !dp_lr_load_rom(lr, v[2], err, sizeof err)) return 1;
    FILE *f = fopen(v[3], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *m = malloc(n); fread(m, 1, n, f); fclose(f);
    int off = atoi(v[4]); long frames = atol(v[5]);
    for (long fr = 0; fr < frames; fr++) {
        long k = fr - off; dp_lr_set_input(lr, 0, k >= 0 && k < n ? to_retro(m[k]) : 0); dp_lr_advance(lr, 1);
        if (fr % 1000 == 999) {
            size_t sz; const uint8_t *r = dp_lr_memory(lr, 2, &sz);
            if (r && sz >= 0x10000) printf("f%ld zone %u act %u lives %u score %u\n", fr + 1, r[0xFE10^1], r[0xFE11^1], r[0xFE12^1], (r[0xFE26^1] << 24 | r[0xFE27^1] << 16 | r[0xFE28^1] << 8 | r[0xFE29^1]) * 10);
        }
    }
    return 0;
}
