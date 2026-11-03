#include "statefile.h"
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int dp_state_export(const char *path, const char *platform, const uint8_t *rom, size_t rom_size,
                    uint32_t depth, const uint8_t *z, uint32_t zlen, uint32_t state_bytes) {
    uint8_t h[36] = { 'D', 'P', 'S', 'T' };
    le32(h + 4, 1);
    strncpy((char *)h + 8, platform, 8);
    le32(h + 16, (uint32_t)crc32(0, rom, (uInt)rom_size));
    le32(h + 20, (uint32_t)rom_size);
    le32(h + 24, depth);
    le32(h + 28, state_bytes);
    le32(h + 32, zlen);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    int bad = fwrite(h, 1, sizeof h, f) != sizeof h || fwrite(z, 1, zlen, f) != zlen;
    return fclose(f) || bad;
}
