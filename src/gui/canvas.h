// canvas.h: the one software compositor of every picture deepeel composes (TAS summary PNG, live window tiles).
// A canvas is an ARGB pixel buffer; the sinks (PNG file, SDL texture) only receive it. Nothing here knows a machine:
// a tile is a (w,h) screen of any source, scaled by an integer factor or an integer divisor to fit its cell.
#ifndef DP_CANVAS_H
#define DP_CANVAS_H
#include <stddef.h>
#include <stdint.h>

typedef struct { int w, h; uint32_t *px; } DpCanvas;

DpCanvas *dp_canvas_new(int w, int h, uint32_t bg);
void dp_canvas_free(DpCanvas *c);
void dp_canvas_rect(DpCanvas *c, int x, int y, int w, int h, uint32_t argb);
void dp_canvas_frame(DpCanvas *c, int x, int y, int w, int h, int t, uint32_t argb);   // hollow rectangle, t px thick
void dp_canvas_text(DpCanvas *c, int x, int y, const char *s, int scale, uint32_t argb);
int dp_canvas_text_w(const char *s, int scale);
// largest integer zoom (>=1) or divisor (<0, -d = 1/d) of a w*h screen that fits cw*ch; reports the drawn size
int dp_canvas_fit(int w, int h, int cw, int ch, int *dw, int *dh);
// draw the screen centred in the cell (cx,cy,cw,ch) with dp_canvas_fit's factor (box average when dividing); returns the factor
int dp_canvas_tile(DpCanvas *c, int cx, int cy, int cw, int ch, const uint32_t *src, int sw, int sh);
// ROM coverage as the .map.png: one tile per bank (`bank` bytes), one square per block of bytes coloured by the dominant class, laid out to
// fit the cell; flags: one byte per compact ROM index. The block (power of two, squares >= 3 px) is returned: a square is not a byte.
size_t dp_canvas_covmap(DpCanvas *c, int cx, int cy, int cw, int ch, const uint8_t *flags, size_t n, size_t bank,
                        const uint8_t *inf, const uint8_t *prov, const uint8_t *xpo);   // suspects (may be NULL): squares nothing observed but inferred/probable are orange; xpo (1 code, 2 data, moved from another ROM version): violet / teal
int dp_canvas_covmap_width(size_t n, size_t bank, int cw, int ch);   // the width the map above really draws
int dp_canvas_save_png(const DpCanvas *c, const char *path);
// grid of `n` equal tiles inside (w,h): best columns for the aspect (tile_w:tile_h); returns columns, rows
int dp_canvas_grid(int n, int tile_w, int tile_h, int w, int h, int *rows);
#endif
