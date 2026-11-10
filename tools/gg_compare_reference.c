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
#include <time.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <signal.h>

#include "../src/gui/watch.h"

#include "gg.h"
#include "z80.h"
#include "backend.h"
#include "tas.h"

/* Native 256-wide Master System instance of the same runtime (libdeepeel_sms:
 * GG_OUT_WIDTH=256, every global prefixed dp_sms_ — the PC Master System
 * panel, no GG window crop, no filter). The bench picks the instance by
 * profile and every reference to the runtime state goes through g_mach. */
extern gg_t dp_sms_machine;
extern void dp_sms_gg_init_system(const uint8_t *rom, size_t size, uint8_t *cart_ram, int sms);
extern void dp_sms_gg_reset(void);
extern void dp_sms_gg_buttons(unsigned b);
extern const DpSegaBackend *dp_sega_backend_sms(void);
static gg_t *g_mach;                 /* the playing instance's runtime state */
static unsigned g_dut_w;             /* DUT panel width: 256 (SMS native) or 160 (GG window) */
#define DUT_W_MAX 256

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
static unsigned g_ref_frames_seen;   /* reference video callbacks (watch f/s) */
static unsigned g_snap;              /* dump both screens at this 1-based frame */
static unsigned g_snap_save;         /* the loaded core may scribble host .bss: g_snap is re-read here each frame */

/* One screen buffer per core, one capture path for both sides: screen_row
 * normalizes the incoming 16-bit pixels to 5:6:5 into the buffer, mixes the
 * same normalized pixels into the frame hash (the screen verdict) and
 * records the panel's own size. No crop, no per-side adaptation: the
 * reference's whole-frame callback and the DUT's per-row callback run the
 * exact same code on the exact same buffer shape. */
typedef struct {
    uint16_t px[DUT_W_MAX * DUT_LINES];
    unsigned w, h;           /* the panel's own size, as captured */
    unsigned fmt;            /* the incoming pixel format (LR_PF_*) */
    uint64_t row_hash;       /* mixed through the frame's rows */
    uint64_t hash;           /* the finished frame's digest */
} Screen;

static Screen g_ref = { .fmt = LR_PF_0RGB1555 }, g_dut = { .fmt = LR_PF_RGB565 }; /* the runtime outputs 5:6:5 */

static void screen_frame_start(Screen *s) {
    s->row_hash = 0xcbf29ce484222325ull;
}

static void screen_row(Screen *s, unsigned y, const void *data, size_t pitch, unsigned w) {
    if (y >= DUT_LINES || w > DUT_W_MAX) return;
    const uint16_t *src = data;
    uint16_t *dst = s->px + (size_t)y * DUT_W_MAX;
    uint64_t h1 = s->row_hash;
    for (unsigned x = 0; x < w; x++) {
        unsigned p = src[x], r, g, b;
        if (s->fmt == LR_PF_RGB565) { r = (p >> 11) & 31; g = (p >> 6) & 31; b = p & 31; }
        else { r = p & 31; g = (p >> 5) & 31; b = (p >> 10) & 31; }
        h1 = (h1 ^ (r * 16 + g * 4 + b)) * 0x100000001b3ull;
        dst[x] = (uint16_t)((r << 11) | (g << 6) | b);
    }
    s->row_hash = h1;
    if (y + 1 > s->h) s->h = y + 1;
    if (w > s->w) s->w = w;
}

static void ref_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    screen_frame_start(&g_ref);
    for (unsigned y = 0; y < h; y++)
        screen_row(&g_ref, y, (const uint8_t *)data + y * pitch, pitch, w);
    g_ref.w = w; g_ref.h = h;
    g_ref.hash = g_ref.row_hash;
    g_ref_frames_seen++;
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
    case LR_ENV_SET_PIXEL_FORMAT: g_ref.fmt = *(const unsigned *)data; return true;
    case LR_ENV_GET_CAN_DUPE: *(bool *)data = true; return true;
    case LR_ENV_GET_SYSTEM_DIRECTORY: *(const char **)data = "/tmp"; return true;
    default: return false;
    }
}

