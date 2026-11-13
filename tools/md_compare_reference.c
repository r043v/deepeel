#define _POSIX_C_SOURCE 200809L
/* Standalone differential probe: run one raw GMV against the external
 * ClownMDEmu library and deepeel's m0boy bridge. The printed RAM offsets
 * describe Sonic 1 World Rev 00, not a platform-wide ABI. The cores expose
 * different frame boundaries, so differences are observations, not proofs
 * of the first divergent instruction. No reference implementation is copied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "clownmdemu.h"
#include "md_full.h"
void dp_mdrom_set(const uint8_t *, size_t);
static unsigned pad;
static cc_bool input(void *u, cc_u8f p, ClownMDEmu_Button b) {
    (void)u;
    unsigned n = b == CLOWNMDEMU_BUTTON_START ? 7 : (unsigned)b;
    return p == 0 && (b <= CLOWNMDEMU_BUTTON_C || b == CLOWNMDEMU_BUTTON_START) && ((pad >> n) & 1);
}
static void colour(void *u, cc_u16f i, cc_u16f c) { (void)u; (void)i; (void)c; }
static void line(void *u, cc_u16f y, const cc_u8l *p, cc_u16f l, cc_u16f r, cc_u16f w, cc_u16f h) {
    (void)u;(void)y;(void)p;(void)l;(void)r;(void)w;(void)h;
}
static void audio(void *u, ClownMDEmu *m, size_t n, void (*gen)(ClownMDEmu *, cc_s16l *, size_t)) {
    cc_s16l buf[2048]; (void)u;
    while (n) { size_t k = n > 1024 ? 1024 : n; gen(m, buf, k); n -= k; }
}
static uint8_t ref_byte(const ClownMDEmu *m, unsigned addr) {
    uint16_t word = m->state.m68k.ram[(addr & 0xffff) >> 1];
    return (addr & 1) ? word : word >> 8;
}
static unsigned ref_word(const ClownMDEmu *m, unsigned addr) {
    return ((unsigned)ref_byte(m, addr) << 8) | ref_byte(m, addr + 1);
}
static unsigned dut_word(const MdFull *m, unsigned addr) {
    return ((unsigned)m->ram[addr & 0xffff] << 8) | m->ram[(addr + 1) & 0xffff];
}
#ifdef DP_CYCLE_TRACE   /* needs an instrumented Clown68000 copy and a traced m0boy runtime */
#include "md_trace.h"
extern int dp_trace_on; extern FILE *dp_trace_f;      /* Clown68000 side (DEEPEEL_C68_TRACE build) */
int dp_trace_on_d; FILE *dp_trace_d;                  /* m0boy side, through the md_trace hooks */
static uint32_t dp_trace_c0;
static void dp_trace_step(void *u, MdFull *m, uint32_t pc, uint16_t op) { (void)u; (void)pc; (void)op; dp_trace_c0 = m->cycles; }
static void dp_trace_end(void *u, MdFull *m, uint32_t pc, uint32_t npc) { (void)u; (void)npc;
    if (dp_trace_on_d) fprintf(dp_trace_d, "%06x %u\n", (unsigned)pc, (unsigned)(m->cycles - dp_trace_c0)); }
