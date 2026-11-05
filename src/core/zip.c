/* zip.c: zip reading and DEFLATE decompression, self-contained.
 * The inflate follows RFC 1951 (stored, fixed and dynamic Huffman blocks);
 * the zlib wrapper of RFC 1950 is detected by its header byte.  The zip
 * layer walks the central directory (PK\\x05\\x06 -> entries) and supports
 * the stored (0) and deflate (8) methods, which is all TAS containers use. */
#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// inflate
// ---------------------------------------------------------------------------
typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_cap, out_len;
    int err;
} DpInf;

static int inf_grow(DpInf *s, size_t need) {
    if (need <= s->out_cap) return 1;
    size_t cap = s->out_cap ? s->out_cap * 2 : 4096;
    while (cap < need) cap *= 2;
    uint8_t *p = realloc(s->out, cap);
    if (!p) return 0;
    s->out = p;
    s->out_cap = cap;
    return 1;
}

static int inf_bits(DpInf *s, int n) {
    while (s->bitcnt < n) {
        if (s->in_pos >= s->in_len) { s->err = 1; return -1; }
        s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << n) - 1));
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

// canonical Huffman: counts per length, symbols sorted; decode one symbol
typedef struct { short count[16]; short sym[288]; } DpHuff;

static int huff_build(DpHuff *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0; // all empty: unused table
    short offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (short)i;
    return 1;
}

static int huff_decode(DpInf *s, const DpHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static const short len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577};
static const short dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                     7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int inf_block(DpInf *s, const DpHuff *lit, const DpHuff *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (!inf_grow(s, s->out_len + 1)) { s->err = 1; return 0; }
            s->out[s->out_len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; return 0; }
            int len = len_base[sym] + inf_bits(s, len_extra[sym]);
            int ds = huff_decode(s, dist);
            if (ds < 0 || ds >= 30) { s->err = 1; return 0; }
            int d = dist_base[ds] + inf_bits(s, dist_extra[ds]);
            if (d > (int)s->out_len) { s->err = 1; return 0; }
            if (!inf_grow(s, s->out_len + (size_t)len)) { s->err = 1; return 0; }
            while (len--) {
                s->out[s->out_len] = s->out[s->out_len - d];
                s->out_len++;
            }
        }
    }
}

static const uint8_t fixed_lits[288] = {
    // 0-143: 8 bits, 144-255: 9, 256-279: 7, 280-287: 8
    [0 ... 143] = 8, [144 ... 255] = 9, [256 ... 279] = 7, [280 ... 287] = 8,
};

uint8_t *dp_inflate(const uint8_t *in, size_t in_len, size_t *out_len) {
    DpInf s = {in, in_len, 0, 0, 0, NULL, 0, 0, 0};
    if (in_len > 2 && in[0] == 0x78 && ((in[0] << 8) | in[1]) % 31 == 0) s.in_pos = 2; // zlib
    for (;;) {
        int last = inf_bits(&s, 1);
        if (last < 0) break;
        int type = inf_bits(&s, 2);
        if (type < 0) break;
        if (type == 0) { // stored
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_len) { s.err = 1; break; }
            uint16_t n = (uint16_t)(s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8));
            uint16_t nn = (uint16_t)(s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8));
            s.in_pos += 4;
            if (n != (uint16_t)~nn || s.in_pos + n > s.in_len) { s.err = 1; break; }
            if (!inf_grow(&s, s.out_len + n)) { s.err = 1; break; }
            memcpy(s.out + s.out_len, s.in + s.in_pos, n);
            s.out_len += n;
            s.in_pos += n;
        } else if (type == 1 || type == 2) {
            DpHuff lit, dist;
            if (type == 1) {
                huff_build(&lit, fixed_lits, 288);
                uint8_t dl[30];
                memset(dl, 5, sizeof dl);
                huff_build(&dist, dl, 30);
            } else {
                static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                11, 4, 12, 3, 13, 2, 14, 1, 15};
                int hlit = inf_bits(&s, 5) + 257;
                int hdist = inf_bits(&s, 5) + 1;
                int hclen = inf_bits(&s, 4) + 4;
                if (hlit < 0 || hdist < 0 || hclen < 0) break;
                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; i++) {
                    int v = inf_bits(&s, 3);
                    if (v < 0) { s.err = 1; break; }
                    cl[ord[i]] = (uint8_t)v;
                }
                DpHuff ch;
                if (s.err || !huff_build(&ch, cl, 19)) { s.err = 1; break; }
                uint8_t lens[288 + 32] = {0};
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = huff_decode(&s, &ch);
                    if (sym < 0) break;
                    if (sym < 16) {
                        lens[n++] = (uint8_t)sym;
                    } else {
                        int rep = 0, v = 0;
                        if (sym == 16) {
                            if (!n) { s.err = 1; break; }
                            rep = 3 + inf_bits(&s, 2);
                            v = lens[n - 1];
                        } else if (sym == 17) {
                            rep = 3 + inf_bits(&s, 3);
                        } else {
                            rep = 11 + inf_bits(&s, 7);
                        }
                        if (n + rep > hlit + hdist) { s.err = 1; break; }
                        while (rep--) lens[n++] = (uint8_t)v;
                    }
                }
                if (s.err || !huff_build(&lit, lens, hlit) ||
                    !huff_build(&dist, lens + hlit, hdist)) {
                    s.err = 1;
                    break;
                }
            }
            if (!inf_block(&s, &lit, &dist)) break;
        } else {
            s.err = 1;
            break;
        }
        if (last) break;
    }
    if (s.err && s.out) {
        free(s.out);
        return NULL;
    }
    if (out_len) *out_len = s.out_len;
    return s.out;
}

