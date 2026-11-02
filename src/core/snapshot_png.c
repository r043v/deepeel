// snapshot_png.c: PNG writer/reader for the snapshot gallery and the memory
// map.  The image is a normal, human-viewable PNG; the tool's data rides
// inside zTXt chunks (machine snapshot "deepeel_snapshot" as hex, map text,
// settings, usage statistics), the way a PICO-8 cartridge hides its code in
// the palette — one file, visible to the player, loadable by the tool.
//
// zlib compresses the image (level 9) and every text chunk.  The reader also
// takes the older files (stored DEFLATE blocks, tEXt chunks): zlib decodes
// both.
#include "snapshot_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// --- chunk helpers ---------------------------------------------------------------
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t chunk_crc(const char *type, const uint8_t *data, size_t len) {
    uLong c = crc32(0L, (const Bytef *)type, 4);
    if (len) c = crc32(c, data, (uInt)len);
    return (uint32_t)c;
}

static void wr_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    uint8_t hdr[8], crcb[4];
    be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    be32(crcb, chunk_crc(type, data, len));
    fwrite(crcb, 1, 4, f);
}

// zlib-compress `n` bytes (level 9); malloc'd result in *out
static int zpack(const uint8_t *src, size_t n, uint8_t **out, size_t *out_len) {
    uLongf cap = compressBound((uLong)n);
    uint8_t *z = malloc(cap ? cap : 1);
    if (!z) return 0;
    if (compress2(z, &cap, src, (uLong)n, 9) != Z_OK) { free(z); return 0; }
    *out = z;
    *out_len = cap;
    return 1;
}

// inflate a whole zlib stream of unknown size; malloc'd result
static int zunpack(const uint8_t *src, size_t n, uint8_t **out, size_t *out_len) {
    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit(&s) != Z_OK) return 0;
    size_t cap = n * 4 + 1024, ol = 0;
    uint8_t *o = malloc(cap);
    if (!o) { inflateEnd(&s); return 0; }
    s.next_in = (Bytef *)src;
    s.avail_in = (uInt)n;
    int r;
    do {
        if (ol == cap) {
            cap *= 2;
            uint8_t *g = realloc(o, cap);
            if (!g) { free(o); inflateEnd(&s); return 0; }
            o = g;
        }
        s.next_out = o + ol;
        s.avail_out = (uInt)(cap - ol);
        r = inflate(&s, Z_NO_FLUSH);
        ol = cap - s.avail_out;
    } while (r == Z_OK);
    inflateEnd(&s);
    if (r != Z_STREAM_END) { free(o); return 0; }
    *out = o;
    *out_len = ol;
    return 1;
}

int dp_png_save_chunks(const char *path, int w, int h, const uint8_t *rgb, const DpPngText *items,
                       int n) {
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

    // raw scanlines with filter byte 0, zlib level 9
    size_t stride = 1 + (size_t)w * 3, raw_len = (size_t)h * stride;
    uint8_t *raw = malloc(raw_len ? raw_len : 1);
    if (!raw) { fclose(f); return 0; }
    for (int y = 0; y < h; y++) {
        raw[y * stride] = 0;
        memcpy(raw + y * stride + 1, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }
    uint8_t *z;
    size_t zl;
    int ok = zpack(raw, raw_len, &z, &zl);
    free(raw);
    if (!ok) { fclose(f); return 0; }
    wr_chunk(f, "IDAT", z, zl);
    free(z);

    // zTXt: keyword NUL method(0) zlib(text); hex-encoded for binary payloads
    for (int i = 0; i < n; i++) {
        const DpPngText *t = &items[i];
        if (!t->key || !t->data || !t->len) continue;
        static const char hexd[] = "0123456789abcdef";
        const uint8_t *src = t->data;
        uint8_t *hexbuf = NULL;
        size_t body = t->len;
        if (t->hex) {
            hexbuf = malloc(t->len * 2);
            if (!hexbuf) continue;
            for (size_t k = 0; k < t->len; k++) {
                hexbuf[k * 2] = (uint8_t)hexd[src[k] >> 4];
                hexbuf[k * 2 + 1] = (uint8_t)hexd[src[k] & 0xF];
            }
            src = hexbuf;
            body = t->len * 2;
        }
        uint8_t *cz;
        size_t czl;
        if (zpack(src, body, &cz, &czl)) {
            size_t kl = strlen(t->key);
            uint8_t *c = malloc(kl + 2 + czl);
            if (c) {
                memcpy(c, t->key, kl);
                c[kl] = 0;     // NUL separator
                c[kl + 1] = 0; // compression method: deflate
                memcpy(c + kl + 2, cz, czl);
                wr_chunk(f, "zTXt", c, kl + 2 + czl);
                free(c);
            }
            free(cz);
        }
        free(hexbuf);
    }
    wr_chunk(f, "IEND", NULL, 0);
    fclose(f);
    return 1;
}

int dp_png_save(const char *path, int w, int h, const uint8_t *rgb, const uint8_t *payload,
                size_t payload_len) {
    DpPngText t = { "deepeel_snapshot", payload, payload_len, 1 };
    return dp_png_save_chunks(path, w, h, rgb, &t, payload ? 1 : 0);
}

int dp_png_save_text(const char *path, int w, int h, const uint8_t *rgb, const char *key,
                     const char *text, size_t text_len) {
    DpPngText t = { key, text, text_len, 0 };
    return dp_png_save_chunks(path, w, h, rgb, &t, 1);
}

int dp_png_save_text2(const char *path, int w, int h, const uint8_t *rgb, const char *key,
                      const char *text, size_t text_len, const char *key2, const char *text2,
                      size_t text2_len) {
    DpPngText t[2] = { { key, text, text_len, 0 }, { key2, text2, text2_len, 0 } };
    return dp_png_save_chunks(path, w, h, rgb, t, 2);
}

// --- reader -------------------------------------------------------------------------
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// one chunk walk: calls back with every chunk whose CRC holds; stops at IEND
// or when the callback returns nonzero
typedef int (*chunk_fn)(const char *type, const uint8_t *data, uint32_t len, void *ctx);

static int walk_chunks(const char *path, chunk_fn fn, void *ctx) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) { fclose(f); return 0; }
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
            uint8_t *g = realloc(chunk, cap);
            if (!g) break;
            chunk = g;
        }
        if (fread(chunk, 1, (size_t)len + 4, f) != (size_t)len + 4) break;
        if (chunk_crc(type, chunk, len) != rd32(chunk + len)) break; // corrupt: stop
        if (!strcmp(type, "IEND")) break;
        if (fn(type, chunk, len, ctx)) break;
    }
    free(chunk);
    fclose(f);
    return 1;
}

