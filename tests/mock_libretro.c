// mock_libretro.c: a tiny libretro core built by the tests (nothing is
// committed as a binary). Its "console": 64 bytes of RAM; each frame adds the
// port-0 joypad mask to ram[0] and counts frames in ram[1]; the screen is an
// 8x8 XRGB8888 square of ram[0].
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
static uint8_t ram[64];
static uint32_t pix[64];

void retro_set_environment(env_cb cb) {
    int fmt = 1; // XRGB8888
    cb(10, &fmt);
}
void retro_set_video_refresh(void (*v)(const void *, unsigned, unsigned, size_t)) { video = v; }
void retro_set_audio_sample(void (*a)(int16_t, int16_t)) { (void)a; }
void retro_set_audio_sample_batch(size_t (*a)(const int16_t *, size_t)) { (void)a; }
void retro_set_input_poll(void (*p)(void)) { poll = p; }
void retro_set_input_state(int16_t (*i)(unsigned, unsigned, unsigned, unsigned)) { input = i; }
void retro_init(void) { memset(ram, 0, sizeof ram); }
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