// ---------------------------------------------------------------------------
// zip
// ---------------------------------------------------------------------------
int dp_zip_is(const uint8_t *in, size_t in_len) {
    return in_len > 4 && in[0] == 'P' && in[1] == 'K' && in[2] == 3 && in[3] == 4;
}

uint8_t *dp_zip_read(const uint8_t *in, size_t in_len, const char *needle, size_t *out_len,
                     char name[128]) {
    if (in_len < 22 || !dp_zip_is(in, in_len)) return NULL;
    // central directory: scan the tail for PK\x05\x06 (no comment assumed, but scan anyway)
    size_t eo = in_len - 22;
    for (;;) {
        if (in[eo] == 'P' && in[eo + 1] == 'K' && in[eo + 2] == 5 && in[eo + 3] == 6) break;
        if (!eo) return NULL;
        eo--;
    }
    uint16_t nent = (uint16_t)(in[eo + 10] | (in[eo + 11] << 8));
    uint32_t cd = in[eo + 16] | (in[eo + 17] << 8) | ((uint32_t)(in[eo + 18] | in[eo + 19] << 8) << 16);
    for (uint16_t e = 0; e < nent; e++) {
        if (cd + 46 > in_len || in[cd] != 'P' || in[cd + 1] != 'K' || in[cd + 2] != 1 ||
            in[cd + 3] != 2)
            return NULL;
        uint16_t method = (uint16_t)(in[cd + 10] | (in[cd + 11] << 8));
        uint32_t csize = in[cd + 20] | (in[cd + 21] << 8) |
                         ((uint32_t)(in[cd + 22] | in[cd + 23] << 8) << 16);
        uint16_t nlen = (uint16_t)(in[cd + 28] | (in[cd + 29] << 8));
        uint16_t elen = (uint16_t)(in[cd + 30] | (in[cd + 31] << 8));
        uint16_t clen = (uint16_t)(in[cd + 32] | (in[cd + 33] << 8));
        uint32_t lho = in[cd + 42] | (in[cd + 43] << 8) |
                       ((uint32_t)(in[cd + 44] | in[cd + 45] << 8) << 16);
        if (name && nlen < 128) {
            memcpy(name, in + cd + 46, nlen);
            name[nlen] = 0;
        }
        int match = !needle;
        if (needle) {
            char buf[128];
            for (uint16_t i = 0; i < nlen && i < sizeof buf - 1; i++) {
                char c = (char)in[cd + 46 + i];
                buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            buf[nlen < sizeof buf - 1 ? nlen : sizeof buf - 1] = 0;
            char want[128];
            size_t w = strlen(needle);
            if (w < sizeof want) {
                for (size_t i = 0; i < w; i++)
                    want[i] = (needle[i] >= 'A' && needle[i] <= 'Z') ? (char)(needle[i] + 32)
                                                                     : needle[i];
                want[w] = 0;
                match = strstr(buf, want) != NULL;
            }
        }
        cd += 46 + nlen + elen + clen;
        if (!match) continue;
        if (lho + 30 > in_len || in[lho] != 'P' || in[lho + 1] != 'K' || in[lho + 2] != 3 ||
            in[lho + 3] != 4)
            return NULL;
        uint16_t lnlen = (uint16_t)(in[lho + 26] | (in[lho + 27] << 8));
        uint16_t lelen = (uint16_t)(in[lho + 28] | (in[lho + 29] << 8));
        size_t data = lho + 30 + lnlen + lelen;
        if (data + csize > in_len) return NULL;
        if (method == 0) {
            uint8_t *out = malloc(csize + 1);
            if (!out) return NULL;
            memcpy(out, in + data, csize);
            out[csize] = 0;
            if (out_len) *out_len = csize;
            return out;
        }
        if (method == 8) return dp_inflate(in + data, csize, out_len);
        return NULL;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// ROM file reader: plain file, or the ROM entry of a .zip romset archive
// ---------------------------------------------------------------------------
uint8_t *dp_read_rom_file(const char *path, size_t *out_len, char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) snprintf(err, errsz, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        if (err) snprintf(err, errsz, "empty rom %s", path);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        if (err) snprintf(err, errsz, "short read on %s", path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (!dp_zip_is(buf, (size_t)sz)) {
        *out_len = (size_t)sz;
        return buf;
    }
    char name[128] = {0};
    size_t n = 0;
    // prefer a Game Boy entry (".gb" also matches ".gbc"), else the first entry
    uint8_t *rom = dp_zip_read(buf, (size_t)sz, ".gb", &n, name);
    if (!rom) rom = dp_zip_read(buf, (size_t)sz, NULL, &n, name);
    free(buf);
    if (!rom || !n) {
        free(rom);
        if (err) snprintf(err, errsz, "no usable entry in zip %s", path);
        return NULL;
    }
    *out_len = n;
    return rom;
}
