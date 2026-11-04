// streams.c: S6 on the Game Boy — decoder replay over pointer tables.
// A routine called on the natural path whose first ROM data read is at an
// address one of HL/DE/BC held at the call is a stream consumer.  Two
// observed starts at a constant stride locate a table of ROM pointers; the
// call is then replayed (from the state image taken before it) with the
// register set to every other entry of the table, through the fork queue.
#include "explorer.h"
#include "explorer_priv.h"
#include "../core/dpmachine.h"
#include <stdlib.h>
#include <string.h>

#define DEC_CAP 64
#define DEC_STARTS 16
#define FRAME_CAP 16
#define CAND_CAP 2048   // replays per decoder when bounded

typedef struct {
    uint32_t key;               // call target (rom index)
    uint8_t reg, nstarts, replayed; // replayed: nstarts at the last replay
    uint16_t bank;
    uint16_t starts[DEC_STARTS];
    uint8_t nx, xreplayed;       // cross-bank starts (any bank), replayed: nx at the last cross-bank replay
    struct { uint16_t bank, addr; } xs[DEC_STARTS];
    uint8_t *snap;              // machine image before the call
} Dec;

struct DpStreams {
    Dec dec[DEC_CAP];
    unsigned ndec;
    struct { uint32_t key; uint16_t sp, reg[3]; uint8_t open; } fr[FRAME_CAP];
    unsigned nfr;
    unsigned tick;
};

static struct DpStreams *get(DpExplorer *ex) {
    if (!ex->stm) ex->stm = calloc(1, sizeof *ex->stm);
    return ex->stm;
}

void stream_free(DpExplorer *ex) {
    if (!ex->stm) return;
    for (unsigned i = 0; i < ex->stm->ndec; i++) free(ex->stm->dec[i].snap);
    free(ex->stm);
    ex->stm = NULL;
}

// a CALL (not conditional) is about to run on a natural path
void stream_call(DpExplorer *ex, uint32_t tkey) {
    struct DpStreams *s = get(ex);
    if (!s) return;
    const DpGblApi *api = ex->mach->api;
    for (unsigned i = 0; i < s->ndec; i++)
        if (s->dec[i].key == tkey && !s->dec[i].snap) {
            s->dec[i].snap = xrealloc(NULL, ex->sb);
            dp_mach_save(ex->mach, s->dec[i].snap);
        }
    if (s->nfr == FRAME_CAP) { ex->res->frame_overflow++; return; }
    typeof(s->fr[0]) *f = &s->fr[s->nfr++];
    f->key = tkey; f->sp = (uint16_t)(api->gbr->sp - 2); f->open = 1;
    f->reg[0] = api->gbr->hl; f->reg[1] = api->gbr->de; f->reg[2] = api->gbr->bc;
}

// every step: frames whose routine returned are dropped
void stream_step(DpExplorer *ex) {
    struct DpStreams *s = ex->stm;
    if (!s) return;
    uint16_t sp = ex->mach->api->gbr->sp;
    while (s->nfr && sp > s->fr[s->nfr - 1].sp) s->nfr--;
}

// ROM data read at a: the first one of each open frame names a stream
void stream_read(DpExplorer *ex, uint16_t a) {
    struct DpStreams *s = ex->stm;
    if (!s || !s->nfr) return;
    for (unsigned k = s->nfr; k-- > 0;) {
        typeof(s->fr[0]) *f = &s->fr[k];
        if (!f->open) break;
        f->open = 0;
        for (unsigned n = 0; n < 3; n++) {
            if (f->reg[n] != a || a < 0x150) continue;
            Dec *d = NULL;
            for (unsigned i = 0; i < s->ndec; i++)
                if (s->dec[i].key == f->key && s->dec[i].reg == n) d = &s->dec[i];
            if (!d) {
                if (s->ndec == DEC_CAP) { ex->res->decoder_overflow++; break; }
                d = &s->dec[s->ndec++];
                memset(d, 0, sizeof *d);
                d->key = f->key; d->reg = (uint8_t)n; d->bank = cur_bank(ex, a);
                ex->res->decoders++;
            }
            if (a >= 0x4000) {   // cross-bank starts: the stream's own bank rides with its address
                const uint16_t sb = cur_bank(ex, a);
                unsigned q;
                for (q = 0; q < d->nx && !(d->xs[q].bank == sb && d->xs[q].addr == a); q++) {}
                if (q == d->nx && q < DEC_STARTS) { d->xs[q].bank = sb; d->xs[q].addr = a; d->nx++; }
            }
            unsigned j;
            for (j = 0; j < d->nstarts && d->starts[j] != a; j++) {}
            if (j == d->nstarts && d->bank == cur_bank(ex, a)) {
                if (j < DEC_STARTS) d->starts[d->nstarts++] = a;
                else ex->res->start_overflow++;
            }
            break;
        }
    }
}

