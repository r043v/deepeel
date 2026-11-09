/* Standalone differential probe: run one Sega TAS (.bk2/log) against the
 * Genesis Plus GX libretro core (reference, level A) and deepeel's m0boy
 * Game Gear / Master System runtime (device under test).  Per frame: the
 * work-RAM diff with the first differing byte, and a coarse screen hash of
 * both (5:5:5 pixels).  The cores cut frames differently and the pixel
 * formats differ, so hashes are observations, not proofs; the RAM diff is
 * the fidelity signal.  No reference implementation is copied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>

#include "gg.h"
#include "z80.h"
#include "backend.h"
#include "tas.h"

/* --- libretro 1.x surface (subset) ---------------------------------------- */
#define LR_ENV_SET_PIXEL_FORMAT 10
#define LR_ENV_GET_SYSTEM_DIRECTORY 9
#define LR_ENV_GET_CAN_DUPE 20
enum { LR_PF_0RGB1555 = 0, LR_PF_XRGB8888 = 1, LR_PF_RGB565 = 2 };
struct lr_system_info { const char *library_name, *library_version, *valid_extensions; bool need_fullpath, block_extract; };
struct lr_game_info { const char *path; const void *data; size_t size; const char *meta; };

typedef bool (*lr_environment_t)(unsigned cmd, void *data);
typedef void (*lr_video_t)(const void *data, unsigned w, unsigned h, size_t pitch);
typedef void (*lr_poll_t)(void);
typedef int16_t (*lr_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

/* RetroPad ids as GPGX's SMS/GG pad (DEVICE_PAD2B) reads them: B -> GG 1, A -> GG 2 */
enum { LR_ID_B = 0, LR_ID_START = 3, LR_ID_UP = 4,
       LR_ID_DOWN = 5, LR_ID_LEFT = 6, LR_ID_RIGHT = 7, LR_ID_A = 8 };

static unsigned g_pad;               /* sega mask of this frame (up=1..start=64) */
/* the SMS profile renders GG_ACTIVE (192) picture lines, the GG 144: both
 * framebuffers cover the taller one (the GG leaves the rest black) — the DUT
 * row callback used to write past a 144-line buffer and clobber host .bss */
#define DUT_LINES GG_ACTIVE
static uint16_t g_ref_fb[160 * DUT_LINES]; /* last reference frame (5:5:5 raw) */
static unsigned g_snap;              /* dump both screens at this 1-based frame */
static unsigned g_snap_save;         /* the loaded core may scribble host .bss: g_snap is re-read here each frame */
static uint64_t g_ref_hash;
static unsigned g_ref_w, g_ref_h;
static unsigned g_ref_fmt = LR_PF_0RGB1555;

static void ref_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    uint64_t h1 = 0xcbf29ce484222325ull;
    const uint8_t *row = data;
    for (unsigned y = 0; y < h; y++, row += pitch) {
        const uint16_t *px = (const uint16_t *)row;
        for (unsigned x = 0; x < w; x++) {
            unsigned p = px[x], r, g, b;
            if (g_ref_fmt == LR_PF_RGB565) { r = (p >> 11) & 31; g = (p >> 6) & 31; b = p & 31; }
            else { r = p & 31; g = (p >> 5) & 31; b = (p >> 10) & 31; }
            h1 = (h1 ^ (r * 16 + g * 4 + b)) * 0x100000001b3ull;
        }
    }
    g_ref_hash = h1;
    g_ref_w = w; g_ref_h = h;
    if (w == 160 && h <= DUT_LINES)
        for (unsigned y = 0; y < h; y++)
            memcpy(g_ref_fb + y * 160, row + y * pitch, 320);
}
static int16_t ref_input(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)device; (void)index;
    if (port != 0) return 0;
    switch (id) {
    case LR_ID_UP: return (g_pad & 1) != 0;
    case LR_ID_DOWN: return (g_pad & 2) != 0;
    case LR_ID_LEFT: return (g_pad & 4) != 0;
    case LR_ID_RIGHT: return (g_pad & 8) != 0;
    case LR_ID_B: return (g_pad & 16) != 0;   /* GG button 1 */
    case LR_ID_A: return (g_pad & 32) != 0;   /* GG button 2 */
    case LR_ID_START: return (g_pad & 64) != 0;
    default: return 0;
    }
}
static void ref_poll(void) {}
static void ref_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static void ref_audio_batch(int16_t *data, size_t frames) { (void)data; (void)frames; }
static bool ref_env(unsigned cmd, void *data) {
    switch (cmd) {
    case LR_ENV_SET_PIXEL_FORMAT: g_ref_fmt = *(const unsigned *)data; return true;
    case LR_ENV_GET_CAN_DUPE: *(bool *)data = true; return true;
    case LR_ENV_GET_SYSTEM_DIRECTORY: *(const char **)data = "/tmp"; return true;
    default: return false;
    }
}

