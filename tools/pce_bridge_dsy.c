// pce_bridge_dsy: the deepeel PCE bridge (src/platform/pce/pcemach.c, what the watch window runs) as a DSY1 writer,
// to score it against bizhawk_dsy without the window.   pce_bridge_dsy ROM.pce masks.bin out.dsy N [pcehawk]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pce.h"
int main(int argc, char **argv) {
    if (argc < 5) return 2;
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 2; fclose(f);
    uint8_t *mk = malloc(1 << 22); f = fopen(argv[2], "rb"); size_t nm = fread(mk, 1, 1 << 22, f); fclose(f);
    size_t N = (size_t)atol(argv[4]); if (N > nm) N = nm;
    if (!dp_pce_init(rom, n)) return 2;
    dp_pce_reference(argc > 5);
    { extern unsigned pce_irq_delay, pce_timer_extra, pce_timer_phase, pce_timer_clk; extern int pce_sat_offset;   // sweep knobs
      if (getenv("PCE_IRQD")) pce_irq_delay = (unsigned)atoi(getenv("PCE_IRQD"));
      if (getenv("PCE_TPH")) pce_timer_phase = (unsigned)atoi(getenv("PCE_TPH"));
      if (getenv("PCE_TCLK")) pce_timer_clk = (unsigned)atoi(getenv("PCE_TCLK"));
      if (getenv("PCE_TDIV")) { extern unsigned pce_timer_div; pce_timer_div = (unsigned)atoi(getenv("PCE_TDIV")); }
      if (getenv("PCE_TEXTRA")) pce_timer_extra = (unsigned)atoi(getenv("PCE_TEXTRA"));
      if (getenv("PCE_VSEXTRA")) { extern unsigned pce_vsync_extra; pce_vsync_extra = (unsigned)atoi(getenv("PCE_VSEXTRA")); }
      if (getenv("PCE_PHASE")) { extern void dp_pce_set_phase(unsigned); dp_pce_set_phase((unsigned)atoi(getenv("PCE_PHASE"))); }
      if (getenv("PCE_CUT")) { extern void dp_pce_set_cut(unsigned); dp_pce_set_cut((unsigned)atoi(getenv("PCE_CUT"))); }
      if (getenv("PCE_MYS")) { extern unsigned pce_mys_off; pce_mys_off = (unsigned)atoi(getenv("PCE_MYS")); }
      if (getenv("PCE_MYSW")) { extern unsigned pce_mys_w; pce_mys_w = (unsigned)atoi(getenv("PCE_MYSW")); }
      if (getenv("PCE_SATHDW")) { extern int pce_sat_hdw; pce_sat_hdw = atoi(getenv("PCE_SATHDW")); }
      if (getenv("PCE_SATOFF")) pce_sat_offset = atoi(getenv("PCE_SATOFF"));
      if (getenv("PCE_VDCWAITS")) { extern unsigned pce_vdc_waits; pce_vdc_waits = (unsigned)atoi(getenv("PCE_VDCWAITS")); }
      if (getenv("PCE_SATWAIT")) { extern unsigned pce_sat_waits; pce_sat_waits = (unsigned)atoi(getenv("PCE_SATWAIT")); } }
    FILE *o = fopen(argv[3], "wb"); fwrite("DSY1", 1, 4, o);
    for (size_t i = 0; i < N; i++) {
        unsigned m = mk[i], k = 0;
        if (m & 0x20) k |= 1; if (m & 0x10) k |= 2; if (m & 0x40) k |= 4; if (m & 0x80) k |= 8;
        if (m & 1) k |= 0x10; if (m & 8) k |= 0x20; if (m & 2) k |= 0x40; if (m & 4) k |= 0x80;
        dp_pce_input((uint8_t)k);
#ifdef PCE_TRACE
        { extern int pce_trace; const char *e = getenv("PCE_TRACE"); int a = e ? atoi(e) : 0, nn = getenv("PCE_TRACE_N") ? atoi(getenv("PCE_TRACE_N")) : 1; pce_trace = a && (int)i + 1 >= a && (int)i + 1 < a + nn; if (pce_trace) fprintf(stderr, "FRAME %zu\n", i + 1); }
#endif
        dp_pce_run_frame();
        uint32_t fn = (uint32_t)(i + 1), len = 0x2000; uint16_t nb = 1; uint8_t nl = 4;
        fwrite(&fn, 4, 1, o); fwrite(&nb, 2, 1, o); fwrite(&nl, 1, 1, o); fwrite("wram", 1, 4, o); fwrite(&len, 4, 1, o);
        { extern uint8_t *dp_pce_wram_cut(void); fwrite(dp_pce_wram_cut(), 1, 0x2000, o); }
        { static FILE *vo; if (!vo) { const char *ev = getenv("DSY_VRAM"); if (ev) vo = fopen(ev, "wb"); }   // DSY_VRAM: the whole VRAM after each frame (fn, 0x10000 bytes)
          if (vo) { extern uint8_t *dp_pce_vram_bytes(void); uint32_t vlen = 0x10000; fwrite(&fn, 4, 1, vo); fwrite(&vlen, 4, 1, vo); fwrite(dp_pce_vram_bytes(), 1, 0x10000, vo); } }
    }
    fclose(o); return 0;
}
