/* Standalone differential probe: run one PC Engine TAS (.bk2 or plain text)
 * against the Beetle PCE libretro core (reference, Mednafen) and deepeel's
 * m0boy PCE runtime (device under test).  Per frame: the 8 KiB work-RAM diff
 * with the first differing byte.  The cores cut frames differently; the RAM
 * diff is the fidelity signal.  No reference implementation is copied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>

#include "pce.h"
#include "tas.h"

/* --- libretro 1.x surface (subset) ---------------------------------------- */
#define LR_ENV_SET_PIXEL_FORMAT 10
#define LR_ENV_GET_SYSTEM_DIRECTORY 9
enum { LR_PF_0RGB1555 = 0, LR_PF_XRGB8888 = 1, LR_PF_RGB565 = 2 };
struct lr_system_info { const char *library_name, *library_version, *valid_extensions; bool need_fullpath, block_extract; };
struct lr_game_info { const char *path; const void *data; size_t size; const char *meta; };

typedef bool (*lr_environment_t)(unsigned cmd, void *data);
typedef void (*lr_video_t)(const void *data, unsigned w, unsigned h, size_t pitch);
typedef void (*lr_poll_t)(void);
typedef int16_t (*lr_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

/* RetroPad ids read by Beetle PCE's gamepad: B -> button I, A -> button II,
 * Y -> III, X -> IV, L -> V, R -> VI, Start = Run */
enum { LR_ID_B = 0, LR_ID_SELECT = 2, LR_ID_START = 3, LR_ID_UP = 4,
       LR_ID_DOWN = 5, LR_ID_LEFT = 6, LR_ID_RIGHT = 7, LR_ID_A = 8 };

static unsigned g_pad;                    /* m0boy pad layout: bit 0 I, 1 II, 2 Select, 3 Run, 4 Up, 5 Right, 6 Down, 7 Left */
static unsigned g_ref_fmt = LR_PF_0RGB1555;
static int g_reset_request;

static int16_t ref_input(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)device; (void)index;
    if (port != 0) return 0;
    switch (id) {
    case LR_ID_B: return (g_pad & 0x01) != 0;     /* I */
    case LR_ID_A: return (g_pad & 0x02) != 0;     /* II */
    case LR_ID_SELECT: return (g_pad & 0x04) != 0;
    case LR_ID_START: return (g_pad & 0x08) != 0; /* Run */
    case LR_ID_UP: return (g_pad & 0x10) != 0;
    case LR_ID_DOWN: return (g_pad & 0x40) != 0;  /* bit 6 */
    case LR_ID_LEFT: return (g_pad & 0x80) != 0;  /* bit 7 */
    case LR_ID_RIGHT: return (g_pad & 0x20) != 0; /* bit 5 */
    default: return 0;
    }
}
static void ref_poll(void) {}
static void ref_video(const void *data, unsigned w, unsigned h, size_t pitch) { (void)data; (void)w; (void)h; (void)pitch; }
static void ref_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static void ref_audio_batch(int16_t *data, size_t frames) { (void)data; (void)frames; }
static bool ref_env(unsigned cmd, void *data) {
    switch (cmd) {
    case LR_ENV_SET_PIXEL_FORMAT: g_ref_fmt = *(const unsigned *)data; return true;
    case LR_ENV_GET_SYSTEM_DIRECTORY: *(const char **)data = "/tmp"; return true;
    default: return false;
    }
}

