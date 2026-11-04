// Frontier lots for the non-GB platforms (spec §6.3), same family as the GB
// .dpf lots: header 'DPFB' + version + platform tag + image size + a zlib base
// image (the first state written), then records holding a zlib XOR delta
// against that base, their metadata and a read position.  Appended when the
// in-memory holding area is full or a wall stops the run, read back (FIFO)
// when the queue runs dry or on resume.  Header-only so every platform
// library carries its own copy without link conflicts.
#ifndef DEEPEEL_LOTS_H
#define DEEPEEL_LOTS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define DP_LF_MAGIC 0x42465044u  /* 'DPFB' */
#define DP_LF_REC   0x4c4c5044u  /* 'DPLL' record */
#define DP_LF_VERSION 3
#define DP_LF_META_MAX 256

typedef struct {
    FILE *f;
    char plat[4];
    uint64_t hdr_end;           // first record offset (0 until the header exists)
    uint32_t base_len;          // image size of this lot (0 until the header exists)
    uint8_t *base;              // base image (raw)
    uint64_t rd, wr;            // byte offsets: next record to read, end of file
    uint64_t written, read_back, unread; // records
} DpLotFile;

// raw deflate (no zlib header, no adler32) for the records: the checksum was a sixth of the run when
// every fork went to the spool (Castle of Illusion --unbounded, perf)
static inline int dp_lf_zraw(uint8_t *dst, uLongf *dlen, const uint8_t *src, uLong n) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, 1, Z_DEFLATED, -15, 8, Z_RLE) != Z_OK) return Z_MEM_ERROR;
    z.next_in = (Bytef *)src; z.avail_in = (uInt)n;
    z.next_out = dst; z.avail_out = (uInt)*dlen;
    int r = deflate(&z, Z_FINISH);
    *dlen = z.total_out;
    deflateEnd(&z);
    return r == Z_STREAM_END ? Z_OK : Z_BUF_ERROR;
}
static inline int dp_lf_unraw(uint8_t *dst, uLongf *dlen, const uint8_t *src, uLong n) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return Z_MEM_ERROR;
    z.next_in = (Bytef *)src; z.avail_in = (uInt)n;
    z.next_out = dst; z.avail_out = (uInt)*dlen;
    int r = inflate(&z, Z_FINISH);
    *dlen = z.total_out;
    inflateEnd(&z);
    return r == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

// header: magic, version, platform(4), image size, zlen, then the zlib base image
static inline int dp_lf_write_header(DpLotFile *l, const uint8_t *img, uint32_t len) {
    uLongf zl = compressBound(len);
    uint8_t *z = malloc(zl ? zl : 1);
    if (!z) return 0;
    if (compress2(z, &zl, img, len, 1) != Z_OK) { free(z); return 0; }
    uint32_t h[4] = { DP_LF_MAGIC, DP_LF_VERSION, len, (uint32_t)zl };
    int ok = fseek(l->f, 0, SEEK_SET) == 0 && fwrite(h, sizeof h, 1, l->f) == 1 &&
             fwrite(l->plat, 1, 4, l->f) == 4 && fwrite(z, 1, zl, l->f) == zl;
    free(z);
    if (!ok) return 0;
    l->base = malloc(len ? len : 1);
    if (!l->base) return 0;
    memcpy(l->base, img, len);
    l->base_len = len;
    l->hdr_end = l->rd = l->wr = sizeof h + 4 + zl;
    return 1;
}

// open: plat = 3-letter platform tag; resume=1 keeps the previous records as the unread backlog
static inline DpLotFile *dp_lf_open(const char *path, const char *plat, int resume) {
    DpLotFile *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    strncpy(l->plat, plat, 3);
    l->f = fopen(path, resume ? "r+b" : "wb+");
    if (!l->f && resume) l->f = fopen(path, "wb+");
    if (!l->f) { free(l); return NULL; }
    if (resume && fseek(l->f, 0, SEEK_END) == 0 && ftell(l->f) > 0) {
        uint64_t end = (uint64_t)ftell(l->f);
        uint32_t h[4];
        char pl[4];
        int good = fseek(l->f, 0, SEEK_SET) == 0 && fread(h, sizeof h, 1, l->f) == 1 && h[0] == DP_LF_MAGIC &&
                   h[1] == DP_LF_VERSION && fread(pl, 1, 4, l->f) == 4 && !memcmp(pl, l->plat, 4) && h[2] <= (1u << 26);
        uint8_t *z = NULL;
        if (good) {
            z = malloc(h[3] ? h[3] : 1);
            l->base = malloc(h[2] ? h[2] : 1);
            uLongf rl = h[2];
            good = z && l->base && fread(z, 1, h[3], l->f) == h[3] && uncompress(l->base, &rl, z, h[3]) == Z_OK && rl == h[2];
        }
        free(z);
        if (!good) { // not a lot of this platform: it is replaced, never silently read as one
            fprintf(stderr, "deepeel: %s is not a %s lot, starting a new one\n", path, l->plat);
            free(l->base); l->base = NULL;
            freopen(path, "wb+", l->f);
            return l;
        }
        l->base_len = h[2];
        l->hdr_end = l->rd = sizeof h + 4 + h[3];
        l->wr = l->hdr_end;
        // count the backlog records by walking the headers
        uint32_t rh[4];
        uint64_t off = l->hdr_end;
        while (off + sizeof rh <= end && fseek(l->f, (long)off, SEEK_SET) == 0 &&
               fread(rh, sizeof rh, 1, l->f) == 1 && rh[0] == DP_LF_REC && off + sizeof rh + rh[1] + rh[3] <= end) {
            off += sizeof rh + rh[1] + rh[3];
            l->unread++;
        }
        if (off != end)
            fprintf(stderr, "deepeel: lot %s has a torn tail, %llu bytes ignored\n", path, (unsigned long long)(end - off));
        l->wr = off;
    }
    return l;
}

