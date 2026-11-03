// mock_libretro.c: a tiny libretro core built by the tests (nothing is
// committed as a binary). Its "console": 64 bytes of RAM; each frame adds the
// port-0 joypad mask to ram[0] and counts frames in ram[1]; the screen is an
// 8x8 XRGB8888 square of ram[0].
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*env_cb)(unsigned, void *);
struct retro_system_info {
    const char *library_name, *library_version, *valid_extensions;
    bool need_fullpath, block_extract;
};
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };

static void (*video)(const void *, unsigned, unsigned, size_t);
static void (*poll)(void);
static int16_t (*input)(unsigned, unsigned, unsigned, unsigned);
// --- level B: a toy CPU (opcodes: 01 xx A=xx, 02 aa store A, 03 aa load A,
// 04 tt jump to tt if A==0, 05 halt) running the loaded ROM, one step at a time
#define DP_LR_ENV_TRACE 0x7DEE0001u
typedef struct { void *user; void (*exec)(void *, uint32_t); void (*read)(void *, uint32_t, uint8_t);
                 void (*write)(void *, uint32_t, uint8_t); void (*dma)(void *, uint32_t, uint32_t, uint32_t);
                 void (*bank)(void *, uint32_t, uint32_t); } Obs;
typedef struct { uint32_t version; int (*step)(void); void (*set_observers)(const Obs *);
                 uint32_t (*pc)(void); int (*force_branch)(int); int64_t (*rom_index)(uint32_t); } Trace;
static uint8_t ram[64];
static uint8_t rom[32];
static size_t rom_n;
static Obs obs;
static int force = -1;
#define PC ram[4]
#define ACC ram[5]
static int t_step(void) {
    if ((size_t)PC >= rom_n) return 0;
    uint8_t op = rom[PC], arg = PC + 1 < rom_n ? rom[PC + 1] : 0;
    if (obs.exec) obs.exec(obs.user, PC);
    switch (op) {
    case 1: ACC = arg; PC += 2; return 1;
    case 2: ram[arg & 0x3F] = ACC; if (obs.write) obs.write(obs.user, arg & 0x3F, ACC); PC += 2; return 1;
    case 3: ACC = ram[arg & 0x3F]; if (obs.read) obs.read(obs.user, arg & 0x3F, ACC); PC += 2; return 1;
    case 4: {
        int taken = force >= 0 ? force : (ACC == 0);
        force = -1;
        PC = taken ? arg : (uint8_t)(PC + 2);
        return 1;
    }
    case 6: if (obs.bank) obs.bank(obs.user, 1, arg); PC += 2; return 1; // bank switch of window 1
    case 7: if (obs.dma) obs.dma(obs.user, arg, 0x20, 4); PC += 2; return 1; // DMA of 4 bytes from arg
    default: return 0; // 05 and anything else: halted
    }
}
static void t_obs(const Obs *o) { if (o) obs = *o; else memset(&obs, 0, sizeof obs); }
static uint32_t t_pc(void) { return PC; }
static int t_force(int taken) { force = taken; return 1; }
static int64_t t_rom_index(uint32_t a) { return a < rom_n ? (int64_t)a : -1; }

static uint32_t pix[64];
static env_cb envcb;

void retro_set_environment(env_cb cb) {
    int fmt = 1; // XRGB8888
    cb(10, &fmt);
    envcb = cb;
}
void retro_set_video_refresh(void (*v)(const void *, unsigned, unsigned, size_t)) { video = v; }
void retro_set_audio_sample(void (*a)(int16_t, int16_t)) { (void)a; }
void retro_set_audio_sample_batch(size_t (*a)(const int16_t *, size_t)) { (void)a; }
void retro_set_input_poll(void (*p)(void)) { poll = p; }
void retro_set_input_state(int16_t (*i)(unsigned, unsigned, unsigned, unsigned)) { input = i; }
void retro_init(void) {
    memset(ram, 0, sizeof ram);
    if (!getenv("DPMOCK_NOTRACE") && envcb) {
        Trace t = { 1, t_step, t_obs, t_pc, t_force, t_rom_index };
        envcb(DP_LR_ENV_TRACE, &t);
    }
}
void retro_deinit(void) {}
void retro_get_system_info(struct retro_system_info *si) {
    si->library_name = "dpmock";
    si->library_version = "1";
    si->valid_extensions = "bin";
    si->need_fullpath = false;
    si->block_extract = false;
}
bool retro_load_game(const struct retro_game_info *g) {
    if (!g || !g->data || g->size < 4) return false;
    memcpy(ram + 8, g->data, g->size < 8 ? g->size : 8);
    rom_n = g->size < sizeof rom ? g->size : sizeof rom;
    memcpy(rom, g->data, rom_n);
    return true;
}
void retro_unload_game(void) {}
void retro_reset(void) { memset(ram, 0, 8); }
void retro_run(void) {
    poll();
    unsigned m = 0;
    for (unsigned id = 0; id < 16; id++)
        if (input(0, 1, 0, id)) m |= 1u << id;
    ram[0] = (uint8_t)(ram[0] + m);
    ram[1]++;
    for (int i = 0; i < 64; i++) pix[i] = 0xFF000000u | ram[0];
    video(pix, 8, 8, 8 * 4);
}
size_t retro_serialize_size(void) { return sizeof ram; }
bool retro_serialize(void *d, size_t n) { if (n < sizeof ram) return false; memcpy(d, ram, sizeof ram); return true; }
bool retro_unserialize(const void *d, size_t n) { if (n < sizeof ram) return false; memcpy(ram, d, sizeof ram); return true; }
void *retro_get_memory_data(unsigned id) { return id == 2 ? ram : NULL; }
size_t retro_get_memory_size(unsigned id) { return id == 2 ? sizeof ram : 0; }
