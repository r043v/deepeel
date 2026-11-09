/* Standalone differential probe: run one MSX TAS (.bk2/plain input log,
 * sega/tas.h loader) against the fMSX libretro core (reference, level A) and
 * deepeel's MSX runtime (device under test).  Per frame: the work-RAM diff
 * over the common window, plus a coarse screen hash of both.  The reference
 * boots through its own BIOS (files in the system directory) while the DUT
 * runs the simulated BIOS, so early frames legitimately differ: the fidelity
 * signal is the game's own work-RAM usage once the title settles.  The RAM
 * window the reference exposes is picked with DP_MSX_REF_OFF / DP_MSX_REF_SIZE
 * (defaults 0 / 8 KiB).  No reference implementation is copied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>

#include "z80.h"
#include "backend.h"
#include "tas.h"
#include "explore.h"
#include "sega.h"
#include <sg1000.h>
/* the runtime archive namespaces its globals (cmake/dp_msx_prefix.cmake) */
#define machine dp_msx_runtime_machine
extern sg1000_t machine;

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

enum { LR_ID_B = 0, LR_ID_Y = 1, LR_ID_SELECT = 2, LR_ID_START = 3, LR_ID_UP = 4,
       LR_ID_DOWN = 5, LR_ID_LEFT = 6, LR_ID_RIGHT = 7, LR_ID_A = 8 };

static unsigned g_pad;               /* sega mask of this frame (up=1..start=64) */
static uint64_t g_ref_hash;
static unsigned g_ref_w, g_ref_h, g_ref_fmt = LR_PF_0RGB1555;

static void ref_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    uint64_t h1 = 0xcbf29ce484222325ull;
    const uint8_t *row = data;
    for (unsigned y = 0; y < h; y++, row += pitch) {
        const uint32_t *px = (const uint32_t *)row;
        for (unsigned x = 0; x < w; x++) {
            unsigned p = px[x], r, g, b;
            if (g_ref_fmt == LR_PF_RGB565) { r = (p >> 11) & 31; g = (p >> 6) & 31; b = p & 31; }
            else if (g_ref_fmt == LR_PF_0RGB1555) { r = p & 31; g = (p >> 5) & 31; b = (p >> 10) & 31; }
            else { r = (p >> 16) & 31; g = (p >> 8) & 31; b = p & 31; }
            h1 = (h1 ^ (r * 16 + g * 4 + b)) * 0x100000001b3ull;
        }
    }
    g_ref_hash = h1; g_ref_w = w; g_ref_h = h;
}
static int16_t ref_input(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)device; (void)index;
    if (port != 0) return 0;
    switch (id) {
    case LR_ID_UP: return (g_pad & 1) != 0;
    case LR_ID_DOWN: return (g_pad & 2) != 0;
    case LR_ID_LEFT: return (g_pad & 4) != 0;
    case LR_ID_RIGHT: return (g_pad & 8) != 0;
    case LR_ID_B: return (g_pad & 16) != 0;    /* trigger / space */
    case LR_ID_A: return (g_pad & 32) != 0;    /* second button */
    case LR_ID_START: return (g_pad & 64) != 0; /* RETURN */
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
    case LR_ENV_GET_SYSTEM_DIRECTORY: { const char *d = getenv("DP_MSX_SYS"); *(const char **)data = d ? d : "/tmp"; return true; }
    default: return false;
    }
}

/* --- device under test: the MSX runtime ------------------------------------ */
static const DpSegaBackend *g_b;
static z80_decoded_t g_d;
static uint64_t g_dut_hash, g_dut_rowhash;
#define DUT_WIDTH SG_OUT_WIDTH
#define DUT_LINES 192
static uint16_t g_dut_last[SG_OUT_WIDTH * 192];
static uint16_t g_dut_store[SG_OUT_WIDTH + 2];   /* +2: the runtime pads row_out on both sides */
static void dut_row(void *u, unsigned y, uint16_t *px) {
    (void)u; (void)y;
    for (unsigned i = 0; i < DUT_WIDTH; i++) {
        unsigned p = px[i], r = (p >> 11) & 31, g = (p >> 6) & 31, b = p & 31;
        g_dut_rowhash = (g_dut_rowhash ^ (r * 16 + g * 4 + b)) * 0x100000001b3ull;
        g_dut_last[y * DUT_WIDTH + i] = p;
    }
}
static int dut_line(void *u, unsigned y) { (void)u; (void)y; return 1; }
static void dut_frame(void *u) {
    (void)u; g_dut_hash = g_dut_rowhash; g_dut_rowhash = 0;
    machine.stop = 1;   /* end the frame's instruction loop */
}