// compaction: unread records move to the file start (nothing left: the file goes away)
static inline void dp_lf_close(DpLotFile *l, const char *path) {
    if (!l) return;
    if (l->unread == 0 || !l->hdr_end) {
        fclose(l->f);
        if (path) remove(path);
    } else {
        if (l->rd > l->hdr_end) {
            uint8_t buf[1 << 16];
            uint64_t from = l->rd, to = l->hdr_end;
            while (from < l->wr) {
                size_t n = l->wr - from < sizeof buf ? (size_t)(l->wr - from) : sizeof buf;
                if (fseek(l->f, (long)from, SEEK_SET) || fread(buf, 1, n, l->f) != n) break;
                if (fseek(l->f, (long)to, SEEK_SET) || fwrite(buf, 1, n, l->f) != n) break;
                from += n; to += n;
            }
            fflush(l->f);
            if (from >= l->wr && ftruncate(fileno(l->f), (off_t)to)) perror("deepeel lot truncate");
        }
        fclose(l->f);
    }
    free(l->base);
    free(l);
}

static inline uint64_t dp_lf_pending(const DpLotFile *l) { return l ? l->unread : 0; }

// record: DP_LF_REC, metalen, rawlen, zlen, meta, zlib(XOR delta against the base)
static inline int dp_lf_put(DpLotFile *l, const void *meta, uint32_t mlen, const void *blob, uint32_t len) {
    if (!l || mlen > DP_LF_META_MAX) return 0;
    if (!l->hdr_end && !dp_lf_write_header(l, blob, len)) return 0; // the first state is the base
    if (len != l->base_len) return 0;                               // one image size per lot
    uint8_t *d = malloc(len ? len : 1);
    if (!d) return 0;
    const uint8_t *b = blob;
    for (uint32_t i = 0; i < len; i++) d[i] = b[i] ^ l->base[i];
    uLongf zl = compressBound(len);
    uint8_t *z = malloc(zl ? zl : 1);
    if (!z) { free(d); return 0; }
    int cr = dp_lf_zraw(z, &zl, d, len);
    free(d);
    if (cr != Z_OK) { free(z); return 0; }
    uint32_t h[4] = { DP_LF_REC, mlen, len, (uint32_t)zl };
    int ok = fseek(l->f, (long)l->wr, SEEK_SET) == 0 && fwrite(h, sizeof h, 1, l->f) == 1 &&
             (!mlen || fwrite(meta, 1, mlen, l->f) == mlen) && fwrite(z, 1, zl, l->f) == zl;
    free(z);
    if (!ok) return 0;
    l->wr += sizeof h + mlen + zl;
    l->written++;
    l->unread++;
    return 1;
}

// next unread record: meta copied out (cap DP_LF_META_MAX), blob malloc'ed; 0 = none / corrupt
static inline int dp_lf_get(DpLotFile *l, void *meta, uint32_t *mlen, uint8_t **blob, uint32_t *len) {
    if (!l || !l->unread || l->rd >= l->wr) return 0;
    uint32_t h[4];
    if (fseek(l->f, (long)l->rd, SEEK_SET) || fread(h, sizeof h, 1, l->f) != 1 ||
        h[0] != DP_LF_REC || h[1] > DP_LF_META_MAX || h[2] != l->base_len) {
        fprintf(stderr, "deepeel: lot corrupt at %llu, dropping the rest\n", (unsigned long long)l->rd);
        l->rd = l->wr; l->unread = 0;
        return 0;
    }
    uint8_t *z = malloc(h[3] ? h[3] : 1), *raw = malloc(h[2] ? h[2] : 1);
    uLongf rl = h[2];
    if (!z || !raw || (h[1] && fread(meta, 1, h[1], l->f) != h[1]) ||
        fread(z, 1, h[3], l->f) != h[3] || dp_lf_unraw(raw, &rl, z, h[3]) != Z_OK || rl != h[2]) {
        free(z); free(raw);
        l->rd = l->wr; l->unread = 0;
        return 0;
    }
    free(z);
    for (uint32_t i = 0; i < h[2]; i++) raw[i] ^= l->base[i];
    l->rd += sizeof h + h[1] + h[3];
    l->read_back++;
    l->unread--;
    *mlen = h[1]; *blob = raw; *len = h[2];
    return 1;
}
#endif
