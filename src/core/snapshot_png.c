// snapshot_png.c: dependency-free PNG writer/reader for the snapshot
// gallery.  The image is a normal, human-viewable PNG; the machine snapshot
// rides inside a tEXt chunk ("deepeel_snapshot", hex), the way a PICO-8
// cartridge hides its code in the palette — one file, visible to the player,
// loadable by the tool.
//
// The writer emits a zlib stream of stored (uncompressed) DEFLATE blocks;
// the reader accepts exactly that (it only reads files this tool wrote).
#include "snapshot_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- crc32 -------------------------------------------------------------------
static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

// --- chunk helpers ---------------------------------------------------------------
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    uint8_t hdr[8], crcb[4];
    be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = 0xFFFFFFFFu;
    for (const char *t = type; *t; t++) c = crc_table[(c ^ (uint8_t)*t) & 0xFF] ^ (c >> 8);
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    be32(crcb, c ^ 0xFFFFFFFFu);
    fwrite(crcb, 1, 4, f);
}

// --- png writer ------------------------------------------------------------------
static int png_write(const char *path, int w, int h, const uint8_t *rgb, const char *key, int hex,
                     const uint8_t *payload, size_t payload_len) {
    static int inited;
    if (!inited) { crc_init(); inited = 1; }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 2;  // color type: truecolor RGB
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    wr_chunk(f, "IHDR", ihdr, 13);

    // raw scanlines with filter byte 0
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return 0; }
    for (int y = 0; y < h; y++) {
        raw[y * (1 + (size_t)w * 3)] = 0;
        memcpy(raw + y * (1 + (size_t)w * 3) + 1, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }
    // zlib stream: 0x78 0x01, stored DEFLATE blocks, adler32
    size_t zlib_len = raw_len + raw_len / 65535 * 5 + 5 + 6 + 64;
    uint8_t *z = malloc(zlib_len);
    if (!z) { free(raw); fclose(f); return 0; }
    size_t zi = 0;
    z[zi++] = 0x78;
    z[zi++] = 0x01;
    const uint8_t *src = raw;
    size_t left = raw_len;
    while (left) {
        size_t blk = left > 65535 ? 65535 : left;
        int last = blk == left;
        z[zi++] = (uint8_t)(last ? 1 : 0);
        z[zi++] = (uint8_t)(blk & 0xFF);
        z[zi++] = (uint8_t)(blk >> 8);
        z[zi++] = (uint8_t)(~blk & 0xFF);
        z[zi++] = (uint8_t)((~blk >> 8) & 0xFF);
        memcpy(z + zi, src, blk);
        zi += blk;
        src += blk;
        left -= blk;
    }
    // adler32 of the raw bytes
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    be32(z + zi, (b << 16) | a);
    zi += 4;
    wr_chunk(f, "IDAT", z, zi);
    free(z);
    free(raw);

    // the payload rides in a tEXt chunk: hex-encoded (snapshots) or verbatim text (maps)
    if (payload && payload_len) {
        static const char hexd[] = "0123456789abcdef";
        size_t kl = strlen(key), body = hex ? payload_len * 2 : payload_len;
        uint8_t *txt = malloc(kl + 1 + body);
        if (txt) {
            memcpy(txt, key, kl);
            txt[kl] = 0; // NUL separator
            for (size_t i = 0; i < payload_len; i++) {
                if (hex) {
                    txt[kl + 1 + i * 2] = (uint8_t)hexd[payload[i] >> 4];
                    txt[kl + 2 + i * 2] = (uint8_t)hexd[payload[i] & 0xF];
                } else {
                    txt[kl + 1 + i] = payload[i];
                }
            }
            wr_chunk(f, "tEXt", txt, kl + 1 + body);
            free(txt);
        }
    }
    wr_chunk(f, "IEND", NULL, 0);
    fclose(f);
    return 1;
}

int dp_png_save(const char *path, int w, int h, const uint8_t *rgb, const uint8_t *payload,
                size_t payload_len) {
    return png_write(path, w, h, rgb, "deepeel_snapshot", 1, payload, payload_len);
}

int dp_png_save_text(const char *path, int w, int h, const uint8_t *rgb, const char *key,
                     const char *text, size_t text_len) {
    return png_write(path, w, h, rgb, key, 0, (const uint8_t *)text, text_len);
}

