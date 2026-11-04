// frontier_disk.c: the disk side of the frontier — the overflow spool, the
// exact-state set persistence and the on-disk frontier lots (spec §6.3).
#include <zlib.h>
#include <sys/stat.h>
#include "explorer.h"
#include "explorer_priv.h"
#include "../core/dpmachine.h"
#include "tree.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// --- disk spool -------------------------------------------------------------------
// Overflow states survive: a full worklist under the mem-cap/q_max writes them
// to <state-dir>/spool.bin instead of dropping them, dp_explore_step feeds them
// back while the worklist runs low, and a flush before exit lets --resume
// continue exactly where the run stopped.  Record: magic 'DPSR', flags byte,
// u32 choice, u32 img length, image bytes.  One file, two cursors: records are
// appended at spool_wr and read back from spool_rd.
#define DP_SPOOL_REC 0x52535044ull // 'DPSR' little-endian: raw image (older spools)
#define DP_SPOOL_ZREC 0x5A535044ull // 'DPSZ': zlib image; hdr[2] = image size | clen << 32
// records are variable-size now: the byte length of the record starting at `at`
// (0: truncated or unknown)
static uint64_t spool_rec_len(FILE *f, uint64_t at, uint64_t end, uint64_t sb) {
    uint64_t h[3];
    if (at + sizeof h > end || fseek(f, (long)at, SEEK_SET) || fread(h, sizeof h, 1, f) != 1) return 0;
    uint64_t body = h[0] == DP_SPOOL_REC ? sb : (h[0] == DP_SPOOL_ZREC ? (h[2] >> 32) : 0);
    if (!body || at + sizeof h + body > end) return 0;
    return sizeof h + body;
}

