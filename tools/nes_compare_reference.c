/* Standalone differential probe: run one NES TAS (.bk2/.fm2) against the
 * Nestopia libretro core (reference) and deepeel's m0boy NES runtime (device
 * under test).  Per frame: the 2 KiB RAM diff with the first differing byte.
 * The cores cut frames differently; the RAM diff is the fidelity signal.
 * No reference implementation is copied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>

#include "nesmach.h"
#include "tas.h"

/* --- device under test ------------------------------------------------------ */
/* one frame = run to the frame boundary: the runtime's frame hook raises the
 * observer stop flag, the run honours it at the next instruction boundary */
static DpNesMach *g_dut;
/* ONE cut per iteration, at the NMI (line 241): the frame-end cut competed
 * with it and the iterations alternated between the two points — the RAM
 * dumps alternated pre/post handler (the $92 counter read 1,1,2,2 where the
 * reference reads 1,2,3: the handler runs every frame, measured by counting
 * the INC $92 events).  Nestopia's retro_run returns one frame; the NMI is
 * the DUT's matching boundary. */
static void dut_on_interrupt(void *user, unsigned vector) {
    (void)user;
    if (vector == 0xFFFA) dp_nesmach_cut(g_dut);
}

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

/* RetroPad ids of the standard NES pad read by Nestopia: B -> NES A, A -> NES B */
enum { LR_ID_B = 0, LR_ID_SELECT = 2, LR_ID_START = 3, LR_ID_UP = 4,
       LR_ID_DOWN = 5, LR_ID_LEFT = 6, LR_ID_RIGHT = 7, LR_ID_A = 8 };

static unsigned g_pad;                    /* fm2 mask of this frame (A=1 B=2 Sel=4 Start=8 Up=16 Down=32 Left=64 Right=128) */
static unsigned g_ref_fmt = LR_PF_0RGB1555;
static int g_reset_request;               /* the movie's reset command */

