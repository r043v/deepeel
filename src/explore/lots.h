// Frontier lots for the non-GB platforms (spec §6): a disk file of deferred
// states (metadata + zlib-compressed machine snapshot), appended when the
// in-memory holding area is full or a wall stops the run, read back (FIFO
// cursor) when the queue runs dry or on --resume.  Header-only so every
// platform library carries its own copy without link conflicts.
#ifndef DEEPEEL_LOTS_H
#define DEEPEEL_LOTS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define DP_LOT_MAGIC 0x4c4c5044u /* 'DPLL' */
#define DP_LOT_META_MAX 256

typedef struct {
    FILE *f;
    uint64_t rd, wr;      // byte offsets: next record to read, end of file
    uint64_t written, read_back; // records
    uint64_t unread;             // records not yet read back
} DpLot;

// open (resume=1 keeps the previous content as the unread backlog)
static inline DpLot *dp_lot_open(const char *path, int resume) {
    DpLot *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->f = fopen(path, resume ? "r+b" : "wb+");
    if (!l->f && resume) l->f = fopen(path, "wb+");
    if (!l->f) { free(l); return NULL; }
    if (resume && fseek(l->f, 0, SEEK_END) == 0) {
        l->wr = (uint64_t)ftell(l->f);
        // count the backlog records by walking the headers
        uint64_t off = 0;
        uint32_t h[4];
        while (off + sizeof h <= l->wr && fseek(l->f, (long)off, SEEK_SET) == 0 &&
               fread(h, sizeof h, 1, l->f) == 1 && h[0] == DP_LOT_MAGIC) {
            off += sizeof h + h[1] + h[3];
            if (off > l->wr) break;
            l->unread++;
        }
        if (off != l->wr) { // torn tail: keep only the whole records
            fprintf(stderr, "deepeel: lot %s has a torn tail, %llu bytes ignored\n", path,
                    (unsigned long long)(l->wr - off));
            l->wr = off;
        }
    }
    return l;
}

// close: the unread backlog is compacted to the file start (nothing left: the file goes away)
static inline void dp_lot_close(DpLot *l, const char *path) {
    if (!l) return;
    if (l->unread == 0 || l->rd >= l->wr) {
        fclose(l->f);
        if (path) remove(path);
    } else {
        if (l->rd) {
            uint8_t buf[1 << 16];
            uint64_t from = l->rd, to = 0;
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
    free(l);
}

static inline uint64_t dp_lot_pending(const DpLot *l) { return l ? l->unread : 0; }

// record: magic, metalen, rawlen, zlen, meta, z
static inline int dp_lot_put(DpLot *l, const void *meta, uint32_t mlen, const void *blob, uint32_t len) {
    if (!l || mlen > DP_LOT_META_MAX) return 0;
    uLongf zl = compressBound(len);
    uint8_t *z = malloc(zl ? zl : 1);
    if (!z) return 0;
    if (compress2(z, &zl, blob, len, 1) != Z_OK) { free(z); return 0; }
    uint32_t h[4] = { DP_LOT_MAGIC, mlen, len, (uint32_t)zl };
    int ok = fseek(l->f, (long)l->wr, SEEK_SET) == 0 && fwrite(h, sizeof h, 1, l->f) == 1 &&
             (!mlen || fwrite(meta, 1, mlen, l->f) == mlen) && fwrite(z, 1, zl, l->f) == zl;
    free(z);
    if (!ok) return 0;
    l->wr += sizeof h + mlen + zl;
    l->written++;
    l->unread++;
    return 1;
}

// next unread record: meta copied out (cap DP_LOT_META_MAX), blob malloc'ed; 0 = none / corrupt
static inline int dp_lot_get(DpLot *l, void *meta, uint32_t *mlen, uint8_t **blob, uint32_t *len) {
    if (!l || l->rd >= l->wr) return 0;
    uint32_t h[4];
    if (fseek(l->f, (long)l->rd, SEEK_SET) || fread(h, sizeof h, 1, l->f) != 1 ||
        h[0] != DP_LOT_MAGIC || h[1] > DP_LOT_META_MAX || h[2] > (1u << 26)) {
        fprintf(stderr, "deepeel: lot corrupt at %llu, dropping the rest\n", (unsigned long long)l->rd);
        l->rd = l->wr;
        l->unread = 0;
        return 0;
    }
    uint8_t *z = malloc(h[3] ? h[3] : 1), *raw = malloc(h[2] ? h[2] : 1);
    uLongf rl = h[2];
    if (!z || !raw || (h[1] && fread(meta, 1, h[1], l->f) != h[1]) ||
        fread(z, 1, h[3], l->f) != h[3] || uncompress(raw, &rl, z, h[3]) != Z_OK || rl != h[2]) {
        free(z); free(raw);
        l->rd = l->wr;
        l->unread = 0;
        return 0;
    }
    free(z);
    l->rd += sizeof h + h[1] + h[3];
    l->read_back++;
    l->unread--;
    *mlen = h[1]; *blob = raw; *len = h[2];
    return 1;
}
#endif