// raw deflate (no zlib header, no adler32): the checksum alone was a fifth of the leader thread
// when every overflowing fork went to the spool.  Same call shape as compress2/uncompress.
static int zraw_compress(uint8_t *dst, uLongf *dlen, const uint8_t *src, uLong n, int level) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_RLE) != Z_OK) return Z_MEM_ERROR;
    z.next_in = (Bytef *)src; z.avail_in = (uInt)n;
    z.next_out = dst; z.avail_out = (uInt)*dlen;
    int r = deflate(&z, Z_FINISH);
    *dlen = z.total_out;
    deflateEnd(&z);
    return r == Z_STREAM_END ? Z_OK : Z_BUF_ERROR;
}
static int zraw_uncompress(uint8_t *dst, uLongf *dlen, const uint8_t *src, uLong n) {
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

int spool_write_rec(DpExplorer *ex, const uint8_t *img, int forced, int kind,
                           int from_reset, uint32_t choice) {
    DpSpool *sp = ex->spool;
    if (!sp) return 0;
    // machine images are mostly zeros and repeats: a fast zlib pass cuts the spool a lot
    uLongf clen = compressBound((uLong)ex->sb);
    uint8_t *cb = malloc(clen);
    uint64_t magic = DP_SPOOL_REC, body = ex->sb;
    const uint8_t *src = img;
    if (cb && zraw_compress(cb, &clen, img, (uLong)ex->sb, 1) == Z_OK && clen < ex->sb) {
        magic = DP_SPOOL_ZREC;
        body = clen;
        src = cb;
    }
    pthread_mutex_lock(&sp->mtx);
    if ((ex->cfg.spool_cap_bytes && sp->wr - sp->rd >= ex->cfg.spool_cap_bytes) ||
        (ex->cfg.spool_cap_mb && (sp->wr - sp->rd) / (1024 * 1024) >= ex->cfg.spool_cap_mb)) {
        pthread_mutex_unlock(&sp->mtx);
        free(cb);
        u32_add(&ex->res->spool_refused, 1);
        return 0;
    }
    uint64_t hdr[3] = { magic,
                        (uint64_t)(forced ? 1 : 0) | ((uint64_t)kind << 8) |
                            ((uint64_t)(from_reset ? 1 : 0) << 16) | ((uint64_t)choice << 32),
                        magic == DP_SPOOL_ZREC ? (ex->sb | (body << 32)) : ex->sb };
    int ok = (sp->wpos == sp->wr || !fseek(sp->f, (long)sp->wr, SEEK_SET)) &&
             fwrite(hdr, sizeof hdr, 1, sp->f) == 1 && fwrite(src, 1, body, sp->f) == body;
    free(cb);
    if (ok) {
        sp->wr += sizeof hdr + body;
        sp->wpos = sp->wr;
        u32_add(&ex->res->spool_writes, 1);
        ex->res->spool_bytes = sp->wr - sp->rd;
    } else {
        sp->wpos = UINT64_MAX;
        perror("deepeel spool");
    }
    pthread_mutex_unlock(&sp->mtx);
    return ok;
}

// feeds pending spool records back into the worklist (never the priority
// inbox: its seeds are retried by their own phases).  Stops when the spool is
// drained or the target holds no more room.  The exact-state dedup already
// contains these states (inserted at push time), so nothing is re-hashed.
void spool_feed(DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp) return;
    for (;;) {
        // lock order: pool before spool (queue_push holds the pool while it writes the spool)
        if (ex->sh) pthread_mutex_lock(&ex->sh->mtx);
        pthread_mutex_lock(&sp->mtx);
        if (sp->rd >= sp->wr) {
            pthread_mutex_unlock(&sp->mtx);
            if (ex->sh) pthread_mutex_unlock(&ex->sh->mtx);
            return;
        }
        // room checked under the pool lock when shared (the push must not overflow)
        int room = ex->sh ? (ex->sh->len < ex->sh->cap) : (ex->q_len < ex->q_cap);
        if (!room) {
            if (ex->sh) pthread_mutex_unlock(&ex->sh->mtx);
            pthread_mutex_unlock(&sp->mtx);
            return;
        }
        uint64_t hdr[3];
        sp->wpos = UINT64_MAX;
        if (fseek(sp->f, (long)sp->rd, SEEK_SET) ||
            fread(hdr, sizeof hdr, 1, sp->f) != 1 ||
            !((hdr[0] == DP_SPOOL_REC && hdr[2] == ex->sb) ||
              (hdr[0] == DP_SPOOL_ZREC && (hdr[2] & 0xFFFFFFFFull) == ex->sb && (hdr[2] >> 32) > 0 &&
               (hdr[2] >> 32) <= compressBound((uLong)ex->sb)))) {
            fprintf(stderr, "deepeel: spool corrupt at %llu, dropping the rest\n",
                    (unsigned long long)sp->rd);
            sp->rd = sp->wr;
            pthread_mutex_unlock(&sp->mtx);
            if (ex->sh) pthread_mutex_unlock(&ex->sh->mtx);
            return;
        }
        uint8_t *img = xrealloc(NULL, ex->sb);
        uint64_t body = hdr[0] == DP_SPOOL_ZREC ? (hdr[2] >> 32) : ex->sb;
        int rok = 0;
        if (img && hdr[0] == DP_SPOOL_REC) rok = fread(img, 1, ex->sb, sp->f) == ex->sb;
        else if (img) {
            uint8_t *cb = malloc(body);
            uLongf dl = (uLongf)ex->sb;
            rok = cb && fread(cb, 1, body, sp->f) == body && zraw_uncompress(img, &dl, cb, (uLong)body) == Z_OK &&
                  dl == ex->sb;
            free(cb);
        }
        if (!rok) {
            free(img);
            pthread_mutex_unlock(&sp->mtx);
            if (ex->sh) pthread_mutex_unlock(&ex->sh->mtx);
            return;
        }
        sp->rd += sizeof hdr + body;
        u32_add(&ex->res->spool_reads, 1);
        ex->res->spool_bytes = sp->wr - sp->rd;
        DpPathState p;
        memset(&p, 0, sizeof p);
        p.img = img;
        p.forced = (uint8_t)(hdr[1] & 1);
        p.kind = (uint8_t)(hdr[1] >> 8);
        p.from_reset = (uint8_t)((hdr[1] >> 16) & 1);
        p.choice = (uint32_t)(hdr[1] >> 32);
        if (ex->sh) { // the parallel drain: everything goes through the pool
            ex->sh->q[(ex->sh->head + ex->sh->len) % ex->sh->cap] = p;
            ex->sh->len++;
            pthread_mutex_unlock(&ex->sh->mtx);
            pthread_mutex_unlock(&sp->mtx);
        } else {
            pthread_mutex_unlock(&sp->mtx);
            ex->queue[(ex->q_head + ex->q_len) % ex->q_cap] = p;
            ex->q_len++;
        }
    }
}

