// nes_dsy: headless m0boy NES runner that writes the DSY1 work RAM per frame (the honest DUT side of
// `deepeel desync`; the explorer's own --dsy replay uses event-probe statics and does not run the PPU).
//   cc -O2 -std=gnu99 -DM0_NO_INPUT_PROBE -DM0_TL= -I$M0/targets/nes/runtime -I$M0/targets/common/runtime \
//      tools/nes_dsy.c $M0/targets/nes/runtime/{nes,6502,apu,ppu}.c -lm -o build/nes_dsy
//   nes_dsy ROM.nes masks.bin out.dsy [N]    masks: 1 byte/frame, bits up,down,left,right,b,a,select,start
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"

static uint16_t panel[160];
static void on_panel(void *h, unsigned r) { (void)h; (void)r; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: nes_dsy ROM masks out.dsy [N]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    uint8_t hd[16];
    if (fread(hd, 1, 16, f) != 16 || memcmp(hd, "NES\x1a", 4)) { fprintf(stderr, "not iNES\n"); return 2; }
    uint32_t pb = hd[4] * 16384u, cb = hd[5] * 8192u;
    uint8_t *prg = malloc(pb), *chr = malloc(cb ? cb : 8192);
    if (fread(prg, 1, pb, f) != pb || (cb && fread(chr, 1, cb, f) != cb)) { fprintf(stderr, "truncated\n"); return 2; }
    fclose(f);
    FILE *mf = fopen(argv[2], "rb"); if (!mf) { perror(argv[2]); return 2; }
    uint8_t *mk = malloc(1 << 22); size_t nm = fread(mk, 1, 1 << 22, mf); fclose(mf);
    size_t n = argc > 4 ? (size_t)atol(argv[4]) : nm; if (n > nm) n = nm;
    FILE *o = fopen(argv[3], "wb"); if (!o) { perror(argv[3]); return 2; }
    fwrite("DSY1", 1, 4, o);
    nes_init(prg, pb, chr, cb, hd[6] & 1);
    machine.ppu.panel = on_panel; machine.ppu.panel_out = panel;
    nes_ppu_set_rows(&machine.ppu, 128);
    for (size_t i = 0; i < n; i++) {
        unsigned m = mk[i], k = 0;   // masks -> NES pad: A B Select Start Up Down Left Right = bits 0..7
        if (m & 0x20) k |= 1; if (m & 0x10) k |= 2; if (m & 0x40) k |= 4; if (m & 0x80) k |= 8;
        if (m & 1) k |= 16; if (m & 2) k |= 32; if (m & 4) k |= 64; if (m & 8) k |= 128;
        machine.input = (uint8_t)k;
        nes_frame();
        uint32_t fn = (uint32_t)(i + 1), len = 0x800; uint16_t nb = 1; uint8_t nl = 4;
        fwrite(&fn, 4, 1, o); fwrite(&nb, 2, 1, o); fwrite(&nl, 1, 1, o); fwrite("wram", 1, 4, o);
        fwrite(&len, 4, 1, o); fwrite(machine.ram, 1, 0x800, o);
    }
    fclose(o);
    return 0;
}
