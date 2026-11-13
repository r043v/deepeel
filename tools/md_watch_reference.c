/* Side-by-side viewer: ClownMDEmu (left) and deepeel's m0boy (right) replaying the same raw GMV.
 * usage: md_watch_reference ROM raw.gmv [frames] [scale]; keys: space pause, right = step, escape quit. */
#define _POSIX_C_SOURCE 200809L
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "clownmdemu.h"
#include "md_full.h"
void dp_mdrom_set(const uint8_t *, size_t);
static unsigned pad;
static uint32_t pal[0x100 * 4], refpx[320 * 224], dutpx[320 * 224];
static cc_bool input(void *u, cc_u8f p, ClownMDEmu_Button b) {
    (void)u; unsigned n = b == CLOWNMDEMU_BUTTON_START ? 7 : (unsigned)b;
    return p == 0 && (b <= CLOWNMDEMU_BUTTON_C || b == CLOWNMDEMU_BUTTON_START) && ((pad >> n) & 1);
}
static void colour(void *u, cc_u16f i, cc_u16f c) {
    (void)u; if (i >= sizeof pal / sizeof *pal) return;
    unsigned r = (c & 0xf) * 17, g = ((c >> 4) & 0xf) * 17, b = ((c >> 8) & 0xf) * 17;
    pal[i] = 0xff000000u | r << 16 | g << 8 | b;
}
static void line(void *u, cc_u16f y, const cc_u8l *p, cc_u16f l, cc_u16f r, cc_u16f w, cc_u16f h) {
    (void)u; (void)h; (void)w; if (y >= 224) return;
    for (cc_u16f x = l; x < r && x < 320; x++) refpx[y * 320 + x] = pal[p[x]];
}
static void audio(void *u, ClownMDEmu *m, size_t n, void (*gen)(ClownMDEmu *, cc_s16l *, size_t)) {
    cc_s16l buf[2048]; (void)u; while (n) { size_t k = n > 1024 ? 1024 : n; gen(m, buf, k); n -= k; }
}
int main(int argc, char **argv) {
    setenv("DP_MD_CLOWN_CLOCK", "exact", 0); setenv("DP_MD_CLOCK_OFF", "3", 0);
    if (argc < 3) { fprintf(stderr, "usage: md_watch_reference ROM raw.gmv [frames] [scale]\n"); return 2; }
    unsigned limit = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 1u << 30, scale = argc > 4 ? (unsigned)atoi(argv[4]) : 2;
    FILE *f = fopen(argv[1], "rb"), *movie = fopen(argv[2], "rb");
    if (!f || !movie) { fprintf(stderr, "cannot open ROM or movie\n"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *rom = malloc(n); cc_u16l *words = malloc((n / 2) * sizeof *words);
    if (!rom || !words || fread(rom, 1, n, f) != (size_t)n) return 2;
    for (long i = 0; i < n / 2; i++) words[i] = (rom[2*i] << 8) | rom[2*i+1];
    fclose(f);
    unsigned char header[64];
    if (fread(header, 1, sizeof header, movie) != sizeof header || memcmp(header, "Gens Movie ", 11)) { fprintf(stderr, "unsupported GMV\n"); return 2; }
    ClownMDEmu *ref = calloc(1, sizeof *ref); MdFull *dut = calloc(1, sizeof *dut);
    ClownMDEmu_InitialConfiguration config = {0}; ClownMDEmu_Callbacks cb = {0};
    config.general.region = CLOWNMDEMU_REGION_OVERSEAS; config.general.tv_standard = CLOWNMDEMU_TV_STANDARD_NTSC;
    cb.input_requested = input; cb.colour_updated = colour; cb.scanline_rendered = line;
    cb.fm_audio_to_be_generated = audio; cb.psg_audio_to_be_generated = audio; cb.pcm_audio_to_be_generated = audio; cb.cdda_audio_to_be_generated = audio;
    ClownMDEmu_Constant_Initialise(); ClownMDEmu_Initialise(ref, &config, &cb);
    ClownMDEmu_SetCartridge(ref, words, n/2); ClownMDEmu_HardReset(ref, 1, 0);
    dp_mdrom_set(rom, n); md_full_reset(dut);
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 2; }
    SDL_Window *w = SDL_CreateWindow("ClownMDEmu (gauche) | m0boy (droite)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * scale + 8, 224 * scale, 0);
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tr = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 224);
    SDL_Texture *td = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 224);
    int paused = 0, step = 0, quit = 0;
    for (unsigned frame = 0; frame < limit && !quit;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) quit = 1;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SPACE) paused = !paused;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RIGHT) step = 1;
        }
        if (paused && !step) { SDL_Delay(10); continue; }
        step = 0;
        unsigned char b[3]; if (fread(b, 1, 3, movie) != 3) break;
        pad = (uint8_t)~b[0]; dut->pad = pad;
        ClownMDEmu_Iterate(ref); md_full_frame(dut, 0); frame++;
        md_full_render(dut, dutpx, 320);
        SDL_UpdateTexture(tr, NULL, refpx, 320 * 4); SDL_UpdateTexture(td, NULL, dutpx, 320 * 4);
        SDL_Rect a = {0, 0, 320 * scale, 224 * scale}, c = {320 * scale + 8, 0, 320 * scale, 224 * scale};
        SDL_SetRenderDrawColor(r, 40, 40, 40, 255); SDL_RenderClear(r);
        SDL_RenderCopy(r, tr, NULL, &a); SDL_RenderCopy(r, td, NULL, &c); SDL_RenderPresent(r);
    }
    SDL_Quit(); return 0;
}
