// libretro.h: level-A libretro backend (docs/COUVERTURE.fr.md §8.1). An
// unmodified core is loaded with dlopen; only the public libretro API is used
// (serialize, run, input, memory, video). There is no instruction step, no
// forced branch and no bus observer at this level: dp_lr_degradations() says
// so, and the explorer must report it instead of pretending.
#ifndef DP_LIBRETRO_H
#define DP_LIBRETRO_H

#include <stddef.h>
#include <stdint.h>

typedef struct DpLibretro DpLibretro;

enum { DP_LR_MEM_SAVE_RAM = 0, DP_LR_MEM_RTC = 1, DP_LR_MEM_SYSTEM_RAM = 2, DP_LR_MEM_VIDEO_RAM = 3 };
enum { DP_LR_FMT_0RGB1555 = 0, DP_LR_FMT_XRGB8888 = 1, DP_LR_FMT_RGB565 = 2 };

// opens `core_path` (dlopen, RTLD_LOCAL) and initializes it; NULL + err on failure
DpLibretro *dp_lr_open(const char *core_path, char *err, size_t errn);
// same, but from a private copy of the .so (§8.3): cores are process-wide
// singletons, so each independent instance needs its own copy of the library
DpLibretro *dp_lr_open_copy(const char *core_path, char *err, size_t errn);
// loads a ROM (read from disk by the backend; the path is also given to cores that need it)
int dp_lr_load_rom(DpLibretro *lr, const char *rom_path, char *err, size_t errn);
void dp_lr_close(DpLibretro *lr);

void dp_lr_advance(DpLibretro *lr, unsigned frames); // retro_run x frames
void dp_lr_reset(DpLibretro *lr);
// input: joypad bitmask (bit = RETRO_DEVICE_ID_JOYPAD_*) for a port
void dp_lr_set_input(DpLibretro *lr, unsigned port, uint16_t mask);

// serialized state (the core's own format)
size_t dp_lr_state_size(DpLibretro *lr);
int dp_lr_save(DpLibretro *lr, void *buf, size_t n);
int dp_lr_load(DpLibretro *lr, const void *buf, size_t n);
// FNV-1a 64 of the serialized state — includes whatever the core serializes
// (clocks too): equality means "same state", inequality may be a clock only
uint64_t dp_lr_state_hash(DpLibretro *lr);
// FNV-1a 64 of SYSTEM_RAM only (0 when the core exposes none)
uint64_t dp_lr_ram_hash(DpLibretro *lr);

const uint8_t *dp_lr_memory(DpLibretro *lr, int id, size_t *size);
// last frame: pointer to the core's pixels (valid until the next advance)
const uint8_t *dp_lr_framebuffer(DpLibretro *lr, int *w, int *h, int *pitch, int *fmt);
unsigned long long dp_lr_frames(const DpLibretro *lr);
const char *dp_lr_core_name(const DpLibretro *lr);

// "what this level cannot do": one line per missing capability (never empty
// for level A: step / forced branch / bus observers are always absent)
const char *dp_lr_degradations(const DpLibretro *lr);

// TAS: replays one joypad mask per frame (port 0) from the current state,
// writing the per-frame RAM hash to hashes[] (may be NULL); returns frames run
unsigned dp_lr_replay(DpLibretro *lr, const uint16_t *masks, unsigned n, uint64_t *hashes);

// --- level B (docs/COUVERTURE.fr.md §8.2): trace extension --------------------------
// A core that implements the private environment call DP_LR_ENV_TRACE fills a
// DpLrTraceApi: one-instruction step, observers, pc, forced branch, rom index.
// The extension lives in a small patch kept outside this repository.
#define DP_LR_ENV_TRACE 0x7DEE0001u
#define DP_LR_TRACE_VERSION 1

typedef struct DpLrObservers {
    void *user;
    void (*exec)(void *user, uint32_t pc);
    void (*read)(void *user, uint32_t addr, uint8_t val);
    void (*write)(void *user, uint32_t addr, uint8_t val);
    void (*dma)(void *user, uint32_t src, uint32_t dst, uint32_t len); // a hardware copy (OAM DMA, HDMA, ...)
    void (*bank)(void *user, uint32_t window, uint32_t bank);          // a mapper bank switch
    void (*branch)(void *user, uint32_t pc);                           // a conditional is decided at pc
} DpLrObservers;

typedef struct DpLrTraceApi {
    uint32_t version;
    int (*step)(void);                            // one instruction; 0 = the CPU is halted
    void (*set_observers)(const DpLrObservers *); // NULL clears
    uint32_t (*pc)(void);
    int (*force_branch)(int taken);               // the next conditional goes this way (1/0); -1 = natural; returns 0 if unsupported
    int64_t (*rom_index)(uint32_t addr);          // compact ROM offset, < 0 when not ROM
} DpLrTraceApi;

int dp_lr_level(const DpLibretro *lr);            // 0 = level A, 1 = level B (trace extension negotiated)
int dp_lr_step(DpLibretro *lr);                   // level B only; 0 at level A or when halted
uint32_t dp_lr_pc(DpLibretro *lr);
int dp_lr_force_branch(DpLibretro *lr, int taken);
int64_t dp_lr_rom_index(DpLibretro *lr, uint32_t addr);
void dp_lr_observe(DpLibretro *lr, const DpLrObservers *obs);

#endif

// Forking exploration over a level-B core: both sides of every conditional are
// explored from a saved state. Every bound is explicit and its saturation is
// reported (never silent).
typedef struct DpLrExploreLimits {
    uint64_t path_steps;  // per-path step budget
    uint64_t total_steps; // global step wall
    uint32_t max_items;   // pending fork items (queue cap)
} DpLrExploreLimits;
typedef struct DpLrExploreStats {
    uint64_t steps, forks, paths, deduped;
    uint64_t cut_path_steps, cut_total, dropped_items; // saturation counters
    uint32_t covered;                                   // distinct ROM bytes executed (opcode starts)
    uint32_t branch_sites;                              // distinct conditional pcs
} DpLrExploreStats;
// exec_map: one byte per ROM index (rom_size bytes), set to 1 where code starts.
int dp_lr_explore(DpLibretro *lr, uint8_t *exec_map, size_t rom_size,
                  const DpLrExploreLimits *lim, DpLrExploreStats *st);
