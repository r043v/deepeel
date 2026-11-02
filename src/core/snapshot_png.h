// snapshot_png.h: PNG snapshots and maps — a human-viewable image with the
// tool's data embedded in zlib-compressed zTXt chunks (machine snapshot
// "deepeel_snapshot" as hex, map text, settings, usage statistics), the way a
// PICO-8 cartridge hides its code in the PNG.
#ifndef DP_SNAPSHOT_PNG_H
#define DP_SNAPSHOT_PNG_H

#include <stddef.h>
#include <stdint.h>

// one embedded zTXt chunk: `data` (len bytes), hex-encoded first when `hex`
typedef struct DpPngText {
    const char *key;
    const void *data;
    size_t len;
    int hex;
} DpPngText;

// writes a truecolor PNG (w*h*3 rgb bytes, zlib level 9) and `n` zTXt chunks
int dp_png_save_chunks(const char *path, int w, int h, const uint8_t *rgb, const DpPngText *items,
                       int n);

// writes a truecolor PNG (w*h*3 rgb bytes); payload may be NULL
int dp_png_save(const char *path, int w, int h, const uint8_t *rgb, const uint8_t *payload,
                size_t payload_len);

// same image, with `text` (Latin-1/ASCII) in a zTXt chunk named `key`
int dp_png_save_text(const char *path, int w, int h, const uint8_t *rgb, const char *key,
                     const char *text, size_t text_len);

// same, with a second zTXt chunk (the map's settings next to the map)
int dp_png_save_text2(const char *path, int w, int h, const uint8_t *rgb, const char *key,
                      const char *text, size_t text_len, const char *key2, const char *text2,
                      size_t text2_len);

// text of the tEXt or zTXt chunk `key` (malloc'd, NUL-terminated) or NULL
char *dp_png_text(const char *path, const char *key, size_t *len_out);

// reads a PNG written by this tool (8-bit RGB; older stored-block files too); returns
// malloc'd rgb and the embedded payload (NULL/0 when absent)
int dp_png_load(const char *path, int *w, int *h, uint8_t **rgb_out, uint8_t **payload_out,
                size_t *payload_len_out);

#endif