static int16_t ref_input(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)device; (void)index;
    if (port != 0) return 0;
    switch (id) {
    case LR_ID_B: return (g_pad & 0x01) != 0;     /* NES A */
    case LR_ID_A: return (g_pad & 0x02) != 0;     /* NES B */
    case LR_ID_SELECT: return (g_pad & 0x04) != 0;
    case LR_ID_START: return (g_pad & 0x08) != 0;
    case LR_ID_UP: return (g_pad & 0x10) != 0;
    case LR_ID_DOWN: return (g_pad & 0x20) != 0;
    case LR_ID_LEFT: return (g_pad & 0x40) != 0;
    case LR_ID_RIGHT: return (g_pad & 0x80) != 0;
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
    if (argc != 4) { fprintf(stderr, "usage: nes_compare_reference ROM movie.bk2|fm2 frames\n"); return 2; }
    const char *so = getenv("DP_NESTOPIA_SO");
    if (!so) so = "../nestopia-libretro/libretro/nestopia_libretro.so";
    void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen %s: %s\n", so, dlerror()); return 2; }
    void (*g_retro_run)(void); RS("retro_run", g_retro_run);
    void (*g_retro_reset)(void); RS("retro_reset", g_retro_reset);
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
    uint8_t *rom = malloc(n);
    if (!rom || fread(rom, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    struct lr_game_info gi = { .path = argv[1], .data = rom, .size = (size_t)n, .meta = NULL };
    if (!load(&gi)) { fprintf(stderr, "retro_load_game refused the ROM\n"); return 2; }

    DpNesTas tas; char terr[512];
    if (!dp_nes_tas_load(argv[2], &tas, terr, sizeof terr)) { fprintf(stderr, "movie: %s\n", terr); return 2; }
    fprintf(stderr, "movie: %u frames (%u resets)\n", tas.frames, tas.resets);
    unsigned limit = (unsigned)strtoul(argv[3], NULL, 0);
    if (tas.frames && limit > tas.frames) limit = tas.frames;

    uint8_t *ref_ram = mem_data(2);   /* RETRO_MEMORY_SYSTEM_RAM: the 2 KiB */
    size_t ref_ram_size = mem_size(2);
    if (!ref_ram || ref_ram_size != 2048) {
        fprintf(stderr, "reference SYSTEM_RAM %zu bytes (expected 2048)\n", ref_ram_size);
        return 2;
    }
    /* device under test: the m0boy NES runtime, mapper from the iNES header */
    char derr[256];
    DpNesMach *dut = dp_nesmach_open(rom, (size_t)n, -1, derr, sizeof derr);
    if (!dut) { fprintf(stderr, "nesmach: %s\n", derr); return 2; }
    g_dut = dut;
    DpNesHooks hooks = { .user = dut, .on_interrupt = dut_on_interrupt };
    dp_nesmach_set_hooks(dut, &hooks);
    dp_nesmach_reset(dut);

    unsigned ram_diff_total = 0; long first_diff_frame = -1; unsigned first_diff_off = 0;
    const int every = getenv("DP_NES_EVERY") ? atoi(getenv("DP_NES_EVERY")) : 0;
    /* DP_NES_SHIFT=n: compare the reference frame n with the DUT frame n+n
     * (measured on DuckTales: a constant +5 — Nestopia's boot spends ~5 more
     * frames before the game's first NMI than the BIOS-less m0boy boot) */
    const int shift = getenv("DP_NES_SHIFT") ? atoi(getenv("DP_NES_SHIFT")) : 0;
    static uint8_t *ref_ring[16]; static int ring_n = 0;   // delayed reference snapshots
    if (shift > 0) {
        for (int i = 0; i < shift && i < 16; i++) ref_ring[i] = malloc(ref_ram_size);
        ring_n = shift < 16 ? shift : 16;
    }
    /* DP_NES_DUMP=prefix: append both RAMs per frame for offline
     * frame-alignment analysis (cross-correlation, as the GG bench) */
    FILE *dump_ref = NULL, *dump_dut = NULL;
    const char *dump = getenv("DP_NES_DUMP");
    if (dump) {
        char path[512];
        snprintf(path, sizeof path, "%s.ref.bin", dump); dump_ref = fopen(path, "wb");
        snprintf(path, sizeof path, "%s.dut.bin", dump); dump_dut = fopen(path, "wb");
        if (!dump_ref || !dump_dut) { fprintf(stderr, "cannot open DP_NES_DUMP files\n"); return 2; }
    }
    for (unsigned frame = 0; frame < limit; frame++) {
        g_pad = tas.masks[frame];
        g_reset_request = (tas.cmds && (tas.cmds[frame] & 2)) != 0;
        dp_nesmach_set_input(dut, g_pad, 0);
        /* the reference plays the same frame, reset included */
        if (g_reset_request) g_retro_reset();
        g_retro_run();
        /* one real frame on the DUT: run until the frame boundary (the
         * frame hook raises the stop flag at the boundary) */
        dp_nesmach_cut(dut);   /* clear the stop flag through the public API */
        dp_nesmach_run(dut, UINT64_MAX);
        const uint8_t *dut_ram = dp_nesmach_ram(dut);
        const uint8_t *ref_cmp = ref_ram;
        if (shift > 0 && ring_n == shift) {
            // the ring holds the reference snapshots of the last `shift` frames
            static int ring_at = 0;
            memcpy(ref_ring[ring_at], ref_ram, ref_ram_size);
            ring_at = (ring_at + 1) % ring_n;
            ref_cmp = ref_ring[ring_at];   // `shift` frames old
        }
        unsigned diff = 0, first = 0;
        for (size_t i = 0; i < ref_ram_size; i++)
            if (ref_cmp[i] != dut_ram[i]) { if (!diff) first = (unsigned)i; diff++; }
        ram_diff_total += diff;
        if (diff && first_diff_frame < 0) { first_diff_frame = (long)frame + 1; first_diff_off = first; }
        if (every || frame < 10 || (frame + 1) % 300 == 0 || (diff && first_diff_frame == (long)frame + 1))
            printf("%u ramdiff=%u first=$%04x\n", frame + 1, diff, first);
        if (diff && first_diff_frame == (long)frame + 1)
            fprintf(stderr, "first work-RAM divergence at frame %u, offset $%04x\n", frame + 1, first);
        fflush(stdout);
        if (dump_ref) { fwrite(ref_ram, 1, ref_ram_size, dump_ref); fwrite(dp_nesmach_ram(dut), 1, 2048, dump_dut); }
    }
    printf("frames %u, mean ramdiff %.2f, first divergence %s (%ld, offset $%04x)\n", limit,
           limit ? (double)ram_diff_total / limit : 0.0,
           first_diff_frame < 0 ? "none" : "at", first_diff_frame, first_diff_off);
    if (dump_ref) { fclose(dump_ref); fclose(dump_dut); }
    free(rom); free(tas.masks); free(tas.cmds);
    dp_nesmach_close(dut);
    dlclose(lib);
    return 0;
}