/* --- device under test: the m0boy GG runtime ------------------------------- */
/* --- watch window (DP_GG_WINDOW=1) ---------------------------------------- */
/* Two tiles side by side: the DUT (deepeel's m0boy runtime) on the left, the
 * reference core on the right. The window itself is src/gui/watch.c, the
 * generic replay watch: the tiles at the platform's own native buffer ratio
 * (SMS 256x192, GG 160x144), letterboxed, never stretched, small screens at
 * x2, captions under each tile. */
/* --- watch window (DP_GG_WINDOW=1) ---------------------------------------- */
/* Two tiles side by side (src/gui/watch.c, the generic replay watch: one tile
 * per core at the platform's own native buffer ratio — SMS 256x192, GG
 * 160x144 — letterboxed, never stretched, small screens at x2, captions
 * under each tile). The left tile is deepeel's m0boy runtime, the right one
 * the reference core. */
static DpWatch *g_watch;
static int have_window;                  /* DP_GG_WINDOW seen (window opened at DP_GG_SEEK) */
static unsigned g_frame_no;              /* absolute movie frame (dump target) */
static unsigned g_watch_mframes;         /* movie total frames (f/s caption) */
static const char *g_watch_movie;        /* movie file name (caption) */
static unsigned g_watch_total, g_watch_rtotal; /* absolute frames (caption) */
static unsigned g_watch_n, g_watch_rn;   /* frames in the current f/s window */
static unsigned g_watch_seen;            /* total frames (one-shot tile dump) */
static unsigned g_watch_t0;              /* f/s window start (ms clock) */
static char g_l2[128], g_r2[128];        /* caption line 2 of each tile */

static unsigned now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void on_term_signal(int sig) {
    fprintf(stderr, "bench: signal %d (%s) received\n", sig,
            sig == SIGTERM ? "SIGTERM" : sig == SIGINT ? "SIGINT" : "SIGHUP");
}

static FILE *hawk;   /* DP_GG_HAWK_FRAMES: SMSHawk's pre-dumped frames (declared in main) */
static void watch_open(unsigned movie_frames, const char *movie_path, unsigned buf_w, unsigned buf_h) {
    g_watch_movie = strrchr(movie_path, '/');
    g_watch_movie = g_watch_movie ? g_watch_movie + 1 : movie_path;
    g_watch_mframes = movie_frames;
    char title[160];
    snprintf(title, sizeof title, "deepeel SMS/GG — %s", g_watch_movie);
    const int wdims[3] = { (int)buf_w, (int)buf_w, (int)buf_w };
    const int hdims[3] = { (int)buf_h, (int)buf_h, (int)buf_h };
    g_watch = dp_watch_open(title, hawk ? 3 : 2, wdims, hdims);
    /* SDL converts SIGTERM/SIGINT into SDL_QUIT: a process outside the
     * terminal (cgroup shutdown, scope stop) then silently closes the
     * window. Log the signal so the cause stays visible. */
    signal(SIGTERM, on_term_signal);
    signal(SIGINT, on_term_signal);
    signal(SIGHUP, on_term_signal);
    g_watch_t0 = now_ms();
}

/* DP_GG_TILE_DUMP=prefix (DP_GG_DUMP_FRAME, default 120): save both tile
 * buffers as PPM once, to check what each tile really shows */
static void watch_dump(void) {
    const unsigned at = getenv("DP_GG_DUMP_FRAME")
        ? (unsigned)strtoul(getenv("DP_GG_DUMP_FRAME"), NULL, 0) : 120;
    if (g_frame_no != at) return;   /* absolute frame number */
    const char *td = getenv("DP_GG_TILE_DUMP");
    if (!td) return;
    const unsigned h = g_dut_w == DUT_W_MAX ? GG_ACTIVE : GG_HEIGHT;
    for (int t = 0; t < 2; t++) {
        char path[512];
        snprintf(path, sizeof path, "%s.%s.ppm", td, t ? "ref" : "dut");
        FILE *f = fopen(path, "wb");
        if (f) {
            const uint16_t *fb = t ? g_ref.px : g_dut.px;
            fprintf(f, "P6\n%u %u\n255\n", g_dut_w, h);
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < g_dut_w; x++) {
                    unsigned p = fb[y * DUT_W_MAX + x];
                    fputc(((p >> 11) & 31) * 255 / 31, f);
                    fputc(((p >> 6) & 31) * 255 / 31, f);
                    fputc((p & 31) * 255 / 31, f);
                }
            fclose(f);
        }
    }
    fprintf(stderr, "watch: tiles dumped to %s.{dut,ref}.ppm\n", td);
}