// read-cursor sidecar: 'DPSC', image size, rd, wr (the wr it was taken at)
#define DP_SPOOL_CURSOR 0x43535044ull // 'DPSC' little-endian

static void spool_save_cursor(DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp || !sp->cursor_path) return;
    pthread_mutex_lock(&sp->mtx);
    uint64_t c[4] = { DP_SPOOL_CURSOR, ex->sb, sp->rd, sp->wr };
    pthread_mutex_unlock(&sp->mtx);
    FILE *f = fopen(sp->cursor_path, "wb");
    if (!f || fwrite(c, sizeof c, 1, f) != 1) perror("deepeel spool cursor");
    if (f) fclose(f);
}

int dp_explorer_spool_attach(DpExplorer *ex, const char *path, int resume) {
    DpSpool *sp = calloc(1, sizeof *sp);
    if (!sp) return 0;
    sp->f = fopen(path, resume ? "r+b" : "wb+");
    // --resume without a previous spool starts an empty one
    if (!sp->f && resume) sp->f = fopen(path, "wb+");
    if (!sp->f) { free(sp); return 0; }
    pthread_mutex_init(&sp->mtx, NULL);
    sp->wpos = UINT64_MAX;
    size_t pl = strlen(path);
    sp->cursor_path = xrealloc(NULL, pl + 4);
    memcpy(sp->cursor_path, path, pl);
    memcpy(sp->cursor_path + pl, ".rd", 4);
    ex->spool = sp;
    ex->spool_owned = 1;
    if (resume && fseek(sp->f, 0, SEEK_END) == 0) {
        long sz = ftell(sp->f);
        if (sz >= 0) {
            // only whole records count: a killed process may have truncated one
            uint64_t end = (uint64_t)sz, at = 0, rl;
            while ((rl = spool_rec_len(sp->f, at, end, ex->sb))) at += rl;
            sp->wr = at;
            // pending records start at the persisted read cursor; without one
            // (older spool, or a kill before the first flush) every record is
            // fed back again: re-running a state costs time, skipping one
            // loses it
            sp->rd = 0;
            uint64_t c[4];
            FILE *cf = fopen(sp->cursor_path, "rb");
            if (cf && fread(c, sizeof c, 1, cf) == 1 && c[0] == DP_SPOOL_CURSOR &&
                c[1] == ex->sb && c[2] <= sp->wr)
                sp->rd = c[2];
            else if (sp->wr)
                fprintf(stderr, "deepeel: spool read cursor missing, feeding back all records (%llu bytes)\n",
                        (unsigned long long)sp->wr);
            if (cf) fclose(cf);
            ex->res->spool_bytes = sp->wr - sp->rd;
        }
    } else {
        unlink(sp->cursor_path); // a fresh spool: a stale cursor would skip records
    }
    return 1;
}

// parallel drain: the worker borrows the leader's spool, exact dedup and tree
void dp_explorer_share(DpExplorer *worker, DpExplorer *leader) {
    worker->spool = leader->spool;
    worker->spool_owned = 0;
    worker->exactp = &leader->exact;
    worker->exact_mtx = leader->exact_mtx;
    worker->tree_mtx = &leader->tmtx;
    worker->sh = leader->sh;
}

uint64_t dp_explorer_spool_pending(const DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp) return 0;
    pthread_mutex_lock(&sp->mtx);
    uint64_t p = sp->wr - sp->rd;
    pthread_mutex_unlock(&sp->mtx);
    return p;
}