#define RS(sym, var) do { void *p = dlsym(lib, sym); if (!p) { fprintf(stderr, "missing %s\n", sym); return 2; } *(void **)&(var) = p; } while (0)

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: msx_compare_reference ROM movie.frames count\n"); return 2; }
    const char *so = getenv("DP_FMSX_SO");
    if (!so) so = "../lr-cores/fmsx_libretro.so";
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
    uint8_t *rom = malloc((size_t)n);
    if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    struct lr_game_info gi = { .path = argv[1], .data = rom, .size = (size_t)n, .meta = NULL };
    if (!load(&gi)) { fprintf(stderr, "retro_load_game refused the ROM\n"); return 2; }

    DpSegaTas tas; char err[512];
    if (!dp_sega_tas_load(argv[2], &tas, err, sizeof err)) { fprintf(stderr, "movie: %s\n", err); return 2; }
    fprintf(stderr, "movie: %u frames\n", tas.frames);
    unsigned limit = (unsigned)strtoul(argv[3], NULL, 0);
    if (!limit) limit = tas.frames;              /* the movie is replayed in full by default */
    if (tas.frames && limit > tas.frames) limit = tas.frames;

    extern const DpSegaBackend *dp_msx_backend(void);
    g_b = dp_msx_backend();
    if (!g_b->init(rom, (size_t)n, NULL, DP_MSX_AUTO)) { fprintf(stderr, "the runtime refused the ROM (mapper?)\n"); return 2; }
    machine.row = dut_row; machine.draw_line = dut_line; machine.frame = dut_frame;
    machine.row_out = g_dut_store + 1;
    { /* advance a whole frame's worth directly: does sg_sync reach the boundary? */
        for (int i = 0; i < 60000; i++) g_b->advance(6);
        fprintf(stderr, "probe: after 360k cycles frames=%u vb=%u cyc=%u\n", machine.frames, machine.vblank_done, machine.cyc);
        extern void dp_msx_runtime_sg_sync(void);
        dp_msx_runtime_sg_sync();
        fprintf(stderr, "probe2: direct sg_sync frames=%u vb=%u\n", machine.frames, machine.vblank_done);
    }

    uint8_t *ref_ram = mem_data(2);   /* RETRO_MEMORY_SYSTEM_RAM */
    size_t ref_ram_size = mem_size(2);
    size_t off = getenv("DP_MSX_REF_OFF") ? (size_t)strtoul(getenv("DP_MSX_REF_OFF"), NULL, 0) : 0;
    size_t win = sizeof machine.ram;
    if (getenv("DP_MSX_REF_SIZE")) win = (size_t)strtoul(getenv("DP_MSX_REF_SIZE"), NULL, 0);
    if (!ref_ram) { fprintf(stderr, "reference has no SYSTEM_RAM\n"); return 2; }
    if (off + win > ref_ram_size) win = ref_ram_size > off ? ref_ram_size - off : 0;
    fprintf(stderr, "reference SYSTEM_RAM %zu bytes, comparing [%zu,%zu) against %zu\n", ref_ram_size, off, off + win, sizeof machine.ram);

    const int every = getenv("DP_MSX_EVERY") ? atoi(getenv("DP_MSX_EVERY")) : 0;
    /* DP_MSX_DUMP=prefix: append both RAM windows per frame for offline analysis */
    FILE *dump_ref = NULL, *dump_dut = NULL;
    const char *dump = getenv("DP_MSX_DUMP");
    if (dump) {
        char path[512];
        snprintf(path, sizeof path, "%s.ref.bin", dump); dump_ref = fopen(path, "wb");
        snprintf(path, sizeof path, "%s.dut.bin", dump); dump_dut = fopen(path, "wb");
        if (!dump_ref || !dump_dut) { fprintf(stderr, "cannot open DP_MSX_DUMP files\n"); return 2; }
    }
    unsigned ram_diff_total = 0; long first_diff_frame = -1; unsigned first_diff_off = 0;
    for (unsigned frame = 0; frame < limit; frame++) {
        g_pad = tas.masks[frame] & 0x7Fu;
        /* the DUT input: pad bits, start = RETURN (bit 6); the reference reads
         * the same mask through the libretro input callback */
        DpSegaInput in = {{0,0},0,0,0};
        in.pad[0] = (uint8_t)(g_pad & 63);
        in.start = !!(g_pad & 64);
        g_b->set_input(in);
        g_retro_run();
        /* one real frame: instructions until the frame-end event */
        machine.stop = 0;
        unsigned long long ins = 0;
        while (!machine.stop && ins < 20000000ull) {
            z80_t *c = g_b->cpu();
            if (c->fault) break;
            unsigned cycles = z80_service(c);
            if (!cycles) {
                if (!z80_decode(c, &g_d)) break;
                uint8_t bytes[8];
                for (unsigned j = 0; j < g_d.length && j < sizeof bytes; j++) bytes[j] = g_b->read((uint16_t)(c->pc + j));
                DpZ80Instruction dec;
                if (!dp_z80_decode(bytes, g_d.length, c->pc, &dec)) break;
                cycles = z80_execute_decoded(c, &g_d);
                z80_finish(c, cycles);
            }
            g_b->advance(cycles);
            ins++;
        }
        if (ins >= 20000000ull && !machine.stop)
            fprintf(stderr, "warning: frame %u ended on the instruction bound, not the frame event\n", frame + 1);
        unsigned diff = 0, first = 0;
        for (size_t i = 0; i < win; i++)
            if (ref_ram[off + i] != machine.ram[i]) { if (!diff) first = (unsigned)i; diff++; }
        ram_diff_total += diff;
        if (diff && first_diff_frame < 0) { first_diff_frame = (long)frame + 1; first_diff_off = first; }
        if (every || frame < 10 || (frame + 1) % 300 == 0 || (diff && first_diff_frame == (long)frame + 1))
            printf("%u ramdiff=%u first=$%04x screen=%s ref=%ux%u\n", frame + 1, diff, first,
                   (g_ref_hash == g_dut_hash) ? "same" : "diff", g_ref_w, g_ref_h);
        if (diff && first_diff_frame == (long)frame + 1)
            fprintf(stderr, "first work-RAM divergence at frame %u, offset $%04x\n", frame + 1, first);
        fflush(stdout);
        if (dump_ref) { fwrite(ref_ram, 1, ref_ram_size, dump_ref); fwrite(machine.ram, 1, sizeof machine.ram, dump_dut); }
    }
    printf("frames %u, mean ramdiff %.2f, first divergence %s (%ld, offset $%04x)\n", limit,
           limit ? (double)ram_diff_total / (double)limit : 0.0,
           first_diff_frame < 0 ? "none" : "at", first_diff_frame, first_diff_off);
    if (dump_ref) fclose(dump_ref);
    if (dump_dut) fclose(dump_dut);
    return 0;
}
