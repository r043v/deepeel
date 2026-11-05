// deepeel bh LIB ROM --tas FILE [options]: replay a movie on BizHawk's libgambatte (a level-B reference
// backend). The library is loaded with dlopen and driven through its C API (BizHawk Assets/dll/libgambatte.so);
// it is never copied into the repository. Its CD-logger callback gives, per ROM byte, whether the byte was
// executed (opcode / operand) or read as data: an independent coverage measure to compare with m0boy's.
#include "common.h"
#include "explore/tas.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*bh_create_fn)(void);
typedef int (*bh_load_fn)(void *, const uint8_t *, unsigned, unsigned);
typedef int (*bh_run_fn)(void *, int *, int, short *, unsigned *);
typedef void (*bh_input_fn)(void *, unsigned (*)(void *), void *);
typedef void (*bh_cd_fn)(void *, void (*)(int, int, int));
typedef void (*bh_pal_fn)(void *, unsigned, unsigned, unsigned);
typedef int (*bh_area_fn)(void *, int, void **, int *);
typedef void (*bh_regs_fn)(void *, int *);
typedef unsigned (*bh_read_fn)(void *, unsigned);

enum { BH_ROM = 0, BH_OPCODE = 1, BH_OPERAND = 2, BH_DATA = 4, BH_MAXROM = 0x800000, BH_FRAME = 35112 };

static uint8_t *g_cov;
static size_t g_cov_n;
static unsigned g_pad;

static unsigned bh_input(void *u) { (void)u; return g_pad; }
static void bh_cd(int addr, int type, int flags) {
    if (type == BH_ROM && addr >= 0 && (size_t)addr < g_cov_n) g_cov[addr] |= (uint8_t)flags;
}

static void bh_ppm(const char *path, const int *vid) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 160 144 255\n");
    for (int i = 0; i < 160 * 144; i++) { fputc(vid[i] >> 16, f); fputc(vid[i] >> 8, f); fputc(vid[i], f); }
    fclose(f);
}