// pours the remaining worklist into the spool (a stop or a normal exit): the
// next --resume picks the states up; returns states still pending
uint64_t dp_explorer_spool_flush(DpExplorer *ex) {
    if (!ex->spool) return 0;
    // a parallel drain may have left states in the shared pool
    if (ex->sh) {
        pthread_mutex_lock(&ex->sh->mtx);
        for (uint32_t i = 0; i < ex->sh->len; i++) {
            DpPathState *p = &ex->sh->q[(ex->sh->head + i) % ex->sh->cap];
            if (p->img) {
                if (!spool_write_rec(ex, p->img, p->forced, p->kind, p->from_reset, p->choice)) {
                if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, DP_R_PATHS)) {
                    memset(p, 0, sizeof *p);
                    continue;
                }
                ex->res->queue_overflows++; // frontier full too: final and counted
            }
                free(p->img);
                memset(p, 0, sizeof *p);
            }
        }
        ex->sh->len = 0;
        ex->sh->head = 0;
        pthread_mutex_unlock(&ex->sh->mtx);
    }
    for (uint32_t i = 0; i < ex->q_len; i++) {
        DpPathState *p = &ex->queue[(ex->q_head + i) % ex->q_cap];
        if (p->img) {
            if (!spool_write_rec(ex, p->img, p->forced, p->kind, p->from_reset, p->choice)) {
                if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, DP_R_PATHS)) {
                    memset(p, 0, sizeof *p);
                    continue;
                }
                ex->res->queue_overflows++; // frontier full too: final and counted
            }
            free(p->img);
            memset(p, 0, sizeof *p);
        }
    }
    ex->q_len = 0;
    ex->q_head = 0;
    for (uint32_t i = 0; i < ex->pq_len; i++) {
        DpPathState *p = &ex->pqueue[(ex->pq_head + i) % ex->pq_cap];
        if (p->img) {
            if (!spool_write_rec(ex, p->img, p->forced, p->kind, p->from_reset, p->choice)) {
                if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, DP_R_PATHS)) {
                    memset(p, 0, sizeof *p);
                    continue;
                }
                ex->res->queue_overflows++; // frontier full too: final and counted
            }
            free(p->img);
            memset(p, 0, sizeof *p);
        }
    }
    ex->pq_len = 0;
    ex->pq_head = 0;
    fflush(ex->spool->f);
    spool_save_cursor(ex);
    return dp_explorer_spool_pending(ex);
}

// --- exact-set persistence -----------------------------------------------------
int dp_explorer_exact_save(const DpExplorer *ex, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const DpHashSet *s = ex->exactp;
    const uint32_t ver = 1;
    fwrite("DPEX", 1, 4, f);
    fwrite(&ver, sizeof ver, 1, f);
    fwrite(&s->cnt, sizeof s->cnt, 1, f);
    for (size_t i = 0; i < s->cap; i++)
        if (s->used[i]) fwrite(&s->keys[i], sizeof(uint64_t), 1, f);
    int ok = !ferror(f);
    fclose(f);
    return ok;
}

int dp_explorer_exact_load(DpExplorer *ex, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    uint32_t ver;
    size_t cnt = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "DPEX", 4) || fread(&ver, sizeof ver, 1, f) != 1 ||
        ver != 1 || fread(&cnt, sizeof cnt, 1, f) != 1) {
        fclose(f);
        return 0;
    }
    // room for the saved keys plus the run's own discoveries
    DpHashSet *s = ex->exactp;
    size_t need = cnt * 2 + 65536;
    if (need > s->cap_abs) {
        dp_set_free(s);
        dp_set_init(s, need);
    }
    size_t n = 0;
    for (uint64_t k; n < cnt && fread(&k, sizeof k, 1, f) == 1; n++)
        if (dp_set_insert(s, k) != 1) break;
    fclose(f);
    fprintf(stderr, "deepeel: resumed dedup (%zu states known)\n", n);
    return n == cnt;
}


// ---- on-disk frontier (docs/COUVERTURE.fr.md §6.3) ---------------------------
// <dir>/frontier/frontier.idx                      text index, one line per lot
// <dir>/frontier/b<bank>-s<sec>-<reason>-<seq>.dpf one lot: 'DPFB', version,
//   platform, image size, count, base image (zlib), then per record 8 header
//   bytes (tier, forced, kind, from_reset, reason) + zlib length + the zlib XOR
//   delta against the base.  A lot is read sequentially; the byte offset read so
//   far is persisted in the index, so a resume never replays nor skips a record.
// <dir>/frontier/seeds.bin                         seeds held back by the caps
#define DP_LOT_MAX_BYTES (64u << 20)
#define DP_LOT_MAGIC 0x42465044u /* 'DPFB' */

static void frontier_path(const DpExplorer *ex, char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/frontier/%s", ex->fdir, name);
}

