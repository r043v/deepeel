#include "cart_md.h"
#include "zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MD_MAX 0x400000u

static void field(char *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
    dst[n] = 0;
    for (size_t i = n; i-- > 0 && (dst[i] == ' ' || dst[i] == 0);) dst[i] = 0;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int dp_md_detect(const uint8_t *b, size_t n) {
    if (n >= 0x104 && !memcmp(b + 0x100, "SEGA", 4)) return 1;
    /* .smd: 512-byte header, interleaved 16 KiB blocks */
    if (n >= 0x200 + 0x4000 && n % 0x4000 == 0x200) {
        const uint8_t *d = b + 0x200;
        /* "SEGA" at $100 lands odd bytes in the first half of the block */
        return d[0x2000 + 0x80] == 'S' && d[0x80] == 'E' && d[0x2000 + 0x81] == 'G' && d[0x81] == 'A';
    }
    return 0;
}

static void err_set(char *e, size_t n, const char *m) {
    if (e && n) snprintf(e, n, "%s", m);
}

int dp_md_cart_from_mem(DpMdCart *c, const uint8_t *buf, size_t size, char *err, size_t errsz) {
    memset(c, 0, sizeof *c);
    if (!dp_md_detect(buf, size)) { err_set(err, errsz, "no SEGA header at $100"); return -1; }
    int smd = size >= 0x200 && !!(size % 0x4000 == 0x200) && memcmp(buf + 0x100, "SEGA", 4);
    if (smd) { buf += 0x200; size -= 0x200; }
    if (size > MD_MAX) { err_set(err, errsz, "ROM larger than 4 MiB (mapper not supported yet)"); return -1; }
    c->rom = malloc(size);
    if (!c->rom) { err_set(err, errsz, "out of memory"); return -1; }
    c->size = size;
    if (smd) {
        for (size_t blk = 0; blk < size; blk += 0x4000)
            for (size_t i = 0; i < 0x2000; i++) {
                c->rom[blk + 2 * i + 1] = buf[blk + i];
                c->rom[blk + 2 * i] = buf[blk + 0x2000 + i];
            }
    } else {
        memcpy(c->rom, buf, size);
    }
    const uint8_t *h = c->rom;
    field(c->console, h + 0x100, 16);
    field(c->domestic, h + 0x120, 48);
    field(c->overseas, h + 0x150, 48);
    field(c->serial, h + 0x180, 14);
    field(c->region, h + 0x1F0, 3);
    c->checksum = (uint16_t)(h[0x18E] << 8 | h[0x18F]);
    c->rom_end = be32(h + 0x1A4);
    for (size_t a = 0x200; a + 1 < size; a += 2) c->sum_computed = (uint16_t)(c->sum_computed + (h[a] << 8 | h[a + 1]));
    if (h[0x1B0] == 'R' && h[0x1B1] == 'A') {
        c->has_sram = 1;
        c->sram_lo = be32(h + 0x1B4);
        c->sram_hi = be32(h + 0x1B8);
    }
    for (int i = 0; i < 64 && (size_t)(i * 4 + 4) <= size; i++) c->vec[i] = be32(h + i * 4);
    return 0;
}

int dp_md_cart_load(DpMdCart *c, const char *path, char *err, size_t errsz) {
    size_t n = 0;
    uint8_t *b = dp_read_rom_file(path, &n, err, errsz);
    if (!b) return -1;
    if (n > MD_MAX + 0x200) { free(b); err_set(err, errsz, "bad ROM size"); return -1; }
    int r = dp_md_cart_from_mem(c, b, n, err, errsz);
    free(b);
    return r;
}

void dp_md_cart_free(DpMdCart *c) {
    free(c->rom);
    memset(c, 0, sizeof *c);
}