static int ptr_ok(const DpExplorer *ex, size_t at, size_t lo, size_t hi, int banked, uint16_t *v) {
    if (at < lo || at + 2 > hi) return 0;
    const uint8_t *rom = ex->res->cart->rom;
    *v = (uint16_t)(rom[at] | rom[at + 1] << 8);
    return banked ? (*v >= 0x4000 && *v < 0x8000) : (*v >= 0x150 && *v < 0x4000);
}

static int entry_ok(const DpCart *c, size_t at, int lay, size_t nb) {
    const uint8_t *e = c->rom + at;
    unsigned b = lay ? e[2] : e[0];
    uint16_t v = lay ? (uint16_t)(e[0] | e[1] << 8) : (uint16_t)(e[1] | e[2] << 8);
    return b >= 1 && b < nb && v >= 0x4000 && v < 0x8000;
}

static void replay_one(DpExplorer *ex, Dec *d, uint16_t v, int bank) {
    DpMach *m = ex->mach;
    uint8_t *cur = xrealloc(NULL, ex->sb);
    dp_mach_save(m, cur);               // the live machine comes back afterwards
    dp_mach_load(m, d->snap);
    if (bank >= 0) m->api->wr(0x2000, (uint8_t)bank);   // the table names the stream's bank: select it like the ROM would
    if (d->reg == 0) m->api->gbr->hl = v;
    else if (d->reg == 1) m->api->gbr->de = v;
    else m->api->gbr->bc = v;
    uint8_t *img = xrealloc(NULL, ex->sb);
    dp_mach_save(m, img);
    dp_mach_load(m, cur);
    free(cur);
    uint16_t pc = image_pc(img);
    if (queue_push(ex, img, 1, DP_PATH_FORK, 0,
                   choice_new(ex, 0, DP_PATH_FORK, 0, (int)cur_bank(ex, pc), pc)))
        ex->res->replay_runs++;
}

// Tables of banked streams: entries of 3 bytes (bank, ptr lo, ptr hi) or (ptr lo, ptr hi, bank)
// whose observed starts carry different banks.  Two matching entries at a constant stride locate
// the table; every entry is replayed with its bank selected.
static int replay_cross(DpExplorer *ex, Dec *d) {
    if (d->nx < 2 || d->nx <= d->xreplayed) return 0;
    d->xreplayed = d->nx;
    const DpCart *c = ex->res->cart;
    const size_t nb = c->rom_size >> 14;
    uint8_t *tried = calloc(c->rom_size ? c->rom_size : 1, 1);
    if (!tried) return 0;
    for (unsigned j = 0; j < d->nx; j++) {
        size_t idx = (size_t)d->xs[j].bank * 0x4000 + (d->xs[j].addr & 0x3FFF);
        if (idx < c->rom_size) tried[idx] = 1;
    }
    int pushed = 0;
    for (int lay = 0; lay < 2; lay++) {   // 0: bank first, 1: bank last
        size_t pos[64]; unsigned np = 0;
        for (size_t at = 0; at + 3 <= c->rom_size && np < 64; at++) {
            const uint8_t *e = c->rom + at;
            unsigned b = lay ? e[2] : e[0];
            uint16_t v = lay ? (uint16_t)(e[0] | e[1] << 8) : (uint16_t)(e[1] | e[2] << 8);
            for (unsigned j = 0; j < d->nx; j++)
                if (d->xs[j].bank == b && d->xs[j].addr == v) { pos[np++] = at; break; }
        }
        unsigned cands = 0;
        for (unsigned i = 0; i + 1 < np; i++) {
            size_t k = pos[i + 1] - pos[i];
            if (k != 3 || (pos[i] >> 14) != (pos[i + 1] >> 14)) continue;
            size_t blo = (pos[i] >> 14) << 14, bhi = blo + 0x4000;
            size_t lo = pos[i], hi = pos[i + 1];
#define ENTRY_OK(at) ((at) >= blo && (at) + 3 <= bhi && entry_ok(c, (at), lay, nb))
            while (lo >= blo + k && ENTRY_OK(lo - k)) lo -= k;
            while (ENTRY_OK(hi + k)) hi += k;
            ex->res->replay_tables++;
            for (size_t at = lo; at <= hi; at += k) {
                const uint8_t *e = c->rom + at;
                unsigned b = lay ? e[2] : e[0];
                uint16_t v = lay ? (uint16_t)(e[0] | e[1] << 8) : (uint16_t)(e[1] | e[2] << 8);
                size_t idx = (size_t)b * 0x4000 + (v & 0x3FFF);
                if (tried[idx]) continue;
                tried[idx] = 1;
                if (!ex->cfg.unbounded && cands >= CAND_CAP) { ex->res->sat_replay_total++; break; }
                cands++;
                ex->res->replay_candidates++;
                replay_one(ex, d, v, (int)b);
                pushed++;
            }
        }
    }
    free(tried);
    return pushed;
}