/* --- device under test: the m0boy GG runtime ------------------------------- */
static uint64_t g_dut_rowhash, g_dut_hash;
/* (DUT_LINES is defined above, shared with the reference framebuffer) */
static uint16_t g_dut_last[160 * DUT_LINES];
static uint16_t g_dut_store[GG_ROW_STRIDE];   /* the runtime renders into row_out (+GG_ROW_PAD) */
static const DpSegaBackend *g_b;              /* deepeel's GG backend (BIOS-less bus profile) */
static z80_decoded_t g_d;                     /* runtime decode cache */
static void dut_row(void *u, unsigned y, uint16_t *px) {
    (void)u;
    if (y >= DUT_LINES) return;   /* never write past the picture lines */
    for (unsigned i = 0; i < GG_WIDTH; i++) {
        unsigned p = px[i], r = (p >> 11) & 31, g = (p >> 6) & 31, b = p & 31;
        g_dut_rowhash = (g_dut_rowhash ^ (r * 16 + g * 4 + b)) * 0x100000001b3ull;
        g_dut_last[y * 160 + i] = p;
    }
}
static int dut_line(void *u, unsigned y) { (void)u; (void)y; return 1; }
static void dut_frame(void *u) {
    (void)u; g_dut_hash = g_dut_rowhash; g_dut_rowhash = 0;
    machine.stop = 1;   /* end the frame's instruction loop */
    /* the frame callback sees the finished frame in g_dut_last */
}

