#include "canvas.h"
#include "font5x7.h"
#include "../core/snapshot_png.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

DpCanvas *dp_canvas_new(int w, int h, uint32_t bg) {
    DpCanvas *c = calloc(1, sizeof *c); if (!c) return NULL;
    c->px = malloc((size_t)w * h * 4); if (!c->px) { free(c); return NULL; }
    c->w = w; c->h = h;
    for (size_t i = 0; i < (size_t)w * h; i++) c->px[i] = bg;
    return c;
}
void dp_canvas_free(DpCanvas *c) { if (c) { free(c->px); free(c); } }

void dp_canvas_rect(DpCanvas *c, int x, int y, int w, int h, uint32_t argb) {
    int x1 = x + w, y1 = y + h; if (x < 0) x = 0; if (y < 0) y = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    for (int j = y; j < y1; j++) for (int i = x; i < x1; i++) c->px[(size_t)j * c->w + i] = argb;
}
void dp_canvas_frame(DpCanvas *c, int x, int y, int w, int h, int t, uint32_t argb) {
    dp_canvas_rect(c, x, y, w, t, argb); dp_canvas_rect(c, x, y + h - t, w, t, argb);
    dp_canvas_rect(c, x, y, t, h, argb); dp_canvas_rect(c, x + w - t, y, t, h, argb);
}
int dp_canvas_text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }
void dp_canvas_text(DpCanvas *c, int x, int y, const char *s, int scale, uint32_t argb) {
    for (; *s; s++, x += 6 * scale) {
        unsigned ch = (unsigned char)*s; if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *g = dp_font5x7[ch - 0x20];
        for (int cx = 0; cx < 5; cx++) for (int cy = 0; cy < 7; cy++) if (g[cx] & (1 << cy)) dp_canvas_rect(c, x + cx * scale, y + cy * scale, scale, scale, argb);
    }
}

int dp_canvas_fit(int w, int h, int cw, int ch, int *dw, int *dh) {
    if (w < 1 || h < 1) { *dw = *dh = 0; return 1; }
    int z = cw / w < ch / h ? cw / w : ch / h;
    if (z >= 1) { *dw = w * z; *dh = h * z; return z; }
    int d = 2; while ((w + d - 1) / d > cw || (h + d - 1) / d > ch) d++;
    *dw = (w + d - 1) / d; *dh = (h + d - 1) / d; return -d;
}
int dp_canvas_tile(DpCanvas *c, int cx, int cy, int cw, int ch, const uint32_t *src, int sw, int sh) {
    int dw, dh, f = dp_canvas_fit(sw, sh, cw, ch, &dw, &dh);
    int ox = cx + (cw - dw) / 2, oy = cy + (ch - dh) / 2;
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        int X = ox + x, Y = oy + y; if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) continue;
        uint32_t v;
        if (f >= 1) v = src[(size_t)(y / f) * sw + x / f];
        else {
            int d = -f, x0 = x * d, y0 = y * d; unsigned r = 0, g = 0, b = 0, n = 0;
            for (int j = y0; j < y0 + d && j < sh; j++) for (int i = x0; i < x0 + d && i < sw; i++) { uint32_t p = src[(size_t)j * sw + i]; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++; }
            v = n ? 0xff000000u | (r / n) << 16 | (g / n) << 8 | (b / n) : 0xff000000u;
        }
        c->px[(size_t)Y * c->w + X] = v | 0xff000000u;
    }
    return f;
}

int dp_canvas_grid(int n, int tw, int th, int w, int h, int *rows) {
    int best = 1, br = n, bs = 0;
    for (int cols = 1; cols <= n; cols++) {
        int r = (n + cols - 1) / cols, s = w / cols < h / r ? w / cols : h / r;   // cell side budget, the tile aspect is applied by the caller
        (void)tw; (void)th;
        if (s > bs) { bs = s; best = cols; br = r; }
    }
    *rows = br; return best;
}

