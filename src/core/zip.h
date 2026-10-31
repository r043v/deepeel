/* zip.h: a minimal zip reader and a full DEFLATE inflate.  Enough for TAS
 * containers (.bk2 is a zip, sometimes nested inside a zip) without pulling
 * in a compression library. */
#ifndef MS_ZIP_H
#define MS_ZIP_H

#include <stddef.h>
#include <stdint.h>

// decompress a raw DEFLATE stream (or a zlib-wrapped one, detected by its
// 0x78 header byte).  Returns a malloc'd buffer, *out_len set.  NULL on error.
uint8_t *ms_inflate(const uint8_t *in, size_t in_len, size_t *out_len);

// find the first zip entry whose name contains `needle` (case-insensitive,
// NULL = first entry) and decompress it.  Returns a malloc'd buffer.
uint8_t *ms_zip_read(const uint8_t *in, size_t in_len, const char *needle,
                     size_t *out_len, char name[128]);

// true when the buffer starts with a zip local-file signature
int ms_zip_is(const uint8_t *in, size_t in_len);

#endif