static void watch_frame(void) {
    if (!g_watch) { watch_dump(); return; }   /* headless: tile dumps still work */
    g_watch_n++;
    g_watch_rn++;
    g_watch_total++;
    g_watch_rtotal++;
    g_watch_seen++;
    watch_dump();   /* tile dumps work even without a window */
    char l1[128];
    snprintf(l1, sizeof l1, "deepeel (m0boy) | %s", g_watch_movie);
    dp_watch_tile(g_watch, 0, g_dut.px, DUT_W_MAX);
    dp_watch_tile(g_watch, 1, g_ref.px, DUT_W_MAX);
    unsigned span = now_ms() - g_watch_t0;
    if (span >= 500) {   /* captions' f/s, both tiles measured independently */
        snprintf(g_l2, sizeof g_l2, "film 1/1  frame %u/%u  %u f/s",
                 g_watch_total, g_watch_mframes, g_watch_n * 1000u / span);
        snprintf(g_r2, sizeof g_r2, "frame %u  %u f/s",
                 g_watch_rtotal, g_watch_rn * 1000u / span);
        g_watch_t0 = now_ms(); g_watch_n = 0; g_watch_rn = 0; g_ref_frames_seen = 0;
    }
    dp_watch_caption(g_watch, 0, l1, g_l2);
    dp_watch_caption(g_watch, 1, "Genesis Plus GX", g_r2);
    int stop = 0;
    dp_watch_frame(g_watch, &stop);
    if (stop) exit(0);
}