static void idx_write(DpExplorer *ex) {
    char p[700];
    frontier_path(ex, p, sizeof p, "frontier.idx");
    if (!ex->nlots) {
        unlink(p);
        return;
    }
    FILE *f = fopen(p, "w");
    if (!f) { perror("deepeel frontier.idx"); return; }
    fprintf(f, "deepeel-frontier 1\nplatform gb\nimage_bytes %u\n", (unsigned)ex->sb);
    for (uint32_t i = 0; i < ex->nlots; i++) {
        const DpLot *l = &ex->lots[i];
        fprintf(f, "lot %s count=%u off=%llu reason=%s bank=%d sec=%d t1=%u t2=%u t3=%u read=%u\n", l->name,
                l->count, (unsigned long long)l->off, dp_reason_name(l->reason), l->bank, l->sec,
                l->tier_n[1], l->tier_n[2], l->tier_n[3], l->read);
    }
    fclose(f);
}

static void idx_read(DpExplorer *ex) {
    char p[700], line[400];
    frontier_path(ex, p, sizeof p, "frontier.idx");
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        DpLot l;
        memset(&l, 0, sizeof l);
        char name[96], rs[32];
        unsigned long long off;
        if (sscanf(line, "lot %95s count=%u off=%llu reason=%31s bank=%d sec=%d t1=%u t2=%u t3=%u read=%u", name,
                   &l.count, &off, rs, &l.bank, &l.sec, &l.tier_n[1], &l.tier_n[2], &l.tier_n[3], &l.read) == 10) {
            snprintf(l.name, sizeof l.name, "%s", name);
            l.off = off;
            l.reason = dp_reason_from_name(rs);
            if (!l.reason) continue;
            if (ex->nlots >= ex->lotcap) {
                ex->lotcap = ex->lotcap ? ex->lotcap * 2 : 16;
                ex->lots = xrealloc(ex->lots, sizeof *ex->lots * ex->lotcap);
            }
            ex->lots[ex->nlots++] = l;
            unsigned sq = 0;
            const char *dash = strrchr(name, '-');
            if (dash && sscanf(dash + 1, "%u", &sq) == 1 && sq >= ex->lot_seq) ex->lot_seq = sq + 1;
        }
    }
    fclose(f);
}

static void lot_remove(DpExplorer *ex, uint32_t i) {
    char p[700];
    frontier_path(ex, p, sizeof p, ex->lots[i].name);
    unlink(p);
    memmove(&ex->lots[i], &ex->lots[i + 1], sizeof *ex->lots * (ex->nlots - i - 1));
    ex->nlots--;
}

// records still on disk (unread)
uint64_t dp_explorer_frontier_disk(const DpExplorer *ex) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < ex->nlots; i++) n += ex->lots[i].count - ex->lots[i].read;
    return n;
}

int reason_eligible(const DpExplorer *ex, int reason) {
    return dp_reason_eligible(reason, ex->defer_all);
}

