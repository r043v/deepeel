// gbshot: run a GB/GBC ROM headless on the m0boy core for N frames, with
// optional button presses, and write the last frame as a PPM (RGB565 pixels
// of the CGB renderer, or four grey shades for DMG).  Observation tool.
// usage: gbshot ROM FRAMES OUT.ppm [frame:buttons[:frames] ...]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/cart.h"
#include "../src/mach/m0boy.h"

static DpMach *M;
static int frames, limit;
static struct { int at, len; uint8_t b; } ev[64];
static int nev;

static char ring[128][96]; static int rp, dumped;
static void step(DpMach *m, uint16_t pc, uint8_t op, int e) {
    (void)e;
    if (getenv("GBSHOT_CRASH") && !dumped) {
        snprintf(ring[rp++ & 127], 96, "f=%d %04x %02x a=%02x bc=%04x de=%04x hl=%04x sp=%04x b=%u", frames, pc, op, m->api->gbr->a, m->api->gbr->bc, m->api->gbr->de, m->api->gbr->hl, m->api->gbr->sp, m->api->gbr->rom_bank);
        if (pc >= 0x1300 && pc < 0x1400 && frames > 100) { dumped = 1; for (int i = 0; i < 128; i++) fprintf(stderr, "%s\n", ring[(rp + i) & 127]); }
    }
    if (getenv("GBSHOT_PC") && frames >= atoi(getenv("GBSHOT_PC")) && frames < atoi(getenv("GBSHOT_PC")) + 25) fprintf(stderr, "%04x %02x a=%02x bc=%04x de=%04x hl=%04x sp=%04x\n", pc, op, m->api->gbr->a, m->api->gbr->bc, m->api->gbr->de, m->api->gbr->hl, m->api->gbr->sp);
}
static void wr(DpMach *m, uint16_t a, uint8_t v) {
    if (getenv("GBSHOT_WATCH") && a == (uint16_t)strtol(getenv("GBSHOT_WATCH"), 0, 16)) fprintf(stderr, "f=%d W %04x=%02x pc=%04x sp=%04x\n", frames, a, v, m->api->gbr->pc, m->api->gbr->sp);
    if (getenv("GBSHOT_IO") && a >= 0xFF4D && a <= 0xFF7F && a != 0xFF69 && a != 0xFF6B) fprintf(stderr, "f=%d W %04x=%02x pc=%04x\n", frames, a, v, m->api->gbr->pc);
}
static void vb(DpMach *m) {
    uint8_t b = 0;
    for (int i = 0; i < nev; i++) if (frames >= ev[i].at && frames < ev[i].at + ev[i].len) b |= ev[i].b;
    m->api->set_buttons(b);
    if (getenv("GBSHOT_LOG") && frames % atoi(getenv("GBSHOT_LOG")) == 0) fprintf(stderr, "f=%d pc=%04x lcdc=%02x ie=%02x bank=%u svbk=%02x\n", frames, m->api->gbr->pc, m->api->io[0x40], *m->api->ie, m->api->gbr->rom_bank, m->api->io[0x70]);
    if (++frames >= limit) dp_mach_cut(M, DP_CUT_QUIT);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: gbshot ROM FRAMES OUT.ppm [frame:buttons[:len]]...\n"); return 2; }
    limit = atoi(argv[2]);
    for (int i = 4; i < argc && nev < 64; i++) {
        int at = 0, len = 4; unsigned b = 0;
        sscanf(argv[i], "%d:%x:%d", &at, &b, &len);
        ev[nev].at = at; ev[nev].b = (uint8_t)b; ev[nev].len = len; nev++;
    }
    char err[256];
    DpCart *c = dp_cart_load(argv[1], err, sizeof err);
    if (!c) { fprintf(stderr, "%s\n", err); return 1; }
    int vi = dp_mach_pick_variant(c->rom);
    if (vi < 0) { fprintf(stderr, "no variant\n"); return 1; }
    M = dp_mach_open(vi, 0);
    M->render = 1;
    dp_mach_set_rom(M, c->rom, c->rom_size);
    dp_mach_reset(M);
    dp_mach_attach(M, NULL, step, NULL, wr, vb);
    dp_mach_run(M);
    FILE *f = fopen(argv[3], "wb");
    fprintf(f, "P6\n160 144\n255\n");
    int colour = (c->rom[0x143] & 0x80) != 0;
    static const uint8_t shade[4] = {0xE0, 0xA8, 0x60, 0x18};
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) {
            uint8_t px[3];
            uint16_t v = M->screen16[y][x];
            if (colour) { px[0] = (uint8_t)((v >> 11) * 255 / 31); px[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); px[2] = (uint8_t)((v & 31) * 255 / 31); }
            else px[0] = px[1] = px[2] = shade[v & 3];
            fwrite(px, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "lcdc=%02x stat=%02x ie=%02x if=%02x ly=%02x scx=%02x scy=%02x bgp=%02x ime=%d hl=%04x a=%02x\n", M->api->io[0x40], M->api->io[0x41], *M->api->ie, M->api->io[0x0F], M->api->io[0x44], M->api->io[0x43], M->api->io[0x42], M->api->io[0x47], M->api->gbr->ime, M->api->gbr->hl, M->api->gbr->a);
    fprintf(stderr, "frames=%d pc=%04x\n", frames, M->api->gbr->pc);
    return 0;
}