// the worklists are empty: replay every decoder over its pointer tables
int stream_replay(DpExplorer *ex) {
    struct DpStreams *s = ex->stm;
    if (!s || ex->session) return 0;
    s->tick = 0;
    const DpCart *c = ex->res->cart;
    int pushed = 0;
    for (unsigned di = 0; di < s->ndec; di++) {
        Dec *d = &s->dec[di];
        if (!d->snap) continue;   // a later call takes the snapshot, then the next poll retries
        pushed += replay_cross(ex, d);
        if (d->nstarts < 2 || d->nstarts <= d->replayed) continue;
        d->replayed = d->nstarts;
        int banked = d->starts[0] >= 0x4000;
        uint8_t *tried = calloc(65536, 1);
        if (!tried) continue;
        for (unsigned j = 0; j < d->nstarts; j++) tried[d->starts[j]] = 1;
        size_t pos[256]; unsigned np = 0;
        for (size_t at = 0; at + 2 <= c->rom_size && np < 256; at++) {
            uint16_t v = (uint16_t)(c->rom[at] | c->rom[at + 1] << 8);
            for (unsigned j = 0; j < d->nstarts; j++)
                if (d->starts[j] == v) { pos[np++] = at; break; }
        }
        unsigned cands = 0;
        for (unsigned i = 0; i + 1 < np; i++) {
            size_t k = pos[i + 1] - pos[i];
            if (k < 2 || k > 16 || (pos[i] >> 14) != (pos[i + 1] >> 14)) continue;
            ex->res->replay_tables++;
            size_t blo = (pos[i] >> 14) << 14, bhi = blo + 0x4000;
            size_t lo = pos[i], hi = pos[i + 1];
            uint16_t v;
            while (lo >= k && ptr_ok(ex, lo - k, blo, bhi, banked, &v)) lo -= k;
            while (ptr_ok(ex, hi + k, blo, bhi, banked, &v)) hi += k;
            {   // record the table once (dpTb chunk of the map PNG)
                DpResults *R = ex->res; uint32_t t;
                for (t = 0; t < R->tb_n && R->tb[t].at != lo; t++) {}
                if (t == R->tb_n && t < 128) { R->tb[t].at = (uint32_t)lo; R->tb[t].stride = (uint16_t)k; R->tb[t].count = (uint16_t)((hi - lo) / k + 1); R->tb_n++; }
            }
            for (size_t at = lo; at <= hi; at += k) {
                if (!ptr_ok(ex, at, blo, bhi, banked, &v) || tried[v]) continue;
                tried[v] = 1;
                if (!ex->cfg.unbounded && cands >= CAND_CAP) { ex->res->sat_replay_total++; break; }
                cands++;
                ex->res->replay_candidates++;
                replay_one(ex, d, v, -1);
                pushed++;
            }
        }
        free(tried);
    }
    return pushed;
}

// every few hundred steps of the explorer: replay the decoders whose observed
// starts grew, without waiting for the worklists to drain (a bounded run may
// never drain them)
void stream_poll(DpExplorer *ex) {
    if (ex->stm && ++ex->stm->tick >= 256) stream_replay(ex);
}
