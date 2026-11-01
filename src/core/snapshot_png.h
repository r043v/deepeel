// snapshot_png.h: PNG snapshots — a human-viewable image with the machine
// snapshot embedded (tEXt chunk "deepeel_snapshot", hex), one file per
// checkpoint, the way a PICO-8 cartridge hides its code in the PNG.
#ifndef DP_SNAPSHOT_PNG_H
#define DP_SNAPSHOT_PNG_H

#include <stddef.h>
#include <stdint.h>

// writes a truecolor PNG (w*h*3 rgb bytes); payload may be NULL
int dp_png_save(const char *path, int w, int h, const uint8_t *rgb, const uint8_t *payload,
                size_t payload_len);

// same image, with `text` stored verbatim (Latin-1/ASCII) in a tEXt chunk named `key`
int dp_png_save_text(const char *path, int w, int h, const uint8_t *rgb, const char *key,
                     const char *text, size_t text_len);

// same, with a second verbatim tEXt chunk (the map's settings next to the map)
int dp_png_save_text2(const char *path, int w, int h, const uint8_t *rgb, const char *key,
                      const char *text, size_t text_len, const char *key2, const char *text2,
                      size_t text2_len);

// text of the tEXt chunk `key` (malloc'd, NUL-terminated) or NULL
char *dp_png_text(const char *path, const char *key, size_t *len_out);

// reads a PNG written by this tool (8-bit RGB, stored-block zlib); returns
// malloc'd rgb and the embedded payload (NULL/0 when absent)
int dp_png_load(const char *path, int *w, int *h, uint8_t **rgb_out, uint8_t **payload_out,
                size_t *payload_len_out);

#endif
