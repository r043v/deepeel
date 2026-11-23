/* Live reference tile for the GB watch window: replays one movie on the
 * Gambatte library (the reference core of the TAS comparison) and publishes
 * each frame into the shared feed (DEEPEEL_REF_FEED, src/explore/md_ref_feed.h),
 * the md_ref_live pattern.  Built outside the repository build; no reference
 * implementation is copied.  The pad is the 8 GB buttons in the deepeel
 * TAS mask order (64,128,32,16,8,4,2,1,0 bits of the input lines file).
 *
 * usage: gb_ref_live BIOS ROM inputs.txt
 * env: DEEPEEL_REF_FEED=<file> (required), LIBGAMBATTE=<path.so> (default
 *      /home/m/dev/BizHawk/output/dll/libgambatte.so) */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "explore/md_ref_feed.h"

static unsigned g_frame;
static unsigned char g_masks[700000];
static unsigned g_nmasks;

static unsigned g_shift;   /* GB_REF_SHIFT: the movie inputs start N frames late */
static unsigned input_cb(void *unused) {
    (void)unused;
    return g_frame >= g_shift && g_frame - g_shift < g_nmasks ? (unsigned)g_masks[g_frame - g_shift] : 0;
}

/* the movie's input lines: one char per button, '.' released, anything else held.
 * GB_REF_BITS=std: gambatte.h's own mask order (R 1, L 2, U 4, D 8, A 0x10, B 0x20, Sel 0x40, Start 0x80)
 * instead of the deepeel pad order (U 0x40, D 0x80, L 0x20, R 0x10, Start 8, Sel 4, B 2, A 1). */