#define RS(sym, var) do { void *p = dlsym(lib, sym); if (!p) { fprintf(stderr, "missing %s\n", sym); return 2; } *(void **)&(var) = p; } while (0)

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: gg_compare_reference ROM movie.bk2 frames\n"); return 2; }
    const char *so = getenv("DP_GPGX_SO");
    if (!so) so = "../Genesis-Plus-GX/genesis_plus_gx_libretro.so";
    void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen %s: %s\n", so, dlerror()); return 2; }
    void (*g_retro_run)(void); RS("retro_run", g_retro_run);
    void (*set_env)(lr_environment_t); RS("retro_set_environment", set_env);
    void (*set_video)(lr_video_t); RS("retro_set_video_refresh", set_video);
    void (*set_poll)(lr_poll_t); RS("retro_set_input_poll", set_poll);
    void (*set_input)(lr_input_state_t); RS("retro_set_input_state", set_input);
    void (*set_audio)(void (*)(int16_t, int16_t)); RS("retro_set_audio_sample", set_audio);
    void (*set_audio_batch)(void (*)(int16_t *, size_t)); RS("retro_set_audio_sample_batch", set_audio_batch);
    unsigned (*api)(void); RS("retro_api_version", api);
    void (*init)(void); RS("retro_init", init);
    bool (*load)(const struct lr_game_info *); RS("retro_load_game", load);
    void *(*mem_data)(unsigned); RS("retro_get_memory_data", mem_data);
    size_t (*mem_size)(unsigned); RS("retro_get_memory_size", mem_size);
    void (*get_info)(struct lr_system_info *); RS("retro_get_system_info", get_info);
    #undef RS
    if (api() != 1) { fprintf(stderr, "unsupported libretro API %u\n", api()); return 2; }

    set_video(ref_video); set_poll(ref_poll); set_input(ref_input);
    set_audio(ref_audio_sample); set_audio_batch(ref_audio_batch);
    set_env(ref_env);
    init();
    struct lr_system_info info; get_info(&info);
    fprintf(stderr, "reference: %s %s (extensions %s)\n", info.library_name, info.library_version, info.valid_extensions);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open ROM\n"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *rom = malloc(n);
    if (!rom || fread(rom, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    struct lr_game_info gi = { .path = argv[1], .data = rom, .size = (size_t)n, .meta = NULL };
    if (!load(&gi)) { fprintf(stderr, "retro_load_game refused the ROM\n"); return 2; }
    fprintf(stderr, "reference pixel format %u, framebuffer %ux%u\n", g_ref_fmt, g_ref_w, g_ref_h);

    DpSegaTas tas; char err[512];
    if (!dp_sega_tas_load(argv[2], &tas, err, sizeof err)) { fprintf(stderr, "movie: %s\n", err); return 2; }
    fprintf(stderr, "movie: %u frames\n", tas.frames);
    if (dp_sega_tas_rom_match(&tas, rom, n, err, sizeof err) <= 0)
        fprintf(stderr, "warning: movie/ROM identity not verified: %s\n", err);
    unsigned limit = (unsigned)strtoul(argv[3], NULL, 0);
    if (tas.frames && limit > tas.frames) limit = tas.frames;

    /* DP_SEGA_SYS=sms runs the Master System profile of the same runtime
     * (the VDP clocking and the 8 KiB work RAM are the machine's; the row
     * callback keeps the compiled width, the screen hash stays an
     * observation) */
    const int is_sms = getenv("DP_SEGA_SYS") && !strcmp(getenv("DP_SEGA_SYS"), "sms");
    gg_init_system(rom, (size_t)n, NULL, is_sms);
    machine.row = dut_row; machine.draw_line = dut_line; machine.frame = dut_frame;
    machine.row_out = g_dut_store + GG_ROW_PAD; machine.row_tmp = NULL;
    g_b = dp_sega_backend_gg();
    g_b->init(rom, (size_t)n, NULL, is_sms);   /* BIOS-less profile, bus hooks, mapper detection */
    machine.row = dut_row; machine.draw_line = dut_line; machine.frame = dut_frame;
    machine.row_out = g_dut_store + GG_ROW_PAD; machine.row_tmp = NULL;
    gg_reset();

    uint8_t *ref_ram = mem_data(2);   /* RETRO_MEMORY_SYSTEM_RAM */
    size_t ref_ram_size = mem_size(2);
    if (!ref_ram || ref_ram_size != sizeof machine.ram) {
        fprintf(stderr, "reference SYSTEM_RAM %zu bytes (expected %zu)\n", ref_ram_size, sizeof machine.ram);
        return 2;
    }
    unsigned ram_diff_total = 0; long first_diff_frame = -1; unsigned first_diff_off = 0;
    /* DP_GG_SHIFT=-1: compare the reference frame n with the DUT frame n-1
     * (the reference boots through the BIOS, the DUT starts the game one
     * frame earlier; measured by cross-correlating dumped RAMs) */
    const int shift = getenv("DP_GG_SHIFT") ? atoi(getenv("DP_GG_SHIFT")) : 0;
    static uint8_t prev_ram[8192]; int have_prev = 0;
    const uint8_t *dut_cmp = machine.ram;
    const int every = getenv("DP_GG_EVERY") ? atoi(getenv("DP_GG_EVERY")) : 0;
    {   const char *se = getenv("DP_GG_SNAP");
        g_snap = se ? (unsigned)strtoul(se, NULL, 0) : 0;
        g_snap_save = g_snap;
        if (getenv("DP_GG_SNAPDBG")) fprintf(stderr, "snapinit raw=%s -> %u\n", se ? se : "(null)", g_snap);
    }
    /* DP_GG_DUMP=prefix: append both work RAMs per frame (prefix.ref.bin,
     * prefix.dut.bin) for offline frame-alignment analysis */
    FILE *dump_ref = NULL, *dump_dut = NULL;
    const char *dump = getenv("DP_GG_DUMP");
    if (dump) {
        char path[512];
        snprintf(path, sizeof path, "%s.ref.bin", dump);
        dump_ref = fopen(path, "wb");
        snprintf(path, sizeof path, "%s.dut.bin", dump);
        dump_dut = fopen(path, "wb");
        if (!dump_ref || !dump_dut) { fprintf(stderr, "cannot open DP_GG_DUMP files\n"); return 2; }
    }
    const unsigned watch = getenv("DP_GG_WATCH") ? (unsigned)strtoul(getenv("DP_GG_WATCH"), NULL, 16) : 0;
    for (unsigned frame = 0; frame < limit; frame++) {
        g_pad = tas.masks[frame] & 0x7Fu;   /* bit 128 is the SMS Reset: kept off (the movies that use it are rare; documented) */
        gg_buttons(g_pad);
        g_retro_run();
        /* one real frame: instructions until the frame-end event (same loop
         * as the sega explorer's play_step: service, decode, execute, advance) */
        machine.stop = 0;
        unsigned long long ins = 0;   /* safety bound: a frame is ~7k instructions */
        while (!machine.stop && ins < 10000000ull) {
            z80_t *c = g_b->cpu();
            if (c->fault) break;
            unsigned cycles = z80_service(c);
            if (!cycles) {
                if (!z80_decode(c, &g_d)) break;
                uint8_t bytes[8];
                for (unsigned j = 0; j < g_d.length && j < sizeof bytes; j++) bytes[j] = g_b->read((uint16_t)(c->pc + j));
                DpZ80Instruction in;
                if (!dp_z80_decode(bytes, g_d.length, c->pc, &in)) break;
                cycles = z80_execute_decoded(c, &g_d);
                z80_finish(c, cycles);
            }
            g_b->advance(cycles);
            ins++;
        }
        if (ins >= 10000000ull && !machine.stop)
            fprintf(stderr, "warning: frame %u ended on the instruction bound, not the frame event\n", frame + 1);
        unsigned diff = 0, first = 0;
        dut_cmp = (shift == -1 && have_prev) ? prev_ram : machine.ram;
        for (size_t i = 0; i < ref_ram_size; i++)
            if (ref_ram[i] != dut_cmp[i]) { if (!diff) first = (unsigned)i; diff++; }
        if (shift == -1) { memcpy(prev_ram, machine.ram, sizeof prev_ram); have_prev = 1; }
        ram_diff_total += diff;
        if (diff && first_diff_frame < 0) { first_diff_frame = (long)frame + 1; first_diff_off = first; }
        if (every || frame < 10 || (frame + 1) % 300 == 0 || (diff && first_diff_frame == (long)frame + 1))
            printf("%u ramdiff=%u first=$%04x screen=%s ref=%ux%u\n", frame + 1, diff, first,
                   (g_ref_hash == g_dut_hash) ? "same" : "diff", g_ref_w, g_ref_h);
        unsigned dut_lit = 0, ref_lit = 0;
        for (unsigned i = 0; i < 160 * DUT_LINES; i++) { if (g_dut_last[i]) dut_lit++; if (i < 160 * 144 && g_ref_fb[i]) ref_lit++; }
        static unsigned plit_r = 1, plit_d = 1;
        if (every && (ref_lit != plit_r || dut_lit != plit_d))
            fprintf(stderr, "lit %u ref=%u dut=%u pc=%04x sp=%04x\n", frame + 1, ref_lit, dut_lit, machine.cpu.pc, machine.cpu.sp);
        plit_r = ref_lit; plit_d = dut_lit;
        if (watch && watch + 1 < ref_ram_size)
            fprintf(stderr, "watch %u $%04x ref=%02x%02x dut=%02x%02x\n", frame + 1, watch,
                    ref_ram[watch + 1], ref_ram[watch], dut_cmp[watch + 1], dut_cmp[watch]);
        if (diff && first_diff_frame == (long)frame + 1)
            fprintf(stderr, "first work-RAM divergence at frame %u, offset $%04x\n", frame + 1, first);
        const char *hex = getenv("DP_GG_DIFF_HEX");
        if (hex && diff && (frame < 20 || (frame + 1) % 300 == 0)) {
            unsigned kmax = (unsigned)strtoul(hex, NULL, 0);
            for (size_t i = 0; i < ref_ram_size && kmax; i++)
                if (ref_ram[i] != machine.ram[i]) {
                    fprintf(stderr, "  %u $%04zx ref=%02x dut=%02x\n", frame + 1, i, ref_ram[i], machine.ram[i]);
                    kmax--;
                }
        }
        fflush(stdout);
        if (dump_ref) { fwrite(ref_ram, 1, ref_ram_size, dump_ref); fwrite(machine.ram, 1, sizeof machine.ram, dump_dut); }
        if (g_snap != g_snap_save) {   /* the loaded core scribbled the host .bss: restore and keep going */
            fprintf(stderr, "warning: frame %u: host global clobbered (g_snap %u -> %u)\n",
                    frame + 1, g_snap_save, g_snap);
            g_snap = g_snap_save;
        }
        if (g_snap && frame + 1 == g_snap) {
            char path[256];
            snprintf(path, sizeof path, "/tmp/gg_ref.ppm");
            FILE *o = fopen(path, "wb");
            if (o) { fprintf(o, "P6\n160 %u\n255\n", g_ref_h); for (unsigned i = 0; i < 160 * g_ref_h && i < 160 * DUT_LINES; i++) { unsigned p = g_ref_fb[i]; fputc(((p >> 10) & 31) * 255 / 31, o); fputc(((p >> 5) & 31) * 255 / 31, o); fputc((p & 31) * 255 / 31, o); } fclose(o); }
            snprintf(path, sizeof path, "/tmp/gg_dut.ppm");
            o = fopen(path, "wb");
            if (o) { fprintf(o, "P6\n160 %d\n255\n", DUT_LINES); for (unsigned i = 0; i < 160 * DUT_LINES; i++) { unsigned p = g_dut_last[i]; fputc(((p >> 11) & 31) * 255 / 31, o); fputc(((p >> 6) & 31) * 255 / 31, o); fputc((p & 31) * 255 / 31, o); } fclose(o); }
            fprintf(stderr, "screens dumped to /tmp/gg_ref.ppm /tmp/gg_dut.ppm\n");
        }
    }
    printf("frames %u, mean ramdiff %.2f, first divergence %s (%ld, offset $%04x)\n", limit,
           limit ? (double)ram_diff_total / limit : 0.0,
           first_diff_frame < 0 ? "none" : "at", first_diff_frame, first_diff_off);
    if (dump_ref) { fclose(dump_ref); fclose(dump_dut); }
    free(rom); free(tas.masks);
    dlclose(lib);
    return 0;
}
