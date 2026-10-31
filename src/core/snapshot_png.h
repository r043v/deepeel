// snapshot_png.h: PNG snapshots — a human-viewable image with the machine
// snapshot embedded (tEXt chunk "memscan_snapshot", hex), one file per
// checkpoint, the way a PICO-8 cartridge hides its code in the PNG.
#ifndef MS_SNAPSHOT_PNG_H
#define MS_SNAPSHOT_PNG_H

#include <stddef.h>
#include <stdint.h>

// writes a truecolor PNG (w*h*3 rgb bytes); payload may be NULL
int ms_png_save(const char *path, int w, int h, const uint8_t *rgb, const uint8_t *payload,
                size_t payload_len);

// reads a PNG written by this tool (8-bit RGB, stored-block zlib); returns
// malloc'd rgb and the embedded payload (NULL/0 when absent)
int ms_png_load(const char *path, int *w, int *h, uint8_t **rgb_out, uint8_t **payload_out,
                size_t *payload_len_out);

#endif