// reads the next records of the cheapest eligible lot into the holding area
int frontier_refill(DpExplorer *ex) {
    if (!ex->fdir || !ex->nlots) return 0;
    int best = -1;
    for (uint32_t i = 0; i < ex->nlots; i++) {
        const DpLot *l = &ex->lots[i];
        if (l->read >= l->count || !reason_eligible(ex, l->reason)) continue;
        if (best < 0 || l->reason < ex->lots[best].reason) best = (int)i;
    }
    if (best < 0) return 0;
    DpLot *l = &ex->lots[best];
    char p[700];
    frontier_path(ex, p, sizeof p, l->name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "deepeel: frontier lot %s missing, dropped\n", l->name); lot_remove(ex, (uint32_t)best); return 1; }
    uint32_t h[5];
    uint8_t *base = xrealloc(NULL, ex->sb);
    int ok = fread(h, sizeof h, 1, f) == 1 && h[0] == DP_LOT_MAGIC && h[1] == 1 && h[3] == (uint32_t)ex->sb;
    uint32_t bl = 0;
    uint8_t *z = NULL;
    if (ok) {
        ok = fread(&bl, 4, 1, f) == 1 && bl <= 4 * (uint32_t)ex->sb + 1024;
        if (ok) { z = xrealloc(NULL, bl); uLongf o = (uLongf)ex->sb;
                  ok = fread(z, 1, bl, f) == bl && zraw_uncompress(base, &o, z, bl) == Z_OK && o == (uLongf)ex->sb; }
        free(z);
    }
    if (!ok) {
        fprintf(stderr, "deepeel: frontier lot %s unusable, dropped (%u records)\n", l->name, l->count - l->read);
        ex->res->defer_dropped += l->count - l->read;
        fclose(f); free(base); lot_remove(ex, (uint32_t)best);
        return 1;
    }
    if (l->off) fseek(f, (long)l->off, SEEK_SET);
    int got = 0;
    while (l->read < l->count && ex->defer_len < DP_DEFER_CAP) {
        uint8_t hd[8];
        uint32_t zl;
        if (fread(hd, 8, 1, f) != 1 || fread(&zl, 4, 1, f) != 1 || zl > 4 * (uint32_t)ex->sb + 1024) { l->read = l->count; break; }
        z = xrealloc(NULL, zl);
        uint8_t *img = xrealloc(NULL, ex->sb);
        uLongf o = (uLongf)ex->sb;
        if (fread(z, 1, zl, f) != zl || zraw_uncompress(img, &o, z, zl) != Z_OK || o != (uLongf)ex->sb) {
            free(z); free(img); l->read = l->count; break;
        }
        free(z);
        for (uint32_t i = 0; i < (uint32_t)ex->sb; i++) img[i] ^= base[i];
        if (ex->defer_len >= ex->defer_cap) {
            uint32_t nc = ex->defer_cap ? ex->defer_cap * 2 : 64;
            ex->defer = xrealloc(ex->defer, sizeof *ex->defer * nc);
            ex->defer_cap = nc;
        }
        ex->defer[ex->defer_len++] = (DpDeferred){ { img, hd[1], hd[2], hd[3], 0 }, hd[0], hd[4] };
        ex->defer_by_reason[hd[4] < DP_R_COUNT ? hd[4] : 0]++;
        l->read++;
        l->off = (uint64_t)ftell(f);
        got++;
    }
    fclose(f);
    free(base);
    if (l->read >= l->count) lot_remove(ex, (uint32_t)best);
    idx_write(ex);
    return got ? got : 1;
}

typedef struct { uint32_t i; int reason, bank, sec; } LotKey;
static int lotkey_cmp(const void *a, const void *b) {
    const LotKey *x = a, *y = b;
    if (x->reason != y->reason) return x->reason - y->reason;
    if (x->bank != y->bank) return x->bank - y->bank;
    if (x->sec != y->sec) return x->sec - y->sec;
    return (int)x->i - (int)y->i;
}

static int section_of(const DpResults *res, int bank, uint16_t pc) {
    for (int i = 0; i < res->section_count; i++) {
        const DpSection *s = &res->sections[i];
        if (s->bank == bank && s->lo <= pc && pc <= s->hi) return (int)s->id;
    }
    return 0;
}

// the states still in the worklists at a stop (a path wall) join the frontier
static void adopt_queues(DpExplorer *ex) {
    DpPathState *q[2] = { ex->queue, ex->pqueue };
    uint32_t cap[2] = { ex->q_cap, ex->pq_cap }, len[2] = { ex->q_len, ex->pq_len }, head[2] = { ex->q_head, ex->pq_head };
    for (int w = 0; w < 2; w++) {
        for (uint32_t i = 0; i < len[w]; i++) {
            DpPathState *p = &q[w][(head[w] + i) % cap[w]];
            if (!p->img) continue;
            int save_tier = ex->cur_tier;
            ex->cur_tier = 0; // never refused by the tier cap: a parked path, not a retry
            if (defer_push(ex, p->img, p->forced, p->kind, p->from_reset, p->choice, w ? DP_R_PRIORITY : DP_R_PATHS))
                p->img = NULL;
            ex->cur_tier = (uint8_t)save_tier;
            if (p->img) { ex->res->defer_dropped++; free(p->img); p->img = NULL; }
        }
        if (w) ex->pq_len = 0; else ex->q_len = 0;
    }
}

