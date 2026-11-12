// pce_dsy: headless m0boy PC Engine runner writing the DSY1 work RAM (8 KiB, deepeel's $2000-$3FFF) per frame,
// the honest DUT side of `deepeel desync` against bizhawk_dsy pce (TurboNyma).
//   cc -O2 -std=gnu99 -DM0_NO_INPUT_PROBE -DM0_TL= -I$M0/targets/pce/runtime -I$M0/targets/common/runtime \
//      tools/pce_dsy.c $M0/targets/pce/runtime/{pce,huc6280,vdc,psg}.c -lm -o build/pce_dsy
//   pce_dsy ROM.pce masks.bin out.dsy [N]    masks: 1 byte/frame, bits up,down,left,right,b(II),a(I),select,start(run)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pce.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: pce_dsy ROM masks out.dsy [N]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); long skip = size % 131072 == 128 ? 128 : 0;
    fseek(f, skip, SEEK_SET);
    uint32_t rb = (uint32_t)((size - skip + 0x1FFF) & ~0x1FFFl);
    uint8_t *rom = calloc(1, rb);
    if (fread(rom, 1, (size_t)(size - skip), f) == 0) { fprintf(stderr, "truncated\n"); return 2; }
    fclose(f);
    FILE *mf = fopen(argv[2], "rb"); if (!mf) { perror(argv[2]); return 2; }
    uint8_t *mk = malloc(1 << 22); size_t nm = fread(mk, 1, 1 << 22, mf); fclose(mf);
    size_t n = argc > 4 ? (size_t)atol(argv[4]) : nm; if (n > nm) n = nm;
    FILE *o = fopen(argv[3], "wb"); if (!o) { perror(argv[3]); return 2; }
    fwrite("DSY1", 1, 4, o);
    pce_init(rom, rb);
    if (getenv("PCE_SATOFF")) pce_sat_offset = atoi(getenv("PCE_SATOFF"));
    if (getenv("PCE_MYSW")) { extern unsigned pce_mys_w; pce_mys_w = (unsigned)atoi(getenv("PCE_MYSW")); }
    if (getenv("PCE_MYS")) { extern unsigned pce_mys_off; pce_mys_off = (unsigned)atoi(getenv("PCE_MYS")); }
    if (getenv("PCE_SCALE")) pce_scale_permille = (unsigned)atoi(getenv("PCE_SCALE"));
    if (getenv("PCE_CUT")) { extern unsigned pce_cut_cycles; pce_cut_cycles = (unsigned)atoi(getenv("PCE_CUT")); }
    if (getenv("PCE_BOOT")) { extern int pce_boot_offset; pce_boot_offset = atoi(getenv("PCE_BOOT")); }
    pce_frame_start_line = getenv("PCE_PHASE") ? (unsigned)atoi(getenv("PCE_PHASE")) : 258;   /* TurboNyma cuts its frames at about this VDC line (the boot-time counters of frames 4-6 match from 258) */
    for (int pre = getenv("PCE_PRE") ? atoi(getenv("PCE_PRE")) : 1; pre > 0; pre--) { machine.input = 0; pce_frame(); }   /* extra boot frames before frame 1 (reference alignment) */
    const int indelay = getenv("PCE_INDELAY") ? atoi(getenv("PCE_INDELAY")) : 0;
    for (size_t i = 0; i < n; i++) {
        unsigned m = (i >= (size_t)indelay) ? mk[i - indelay] : 0, k = 0;   /* TurboNyma applies the movie's input one frame late */   // masks -> pce.input: I II Select Run Up Right Down Left = bits 0..7
        if (m & 0x20) k |= PCE_BTN_I; if (m & 0x10) k |= PCE_BTN_II; if (m & 0x40) k |= PCE_BTN_SELECT; if (m & 0x80) k |= PCE_BTN_RUN;
        if (m & 1) k |= 0x10; if (m & 8) k |= 0x20; if (m & 2) k |= 0x40; if (m & 4) k |= 0x80;
        machine.input = (uint8_t)k;
        { extern int pce_trace; int a = getenv("PCE_TRACE") ? atoi(getenv("PCE_TRACE")) : 0; pce_trace = a && (int)i + 1 >= a && (int)i + 1 <= a + (getenv("PCE_TRACE_N") ? atoi(getenv("PCE_TRACE_N")) : 1) - 1; if (pce_trace) fprintf(stderr, "FRAME %zu\n", i + 1); }
        pce_frame();
        if (getenv("PCE_SP")) fprintf(stderr, "SP %zu %02x\n", i + 1, machine.cpu.s);
        uint32_t fn = (uint32_t)(i + 1), len = 0x2000; uint16_t nb = 1; uint8_t nl = 4;
        fwrite(&fn, 4, 1, o); fwrite(&nb, 2, 1, o); fwrite(&nl, 1, 1, o); fwrite("wram", 1, 4, o);
        fwrite(&len, 4, 1, o); { extern uint8_t pce_cut_wram[]; extern int pce_cut_valid; fwrite(pce_cut_valid ? pce_cut_wram : machine.wram, 1, 0x2000, o); }
    }
    if (getenv("PCE_STATS")) { extern unsigned pce_stat_waits; fprintf(stderr, "vdc waits: %u\n", pce_stat_waits); }
    fclose(o);
    return 0;
}