// the text of a tEXt/zTXt chunk named `key` (malloc'd, NUL-terminated)
static char *chunk_text(const char *type, const uint8_t *d, uint32_t len, const char *key,
                        size_t *n_out) {
    size_t kl = strlen(key);
    if (len <= kl || memcmp(d, key, kl) || d[kl] != 0) return NULL;
    if (!strcmp(type, "tEXt")) {
        size_t n = len - kl - 1;
        char *r = malloc(n + 1);
        if (!r) return NULL;
        memcpy(r, d + kl + 1, n);
        r[n] = 0;
        *n_out = n;
        return r;
    }
    if (!strcmp(type, "zTXt") && len > kl + 1 && d[kl + 1] == 0) {
        uint8_t *o;
        size_t ol;
        if (!zunpack(d + kl + 2, len - kl - 2, &o, &ol)) return NULL;
        char *r = realloc(o, ol + 1);
        if (!r) { free(o); return NULL; }
        r[ol] = 0;
        *n_out = ol;
        return r;
    }
    return NULL;
}

typedef struct {
    int w, h;
    uint8_t *idat;
    size_t idat_n;
    char *snap;
    size_t snap_n;
} LoadCtx;

static int load_cb(const char *type, const uint8_t *d, uint32_t len, void *ctx) {
    LoadCtx *c = ctx;
    if (!strcmp(type, "IHDR") && len >= 13) {
        c->w = (int)rd32(d);
        c->h = (int)rd32(d + 4);
        if (d[8] != 8 || d[9] != 2) c->w = 0; // only 8-bit RGB
    } else if (!strcmp(type, "IDAT")) {
        uint8_t *g = realloc(c->idat, c->idat_n + len);
        if (!g) return 1;
        c->idat = g;
        memcpy(c->idat + c->idat_n, d, len);
        c->idat_n += len;
    } else if (!c->snap) {
        size_t n;
        char *t = chunk_text(type, d, len, "deepeel_snapshot", &n);
        if (t) { c->snap = t; c->snap_n = n; }
    }
    return 0;
}

int dp_png_load(const char *path, int *w, int *h, uint8_t **rgb_out, uint8_t **payload_out,
                size_t *payload_len_out) {
    LoadCtx c;
    memset(&c, 0, sizeof c);
    if (!walk_chunks(path, load_cb, &c)) return 0;
    int ok = c.w && c.h && c.idat;
    uint8_t *rgb = NULL, *payload = NULL;
    size_t payload_n = 0;
    if (ok) {
        uint8_t *raw;
        size_t raw_n;
        ok = zunpack(c.idat, c.idat_n, &raw, &raw_n);
        if (ok) {
            size_t stride = 1 + (size_t)c.w * 3;
            if (raw_n >= (size_t)c.h * stride) {
                rgb = malloc((size_t)c.w * c.h * 3);
                for (int y = 0; y < c.h && rgb; y++)
                    memcpy(rgb + (size_t)y * c.w * 3, raw + y * stride + 1, (size_t)c.w * 3);
                ok = rgb != NULL;
            } else {
                ok = 0;
            }
            free(raw);
        }
    }
    if (ok && c.snap) { // hex payload
        payload_n = c.snap_n / 2;
        payload = malloc(payload_n ? payload_n : 1);
        for (size_t i = 0; payload && i < payload_n; i++) {
            int hi = hexval(c.snap[i * 2]), lo = hexval(c.snap[i * 2 + 1]);
            if (hi < 0 || lo < 0) { payload_n = 0; break; }
            payload[i] = (uint8_t)(hi * 16 + lo);
        }
    }
    free(c.idat);
    free(c.snap);
    if (!ok) {
        free(rgb);
        free(payload);
        return 0;
    }
    *w = c.w;
    *h = c.h;
    *rgb_out = rgb;
    *payload_out = payload; // may be NULL: no snapshot inside
    *payload_len_out = payload_n;
    return 1;
}

typedef struct {
    const char *key;
    char *res;
    size_t n;
} TextCtx;

static int text_cb(const char *type, const uint8_t *d, uint32_t len, void *ctx) {
    TextCtx *c = ctx;
    c->res = chunk_text(type, d, len, c->key, &c->n);
    return c->res != NULL;
}

char *dp_png_text(const char *path, const char *key, size_t *len_out) {
    TextCtx c = { key, NULL, 0 };
    if (!walk_chunks(path, text_cb, &c)) return NULL;
    if (c.res && len_out) *len_out = c.n;
    return c.res;
}