// writes the whole holding area as lots (one per bank x section x reason, closed at 64 MiB)
// Converts the records of an old state spool ('DPSR') into frontier lots
// (reason paths): the spool is drained into the holding area, which is flushed
// to .dpf lots every DP_DEFER_CAP records. Returns the number converted.
uint64_t dp_explorer_spool_convert(DpExplorer *ex) {
    DpSpool *sp = ex->spool;
    if (!sp || !ex->fdir) return 0;
    uint64_t n = 0;
    for (;;) {
        while (sp->rd < sp->wr && ex->defer_len < DP_DEFER_CAP) {
            uint64_t hdr[3];
            sp->wpos = UINT64_MAX;
            if (fseek(sp->f, (long)sp->rd, SEEK_SET) || fread(hdr, sizeof hdr, 1, sp->f) != 1 ||
                hdr[0] != DP_SPOOL_REC || hdr[2] != ex->sb) {
                fprintf(stderr, "deepeel: spool corrupt at %llu, conversion stops\n", (unsigned long long)sp->rd);
                sp->rd = sp->wr;
                break;
            }
            uint8_t *img = xrealloc(NULL, ex->sb);
            if (fread(img, 1, ex->sb, sp->f) != ex->sb) { free(img); sp->rd = sp->wr; break; }
            sp->rd += sizeof hdr + ex->sb;
            if (ex->defer_len >= ex->defer_cap) {
                uint32_t nc = ex->defer_cap ? ex->defer_cap * 2 : 64;
                ex->defer = xrealloc(ex->defer, sizeof *ex->defer * nc);
                ex->defer_cap = nc;
            }
            ex->defer[ex->defer_len++] = (DpDeferred){
                { img, (uint8_t)(hdr[1] & 1), (uint8_t)(hdr[1] >> 8), (uint8_t)((hdr[1] >> 16) & 1),
                  (uint32_t)(hdr[1] >> 32) },
                1, DP_R_PATHS };
            n++;
        }
        if (!ex->defer_len) break;
        dp_explorer_frontier_save(ex, ex->fdir);
    }
    ex->res->spool_bytes = 0;
    spool_save_cursor(ex);
    return n;
}