#endif
int main(int argc, char **argv) {
    /* ClownMDEmu's own clock: the m0boy exact clock with a 3-master-clock phase tracks 1843S/1437S/s6k (13 800 frames)
     * where the default 3420-master line drifts by one frame near 1423; both still overridable from the environment. */
    setenv("DP_MD_CLOWN_CLOCK", "exact", 0); setenv("DP_MD_CLOCK_OFF", "3", 0);
    if (argc != 4) {
        fprintf(stderr, "usage: md_compare_reference ROM raw.gmv frames\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb"), *movie = fopen(argv[2], "rb");
    if (!f || !movie) { fprintf(stderr, "cannot open ROM or movie\n"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n < 512 || n > 0x400000 || (n & 1)) { fprintf(stderr, "invalid ROM size\n"); return 2; }
    uint8_t *rom = malloc(n); cc_u16l *words = malloc((n / 2) * sizeof *words);
    if (!rom || !words || fread(rom, 1, n, f) != (size_t)n) return 2;
    for (long i = 0; i < n / 2; i++) words[i] = (rom[2*i] << 8) | rom[2*i+1];
    fclose(f);
    unsigned char header[64];
    if (fread(header, 1, sizeof header, movie) != sizeof header || memcmp(header, "Gens Movie ", 11) ||
        (header[20] != '3' && header[20] != '6') || (header[21] != '3' && header[21] != '6') || (header[15] >= 'A' && (header[22] & 0xe0))) {
        fprintf(stderr, "unsupported GMV\n"); return 2;
    }
    ClownMDEmu *ref = calloc(1, sizeof *ref); MdFull *dut = calloc(1, sizeof *dut);
    ClownMDEmu_InitialConfiguration config = {0}; ClownMDEmu_Callbacks cb = {0};
    config.general.region = CLOWNMDEMU_REGION_OVERSEAS;
    config.general.tv_standard = CLOWNMDEMU_TV_STANDARD_NTSC;
    cb.input_requested = input; cb.colour_updated = colour; cb.scanline_rendered = line;
    cb.fm_audio_to_be_generated = audio; cb.psg_audio_to_be_generated = audio;
    cb.pcm_audio_to_be_generated = audio; cb.cdda_audio_to_be_generated = audio;
    ClownMDEmu_Constant_Initialise(); ClownMDEmu_Initialise(ref, &config, &cb);
    ClownMDEmu_SetCartridge(ref, words, n/2); ClownMDEmu_HardReset(ref, 1, 0);
    dp_mdrom_set(rom, n); md_full_reset(dut);
    unsigned limit = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned trace_begin = 1, trace_end = 0;
    const char *range = getenv("DP_MD_TRACE_RANGE");
    if (range && sscanf(range, "%u:%u", &trace_begin, &trace_end) != 2) {
        fprintf(stderr, "DP_MD_TRACE_RANGE must be start:end\n"); return 2;
    }
    unsigned prev_ref_x = 0, prev_dut_x = 0, mismatch_streak = 0;
    for (unsigned frame = 0; frame < limit; frame++) {
        unsigned char b[3]; if (fread(b, 1, 3, movie) != 3) break;
        if (b[1] != 0xff || b[2] != 0xff) { fprintf(stderr, "unsupported GMV input at frame %u\n", frame); return 2; }
        pad = (uint8_t)~b[0]; dut->pad = pad;
#ifdef DP_CYCLE_TRACE
        if (getenv("DP_MD_CYCLE_TRACE") && frame + 1 == (unsigned)atoi(getenv("DP_MD_CYCLE_TRACE"))) {
            dp_trace_f = fopen("cyc_ref.txt", "w"); dp_trace_d = fopen("cyc_dut.txt", "w"); dp_trace_on = dp_trace_on_d = 1; md_trace.step = dp_trace_step; md_trace.end = dp_trace_end; }
#endif
        ClownMDEmu_Iterate(ref); md_full_frame(dut, 0);
#ifdef DP_CYCLE_TRACE
        if (dp_trace_on) { dp_trace_on = dp_trace_on_d = 0; fclose(dp_trace_f); fclose(dp_trace_d); }
#endif
        if (getenv("DP_MD_ENDSTATE") && frame + 1 >= trace_begin && frame + 1 <= trace_end)   /* CPU state at the end of each frame */
            printf("end %u pc=%06x/%06x sr=%04x/%04x a7=%08x/%08x d0=%08x/%08x frame_cycles(dut)=%u\n", frame + 1,
                   (unsigned)ref->m68k.program_counter, dut->pc, (unsigned)ref->m68k.status_register, (unsigned)dut->sr,
                   (unsigned)ref->m68k.address_registers[7], dut->a[7], (unsigned)ref->m68k.data_registers[0], dut->d[0], (unsigned)dut->frame_cycles);
        if (frame + 1 >= trace_begin && frame + 1 <= trace_end)
            printf("detail %u pad=%02x x=%04x/%04x y=%04x/%04x vx=%04x/%04x vy=%04x/%04x inertia=%04x/%04x status=%02x/%02x\n",
                   frame + 1, pad, ref_word(ref, 0xd008), dut_word(dut, 0xd008),
                   ref_word(ref, 0xd00c), dut_word(dut, 0xd00c), ref_word(ref, 0xd010), dut_word(dut, 0xd010),
                   ref_word(ref, 0xd012), dut_word(dut, 0xd012), ref_word(ref, 0xd014), dut_word(dut, 0xd014),
                   ref_byte(ref, 0xd022), dut->ram[0xd022]);
        if (getenv("DP_MD_MODE_TRACE") && ref_byte(ref, 0xf600) != dut->ram[0xf600] && frame < 600)
            printf("mode-diff %u ref=%02x dut=%02x\n", frame + 1, ref_byte(ref, 0xf600), dut->ram[0xf600]);
        if (getenv("DP_MD_MODE_TRACE")) { static unsigned pr = 0xff, pd = 0xff; unsigned r = ref_byte(ref, 0xf600), d = dut->ram[0xf600];
            if (r != pr || d != pd) printf("mode-change %u ref=%02x dut=%02x\n", frame + 1, r, d); pr = r; pd = d; }
        unsigned ref_x = ((unsigned)ref_byte(ref, 0xd008) << 8) | ref_byte(ref, 0xd009);
        unsigned dut_x = ((unsigned)dut->ram[0xd008] << 8) | dut->ram[0xd009];
        if (ref_byte(ref, 0xf600) == 0x0c && dut->ram[0xf600] == 0x0c &&
            ref_x && dut_x && ref_x != dut_x && ref_x != prev_dut_x && dut_x != prev_ref_x) mismatch_streak++;
        else mismatch_streak = 0;
        prev_ref_x = ref_x; prev_dut_x = dut_x;
        if (getenv("DP_MD_PC_DIFF")) { static int shown3; if (shown3 < 12 && (unsigned)ref->m68k.program_counter != dut->pc) { shown3++; printf("pc-diff frame %u: pc=%06x/%06x sr=%04x/%04x\n", frame + 1, (unsigned)ref->m68k.program_counter, dut->pc, (unsigned)ref->m68k.status_register, (unsigned)dut->sr); } }
        if (getenv("DP_MD_STACK_DIFF")) { static int shown2; unsigned nd = 0, first = 0;
            for (unsigned i = (getenv("DP_MD_DEEP") ? 0xfd7e : 0xfd00); i < (getenv("DP_MD_DEEP") ? 0xfd8e : 0xfe00); i++) if (ref_byte(ref, i) != dut->ram[i]) { if (!nd) first = i; nd++; }
            if (nd && shown2 < 6) { shown2++; printf("stack-diff frame %u: %u bytes in fd00-fdff, first %04x ref=%02x dut=%02x sp=%08x/%08x pc=%06x/%06x\n", frame + 1, nd, first, ref_byte(ref, first), dut->ram[first], (unsigned)ref->m68k.address_registers[7], dut->a[7], (unsigned)ref->m68k.program_counter, dut->pc); } }
        if (getenv("DP_MD_GENS_DUMP")) {   /* Gens-rr work RAM dump (tools/gens_dump.lua): 64 KiB per frame, DP_MD_GENS_SKIP frames of offset */
            static FILE *gf; static unsigned char gbuf[0x10000]; static unsigned first_ref = 0, first_dut = 0;
            if (!gf) gf = fopen(getenv("DP_MD_GENS_DUMP"), "rb");
            long idx = (long)frame - (getenv("DP_MD_GENS_SKIP") ? atol(getenv("DP_MD_GENS_SKIP")) : 0);
            if (gf && idx >= 0 && fseek(gf, idx * 0x10000L, SEEK_SET) == 0 && fread(gbuf, 1, 0x10000, gf) == 0x10000) {
                unsigned dr = 0, dd = 0;
                for (unsigned i = 0; i < 0x10000; i++) { dr += gbuf[i] != ref_byte(ref, i); dd += gbuf[i] != dut->ram[i]; }
                if ((dr && !first_ref) || (dd && !first_dut) || (frame + 1) % 60 == 0) {
                    printf("gens frame %u: differs from ClownMDEmu %u, from m0boy %u bytes\n", frame + 1, dr, dd);
                    if (dr && !first_ref) first_ref = frame + 1; if (dd && !first_dut) first_dut = frame + 1; }
            }
        }
        if (getenv("DP_MD_FIRSTLOW")) {   /* first frames whose low work RAM ($0000-$EFFF: objects, level, state) differs, with the first addresses */
            static int shown; unsigned low = 0, a0 = 0, a1 = 0;
            for (unsigned i = 0xD000; i < 0xF000; i++) if (ref_byte(ref, i) != dut->ram[i]) { if (!low++) a0 = i; a1 = i; }
            if (low && shown < 8) { shown++; printf("low-RAM diff frame %u: %u bytes, first %04x last %04x (ref=%02x dut=%02x)\n", frame + 1, low, a0, a1, ref_byte(ref, a0), dut->ram[a0]); }
        }
        if ((frame + 1 >= 50 && frame + 1 <= 62) || (frame + 1) % 300 == 0 || mismatch_streak == 20 || dut->fault) {
            unsigned diff = 0;
            for (unsigned i = 0; i < 0x10000; i++) { int d = ref_byte(ref, i) != dut->ram[i]; diff += d; if (d && getenv("DP_MD_DIFF_LIST") && (frame + 1) % (getenv("DP_MD_DIFF_MOD") ? (unsigned)atoi(getenv("DP_MD_DIFF_MOD")) : 3000u) == 0) printf("  diff %04x ref=%02x dut=%02x\n", i, ref_byte(ref, i), dut->ram[i]); }
            if (getenv("DP_MD_DIFF_LIST") && (frame + 1) % (getenv("DP_MD_DIFF_MOD") ? (unsigned)atoi(getenv("DP_MD_DIFF_MOD")) : 3000u) == 0) printf("  sp ref=%08x dut=%08x\n", (unsigned)ref->m68k.address_registers[7], dut->a[7]);
            printf("%u mode=%02x/%02x zone=%02x%02x/%02x%02x playerx=%02x%02x/%02x%02x pc=%06x/%06x diff=%u fault=%u\n",
                   frame + 1, ref_byte(ref, 0xf600), dut->ram[0xf600],
                   ref_byte(ref, 0xfe10), ref_byte(ref, 0xfe11), dut->ram[0xfe10], dut->ram[0xfe11],
                   ref_byte(ref, 0xd008), ref_byte(ref, 0xd009), dut->ram[0xd008], dut->ram[0xd009],
                   (unsigned)ref->m68k.program_counter, dut->pc, diff, dut->fault);
            fflush(stdout);
            if (mismatch_streak == 20)
                fprintf(stderr, "player X differs for 20 frames even with a one-frame offset at frame %u\n", frame + 1);
        }
        if (getenv("DP_MD_PENALTY") && frame == 52) dut->frame_cycles += (unsigned)atoi(getenv("DP_MD_PENALTY"));
        if (getenv("DP_MD_VBLA")) { static unsigned pv = 0, shown; unsigned r = (ref_byte(ref,0xfe0c)<<24)|(ref_byte(ref,0xfe0d)<<16)|(ref_byte(ref,0xfe0e)<<8)|ref_byte(ref,0xfe0f);
            unsigned d = (dut->ram[0xfe0c]<<24)|(dut->ram[0xfe0d]<<16)|(dut->ram[0xfe0e]<<8)|dut->ram[0xfe0f];
            if (r != d && shown < 200 && frame > 0) { shown++; printf("vbla f%u ref=%08x dut=%08x mode=%02x/%02x\n", frame + 1, r, d, ref_byte(ref,0xf600), dut->ram[0xf600]); } (void)pv; }
        if (getenv("DP_MD_FIRST_DIFF") && frame + 1 == 49) {   // interrupts are masked during the ROM checksum: step to the exact marker cycle
            MdFull *c = malloc(sizeof *c); memcpy(c, dut, sizeof *c);
            for (unsigned n = 0; n < 4000000 && c->ram[0xfffd] != 0x6e; n++) md_full_step_interp(c);
            printf("m0boy: init marker written at total cycle %u (frame %u starts at %u)\n", (unsigned)c->cycles, (unsigned)(c->cycles / 127856u) + 1, (unsigned)(c->cycles / 127856u) * 127856u);
            free(c);
        }
        if (getenv("DP_MD_FIRST_DIFF")) { static int rs, ds; if (!rs && ref_byte(ref, 0xfffd) == 0x6e) { rs = 1; printf("ref init marker at frame %u\n", frame + 1); }
            if (!ds && dut->ram[0xfffd] == 0x6e) { ds = 1; printf("dut init marker at frame %u (m0boy cycles %u, frame_cycles %u)\n", frame + 1, (unsigned)dut->cycles, (unsigned)dut->frame_cycles); } }
        if (getenv("DP_MD_FIRST_DIFF") && frame < 300) {   // first frame whose work RAM differs, with the first bytes
            static int shown; unsigned nd = 0, first[8]; for (unsigned i = 0; i < 0x10000; i++) if (ref_byte(ref, i) != dut->ram[i]) { if (nd < 8) first[nd] = i; nd++; }
            if (nd && !shown) { shown = 1; printf("first-diff frame %u: %u bytes:", frame + 1, nd); for (unsigned k = 0; k < nd && k < 8; k++) printf(" %04x(%02x/%02x)", first[k], ref_byte(ref, first[k]), dut->ram[first[k]]); printf("\n"); }
        }
        if (dut->fault) break;
    }
    fclose(movie); free(rom); free(words); free(ref); free(dut); return 0;
}