int dp_bh_command(int argc, char **argv) {
    const char *usage = "usage: deepeel bh LIBGAMBATTE ROM --tas FILE [--frames N] [--cov FILE] [--ranges FILE] "
                        "[--ppm FILE] [--shot-every N PREFIX] [--bios] [--dump FILE]\n"
                        "  --cov FILE     raw per-ROM-byte flags (1 opcode, 2 operand, 4 data)\n"
                        "  --ranges FILE  text list of executed ROM ranges (bank:addr, one per line)\n"
                        "  --bios         boot through the DMG boot ROM when the library has one (default: no boot ROM)\n";
    if (argc < 5) { fputs(usage, stderr); return 2; }
    const char *dumpn = NULL;
    const char *lib = argv[2], *rom = argv[3], *tasf = NULL, *cov = NULL, *rng = NULL, *ppm = NULL, *shot = NULL;
    unsigned long frames = 0, every = 0;
    int bios = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--tas") && i + 1 < argc) tasf = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cov") && i + 1 < argc) cov = argv[++i];
        else if (!strcmp(argv[i], "--ranges") && i + 1 < argc) rng = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 2 < argc) { every = strtoul(argv[i + 1], NULL, 0); shot = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--bios")) bios = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpn = argv[++i];
        else { fprintf(stderr, "deepeel bh: unknown option %s\n%s", argv[i], usage); return 2; }
    }
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "deepeel bh: %s\n", dlerror()); return 1; }
    bh_create_fn create = (bh_create_fn)dlsym(h, "gambatte_create");
    bh_load_fn load = (bh_load_fn)dlsym(h, "gambatte_loadbuf");
    bh_run_fn run = (bh_run_fn)dlsym(h, "gambatte_runfor");
    bh_input_fn setin = (bh_input_fn)dlsym(h, "gambatte_setinputgetter");
    bh_cd_fn setcd = (bh_cd_fn)dlsym(h, "gambatte_setcdcallback");
    bh_pal_fn setpal = (bh_pal_fn)dlsym(h, "gambatte_setdmgpalettecolor");
    if (!create || !load || !run || !setin || !setcd) {
        fprintf(stderr, "deepeel bh: %s is not a BizHawk libgambatte (missing C API)\n", lib);
        return 1;
    }
    FILE *f = fopen(rom, "rb");
    if (!f) { fprintf(stderr, "deepeel bh: cannot open %s\n", rom); return 1; }
    fseek(f, 0, SEEK_END);
    long rn = ftell(f);
    rewind(f);
    uint8_t *rd = malloc(rn > 0 ? (size_t)rn : 1);
    if (!rd || fread(rd, 1, (size_t)rn, f) != (size_t)rn) { fclose(f); fprintf(stderr, "deepeel bh: read error\n"); return 1; }
    fclose(f);
    DpTas tas;
    memset(&tas, 0, sizeof tas);
    if (tasf) {
        char err[256] = "", why[256] = "";
        if (!dp_tas_load(tasf, &tas, err, sizeof err)) { fprintf(stderr, "deepeel bh: %s\n", err); return 1; }
        int m = dp_tas_rom_match(&tas, rd, (size_t)rn, why, sizeof why);
        fprintf(stderr, "deepeel bh: movie %s: %u frames, ROM identity %s%s%s\n", tas.source, tas.frames,
                m == 1 ? "verified" : m == 0 ? "MISMATCH" : "not recorded", *why ? " (" : "", *why ? why : "");
        if (*why) fputc('\n', stderr);
    }
    if (!frames) frames = tas.frames ? tas.frames : 600;
    if (tas.frames) { unsigned nz = 0; for (unsigned k = 0; k < tas.frames; k++) nz += tas.masks[k] != 0; fprintf(stderr, "deepeel bh: %u of %u movie frames press a button\n", nz, tas.frames); }
    g_cov_n = (size_t)rn < BH_MAXROM ? (size_t)rn : BH_MAXROM;
    g_cov = calloc(g_cov_n ? g_cov_n : 1, 1);
    void *g = create();
    // flags: READONLY_SAV (16), NO_BIOS (32) unless the boot ROM is wanted
    int r = load(g, rd, (unsigned)rn, 16u | (bios ? 0u : 32u));
    if (r) { fprintf(stderr, "deepeel bh: gambatte_loadbuf returned %d\n", r); return 1; }
    if (setpal && !getenv("BH_NOPAL")) {
        static const unsigned gray[4] = { 0xe0f8d0, 0x88c070, 0x346856, 0x081820 };
        for (unsigned p = 0; p < 3; p++) for (unsigned c = 0; c < 4; c++) setpal(g, p, c, gray[c]);
    }
    setin(g, bh_input, NULL);
    setcd(g, bh_cd);
    static int vid[160 * 144];
    static short snd[BH_FRAME + 2064 + 64];
    // --dump: per frame "<n> <wram fnv> pc=<pc> if ie ff82 lcdc", the columns of DEEPEEL_TAS_DUMP, to
    // find where deepeel's replay leaves the reference engine
    bh_area_fn area = (bh_area_fn)dlsym(h, "gambatte_getmemoryarea");
    bh_regs_fn gregs = (bh_regs_fn)dlsym(h, "gambatte_getregs");
    bh_read_fn rdmem = (bh_read_fn)dlsym(h, "gambatte_cpuread");
    FILE *dumpf = dumpn ? fopen(dumpn, "w") : NULL;
    for (unsigned long fr = 0; fr < frames; fr++) {
        g_pad = fr < tas.frames ? tas.masks[fr] : 0;
        unsigned s = BH_FRAME;
        run(g, vid, 160, snd, &s);
        if (dumpf && area && gregs && rdmem) {
            void *wp = NULL;
            int wl = 0, rg[16] = {0};
            area(g, 2, &wp, &wl);
            gregs(g, rg);
            uint64_t hh = 1469598103934665603ull;
            for (int i = 0; wp && i < wl && i < 0x2000; i++) hh = (hh ^ ((uint8_t *)wp)[i]) * 1099511628211ull;
            fprintf(dumpf, "%lu %016llx pc=%04x if=%02x ie=%02x 82=%02x 40=%02x\n", fr + 1, (unsigned long long)hh,
                    (unsigned)rg[0] & 0xffff, rdmem(g, 0xFF0F) & 0xff, rdmem(g, 0xFFFF) & 0xff, rdmem(g, 0xFF82) & 0xff,
                    rdmem(g, 0xFF40) & 0xff);
            // cart-RAM bytes $A8E8..$A8FF (SML2 keeps its stack there)
            fseek(dumpf, -1, SEEK_CUR);
            fprintf(dumpf, " rg=%x,%x,%x,%x,%x,%x,%x,%x st=", rg[1] & 0xffff, rg[2] & 0xffff, rg[3] & 0xffff, rg[4] & 0xffff, rg[5] & 0xffff, rg[6] & 0xffff, rg[7] & 0xffff, rg[8] & 0xffff);
            for (int a = 0xA8E8; a < 0xA900; a++) fprintf(dumpf, "%02x", rdmem(g, a) & 0xff);
            fputc('\n', dumpf);
        }
        if (every && shot && (fr + 1) % every == 0) {
            char nm[600];
            snprintf(nm, sizeof nm, "%s%06lu.ppm", shot, fr + 1);
            bh_ppm(nm, vid);
        }
    }
    size_t op = 0, opd = 0, dat = 0;
    for (size_t i = 0; i < g_cov_n; i++) {
        if (g_cov[i] & BH_OPCODE) op++;
        if (g_cov[i] & (BH_OPCODE | BH_OPERAND)) opd++;
        if (g_cov[i] & BH_DATA) dat++;
    }
    printf("frames %lu  rom bytes: executed %zu (opcodes %zu)  read as data %zu  of %zu\n", frames, opd, op, dat, g_cov_n);
    if (ppm) bh_ppm(ppm, vid);
    if (cov) { FILE *o = fopen(cov, "wb"); if (o) { fwrite(g_cov, 1, g_cov_n, o); fclose(o); } }
    if (rng) {
        FILE *o = fopen(rng, "w");
        if (o) {
            for (size_t i = 0; i < g_cov_n;) {
                if (!(g_cov[i] & (BH_OPCODE | BH_OPERAND))) { i++; continue; }
                size_t j = i;
                while (j < g_cov_n && (g_cov[j] & (BH_OPCODE | BH_OPERAND)) && j / 0x4000 == i / 0x4000) j++;
                fprintf(o, "%02zx:%04zx-%04zx\n", i / 0x4000, i / 0x4000 ? 0x4000 + i % 0x4000 : i, (i / 0x4000 ? 0x4000 + (j - 1) % 0x4000 : j - 1));
                i = j;
            }
            fclose(o);
        }
    }
    if (dumpf) fclose(dumpf);
    free(g_cov); free(rd); dp_tas_free(&tas);
    return 0;
}