static unsigned input23(void) { return input_cb(0); }
static unsigned rtc23(void) { return g_frame * 4389u / 262144u; }
static const int *bits;
static const int bits_d[9] = {64, 128, 32, 16, 8, 4, 2, 1, 0};
static const int bits_s[9] = {4, 8, 2, 1, 128, 64, 32, 16, 0};

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: gb_ref_live BIOS ROM inputs.txt\n");
        fprintf(stderr, "DEEPEEL_REF_FEED=<file> names the shared feed, LIBGAMBATTE=<so> the core\n");
        return 2;
    }
    DpMdRefFeed *feed = dp_md_ref_feed_open(getenv("DEEPEEL_REF_FEED"), 1);
    if (!feed) { fprintf(stderr, "gb_ref_live: cannot open the feed (DEEPEEL_REF_FEED)\n"); return 2; }

    bits = getenv("GB_REF_BITS") && !strcmp(getenv("GB_REF_BITS"), "std") ? bits_s : bits_d;
    FILE *fi = fopen(argv[3], "r");
    if (!fi) { fprintf(stderr, "gb_ref_live: cannot open %s\n", argv[3]); return 2; }
    char line[64];
    while (fgets(line, sizeof line, fi) && g_nmasks < 700000) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (l < 9) continue;
        int m = 0;
        for (int k = 0; k < 9; k++) if (line[k] != '.') m |= bits[k];
        g_masks[g_nmasks++] = (unsigned char)m;
    }
    fclose(fi);
    if (!g_nmasks) { fprintf(stderr, "gb_ref_live: no input frames\n"); return 2; }

    const char *soname = getenv("LIBGAMBATTE");
    if (!soname || !*soname) soname = "/home/m/dev/BizHawk/output/dll/libgambatte.so";
    void *so = dlopen(soname, RTLD_NOW);
    if (!so) { fprintf(stderr, "gb_ref_live: dlopen %s: %s\n", soname, dlerror()); return 2; }
    void *(*create)(void);
    int (*loadbiosbuf)(void *, const char *, unsigned);
    int (*loadbuf)(void *, const char *, unsigned, unsigned);
    int (*runfor)(void *, uint32_t *, long, int16_t *, unsigned *);
    void (*setinputgetter)(void *, unsigned (*)(void *), void *);
    void (*setlayers)(void *, int);
    void (*setcgbpalette)(void *, unsigned *);
    int (*area)(void *, int, void **, int *);
    #define SYM(v, n) do { *(void **)(&v) = dlsym(so, "gambatte_" n); if (!v) { fprintf(stderr, "gb_ref_live: %s missing\n", n); return 2; } } while (0)
    SYM(create, "create"); SYM(area, "getmemoryarea");
    int want23 = getenv("GB_REF_ABI23") && *getenv("GB_REF_ABI23") == '1';
    if (!want23) { SYM(loadbiosbuf, "loadbiosbuf"); SYM(loadbuf, "loadbuf"); SYM(runfor, "runfor"); SYM(setinputgetter, "setinputgetter"); SYM(setlayers, "setlayers"); }
    #define SYMO(v, n) do { *(void **)(&v) = dlsym(so, "gambatte_" n); } while (0)
    SYMO(setcgbpalette, "setcgbpalette");


    /* GB_REF_ABI23=1: the BizHawk 2.3 libgambatte (load/loadgbcbios/runfor+blitto, RTC callback), the reference
     * of the power-on CGB movies recorded on 2.3 (same calls as tools/gambatte_dsy.py --bizhawk-23) */
    int abi23 = getenv("GB_REF_ABI23") && *getenv("GB_REF_ABI23") == '1';
    int (*load23)(void *, const char *, unsigned, long long, unsigned, unsigned) = 0;
    int (*bios23)(void *, const char *) = 0;
    int (*runfor23)(void *, void *, unsigned *) = 0;
    void (*blit23)(void *, void *, int) = 0;
    void (*setin23)(void *, unsigned (*)(void)) = 0, (*setrtc23)(void *, unsigned (*)(void)) = 0;
    if (abi23) {
        SYM(load23, "load"); SYM(bios23, "loadgbcbios"); SYM(runfor23, "runfor"); SYM(blit23, "blitto");
        SYM(setin23, "setinputgetter"); SYM(setrtc23, "setrtccallback");
    }
    void *g = create();
    /* the linear CGB colour profile: the m0boy one, byte for byte (5-bit channel
     * scaled to 8 bits, exactly what the TAS video shows) */
    {
        static unsigned lut[32768];
        for (unsigned c = 0; c < 32768; c++) {
            unsigned r = (c & 31) * 255u / 31u, b = (c >> 10 & 31) * 255u / 31u;
            unsigned g6 = ((c >> 5 & 31) << 1 | (c >> 5 & 31) >> 4) * 255u / 63u;   /* m0boy's 6-bit green replication */
            lut[c] = 0xFF000000u | r << 16 | g6 << 8 | b;
        }
        setcgbpalette(g, lut);
    }
    FILE *fb = fopen(argv[1], "rb");
    if (!fb) { fprintf(stderr, "gb_ref_live: cannot open the BIOS\n"); return 2; }
    static uint8_t bbuf[4096];
    size_t bn = fread(bbuf, 1, sizeof bbuf, fb);
    fclose(fb);
    if (bn < 256) { fprintf(stderr, "gb_ref_live: short BIOS\n"); return 2; }
    if (abi23 ? bios23(g, (char *)bbuf) : loadbiosbuf(g, (char *)bbuf, (unsigned)bn)) { fprintf(stderr, "gb_ref_live: BIOS rejected\n"); return 2; }
    FILE *fr = fopen(argv[2], "rb");
    if (!fr) { fprintf(stderr, "gb_ref_live: cannot open %s\n", argv[2]); return 2; }
    fseek(fr, 0, SEEK_END); long rs = ftell(fr); rewind(fr);
    uint8_t *rbuf = malloc((size_t)rs);
    if (!rbuf || fread(rbuf, 1, (size_t)rs, fr) != (size_t)rs) return 2;
    fclose(fr);
    /* GB_REF_FLAGS overrides the load flags: 17 = CGB + skipBIOS(0x10), 1 = CGB with the BIOS boot */
    unsigned lflags = getenv("GB_REF_FLAGS") ? (unsigned)atoi(getenv("GB_REF_FLAGS")) : 17u;
    if (abi23 ? load23(g, (char *)rbuf, (unsigned)rs, 0, 2u, 0) : loadbuf(g, (char *)rbuf, (unsigned)rs, lflags)) { fprintf(stderr, "gb_ref_live: ROM rejected\n"); return 2; }
    if (abi23) { setin23(g, input23); setrtc23(g, rtc23); }
    else { setinputgetter(g, input_cb, 0); setlayers(g, 7); }

    static uint32_t video[160 * 144];
    static int16_t audio[2 * 40000];
    static uint32_t packed[DP_MDRF_MAX_W * DP_MDRF_MAX_H];
    /* GB_REF_SHIFT=N: the movie inputs start N frames late (alignment probe when the
     * boot length differs); GB_REF_DUMP_AT=a,b: dump these movie frames as PPM */
    g_shift = getenv("GB_REF_SHIFT") ? (unsigned)atoi(getenv("GB_REF_SHIFT")) : 0;
    unsigned dump_at[32]; int n_dump = 0;
    unsigned dump_lo = 0, dump_hi = 0;   /* GB_REF_DUMP_RANGE=a,b: every frame in [a,b] */
    if (getenv("GB_REF_DUMP_RANGE")) {
        char *s = getenv("GB_REF_DUMP_RANGE");
        dump_lo = (unsigned)strtoul(s, &s, 10);
        if (*s == '-') dump_hi = (unsigned)strtoul(s + 1, NULL, 10);
    }
    if (getenv("GB_REF_DUMP_AT")) {
        char *s = strdup(getenv("GB_REF_DUMP_AT")), *tok = strtok(s, ",");
        while (tok && n_dump < 32) { dump_at[n_dump++] = (unsigned)atoi(tok); tok = strtok(NULL, ","); }
        free(s);
    }
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    /* GB_REF_RELEASE_S: a leader stalled this long releases us (default 2 s, the md_ref_live
     * rule; a higher value keeps the lock through the explorer's slow phases) */
    double release_s = getenv("GB_REF_RELEASE_S") ? atof(getenv("GB_REF_RELEASE_S")) : 2.0;
    double waited = 0, last_move = 0; uint32_t last_leader = 0;
    for (g_frame = 0; g_frame < g_nmasks + g_shift; g_frame++) {
        /* replay sync: stay within two frames of the deepeel core; a leader stalled beyond
         * release_s releases us, and the lock re-engages as soon as it moves again */
        for (;;) {
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            double now = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
            if (feed->leader != last_leader) { last_leader = feed->leader; last_move = now; }
            if (g_frame <= feed->leader + 2 || (last_leader != 0 && now - last_move > release_s) || (last_leader == 0 && now > release_s)) break;
            usleep(300); waited += 300e-6;
        }
        if (g_frame && g_frame % 30 == 0) {
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            double el = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9 - waited;
            if (el > 0.01) feed->unsync_x10 = (uint32_t)(10.0 * g_frame / el);
        }
        unsigned samples = 35112;
        long csb = 0;
        if (abi23) { if (runfor23(g, audio, &samples) > 0) blit23(g, video, 160); }
        else csb = runfor(g, video, 160, audio, &samples);
        if (getenv("GB_REF_BOUND_LOG") && csb >= 0)
            fprintf(stderr, "refbound %u %ld\n", g_frame + 1, (long)((long long)(g_frame + 1) * 70224 - csb));
        if (getenv("GB_PAL_LOG")) {
            void *vp; int vs;
            static char hx[257]; static const char hc[] = "0123456789abcdef";
            if (area(g, 8, &vp, &vs) && vs == 64) {
                const uint8_t *pb = (const uint8_t *)vp;
                for (int i = 0; i < 64; i++) { hx[i*2] = hc[pb[i] >> 4]; hx[i*2+1] = hc[pb[i] & 15]; }
                hx[128] = 0;
                fprintf(stderr, "pal f%u %s\n", g_frame + 1, hx);
            } else if (area(g, 6, &vp, &vs) && vs == 32) {
                uint8_t pal[64];
                memcpy(pal, vp, 32);
                if (area(g, 7, &vp, &vs) && vs == 32) memcpy(pal + 32, vp, 32);
                for (int i = 0; i < 64; i++) { hx[i*2] = hc[pal[i] >> 4]; hx[i*2+1] = hc[pal[i] & 15]; }
                hx[128] = 0;
                fprintf(stderr, "pal f%u %s\n", g_frame + 1, hx);
            }
        }
        {   /* GB_REF_STATE_RANGE=a-b: full core state per frame (.gbs: wram,oam,hram,vram) */
            static unsigned slo, shi; static int sinited;
            if (!sinited) { sinited = 1; const char *e = getenv("GB_REF_STATE_RANGE");
                if (e && *e) { slo = strtoul(e, (char **)&e, 10); if (*e == '-') shi = strtoul(e + 1, NULL, 10); } }
            if (slo && g_frame + 1 >= slo && g_frame + 1 <= shi) {
                static char sp2[512];
                snprintf(sp2, sizeof sp2, "%s/s%u_g%07u.gbs", getenv("GB_REF_DUMP_DIR"), g_shift, g_frame + 1);
                FILE *dv = fopen(sp2, "wb");
                if (dv) {
                    void *vp; int vs;
                    for (int a = 2; a <= 5; a++) {   /* 2=wram 4=oam 5=hram */
                        if (!area(g, a, &vp, &vs)) break;
                        if (a == 3) continue;
                        fwrite(&vs, sizeof vs, 1, dv);
                        fwrite(vp, 1, vs, dv);
                    }
                    area(g, 0, &vp, &vs);
                    fwrite(&vs, sizeof vs, 1, dv); fwrite(vp, 1, vs, dv);
                    fclose(dv);
                }
            }
        }
        if (dump_hi && g_frame + 1 >= dump_lo && g_frame + 1 <= dump_hi) goto dumpit;
        for (int k = 0; k < n_dump; k++) if (dump_at[k] == g_frame + 1) { dumpit:;
                static char p[512];
                snprintf(p, sizeof p, "%s/s%u_f%07u.ppm", getenv("GB_REF_DUMP_DIR"), g_shift, g_frame + 1);
                FILE *d = fopen(p, "wb");
                if (d) { fprintf(d, "P6\n160 144\n255\n"); for (int i = 0; i < 160 * 144; i++) { uint32_t q = packed[i]; uint8_t rgb[3] = { q >> 16, q >> 8, q }; fwrite(rgb, 1, 3, d); } fclose(d); }
                {   /* VRAM companion dump: .vram = the two 8 KiB banks; .pal = BG then OBJ palette RAM (areas 6/7) */
                    void *vp; int vs; char vp2[512];
                    snprintf(vp2, sizeof vp2, "%s/s%u_f%07u.vram", getenv("GB_REF_DUMP_DIR"), g_shift, g_frame + 1);
                    FILE *dv = fopen(vp2, "wb");
                    area(g, 0, &vp, &vs);
                    if (dv && vs > 0) fwrite(vp, 1, vs, dv), fclose(dv); else if (dv) fclose(dv);
                    snprintf(vp2, sizeof vp2, "%s/s%u_f%07u.pal", getenv("GB_REF_DUMP_DIR"), g_shift, g_frame + 1);
                    dv = fopen(vp2, "wb");
                    if (dv) {
                        for (int a = 6; a <= 7; a++) {
                            if (!area(g, a, &vp, &vs)) break;
                            fwrite(vp, 1, vs, dv);
                        }
                        fclose(dv);
                    }
                }
            }
        if (getenv("GB_REF_DUMP_DIR") && g_frame % 500 == 0) {   // diagnosis: what the reference really renders
            static char p[512];
            snprintf(p, sizeof p, "%s/g%06u.ppm", getenv("GB_REF_DUMP_DIR"), g_frame);
            FILE *d = fopen(p, "wb");
            if (d) { fprintf(d, "P6\n160 144\n255\n"); for (int i = 0; i < 160 * 144; i++) { uint32_t q = packed[i]; uint8_t rgb[3] = { q >> 16, q >> 8, q }; fwrite(rgb, 1, 3, d); } fclose(d); }
        }
        for (int i = 0; i < 160 * 144; i++) {
            /* gambatte's video buffer is xRGB888: the LUT (default or custom) emits full 8-bit channels */
            uint32_t q = video[i];
            packed[i] = q | 0xFF000000u;
        }
        dp_md_ref_publish(feed, packed, 160, 144, g_frame + 1, g_nmasks, g_frame + 1 == g_nmasks);
    }
    fprintf(stderr, "gb_ref_live: done, %u frames\n", g_nmasks);
    return 0;
}