int dp_explorer_frontier_save(DpExplorer *ex, const char *dir) {
    if (!ex->fdir) { ex->fdir = strdup(dir); }
    char p[700];
    mkdir(dir, 0777); // the state directory itself may not exist yet
    snprintf(p, sizeof p, "%s/frontier", dir);
    mkdir(p, 0777);
    adopt_queues(ex);
    int saved = 0;
    if (ex->defer_len) {
        LotKey *k = xrealloc(NULL, sizeof *k * ex->defer_len);
        for (uint32_t i = 0; i < ex->defer_len; i++) {
            uint16_t pc = image_pc(ex->defer[i].p.img);
            int bank = pc < 0x4000 ? 0 : (int)((const gbl_cpu_t *)ex->defer[i].p.img)->rom_bank;
            k[i] = (LotKey){ i, ex->defer[i].reason, bank, section_of(ex->res, bank, pc) };
        }
        qsort(k, ex->defer_len, sizeof *k, lotkey_cmp);
        uLongf cap = compressBound((uLong)ex->sb);
        uint8_t *z = xrealloc(NULL, cap), *d = xrealloc(NULL, ex->sb);
        uint32_t a = 0;
        while (a < ex->defer_len) {
            uint32_t b = a;
            while (b < ex->defer_len && k[b].reason == k[a].reason && k[b].bank == k[a].bank && k[b].sec == k[a].sec) b++;
            // [a,b) is one group: cut it into lots
            for (uint32_t s = a; s < b;) {
                DpLot l;
                memset(&l, 0, sizeof l);
                l.reason = k[a].reason; l.bank = k[a].bank; l.sec = k[a].sec;
                snprintf(l.name, sizeof l.name, "b%d-s%d-%s-%u.dpf", l.bank, l.sec, dp_reason_name(l.reason), ex->lot_seq++);
                char lp[700];
                frontier_path(ex, lp, sizeof lp, l.name);
                FILE *f = fopen(lp, "wb");
                if (!f) { perror("deepeel frontier"); ex->res->defer_dropped += b - s; break; }
                const uint8_t *base = ex->defer[k[s].i].p.img;
                uLongf zl = cap;
                zraw_compress(z, &zl, base, (uLong)ex->sb, 6);
                uint32_t hdr[5] = { DP_LOT_MAGIC, 1, 1 /* gb */, (uint32_t)ex->sb, 0 };
                fwrite(hdr, sizeof hdr, 1, f);
                uint32_t bl = (uint32_t)zl;
                fwrite(&bl, 4, 1, f); fwrite(z, 1, zl, f);
                uint64_t bytes = zl;
                uint32_t n = 0;
                while (s < b && bytes < DP_LOT_MAX_BYTES) {
                    DpDeferred *r = &ex->defer[k[s].i];
                    if (ex->cfg.frontier_cap_mb && ex->fbytes >> 20 >= ex->cfg.frontier_cap_mb) {
                        // --frontier-cap-gb reached: the loss is final, counted per reason
                        ex->res->defer_final[r->reason]++;
                        ex->res->defer_dropped++;
                        s++;
                        continue;
                    }
                    for (uint32_t j = 0; j < (uint32_t)ex->sb; j++) d[j] = r->p.img[j] ^ base[j];
                    zl = cap;
                    if (zraw_compress(z, &zl, d, (uLong)ex->sb, 6) == Z_OK) {
                        uint8_t hd[8] = { r->tier, r->p.forced, r->p.kind, r->p.from_reset, r->reason, 0, 0, 0 };
                        uint32_t l32 = (uint32_t)zl;
                        fwrite(hd, 8, 1, f); fwrite(&l32, 4, 1, f); fwrite(z, 1, zl, f);
                        bytes += 12 + zl;
                        ex->fbytes += 12 + zl;
                        l.tier_n[r->tier <= DP_TIER_MAX ? r->tier : DP_TIER_MAX]++;
                        n++;
                    } else {
                        ex->res->defer_dropped++;
                    }
                    s++;
                }
                fseek(f, 16, SEEK_SET);
                fwrite(&n, 4, 1, f);
                fclose(f);
                // the offset of the first record (after header + base)
                l.count = n;
                l.off = 0; // 0 = start at the first record
                if (ex->nlots >= ex->lotcap) {
                    ex->lotcap = ex->lotcap ? ex->lotcap * 2 : 16;
                    ex->lots = xrealloc(ex->lots, sizeof *ex->lots * ex->lotcap);
                }
                ex->lots[ex->nlots++] = l;
                saved += (int)n;
            }
            a = b;
        }
        free(k); free(z); free(d);
        for (uint32_t i = 0; i < ex->defer_len; i++) free(ex->defer[i].p.img);
        ex->defer_len = 0;
        memset(ex->defer_by_reason, 0, sizeof ex->defer_by_reason);
    }
    idx_write(ex);
    // seeds held back by the caps
    char sp[700];
    frontier_path(ex, sp, sizeof sp, "seeds.bin");
    if (ex->pseed_len) {
        FILE *f = fopen(sp, "wb");
        if (f) { fwrite(ex->pseed, sizeof *ex->pseed, ex->pseed_len, f); fclose(f); }
        saved += (int)ex->pseed_len;
        ex->res->seeds_pending_disk = ex->pseed_len;
        ex->pseed_len = 0;
    } else {
        unlink(sp);
    }
    ex->res->defer_on_disk = dp_explorer_frontier_disk(ex);
    return saved;
}

// attaches the frontier directory: reads the index and loads the first records
int dp_explorer_frontier_load(DpExplorer *ex, const char *dir) {
    free(ex->fdir);
    ex->fdir = strdup(dir);
    ex->nlots = 0;
    idx_read(ex);
    char sp[700];
    frontier_path(ex, sp, sizeof sp, "seeds.bin");
    FILE *f = fopen(sp, "rb");
    if (f) {
        DpPendingSeed s;
        while (fread(&s, sizeof s, 1, f) == 1) pseed_add(ex, s.bank, s.pc, s.kind, s.forced);
        fclose(f);
        unlink(sp);
        ex->res->seeds_deferred -= ex->pseed_len; // reloaded, not new
    }
    uint32_t before = ex->defer_len;
    while (ex->defer_len < DP_DEFER_CAP && frontier_refill(ex)) {
        if (!dp_explorer_frontier_disk(ex)) break;
        int more = 0;
        for (uint32_t i = 0; i < ex->nlots; i++)
            if (ex->lots[i].read < ex->lots[i].count && reason_eligible(ex, ex->lots[i].reason)) more = 1;
        if (!more) break;
    }
    return (int)(ex->defer_len - before) + (int)ex->pseed_len;
}