static uint16_t g_dut_store[GG_ROW_STRIDE];        /* 160-wide runtime row_out (+GG_ROW_PAD) */
static uint16_t g_dut_store_sms[DUT_W_MAX + 2 * GG_ROW_PAD]; /* 256-wide runtime row_out */
static const DpSegaBackend *g_b;              /* deepeel's GG backend (BIOS-less bus profile) */
static z80_decoded_t g_d;                     /* runtime decode cache */
static void dut_row(void *u, unsigned y, uint16_t *px) {
    (void)u;
    screen_row(&g_dut, y, px, 0, g_dut_w);   /* same capture path as the reference */
}
static int dut_line(void *u, unsigned y) { (void)u; (void)y; return 1; }
static void dut_frame(void *u) {
    (void)u; g_dut.hash = g_dut.row_hash;
    g_mach->stop = 1;   /* end the frame's instruction loop */
    /* the frame callback sees the finished frame in g_dut.px */
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
    fprintf(stderr, "reference pixel format %u, framebuffer %ux%u\n", g_ref.fmt, g_ref.w, g_ref.h);

    DpSegaTas tas; char err[512];
    if (!dp_sega_tas_load(argv[2], &tas, err, sizeof err)) { fprintf(stderr, "movie: %s\n", err); return 2; }
    fprintf(stderr, "movie: %u frames\n", tas.frames);
    if (dp_sega_tas_rom_match(&tas, rom, n, err, sizeof err) <= 0)
        fprintf(stderr, "warning: movie/ROM identity not verified: %s\n", err);
    unsigned limit = (unsigned)strtoul(argv[3], NULL, 0);
    if (tas.frames && limit > tas.frames) limit = tas.frames;
    const char *movie_path = argv[2];
    /* DP_SEGA_SYS=sms runs the Master System profile on the native 256-wide
     * instance of the runtime (the console's own screen: no GG window crop);
     * the default profile is the Game Gear and its 160x144 window */
    const int is_sms = getenv("DP_SEGA_SYS") && !strcmp(getenv("DP_SEGA_SYS"), "sms");
    g_dut_w = is_sms ? DUT_W_MAX : GG_WIDTH;
    g_mach = is_sms ? &dp_sms_machine : &machine;
    /* the watch tiles show the platform's own buffer (SMS 256x192, GG 160x144):
     * the window ratio is the buffer ratio, never a stretch */
    if (getenv("DP_GG_WINDOW")) have_window = 1;
    if (is_sms) dp_sms_gg_init_system(rom, (size_t)n, NULL, 1);
    else gg_init_system(rom, (size_t)n, NULL, 0);
    g_mach->row = dut_row; g_mach->draw_line = dut_line; g_mach->frame = dut_frame;
    g_mach->row_out = (is_sms ? g_dut_store_sms : g_dut_store) + GG_ROW_PAD; g_mach->row_tmp = NULL;
    g_b = is_sms ? dp_sega_backend_sms() : dp_sega_backend_gg();
    g_b->init(rom, (size_t)n, NULL, is_sms);   /* BIOS-less profile, bus hooks, mapper detection */
    g_mach->row = dut_row; g_mach->draw_line = dut_line; g_mach->frame = dut_frame;
    g_mach->row_out = (is_sms ? g_dut_store_sms : g_dut_store) + GG_ROW_PAD; g_mach->row_tmp = NULL;
    if (is_sms) dp_sms_gg_reset(); else gg_reset();

    uint8_t *ref_ram = mem_data(2);   /* RETRO_MEMORY_SYSTEM_RAM */
    size_t ref_ram_size = mem_size(2);
    if (!ref_ram || ref_ram_size != sizeof g_mach->ram) {
        fprintf(stderr, "reference SYSTEM_RAM %zu bytes (expected %zu)\n", ref_ram_size, sizeof g_mach->ram);
        return 2;
    }
    unsigned ram_diff_total = 0; long first_diff_frame = -1; unsigned first_diff_off = 0;
    /* DP_GG_SHIFT=-1: compare the reference frame n with the DUT frame n-1
     * (the reference boots through the BIOS, the DUT starts the game one
     * frame earlier; measured by cross-correlating dumped RAMs) */
    const int shift = getenv("DP_GG_SHIFT") ? atoi(getenv("DP_GG_SHIFT")) : 0;
    static uint8_t prev_ram[8192]; int have_prev = 0;
    const uint8_t *dut_cmp = g_mach->ram;
    const int every = getenv("DP_GG_EVERY") ? atoi(getenv("DP_GG_EVERY")) : 0;
    /* DP_GG_TRACE_ADDR=$xxxx (SMS address): log the DUT pc whenever that work-RAM byte changes */
    unsigned trace_addr = getenv("DP_GG_TRACE_ADDR") ? (unsigned)strtoul(getenv("DP_GG_TRACE_ADDR"), NULL, 16) - 0xC000 : (unsigned)-1;
    /* DP_GG_TRACE_FRAMES=first:last (1-based, inclusive): log every work-RAM
     * byte each instruction changes (diff before/after, pc after) — the
     * complete intra-frame write sequence, the bench's counterpart of the
     * dumper's SMSSHAWK_WATCH=ALL */
    long trace_fr_lo = 0, trace_fr_hi = -1;
    /* DP_GG_RTRACE=first:last (1-based): print the Z80 R register at each
     * frame end — R counts executed instructions, so its per-frame delta
     * measures the poll-loop iteration drift against the reference core */
    long rtrace_lo = 0, rtrace_hi = -1;
    {   const char *rt = getenv("DP_GG_RTRACE");
        if (rt) { rtrace_lo = atol(rt); char *x = strchr(rt, ':'); rtrace_hi = x ? atol(x + 1) : rtrace_lo; }
    }
    {   const char *tf = getenv("DP_GG_TRACE_FRAMES");
        if (tf) { trace_fr_lo = atol(tf); char *x = strchr(tf, ':'); trace_fr_hi = x ? atol(x + 1) : trace_fr_lo; }
    }
    static uint8_t insn_ram[8192];
    {   const char *se = getenv("DP_GG_SNAP");
        g_snap = se ? (unsigned)strtoul(se, NULL, 0) : 0;
        g_snap_save = g_snap;
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
    /* DP_GG_DUMP_VBL=prefix: append the DUT work RAM sampled at the VBlank
     * event (before the game's tick runs) — a tick-aligned snapshot for the
     * offline convergence analysis: the game updates its positions in the
     * VBlank handler, so the frame-end snapshots of the two cores straddle
     * that handler and show a constant ±1-tick phase on the position bytes */
    FILE *dump_vbl = NULL;
    static uint8_t dut_vbl[8192]; int have_vbl = 0;
    const char *dumpv = getenv("DP_GG_DUMP_VBL");
    if (dumpv) {
        char path[512];
        snprintf(path, sizeof path, "%s.vbl.bin", dumpv);
        dump_vbl = fopen(path, "wb");
        if (!dump_vbl) { fprintf(stderr, "cannot open DP_GG_DUMP_VBL file\n"); return 2; }
    }
    const unsigned watch = getenv("DP_GG_WATCH") ? (unsigned)strtoul(getenv("DP_GG_WATCH"), NULL, 16) : 0;
    static const char *const pad_names[] = { "U", "D", "L", "R", "B1", "B2", "START" };
    unsigned prev_pad = 0xFFFFu;   /* log every movie input transition */
    /* DP_GG_SEEK=N: run the movie's N first frames without the window (still
     * feeding the inputs) so the watch opens straight into the action */
    const unsigned seek = getenv("DP_GG_SEEK") ? (unsigned)strtoul(getenv("DP_GG_SEEK"), NULL, 0) : 0;
    /* DP_GG_PAD_SHIFT=n: read the movie input n frames earlier (negative: later) —
     * replay-sync experiment (BizHawk lag-frame conventions) */
    const int pad_shift = getenv("DP_GG_PAD_SHIFT") ? atoi(getenv("DP_GG_PAD_SHIFT")) : 0;
    const unsigned pc_sample = getenv("DP_GG_PC_SAMPLE") ? (unsigned)strtoul(getenv("DP_GG_PC_SAMPLE"), NULL, 0) : 0;
    unsigned long long pc_hist[4096];  /* index = pc >> 4 */
    memset(pc_hist, 0, sizeof pc_hist);
    /* DP_GG_PC_RANGE=start:end (hex), DP_GG_PC_RANGE_FRAMES=n: log every pc
     * executed inside [start,end) for the first n frames — who really
     * executes there */
    unsigned pc_lo = 0, pc_hi = 0;
    long pc_range_frames = 0;
    {   const char *pr = getenv("DP_GG_PC_RANGE");
        if (pr) {
            pc_lo = (unsigned)strtoul(pr, (char **)&pr, 16);
            if (*pr == ':') pc_hi = (unsigned)strtoul(pr + 1, NULL, 16);
            const char *pf = getenv("DP_GG_PC_RANGE_FRAMES");
            pc_range_frames = pf ? atol(pf) : 20;
        }
    }
    /* DP_GG_LAG_FRAMES=N: BizHawk lag-frame model — the N first input rows are
     * consumed by the recorder's lag frames, so the game's frame f reads the
     * movie row f+N (and the movie's tail rows are never played).  Same
     * mapping as DP_GG_PAD_SHIFT=-N minus the tail: rows past the movie end
     * (which the shift keeps pulling in) stay silent here. */
    unsigned lag = getenv("DP_GG_LAG_FRAMES") ? (unsigned)strtoul(getenv("DP_GG_LAG_FRAMES"), NULL, 0) : 0;
    /* DP_GG_HAWK_FRAMES=file.raw: SMSHawk's frames pre-dumped by
     * tools/smshawk_dump (raw RGB565, native buffer size) — the third tile */
    uint16_t *hawk_fb = NULL;
    {   const char *hf = getenv("DP_GG_HAWK_FRAMES");
        if (hf) {
            hawk = fopen(hf, "rb");
            if (!hawk) { fprintf(stderr, "cannot open DP_GG_HAWK_FRAMES file\n"); return 2; }
            hawk_fb = malloc((size_t)g_dut_w * (size_t)(g_dut_w == DUT_W_MAX ? GG_ACTIVE : GG_HEIGHT) * sizeof *hawk_fb);
            if (!hawk_fb) { fprintf(stderr, "out of memory (hawk tile)\n"); return 2; }
        }
    }
    for (unsigned frame = 0; frame < limit; frame++) {
        if (have_window && frame == seek) {
            watch_open(tas.frames, movie_path, g_dut_w, is_sms ? GG_ACTIVE : GG_HEIGHT);
            if (hawk) dp_watch_caption(g_watch, 2, "SMSHawk (BizHawk)", "");
        }
        long mi = pad_shift ? (long)frame - pad_shift : (long)frame + (long)lag;
        g_pad = (mi >= 0 && (unsigned)mi < tas.frames ? tas.masks[mi] : 0) & 0x7Fu;   /* bit 128 = SMS Reset: kept off */
        if (g_pad != prev_pad) {
            char pl[64] = "";
            for (int b = 0; b < 7; b++)
                if (g_pad & (1u << b)) { strncat(pl, " ", sizeof pl - strlen(pl) - 1); strncat(pl, pad_names[b], sizeof pl - strlen(pl) - 1); }
            fprintf(stderr, "frame %u pad=%02x%s\n", frame + 1, g_pad, pl);
            prev_pad = g_pad;
        }
        /* the movie's Pause (bit 64) is the SMS pause button: the runtime
         * expects it on bit 128 (GG_PAUSE), not the Game Gear START bit */
        const unsigned dut_pad = is_sms ? ((g_pad & 0x3Fu) | ((g_pad & 0x40u) << 1)) : g_pad;
        if (g_dut_w == DUT_W_MAX) dp_sms_gg_buttons(dut_pad); else gg_buttons(dut_pad);
        g_retro_run();
        /* one real frame: instructions until the frame-end event (same loop
         * as the sega explorer's play_step: service, decode, execute, advance) */
        g_mach->stop = 0;
        screen_frame_start(&g_dut);
        unsigned long long ins = 0;   /* safety bound: a frame is ~7k instructions */
        while (!g_mach->stop && ins < 10000000ull) {
            z80_t *c = g_b->cpu();
            if (c->fault) break;
            unsigned cycles = z80_service(c);
            if (!cycles) {
                if (!z80_decode(c, &g_d)) break;
                uint8_t bytes[8];
                for (unsigned j = 0; j < g_d.length && j < sizeof bytes; j++) bytes[j] = g_b->read((uint16_t)(c->pc + j));
                DpZ80Instruction in;
                if (!dp_z80_decode(bytes, g_d.length, c->pc, &in)) break;
                uint8_t before = trace_addr < sizeof g_mach->ram ? g_mach->ram[trace_addr] : 0;
                const int trace_all = frame + 1 >= trace_fr_lo && frame + 1 <= trace_fr_hi;
                if (trace_all) memcpy(insn_ram, g_mach->ram, sizeof insn_ram);
                cycles = z80_execute_decoded(c, &g_d);
                if (trace_addr < sizeof g_mach->ram && g_mach->ram[trace_addr] != before)
                    fprintf(stderr, "trace: frame %u pc=%04x -> $%04x=%02x (sp=%04x)\n",
                            frame + 1, c->pc, (unsigned)(0xC000 + trace_addr), g_mach->ram[trace_addr], c->sp);
                /* register trace on the object routine's pc window: where does
                 * the A of the $C31F write come from */
                if (trace_all)   /* full-frame register trace: find the dispatcher's bit source */
                    fprintf(stderr, "aregs f%u pc=%04x a=%02x b=%02x c=%02x d=%02x e=%02x h=%02x l=%02x ix=%04x r=%02x op=%02x%02x%02x\n",
                            frame + 1, c->pc, c->a, c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->r,
                            bytes[0], g_d.length > 1 ? bytes[1] : (uint8_t)0, g_d.length > 2 ? bytes[2] : (uint8_t)0);
                if (trace_all)
                    for (unsigned ra = 0; ra < sizeof insn_ram; ra++)
                        if (insn_ram[ra] != g_mach->ram[ra])
                            fprintf(stderr, "wtrace f%u pc=%04x $%04x %02x->%02x\n",
                                    frame + 1, c->pc, (unsigned)(0xC000 + ra), insn_ram[ra], g_mach->ram[ra]);
                z80_finish(c, cycles);
                pc_hist[c->pc >> 4]++;
                if (pc_range_frames > 0 && c->pc >= pc_lo && c->pc < pc_hi)
                    fprintf(stderr, "pcr f%u pc=%04x sp=%04x\n", frame + 1, c->pc, c->sp);
            }
            g_b->advance(cycles);
            if (dump_vbl && !have_vbl && g_mach->vblank_done) {
                memcpy(dut_vbl, g_mach->ram, sizeof dut_vbl);
                have_vbl = 1;
            }
            ins++;
        }
        if (rtrace_lo <= frame + 1 && frame + 1 <= rtrace_hi)
            fprintf(stderr, "regr f%u r=%02x pc=%04x ins=%llu\n", frame + 1, g_mach->cpu.r, g_mach->cpu.pc, ins);
        if (ins >= 10000000ull && !g_mach->stop)
            fprintf(stderr, "warning: frame %u ended on the instruction bound, not the frame event\n", frame + 1);
        /* DP_GG_PC_SAMPLE=N: every N frames, histogram the PC's page (top
         * byte) over the frame's executed instructions — where does the CPU
         * actually spend the frame */
        if (pc_sample && (frame + 1) % pc_sample == 0) {
            fprintf(stderr, "pcs f%u:", frame + 1);
            for (int pgi = 0; pgi < 4096; pgi++)
                if (pc_hist[pgi]) fprintf(stderr, " %03x=%llu", pgi, pc_hist[pgi]);
            fprintf(stderr, "\n");
            memset(pc_hist, 0, sizeof pc_hist);
        }
        if (hawk) {
            static unsigned hawk_frames, hawk_eof;
            if (fread(hawk_fb, sizeof *hawk_fb, (size_t)g_dut_w * (size_t)(g_dut_w == DUT_W_MAX ? GG_ACTIVE : GG_HEIGHT), hawk) == (size_t)g_dut_w * (size_t)(g_dut_w == DUT_W_MAX ? GG_ACTIVE : GG_HEIGHT)) {
                hawk_frames++;
                if (g_watch) {
                    dp_watch_tile(g_watch, 2, hawk_fb, g_dut_w);
                    char h1[64], h2[96];
                    snprintf(h1, sizeof h1, "SMSHawk (BizHawk)");
                    if (hawk_eof) snprintf(h2, sizeof h2, "fin du dump (frame %u)", hawk_frames);
                    else snprintf(h2, sizeof h2, "frame %u/%u", hawk_frames, g_watch_mframes);
                    dp_watch_caption(g_watch, 2, h1, h2);
                }
            } else hawk_eof = 1;
        }
        if (pc_range_frames > 0) pc_range_frames--;
        /* DP_GG_VDPLOG: per-frame VDP interrupt state (reg0 line IE, reg1
         * frame IE, status flags, line_irq latch) */
        if (getenv("DP_GG_VDPLOG"))
            fprintf(stderr, "vdp f%u r0=%02x r1=%02x st=%02x line_irq=%d irq=%d iff1=%d\n",
                    frame + 1, g_mach->vdp[0], g_mach->vdp[1], g_mach->vdp_status,
                    g_mach->line_irq, g_mach->cpu.irq, g_mach->cpu.iff1);
        unsigned diff = 0, first = 0;
        dut_cmp = (shift == -1 && have_prev) ? prev_ram : g_mach->ram;
        for (size_t i = 0; i < ref_ram_size; i++)
            if (ref_ram[i] != dut_cmp[i]) { if (!diff) first = (unsigned)i; diff++; }
        if (shift == -1) { memcpy(prev_ram, g_mach->ram, sizeof prev_ram); have_prev = 1; }
        ram_diff_total += diff;
        if (diff && first_diff_frame < 0) { first_diff_frame = (long)frame + 1; first_diff_off = first; }
        if (every || frame < 10 || (frame + 1) % 300 == 0 || (diff && first_diff_frame == (long)frame + 1))
            printf("%u ramdiff=%u first=$%04x screen=%s ref=%ux%u\n", frame + 1, diff, first,
                   (g_ref.hash == g_dut.hash) ? "same" : "diff", g_ref.w, g_ref.h);
        unsigned dut_lit = 0, ref_lit = 0;
        for (unsigned i = 0; i < DUT_W_MAX * DUT_LINES; i++) { if (g_dut.px[i]) dut_lit++; if (i < 160 * 144 && g_ref.px[i]) ref_lit++; }
        static unsigned plit_r = 1, plit_d = 1;
        if (every && (ref_lit != plit_r || dut_lit != plit_d))
            fprintf(stderr, "lit %u ref=%u dut=%u pc=%04x sp=%04x\n", frame + 1, ref_lit, dut_lit, g_mach->cpu.pc, g_mach->cpu.sp);
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
                if (ref_ram[i] != g_mach->ram[i]) {
                    fprintf(stderr, "  %u $%04zx ref=%02x dut=%02x\n", frame + 1, i, ref_ram[i], g_mach->ram[i]);
                    kmax--;
                }
        }
        fflush(stdout);
        if (dump_ref) { fwrite(ref_ram, 1, ref_ram_size, dump_ref); fwrite(g_mach->ram, 1, sizeof g_mach->ram, dump_dut); }
        if (dump_vbl) { fwrite(dut_vbl, 1, sizeof dut_vbl, dump_vbl); have_vbl = 0; }
        if (g_snap != g_snap_save) {   /* the loaded core scribbled the host .bss: restore and keep going */
            fprintf(stderr, "warning: frame %u: host global clobbered (g_snap %u -> %u)\n",
                    frame + 1, g_snap_save, g_snap);
            g_snap = g_snap_save;
        }
        if (g_snap && frame + 1 == g_snap) {
            char path[256];
            snprintf(path, sizeof path, "/tmp/gg_ref.ppm");
            FILE *o = fopen(path, "wb");
            if (o) { fprintf(o, "P6\n%u %u\n255\n", g_ref.w, g_ref.h); for (unsigned y = 0; y < g_ref.h; y++) for (unsigned x = 0; x < g_ref.w; x++) { unsigned p = g_ref.px[y * DUT_W_MAX + x]; fputc(((p >> 10) & 31) * 255 / 31, o); fputc(((p >> 5) & 31) * 255 / 31, o); fputc((p & 31) * 255 / 31, o); } fclose(o); }
            snprintf(path, sizeof path, "/tmp/gg_dut.ppm");
            o = fopen(path, "wb");
            if (o) { fprintf(o, "P6\n%u %d\n255\n", g_dut_w, DUT_LINES); for (unsigned y = 0; y < DUT_LINES; y++) for (unsigned x = 0; x < g_dut_w; x++) { unsigned p = g_dut.px[y * DUT_W_MAX + x]; fputc(((p >> 11) & 31) * 255 / 31, o); fputc(((p >> 6) & 31) * 255 / 31, o); fputc((p & 31) * 255 / 31, o); } fclose(o); }
            fprintf(stderr, "screens dumped to /tmp/gg_ref.ppm /tmp/gg_dut.ppm\n");
        }
        if (getenv("DP_GG_HASHLOG"))
            fprintf(stderr, "f %u dut=%08x ref=%08x pad=%02x\n", frame + 1,
                    (unsigned)g_dut.hash, (unsigned)g_ref.hash, g_pad);
        g_frame_no++;
        watch_frame();
    }
    printf("frames %u, mean ramdiff %.2f, first divergence %s (%ld, offset $%04x)\n", limit,
           limit ? (double)ram_diff_total / limit : 0.0,
           first_diff_frame < 0 ? "none" : "at", first_diff_frame, first_diff_off);
    if (dump_ref) { fclose(dump_ref); fclose(dump_dut); }
    if (dump_vbl) fclose(dump_vbl);
    free(rom); free(tas.masks);
    dlclose(lib);
    return 0;
}