#define RS(sym, var) do { void *p = dlsym(lib, sym); if (!p) { fprintf(stderr, "missing %s\n", sym); return 2; } *(void **)&(var) = p; } while (0)

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: pce_compare_reference ROM movie frames\n"); return 2; }
    const char *so = getenv("DP_BEETLE_SO");
    if (!so) so = "../beetle-pce-libretro/mednafen_pce_libretro.so";
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
    #undef RS
    if (api() != 1) { fprintf(stderr, "unsupported libretro API %u\n", api()); return 2; }
    set_video(ref_video); set_poll(ref_poll); set_input(ref_input);
    set_audio(ref_audio_sample); set_audio_batch(ref_audio_batch);
    set_env(ref_env);
    init();

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open ROM\n"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *rom = malloc((size_t)n);
    if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    struct lr_game_info gi = { .path = argv[1], .data = rom, .size = (size_t)n, .meta = NULL };
    if (!load(&gi)) { fprintf(stderr, "retro_load_game refused the ROM\n"); return 2; }

    DpPceTas tas; char terr[512];
    if (!dp_pce_tas_load(argv[2], &tas, terr, sizeof terr)) { fprintf(stderr, "movie: %s\n", terr); return 2; }
    fprintf(stderr, "movie: %u frames\n", tas.frames);
    unsigned limit = (unsigned)strtoul(argv[3], NULL, 0);
    if (tas.frames && limit > tas.frames) limit = tas.frames;

    uint8_t *ref_ram = mem_data(2);   /* RETRO_MEMORY_SYSTEM_RAM: the 8 KiB */
    size_t ref_ram_size = mem_size(2);
    /* Beetle's SYSTEM_RAM carries the SuperGrafx/CD extra banks after the
     * 8 KiB the HuCard games use: compare the shared low 8 KiB */
    if (!ref_ram || ref_ram_size < 8192) {
        fprintf(stderr, "reference SYSTEM_RAM %zu bytes (expected >= 8192)\n", ref_ram_size);
        return 2;
    }
    ref_ram_size = 8192;
    /* device under test: the m0boy PCE runtime (singleton) */
    if (!dp_pce_init(rom, (size_t)n)) { fprintf(stderr, "pce: init refused the ROM\n"); return 2; }
    dp_pce_reset();

    unsigned ram_diff_total = 0; long first_diff_frame = -1; unsigned first_diff_off = 0;
    const int every = getenv("DP_PCE_EVERY") ? atoi(getenv("DP_PCE_EVERY")) : 0;
    for (unsigned frame = 0; frame < limit; frame++) {
        g_pad = tas.pads[frame];
        g_reset_request = tas.resets ? tas.resets[frame] : 0;
        dp_pce_input((uint8_t)g_pad);
        if (g_reset_request) { dp_pce_reset(); }
        g_retro_run();
        /* one real frame on the DUT: steps until the frame event */
        int frame_done = 0;
        unsigned guard = 0;
        while (!frame_done && guard++ < 2000000u) dp_pce_step(&frame_done);
        unsigned diff = 0, first = 0;
        const uint8_t *dut_ram = dp_pce_wram();
        static uint8_t prev_dut[8192]; static unsigned have_prev;
        if (getenv("DP_PCE_SHIFT")) {   // compare ref frame N with dut frame N+1 (phase-shift probe)
            if (have_prev) {
                unsigned sd = 0; unsigned sfirst = 0;
                for (size_t i = 0; i < ref_ram_size; i++)
                    if (ref_ram[i] != prev_dut[i]) { if (!sd) sfirst = (unsigned)i; sd++; }
                printf("shift %u ramdiff=%u first=$%04x\n", frame, sd, sfirst);
            }
            memcpy(prev_dut, dut_ram, ref_ram_size); have_prev = 1;
        }
        for (size_t i = 0; i < ref_ram_size; i++)
            if (ref_ram[i] != dut_ram[i]) { if (!diff) first = (unsigned)i; diff++; }
        ram_diff_total += diff;
        if (diff && first_diff_frame < 0) { first_diff_frame = (long)frame + 1; first_diff_off = first; }
        {   // DP_PCE_DUMP=FRAME: the first 16 differing cells of that frame
            const char *dp = getenv("DP_PCE_DUMP");
            if (dp && (long)frame + 1 == atoi(dp)) {
                unsigned shown = 0;
                for (size_t i = 0; i < ref_ram_size && shown < 16; i++)
                    if (ref_ram[i] != dut_ram[i]) { printf("  $%04zx ref=%02x dut=%02x\n", i, ref_ram[i], dut_ram[i]); shown++; }
            }
        }
        if (every || frame < 10 || (frame + 1) % 300 == 0 || (diff && first_diff_frame == (long)frame + 1))
            printf("%u ramdiff=%u first=$%04x\n", frame + 1, diff, first);
        if (diff && first_diff_frame == (long)frame + 1)
            fprintf(stderr, "first work-RAM divergence at frame %u, offset $%04x (ref=%02x dut=%02x)\n",
                    frame + 1, first, ref_ram[first], dut_ram[first]);
        fflush(stdout);
    }
    printf("frames %u, mean ramdiff %.2f, first divergence %s (%ld, offset $%04x)\n", limit,
           limit ? (double)ram_diff_total / limit : 0.0,
           first_diff_frame < 0 ? "none" : "at", first_diff_frame, first_diff_off);
    free(rom); free(tas.pads); free(tas.resets);
    dlclose(lib);
    return 0;
}
