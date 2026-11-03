// libretro.c: level-A libretro backend, see libretro.h.
#include "libretro.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// the subset of libretro.h this backend uses (public, stable ABI)
#define RETRO_ENV_GET_CAN_DUPE 3
#define RETRO_ENV_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENV_SET_PIXEL_FORMAT 10
#define RETRO_ENV_GET_SAVE_DIRECTORY 31
#define RETRO_ENV_SET_MEMORY_MAPS (36 | 0x10000)
#define RETRO_DEVICE_JOYPAD 1

struct retro_system_info {
    const char *library_name, *library_version, *valid_extensions;
    bool need_fullpath, block_extract;
};
struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};
struct retro_geometry {
    unsigned base_width, base_height, max_width, max_height;
    float aspect_ratio;
};
struct retro_system_av_info {
    struct retro_geometry geometry;
    struct { double fps, sample_rate; } timing;
};

typedef bool (*env_cb)(unsigned, void *);
typedef void (*video_cb)(const void *, unsigned, unsigned, size_t);
typedef void (*audio_cb)(int16_t, int16_t);
typedef size_t (*audio_batch_cb)(const int16_t *, size_t);
typedef void (*poll_cb)(void);
typedef int16_t (*input_cb)(unsigned, unsigned, unsigned, unsigned);

struct DpLibretro {
    void *h;
    char name[64];
    unsigned long long frames;
    uint16_t pad[2];
    int loaded, fmt;
    const uint8_t *fb;
    int fw, fh, fpitch;
    int memmaps; // the core declared RETRO_ENVIRONMENT_SET_MEMORY_MAPS
    char degr[512];
    void (*r_init)(void), (*r_deinit)(void), (*r_run)(void), (*r_reset)(void), (*r_unload)(void);
    void (*r_set_env)(env_cb), (*r_set_video)(video_cb), (*r_set_audio)(audio_cb),
        (*r_set_audio_batch)(audio_batch_cb), (*r_set_poll)(poll_cb), (*r_set_input)(input_cb);
    void (*r_sysinfo)(struct retro_system_info *);
    bool (*r_load)(const struct retro_game_info *);
    size_t (*r_ssize)(void);
    bool (*r_ser)(void *, size_t), (*r_unser)(const void *, size_t);
    void *(*r_mem)(unsigned);
    size_t (*r_msize)(unsigned);
    uint8_t *rom;
    DpLrTraceApi trace; // filled by the core at level B
    int level;
};

// libretro callbacks carry no user pointer: one active instance per thread of
// control (the multi-instance scheme of §8.3 copies the .so instead)
static __thread DpLibretro *g_cur;

