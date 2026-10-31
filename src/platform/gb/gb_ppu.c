// gb_ppu.c: screenshot renderer for the play sessions — turns the machine's
// VRAM, OAM and video registers into a 160x144 DMG grayscale frame so the
// GUI and the PNG snapshots show the game a human can play.  This is a
// snapshot renderer, not a cycle PPU: it draws the current VRAM with the
// current scroll and palettes; mid-frame register changes are not reproduced.
#include "gb.h"

#include <string.h>

#define MS_GB_W 160
#define MS_GB_H 144

#define IO(r) (m->io[(r) - 0xFF00])

// shade of a 2bpp pixel through a palette register: bits select 3,2,1,0
static uint8_t shade(uint8_t pal, uint8_t c) {
    return (uint8_t)((pal >> (c * 2)) & 3);
}

static uint8_t tile_pixel(const MsMachine *m, uint16_t tile_addr, int y, int x, int flip_x, int flip_y) {
    // two bytes per row: low bit plane then high bit plane
    uint16_t row = (uint16_t)(tile_addr + (flip_y ? (7 - y) : y) * 2);
    uint8_t lo = m->vram[0][row - 0x8000];
    uint8_t hi = m->vram[0][row + 1 - 0x8000];
    int bit = flip_x ? x : 7 - x;
    return (uint8_t)(((hi >> bit) & 1) << 1 | ((lo >> bit) & 1));
}

void gb_render_frame(const MsMachine *m, uint8_t *px) {
    uint8_t lcdc = IO(0xFF40);
    if (!(lcdc & 0x80)) {
        memset(px, 0, (size_t)MS_GB_W * MS_GB_H); // LCD off: white
        return;
    }
    uint8_t bgp = IO(0xFF47);
    int bg_map = (lcdc & 0x08) ? 0x9C00 : 0x9800;
    int signed_tiles = !(lcdc & 0x10);
    uint8_t scy = IO(0xFF42), scx = IO(0xFF43);
    int win_on = (lcdc & 0x20) && IO(0xFF4A) < 144;
    int win_map = (lcdc & 0x40) ? 0x9C00 : 0x9800;
    int bg_priority = !(lcdc & 0x01);

    for (int ly = 0; ly < MS_GB_H; ly++) {
        for (int lx = 0; lx < MS_GB_W; lx++) {
            uint8_t col;
            int in_window = 0;
            if (win_on && ly >= IO(0xFF4A) && lx + 7 >= IO(0xFF4B)) in_window = 1;
            if (in_window) {
                int wy = ly - IO(0xFF4A);
                int wx = lx - (IO(0xFF4B) - 7);
                uint8_t id = m->vram[0][win_map - 0x8000 + ((wy >> 3) & 31) * 32 + ((wx >> 3) & 31)];
                uint16_t ta = signed_tiles ? (uint16_t)(0x9000 + (int8_t)id * 16) : (uint16_t)(0x8000 + id * 16);
                col = shade(bgp, tile_pixel(m, ta, wy & 7, wx & 7, 0, 0));
            } else {
                int by = (scy + ly) & 0xFF, bx = (scx + lx) & 0xFF;
                uint8_t id = m->vram[0][bg_map - 0x8000 + (by >> 3) * 32 + (bx >> 3)];
                uint16_t ta = signed_tiles ? (uint16_t)(0x9000 + (int8_t)id * 16) : (uint16_t)(0x8000 + id * 16);
                col = shade(bgp, tile_pixel(m, ta, by & 7, bx & 7, 0, 0));
            }
            px[ly * MS_GB_W + lx] = col;
        }
    }

    // sprites: 40 entries, DMG priorities (x order, 10 per line)
    if (!(lcdc & 0x02)) return;
    int big = (lcdc & 0x04) ? 16 : 8;
    for (int ly = 0; ly < MS_GB_H; ly++) {
        int drawn = 0;
        for (int s = 0; s < 40 && drawn < 10; s++) {
            const uint8_t *o = &m->oam[s * 4];
            int sy = o[0] - 16, sx = o[1] - 8;
            if (ly < sy || ly >= sy + big || o[1] == 0 || o[1] >= 168) continue;
            int ty = (o[2] & (big == 16 ? 0xFE : 0xFF)) + (o[3] & 0x40 ? (big - 1 - (ly - sy)) : (ly - sy));
            uint16_t ta = (uint16_t)(0x8000 + ty * 16);
            uint8_t pal = (o[3] & 0x10) ? IO(0xFF49) : IO(0xFF48);
            int front = !(o[3] & 0x80); // attr bit 7: 0 = sprite above BG
            for (int dx = 0; dx < 8; dx++) {
                int x = sx + dx;
                if (x < 0 || x >= MS_GB_W) continue;
                uint8_t c = tile_pixel(m, ta, (ly - sy) & 7, dx, o[3] & 0x20 ? 1 : 0, 0);
                if (!c) continue; // transparent
                if (front || px[ly * MS_GB_W + x] == 0) {
                    px[ly * MS_GB_W + x] = shade(pal, c);
                    drawn++;
                }
            }
        }
    }
}