// class colour of a coverage flag byte (bits of core.h: natural 1, forced 2, copied 4, compressed 8, data 16, static 32); 0 = not reached
static uint32_t cov_color(uint8_t f) {
    if (f & 1) return 0xff2f6fd6u;    // natural code
    if (f & 2) return 0xff5aa0e8u;    // forced code
    if (f & 32) return 0xff9cc4f0u;   // static code
    if (f & 16) return 0xff6cc06cu;   // data read
    if (f & 12) return 0xff30a040u;   // copied / compressed data
    return 0xff26262eu;
}
static uint32_t cov_square(const uint8_t *fl, size_t lo, size_t hi) {   // dominant class of the bytes [lo,hi): reached classes outweigh unreached
    size_t cnt[7] = {0}; uint32_t col[7] = { 0xff26262eu, 0xff2f6fd6u, 0xff5aa0e8u, 0xff9cc4f0u, 0xff6cc06cu, 0xff30a040u, 0 };
    for (size_t j = lo; j < hi; j++) { uint32_t c = cov_color(fl[j]); for (int k = 0; k < 6; k++) if (c == col[k]) { cnt[k]++; break; } }
    int best = 0; size_t bc = 0; for (int k = 1; k < 6; k++) if (cnt[k] > bc) { bc = cnt[k]; best = k; }
    return bc ? col[best] : col[0];
}
// one tile per ROM bank, one square per `block` bytes; the block is the smallest power of two whose squares stay >= 3 px, so a square is
// never one byte except for tiny ROMs. Returns the block used.
int dp_canvas_covmap_width(size_t n, size_t bank, int cw, int ch) {   // the width dp_canvas_covmap really draws in a (cw, ch) cell
    if (!n || !bank || cw < 8 || ch < 8) return 0;
    size_t banks = (n + bank - 1) / bank; int rows;
    int cols = dp_canvas_grid((int)banks, 1, 1, cw, ch, &rows);
    int tile = (cw / cols < ch / rows ? cw / cols : ch / rows), gap = tile > 24 ? 4 : 1; tile -= gap;
    int per = 1; size_t block = 1;
    for (;; block <<= 1) { per = 1; while ((size_t)per * per * block < bank) per++; if (tile / per >= 3 || block >= bank) break; }
    int sq = tile / per; if (sq < 1) sq = 1;
    return cols * per * sq + (cols - 1) * gap;
}
size_t dp_canvas_covmap(DpCanvas *c, int cx, int cy, int cw, int ch, const uint8_t *fl, size_t n, size_t bank, const uint8_t *inf, const uint8_t *prov) {
    if (!n || !bank || cw < 8 || ch < 8) return 0;
    size_t banks = (n + bank - 1) / bank; int rows;
    int cols = dp_canvas_grid((int)banks, 1, 1, cw, ch, &rows);
    int tile = (cw / cols < ch / rows ? cw / cols : ch / rows), gap = tile > 24 ? 4 : 1; tile -= gap;
    size_t block = 1; int per = 1, sq = 1;
    for (;; block <<= 1) {
        per = 1; while ((size_t)per * per * block < bank) per++;
        sq = tile / per; if (sq >= 3 || block >= bank) break;
    }
    if (sq < 1) sq = 1;
    tile = per * sq;   // the banks are packed: what rounding leaves over stays outside the map
    for (size_t b = 0; b < banks; b++) {
        int tx = cx + (int)(b % cols) * (tile + gap), ty = cy + (int)(b / cols) * (tile + gap);
        dp_canvas_rect(c, tx, ty, per * sq, per * sq, 0xff101014u);
        for (size_t i = 0; i * block < bank; i++) {
            size_t lo = b * bank + i * block, hi = lo + block; if (lo >= n) break; if (hi > n) hi = n;
            int x = tx + (int)(i % per) * sq, y = ty + (int)(i / per) * sq;
            uint32_t col = cov_square(fl, lo, hi);
            if (col == 0xff26262eu) for (size_t k = lo; k < hi; k++) if ((inf && inf[k]) || (prov && prov[k])) { col = 0xffb07828u; break; }
            dp_canvas_rect(c, x, y, sq > 2 ? sq - 1 : sq, sq > 2 ? sq - 1 : sq, col);
        }
    }
    return block;
}

int dp_canvas_save_png(const DpCanvas *c, const char *path) {
    uint8_t *rgb = malloc((size_t)c->w * c->h * 3); if (!rgb) return 1;
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) { rgb[3 * i] = c->px[i] >> 16; rgb[3 * i + 1] = c->px[i] >> 8; rgb[3 * i + 2] = c->px[i]; }
    int rc = dp_png_save(path, c->w, c->h, rgb, NULL, 0); free(rgb); return rc;
}