static bool env_cb_fn(unsigned cmd, void *data) {
    DpLibretro *lr = g_cur;
    if (!lr) return false;
    switch (cmd) {
    case RETRO_ENV_SET_PIXEL_FORMAT:
        if (data) lr->fmt = *(int *)data;
        return true;
    case RETRO_ENV_GET_CAN_DUPE:
        if (data) *(bool *)data = true;
        return true;
    case RETRO_ENV_GET_SYSTEM_DIRECTORY:
    case RETRO_ENV_GET_SAVE_DIRECTORY:
        if (data) *(const char **)data = ".";
        return true;
    case DP_LR_ENV_TRACE:
        if (!data) return false;
        // the core fills the table; only a complete one of our version is accepted
        {
            DpLrTraceApi *t = data;
            if (t->version != DP_LR_TRACE_VERSION || !t->step || !t->set_observers || !t->pc) return false;
            lr->trace = *t;
            lr->level = 1;
            return true;
        }
    case RETRO_ENV_SET_MEMORY_MAPS:
        lr->memmaps = 1;
        return true;
    default:
        return false; // anything else is declined, never silently accepted
    }
}
static void video_fn(const void *d, unsigned w, unsigned h, size_t pitch) {
    DpLibretro *lr = g_cur;
    if (!lr || !d) return; // a NULL frame is a duplicate: keep the last one
    lr->fb = d;
    lr->fw = (int)w;
    lr->fh = (int)h;
    lr->fpitch = (int)pitch;
}
static void audio_fn(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_fn(const int16_t *d, size_t n) { (void)d; return n; }
static void poll_fn(void) {}
static int16_t input_fn(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    DpLibretro *lr = g_cur;
    if (!lr || port > 1 || dev != RETRO_DEVICE_JOYPAD || idx || id > 15) return 0;
    return (lr->pad[port] >> id) & 1;
}

static void seterr(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

#define SYM(field, name)                                                                         \
    do {                                                                                         \
        *(void **)&lr->field = dlsym(lr->h, name);                                               \
        if (!lr->field) {                                                                        \
            seterr(err, errn, "libretro: missing symbol %s", name);                              \
            dlclose(lr->h);                                                                      \
            free(lr);                                                                            \
            return NULL;                                                                         \
        }                                                                                        \
    } while (0)

DpLibretro *dp_lr_open_copy(const char *core_path, char *err, size_t errn) {
    FILE *in = fopen(core_path, "rb");
    if (!in) { seterr(err, errn, "libretro: cannot read %s", core_path); return NULL; }
    char tmp[] = "/tmp/dpcore_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { fclose(in); seterr(err, errn, "libretro: %s", "cannot create a temporary copy"); return NULL; }
    FILE *out = fdopen(fd, "wb");
    char buf[65536];
    size_t n;
    int ok = out != NULL;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) ok = 0;
    fclose(in);
    if (out) fclose(out);
    DpLibretro *lr = ok ? dp_lr_open(tmp, err, errn) : NULL;
    if (!ok) seterr(err, errn, "libretro: %s", "copy failed");
    unlink(tmp); // the mapping stays valid after dlopen
    return lr;
}

DpLibretro *dp_lr_open(const char *core_path, char *err, size_t errn) {
    DpLibretro *lr = calloc(1, sizeof *lr);
    if (!lr) return NULL;
    lr->h = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (!lr->h) {
        seterr(err, errn, "libretro: %s", dlerror());
        free(lr);
        return NULL;
    }
    SYM(r_init, "retro_init");
    SYM(r_deinit, "retro_deinit");
    SYM(r_run, "retro_run");
    SYM(r_reset, "retro_reset");
    SYM(r_unload, "retro_unload_game");
    SYM(r_set_env, "retro_set_environment");
    SYM(r_set_video, "retro_set_video_refresh");
    SYM(r_set_audio, "retro_set_audio_sample");
    SYM(r_set_audio_batch, "retro_set_audio_sample_batch");
    SYM(r_set_poll, "retro_set_input_poll");
    SYM(r_set_input, "retro_set_input_state");
    SYM(r_sysinfo, "retro_get_system_info");
    SYM(r_load, "retro_load_game");
    SYM(r_ssize, "retro_serialize_size");
    SYM(r_ser, "retro_serialize");
    SYM(r_unser, "retro_unserialize");
    SYM(r_mem, "retro_get_memory_data");
    SYM(r_msize, "retro_get_memory_size");
    g_cur = lr;
    lr->r_set_env(env_cb_fn);
    lr->r_init();
    lr->r_set_video(video_fn);
    lr->r_set_audio(audio_fn);
    lr->r_set_audio_batch(audio_batch_fn);
    lr->r_set_poll(poll_fn);
    lr->r_set_input(input_fn);
    struct retro_system_info si;
    memset(&si, 0, sizeof si);
    lr->r_sysinfo(&si);
    snprintf(lr->name, sizeof lr->name, "%s", si.library_name ? si.library_name : "core");
    return lr;
}

int dp_lr_load_rom(DpLibretro *lr, const char *rom_path, char *err, size_t errn) {
    FILE *f = fopen(rom_path, "rb");
    if (!f) { seterr(err, errn, "libretro: cannot read %s", rom_path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(lr->rom);
    lr->rom = malloc(n > 0 ? (size_t)n : 1);
    if (!lr->rom || fread(lr->rom, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        seterr(err, errn, "libretro: short read of %s", rom_path);
        return 0;
    }
    fclose(f);
    g_cur = lr;
    struct retro_game_info gi = { rom_path, lr->rom, (size_t)n, NULL };
    if (!lr->r_load(&gi)) { seterr(err, errn, "libretro: the core refused %s", rom_path); return 0; }
    lr->loaded = 1;
    if (lr->level)
        snprintf(lr->degr, sizeof lr->degr,
                 "level B (%s): trace extension negotiated (step, observers, forced branch %s, rom index %s)",
                 lr->name, lr->trace.force_branch ? "available" : "unavailable",
                 lr->trace.rom_index ? "available" : "unavailable");
    else
    snprintf(lr->degr, sizeof lr->degr,
             "level A (%s): no instruction step, no forced branch (D7-D11), no bus observers "
             "(exec/read/write/dma/bank), no precise ROM coverage; ROM coverage by memory maps %s",
             lr->name, lr->memmaps ? "declared by the core (not decoded yet)" : "unavailable");
    return 1;
}

void dp_lr_close(DpLibretro *lr) {
    if (!lr) return;
    g_cur = lr;
    if (lr->loaded) lr->r_unload();
    lr->r_deinit();
    dlclose(lr->h);
    free(lr->rom);
    free(lr);
}

void dp_lr_advance(DpLibretro *lr, unsigned frames) {
    g_cur = lr;
    for (unsigned i = 0; i < frames; i++) {
        lr->r_run();
        lr->frames++;
    }
}
void dp_lr_reset(DpLibretro *lr) { g_cur = lr; lr->r_reset(); }
void dp_lr_set_input(DpLibretro *lr, unsigned port, uint16_t mask) {
    if (port < 2) lr->pad[port] = mask;
}

size_t dp_lr_state_size(DpLibretro *lr) { g_cur = lr; return lr->r_ssize(); }
int dp_lr_save(DpLibretro *lr, void *buf, size_t n) { g_cur = lr; return lr->r_ser(buf, n); }
int dp_lr_load(DpLibretro *lr, const void *buf, size_t n) { g_cur = lr; return lr->r_unser(buf, n); }

static uint64_t fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h;
}
uint64_t dp_lr_state_hash(DpLibretro *lr) {
    size_t n = dp_lr_state_size(lr);
    uint8_t *b = malloc(n ? n : 1);
    uint64_t h = 0;
    if (b && dp_lr_save(lr, b, n)) h = fnv64(b, n);
    free(b);
    return h;
}
uint64_t dp_lr_ram_hash(DpLibretro *lr) {
    size_t n = 0;
    const uint8_t *p = dp_lr_memory(lr, DP_LR_MEM_SYSTEM_RAM, &n);
    return p ? fnv64(p, n) : 0;
}
const uint8_t *dp_lr_memory(DpLibretro *lr, int id, size_t *size) {
    g_cur = lr;
    const uint8_t *p = lr->r_mem((unsigned)id);
    if (size) *size = p ? lr->r_msize((unsigned)id) : 0;
    return p;
}
const uint8_t *dp_lr_framebuffer(DpLibretro *lr, int *w, int *h, int *pitch, int *fmt) {
    if (w) *w = lr->fw;
    if (h) *h = lr->fh;
    if (pitch) *pitch = lr->fpitch;
    if (fmt) *fmt = lr->fmt;
    return lr->fb;
}
unsigned long long dp_lr_frames(const DpLibretro *lr) { return lr->frames; }
const char *dp_lr_core_name(const DpLibretro *lr) { return lr->name; }
const char *dp_lr_degradations(const DpLibretro *lr) { return lr->degr; }

unsigned dp_lr_replay(DpLibretro *lr, const uint16_t *masks, unsigned n, uint64_t *hashes) {
    for (unsigned i = 0; i < n; i++) {
        dp_lr_set_input(lr, 0, masks[i]);
        dp_lr_advance(lr, 1);
        if (hashes) hashes[i] = dp_lr_ram_hash(lr);
    }
    dp_lr_set_input(lr, 0, 0);
    return n;
}

int dp_lr_level(const DpLibretro *lr) { return lr->level; }
int dp_lr_step(DpLibretro *lr) {
    if (!lr->level) return 0;
    g_cur = lr;
    return lr->trace.step();
}
uint32_t dp_lr_pc(DpLibretro *lr) { return lr->level ? lr->trace.pc() : 0; }
int dp_lr_force_branch(DpLibretro *lr, int taken) {
    return lr->level && lr->trace.force_branch ? lr->trace.force_branch(taken) : 0;
}
int64_t dp_lr_rom_index(DpLibretro *lr, uint32_t addr) {
    return lr->level && lr->trace.rom_index ? lr->trace.rom_index(addr) : -1;
}
void dp_lr_observe(DpLibretro *lr, const DpLrObservers *obs) {
    if (lr->level) lr->trace.set_observers(obs);
}

static uint64_t lr_fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h;
}
typedef struct { uint8_t *snap; int force; } LrItem;
typedef struct {
    DpLibretro *lr; uint8_t *map; size_t n; DpLrExploreStats *st;
    int branch_hit; uint32_t branch_pc; uint8_t *sites; size_t site_n;
} LrExpCtx;
static void lrx_exec(void *u, uint32_t pc) {
    LrExpCtx *c = u;
    int64_t i = c->lr->trace.rom_index ? c->lr->trace.rom_index(pc) : (int64_t)pc;
    if (i >= 0 && (size_t)i < c->n && !c->map[i]) { c->map[i] = 1; c->st->covered++; }
}
static void lrx_branch(void *u, uint32_t pc) { LrExpCtx *c = u; c->branch_hit = 1; c->branch_pc = pc; }

int dp_lr_explore(DpLibretro *lr, uint8_t *exec_map, size_t rom_size,
                  const DpLrExploreLimits *lim, DpLrExploreStats *st) {
    memset(st, 0, sizeof *st);
    if (!lr->level || !lr->trace.force_branch) return 0;
    size_t sz = dp_lr_state_size(lr);
    uint32_t cap = lim->max_items ? lim->max_items : 1024;
    LrItem *q = calloc(cap, sizeof *q);
    uint64_t *seen = calloc(65536, sizeof *seen); // (state hash, force) dedup of fork items
    uint8_t *cur = malloc(sz), *pre = malloc(sz);
    LrExpCtx c = { lr, exec_map, rom_size, st, 0, 0, calloc(65536, 1), 65536 };
    DpLrObservers o = { &c, lrx_exec, NULL, NULL, NULL, NULL, lrx_branch };
    lr->trace.set_observers(&o);
    uint32_t nq = 0;
    dp_lr_save(lr, cur, sz);
    q[nq].snap = malloc(sz); memcpy(q[nq].snap, cur, sz); q[nq].force = -1; nq++;
    while (nq) {
        LrItem it = q[--nq];
        dp_lr_load(lr, it.snap, sz);
        free(it.snap);
        st->paths++;
        if (it.force >= 0) lr->trace.force_branch(it.force);
        uint64_t ps = 0;
        for (;;) {
            if (st->steps >= lim->total_steps) { st->cut_total++; break; }
            if (ps >= lim->path_steps) { st->cut_path_steps++; break; }
            dp_lr_save(lr, pre, sz);
            c.branch_hit = 0;
            g_cur = lr;
            int ok = lr->trace.step();
            st->steps++; ps++;
            if (!ok) break;
            if (!c.branch_hit || (it.force >= 0 && ps == 1)) continue; // a forced item replays its own branch
            // conditional: abandon this run and queue both sides from the pre-step state
            if (!c.sites[c.branch_pc & 0xFFFF]) { c.sites[c.branch_pc & 0xFFFF] = 1; st->branch_sites++; }
            uint64_t h = lr_fnv(pre, sz);
            for (int f = 0; f < 2; f++) {
                uint64_t key = (h ^ (uint64_t)(f + 1) * 0x9E3779B97F4A7C15ull) | 1;
                uint64_t slot = key & 0xFFFF;
                if (seen[slot] == key) { st->deduped++; continue; }
                if (nq >= cap) { st->dropped_items++; continue; }
                seen[slot] = key;
                q[nq].snap = malloc(sz); memcpy(q[nq].snap, pre, sz); q[nq].force = f; nq++;
                st->forks++;
            }
            break;
        }
    }
    lr->trace.set_observers(NULL);
    free(q); free(seen); free(cur); free(pre); free(c.sites);
    return 1;
}
