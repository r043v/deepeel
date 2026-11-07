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
static void dut_row(void *u, unsigned y, uint16_t *px) {
    (void)u; (void)y;
    for (unsigned i = 0; i < GG_WIDTH; i++) {
        unsigned p = px[i], r = (p >> 11) & 31, g = (p >> 6) & 31, b = p & 31;
        g_dut_rowhash = (g_dut_rowhash ^ (r * 16 + g * 4 + b)) * 0x100000001b3ull;
    }
}
static int dut_line(void *u, unsigned y) { (void)u; (void)y; return 1; }
static void dut_frame(void *u) { (void)u; g_dut_hash = g_dut_rowhash; g_dut_rowhash = 0; }

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
    if (dp_sega_tas_rom_match(&tas, rom, n, err, sizeof err) == 0)
        fprintf(stderr, "warning: movie/ROM identity mismatch: %s\n", err);
    unsigned limit = (unsigned)strtoul(argv[3], NULL, 0);
    if (tas.frames && limit > tas.frames) limit = tas.frames;

    gg_init_system(rom, (size_t)n, NULL, 0);
    machine.row = dut_row; machine.draw_line = dut_line; machine.frame = dut_frame;
    gg_reset();

    uint8_t *ref_ram = mem_data(2);   /* RETRO_MEMORY_SYSTEM_RAM */
    size_t ref_ram_size = mem_size(2);
    if (!ref_ram || ref_ram_size != sizeof machine.ram) {
        fprintf(stderr, "reference SYSTEM_RAM %zu bytes (expected %zu)\n", ref_ram_size, sizeof machine.ram);
        return 2;
    }
    unsigned ram_diff_total = 0; long first_diff_frame = -1; unsigned first_diff_off = 0;
    for (unsigned frame = 0; frame < limit; frame++) {
        g_pad = tas.masks[frame] & 0x7Fu;   /* bit 128 is the SMS Reset: GG ignores it */
        gg_buttons(g_pad);
        g_retro_run();
        machine.cyc += GG_FRAME_CYCLES;
        sg_sync();
        unsigned diff = 0, first = 0;
        for (size_t i = 0; i < ref_ram_size; i++)
            if (ref_ram[i] != machine.ram[i]) { if (!diff) first = (unsigned)i; diff++; }
        ram_diff_total += diff;
        if (diff && first_diff_frame < 0) { first_diff_frame = (long)frame + 1; first_diff_off = first; }
        if (frame < 10 || (frame + 1) % 300 == 0 || (diff && first_diff_frame == (long)frame + 1))
            printf("%u ramdiff=%u first=$%04x screen=%s ref=%ux%u\n", frame + 1, diff, first,
                   (g_ref_hash == g_dut_hash) ? "same" : "diff", g_ref_w, g_ref_h);
        if (diff && first_diff_frame == (long)frame + 1) {
            fprintf(stderr, "first work-RAM divergence at frame %u, offset $%04x\n", frame + 1, first);
            const char *hex = getenv("DP_GG_DIFF_HEX");
            if (hex) {
                unsigned kmax = (unsigned)strtoul(hex, NULL, 0);
                for (size_t i = first; i < ref_ram_size && i < first + kmax; i++)
                    fprintf(stderr, "  $%04zx ref=%02x dut=%02x\n", i, ref_ram[i], machine.ram[i]);
            }
        }
        fflush(stdout);
    }
    printf("frames %u, mean ramdiff %.2f, first divergence %s (%ld, offset $%04x)\n", limit,
           limit ? (double)ram_diff_total / limit : 0.0,
           first_diff_frame < 0 ? "none" : "at", first_diff_frame, first_diff_off);
    free(rom); free(tas.masks);
    dlclose(lib);
    return 0;
}