// --- png reader (stored-block zlib only) ---------------------------------------------
static int inflate_stored(const uint8_t *z, size_t n, uint8_t **out, size_t *out_len) {
    if (n < 2 || (z[0] & 0x0F) != 8) return 0;
    size_t i = 2;
    size_t cap = 65536;
    uint8_t *o = malloc(cap);
    size_t ol = 0;
    while (i < n) {
        uint8_t hdr = z[i++];
        int last = hdr & 1;
        if ((hdr >> 1) != 0) { free(o); return 0; } // only stored blocks
        uint16_t blk = (uint16_t)(z[i] | (z[i + 1] << 8));
        i += 4; // LEN + NLEN
        if (i + blk > n) { free(o); return 0; }
        if (ol + blk > cap) {
            while (ol + blk > cap) cap *= 2;
            o = realloc(o, cap);
        }
        memcpy(o + ol, z + i, blk);
        ol += blk;
        i += blk;
        if (last) break;
    }
    *out = o;
    *out_len = ol;
    return 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int dp_png_load(const char *path, int *w, int *h, uint8_t **rgb_out, uint8_t **payload_out,
                size_t *payload_len_out) {
    static int inited;
    if (!inited) { crc_init(); inited = 1; }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) { fclose(f); return 0; }
    int iw = 0, ih = 0;
    uint8_t *idat = NULL;
    size_t idat_n = 0, idat_cap = 0;
    uint8_t *payload = NULL;
    size_t payload_n = 0;
    uint8_t *chunk = NULL;
    size_t chunk_cap = 0;
    for (;;) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;
        uint32_t len = rd32(hdr);
        char type[5] = { 0 };
        memcpy(type, hdr + 4, 4);
        if (len + 4 > chunk_cap) {
            chunk_cap = len + 4 + 1024;
            chunk = realloc(chunk, chunk_cap);
        }
        if (fread(chunk, 1, len + 4, f) != len + 4) break;
        uint32_t crc = rd32(chunk + len);
        uint32_t c = 0xFFFFFFFFu;
        for (const char *t = type; *t; t++) c = crc_table[(c ^ (uint8_t)*t) & 0xFF] ^ (c >> 8);
        for (uint32_t i = 0; i < len; i++) c = crc_table[(c ^ chunk[i]) & 0xFF] ^ (c >> 8);
        if ((c ^ 0xFFFFFFFFu) != crc) break; // corrupt: stop
        if (!strcmp(type, "IHDR") && len >= 13) {
            iw = (int)rd32(chunk);
            ih = (int)rd32(chunk + 4);
            if (chunk[8] != 8 || chunk[9] != 2) { iw = 0; } // only 8-bit RGB
        } else if (!strcmp(type, "IDAT")) {
            idat = realloc(idat, idat_cap + len);
            memcpy(idat + idat_cap, chunk, len);
            idat_cap += len;
            idat_n = idat_cap;
        } else if (!strcmp(type, "tEXt") && len > 17 && !memcmp(chunk, "deepeel_snapshot", 16)) {
            // keyword NUL hex...
            if (chunk[16] == 0) {
                size_t hexn = len - 17;
                payload_n = hexn / 2;
                payload = malloc(payload_n ? payload_n : 1);
                for (size_t i = 0; i < payload_n; i++) {
                    int hi = hexval((char)chunk[17 + i * 2]), lo = hexval((char)chunk[18 + i * 2]);
                    if (hi < 0 || lo < 0) { payload_n = 0; break; }
                    payload[i] = (uint8_t)(hi * 16 + lo);
                }
            }
        } else if (!strcmp(type, "IEND")) {
            break;
        }
    }
    free(chunk);
    fclose(f);
    int ok = iw && ih && idat;
    uint8_t *rgb = NULL;
    if (ok) {
        uint8_t *raw;
        size_t raw_n;
        ok = inflate_stored(idat, idat_n, &raw, &raw_n);
        if (ok) {
            size_t stride = 1 + (size_t)iw * 3;
            if (raw_n >= (size_t)ih * stride) {
                rgb = malloc((size_t)iw * ih * 3);
                for (int y = 0; y < ih; y++)
                    memcpy(rgb + (size_t)y * iw * 3, raw + y * stride + 1, (size_t)iw * 3);
            } else {
                ok = 0;
            }
            free(raw);
        }
    }
    free(idat);
    if (!ok) {
        free(rgb);
        free(payload);
        return 0;
    }
    *w = iw;
    *h = ih;
    *rgb_out = rgb;
    *payload_out = payload; // may be NULL: no snapshot inside
    *payload_len_out = payload_n;
    return 1;
}

// returns the malloc'd, NUL-terminated text of the tEXt chunk `key` (CRC checked)
char *dp_png_text(const char *path, const char *key, size_t *len_out) {
    static int inited;
    if (!inited) { crc_init(); inited = 1; }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) { fclose(f); return NULL; }
    size_t kl = strlen(key);
    char *res = NULL;
    uint8_t *chunk = NULL;
    size_t cap = 0;
    for (;;) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;
        uint32_t len = rd32(hdr);
        char type[5] = { 0 };
        memcpy(type, hdr + 4, 4);
        if ((size_t)len + 5 > cap) {
            cap = (size_t)len + 1024;
            chunk = realloc(chunk, cap);
            if (!chunk) break;
        }
        if (fread(chunk, 1, (size_t)len + 4, f) != (size_t)len + 4) break;
        uint32_t c = 0xFFFFFFFFu;
        for (const char *t = type; *t; t++) c = crc_table[(c ^ (uint8_t)*t) & 0xFF] ^ (c >> 8);
        for (uint32_t i = 0; i < len; i++) c = crc_table[(c ^ chunk[i]) & 0xFF] ^ (c >> 8);
        if ((c ^ 0xFFFFFFFFu) != rd32(chunk + len)) break;
        if (!strcmp(type, "tEXt") && len > kl && !memcmp(chunk, key, kl) && chunk[kl] == 0) {
            size_t n = len - kl - 1;
            res = malloc(n + 1);
            if (res) { memcpy(res, chunk + kl + 1, n); res[n] = 0; if (len_out) *len_out = n; }
            break;
        }
        if (!strcmp(type, "IEND")) break;
    }
    free(chunk);
    fclose(f);
    return res;
}
