// sessions.c: the play-session engine.  See sessions.h for the design; the
// input policy and the checkpoint pool follow m0boy's m0_explore.h (the
// coverage machinery behind its --explore profiles), adapted to memscan's
// concrete machines: a checkpoint is a full MsMachine plus a private SRAM
// copy, and discovery credit is measured by the same first-flip counters the
// trace handlers fill.
//
// Threading: every worker owns an MsExplorer (its recording context) and a
// private machine + SRAM buffer; the shared MsResults maps are updated
// through atomics and the results mutexes (res->mt stays 1 for the whole
// play).  The fork explorer only runs in the scheduler section, while the
// workers wait at the epoch barrier, so it never races them.
#include "sessions.h"
#include "../platform/gb/gb.h"
#include "../cpu/sm83/sm83.h"
#include "../core/snapshot_png.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// --- sigint -------------------------------------------------------------------
static volatile sig_atomic_t g_sigint;
static void on_sigint(int sig) { (void)sig; g_sigint = 1; }
void ms_play_install_sigint(void) { signal(SIGINT, on_sigint); }

// --- config -------------------------------------------------------------------
void ms_session_cfg_defaults(MsSessionCfg *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->sessions = 10;
    cfg->epoch_frames = 480;
    cfg->segment_min = 480;
    cfg->segment_span = 1920;
    cfg->pool_cap = 48;
    cfg->stall_epochs = 3;
    cfg->restart_rate = 8;
    cfg->save_epochs = 10;
    cfg->fork_paths_per_epoch = 200;
}

// --- input generation (port of m0_explore.h m0x_move/m0x_keys) ----------------
// GB mask: a=1 b=2 select=4 start=8 right=16 left=32 up=64 down=128
#define K_RIGHT 0x10
#define K_LEFT 0x20
#define K_UP 0x40
#define K_DOWN 0x80
#define K_A 0x01
#define K_B 0x02
#define K_START 0x08

typedef struct MsInputState {
    uint32_t rng;
    uint32_t move_left, move_keys, tap_left;
} MsInputState;

static uint32_t rnd(uint32_t *rng) {
    uint32_t x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x ? x : 0x9e3779b9u;
    return x;
}

static void next_move(MsInputState *in) {
    const uint32_t r = rnd(&in->rng) % 100;
    uint32_t keys = r < 20 ? 0 : r < 50 ? K_RIGHT : r < 65 ? K_LEFT : r < 73 ? K_UP : r < 81 ? K_DOWN :
                    r < 86 ? (K_RIGHT | K_UP) : r < 91 ? (K_RIGHT | K_DOWN) : r < 96 ? (K_LEFT | K_UP) :
                    (K_LEFT | K_DOWN);
    in->move_left = 8 + rnd(&in->rng) % 56;
    in->tap_left = 0;
    if (rnd(&in->rng) % 100 < 45) keys |= K_A;
    if (rnd(&in->rng) % 100 < 25) keys |= K_B;
    if (rnd(&in->rng) % 100 < 2) keys |= K_START;
    if ((keys & (K_A | K_B)) && rnd(&in->rng) % 2) in->tap_left = 4 + rnd(&in->rng) % 8;
    in->move_keys = keys;
}

static uint8_t input_keys(MsInputState *in) {
    if (!in->move_left) next_move(in);
    in->move_left--;
    uint32_t keys = in->move_keys;
    if (in->move_keys & K_START && in->tap_left == 0) in->tap_left = 5;
    if (in->tap_left && !--in->tap_left) in->move_keys &= ~(uint32_t)(K_A | K_B | K_START);
    return (uint8_t)keys;
}

// m0boy held-named script: "<frame> a+b+start" or "none", a line only when
// the held state changes; the mask latches until the next line
typedef struct { uint32_t frame; uint8_t mask; } MsScriptEv;

static int parse_button_word(const char *w, uint8_t *mask) {
    static const struct { const char *n; uint8_t b; } tab[] = {
        { "a", K_A }, { "b", K_B }, { "select", 0x04 }, { "start", K_START },
        { "right", K_RIGHT }, { "left", K_LEFT }, { "up", K_UP }, { "down", K_DOWN },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(w, tab[i].n)) { *mask |= tab[i].b; return 1; }
    return 0;
}

static int script_load(const char *path, MsScriptEv **evs, int *n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int cap = 64, cnt = 0;
    MsScriptEv *e = malloc(sizeof(MsScriptEv) * (size_t)cap);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char *end;
        long frame = strtol(p, &end, 10);
        if (end == p) continue;
        p = end;
        uint8_t mask = 0;
        while (*p == ' ') p++;
        if (!strncmp(p, "none", 4)) {
            mask = 0;
        } else {
            char word[16];
            int wi = 0;
            for (;; p++) {
                char c = *p;
                if (c == '+' || c == ' ' || c == '\n' || c == '\t' || c == '\r' || !c) {
                    if (wi && wi < 15) {
                        word[wi] = 0;
                        parse_button_word(word, &mask); // unknown keys are ignored
                    }
                    wi = 0;
                    if (!c || c == '\n') break;
                } else if (wi < 15) {
                    word[wi++] = c;
                }
            }
        }
        if (cnt == cap) {
            cap *= 2;
            e = realloc(e, sizeof(MsScriptEv) * (size_t)cap);
        }
        e[cnt].frame = (uint32_t)(frame < 0 ? 0 : frame);
        e[cnt].mask = mask;
        cnt++;
    }
    fclose(f);
    *evs = e;
    *n = cnt;
    return 1;
}

static uint8_t script_keys(const MsScriptEv *evs, int n, int *pos, uint32_t frame) {
    uint8_t mask = 0;
    while (*pos < n && evs[*pos].frame <= frame) mask = evs[(*pos)++].mask;
    return mask;
}

// --- machine copy helpers -------------------------------------------------------
// page hashes ride inside the machine struct; SRAM does not.  After replacing
// or reloading the SRAM buffer the cart-RAM page hashes must be rebuilt.
static void refresh_cram_hashes(MsMachine *m) {
    uint32_t pages = m->sram_size ? (m->sram_size + 255) / 256 : 0;
    if (pages > MS_CRAM_PAGES) pages = MS_CRAM_PAGES;
    for (uint32_t pg = 0; pg < pages; pg++) {
        uint64_t h = 0xcbf29ce484222325ull;
        uint32_t rem = m->sram_size - (pg << 8);
        uint32_t n = rem < 256 ? rem : 256;
        for (uint32_t i = 0; i < n; i++) { h ^= m->sram[(pg << 8) + i]; h *= 0x100000001b3ull; }
        m->ph_cram[pg] = h;
    }
}

// --- checkpoint pool -------------------------------------------------------------
typedef struct MsCheckpoint {
    MsMachine m; // m.sram points at own
    uint8_t *sram;
    uint32_t frame;
    uint64_t credit;
    uint32_t chosen;
    uint8_t slot0;
} MsCheckpoint;

typedef struct MsPool {
    pthread_mutex_t mtx;
    MsCheckpoint *slot[MS_MAX_POOL];
    int n, cap;
} MsPool;

static void ckpt_free(MsCheckpoint *c) {
    if (!c) return;
    free(c->sram);
    free(c);
}

// deep snapshot of a live machine (the source keeps running)
static MsCheckpoint *ckpt_take(const MsMachine *m, uint32_t frame, uint64_t credit, int slot0) {
    MsCheckpoint *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->m = *m;
    c->m.trace = NULL;
    c->m.trace_ctx = NULL;
    c->sram = malloc(m->sram_size ? m->sram_size : 1);
    if (m->sram_size) memcpy(c->sram, m->sram, m->sram_size);
    c->m.sram = c->sram;
    c->frame = frame;
    c->credit = credit;
    c->slot0 = (uint8_t)(slot0 ? 1 : 0);
    return c;
}

// loads a checkpoint into a live worker machine (keeps its own sram buffer
// and its trace wiring)
static void ckpt_load(const MsCheckpoint *c, MsMachine *m) {
    MsMachine t = c->m;
    uint8_t *sram = m->sram;
    const MsTraceVt *trace = m->trace;
    void *ctx = m->trace_ctx;
    t.sram = sram;
    t.trace = trace;
    t.trace_ctx = ctx;
    *m = t;
    if (c->m.sram_size) memcpy(sram, c->m.sram, c->m.sram_size);
    refresh_cram_hashes(m);
}

// --- seed inbox (worker -> fork explorer) ----------------------------------------
typedef struct MsInboxEnt {
    MsMachine m; // m.sram points at own buffer, freed with the entry
    uint8_t *sram;
} MsInboxEnt;

#define MS_INBOX_CAP 512

// --- play engine ------------------------------------------------------------------
typedef enum { IN_RNG = 0, IN_ATTRACT, IN_SCRIPT, IN_FIXED, IN_HUMAN } InMode;


typedef struct MsWorker {
    MsPlay *p;
    int id;
    InMode mode;
    pthread_t th;
    int started;
    // recording context: one explorer of our own, sharing the results maps
    MsExplorer *ex;
    MsMachine m;
    uint8_t *sram_buf;
    // input state
    MsInputState in;
    MsScriptEv *script;
    int script_n, script_pos;
    uint8_t fixed_mask;
    uint8_t human_buttons; // IN_HUMAN: set by the GUI thread before each frame
    // segment state (m0x)
    uint32_t seg_left;
    uint64_t credit_prev; // credit at the last observe
    int has_pending;
    MsCheckpoint pending;
    // bookkeeping
    uint32_t frames_done;
    uint32_t seen_frame;      // frames_done as last seen by the scheduler
    uint64_t credit_base;     // credit_total as last seen by the scheduler
    uint64_t credit_epoch;    // credit gained during the current epoch
    int halt_stalls;
    int deaths;
    int pad_slide;             // consecutive padding ops (garbage slide)
    int dead;
    int stagnant_epochs;
    uint64_t credit_total;
    int backtracked;
    uint64_t deposits;
    // scheduler-visible lane snapshot (written under bar_mtx)
    uint32_t my_epoch;
    uint32_t lane_frame, lane_seg_left;
    uint8_t lane_bank;
    uint16_t lane_pc;
    uint8_t lane_buttons, lane_dead, lane_backtracked;
} MsWorker;

struct MsPlay {
    MsSessionCfg cfg;
    MsResults *res;
    MsExplorer *fork;
    const MsCart *cart;
    const MsPlatform *plat;
    MsPool pool;
    MsWorker w[MS_MAX_SESSIONS];
    int nworkers;
    // human takeover lane (driven by the GUI thread, not threaded)
    int human_on;
    MsWorker human;
    uint8_t human_buttons;
    uint8_t *human_screen;
    uint64_t human_png_credit; // credit at the last PNG save
    // epoch barrier
    pthread_mutex_t bar_mtx;
    pthread_cond_t cv;
    uint32_t epoch_no;
    int workers_done;
    int quit;
    int stop_reason;
    int epoch_ran;
    // seed inbox
    pthread_mutex_t inb_mtx;
    MsInboxEnt *inb;
    int inb_n;
    uint64_t inb_dropped;
    // totals
    uint64_t total_frames;
    uint64_t stagnation;
    uint32_t epoch;
    uint64_t unexplained;
    // snapshot gallery
    char snap_dir[512];
    char last_snap[600];
    uint32_t snap_seq;
};

// worker credit: everything its trace handlers discovered so far
static uint64_t worker_credit(const MsWorker *w) {
    return w->ex->credit_exec + w->ex->credit_data + w->ex->credit_copies + w->ex->credit_io +
           w->ex->credit_banks;
}

static void worker_reset_run_state(MsWorker *w) {
    w->ex->cur = &w->m;
    w->ex->cur_forced = 0;
    w->ex->cur_kind = 3; // MS_SEED_POINTER: real play state, not a fork
    w->ex->cur_from_reset = 0;
    w->ex->cur_irq = 0;
    w->ex->irq_depth = 0;
    w->ex->have_last = 0;
    w->ex->cur_sec = 0;
    w->ex->steps_this_path = 0;
    memset(w->ex->back_ring, 0, sizeof w->ex->back_ring);
    w->ex->back_idx = 0;
    ms_copy_reset(&w->ex->ct);
    w->halt_stalls = 0;
}

static void deposit_seed(MsWorker *w, const MsMachine *m) {
    MsPlay *p = w->p;
    pthread_mutex_lock(&p->inb_mtx);
    if (p->inb_n < MS_INBOX_CAP) {
        MsInboxEnt *e = &p->inb[p->inb_n++];
        e->m = *m;
        e->m.trace = NULL;
        e->m.trace_ctx = NULL;
        e->sram = malloc(m->sram_size ? m->sram_size : 1);
        if (m->sram_size) memcpy(e->sram, m->sram, m->sram_size);
        e->m.sram = e->sram;
    } else {
        w->deposits++;
    }
    pthread_mutex_unlock(&p->inb_mtx);
}

// m0x_observe: on discovery, keep the current machine as the pending snapshot
static void observe(MsWorker *w, uint64_t credit) {
    if (w->has_pending) free(w->pending.sram);
    memset(&w->pending, 0, sizeof w->pending);
    w->pending.m = w->m;
    w->pending.m.trace = NULL;
    w->pending.m.trace_ctx = NULL;
    w->pending.sram = malloc(w->m.sram_size ? w->m.sram_size : 1);
    if (w->m.sram_size) memcpy(w->pending.sram, w->m.sram, w->m.sram_size);
    w->pending.m.sram = w->pending.sram;
    w->pending.frame = w->frames_done;
    w->pending.credit = credit;
    w->has_pending = 1;
}

// m0x_next's promotion: the pending discovery joins the pool and the seed
// inbox, so every engine (sessions, fork explorer, human gallery) gets it.
// Returns the pool slot it landed in.
static int promote_pending(MsWorker *w) {
    MsPlay *p = w->p;
    deposit_seed(w, &w->pending.m);
    pthread_mutex_lock(&p->pool.mtx);
    int slot;
    if (p->pool.n < p->pool.cap) {
        slot = p->pool.n++;
    } else { // replace the most chosen, never the start (m0x)
        slot = 1;
        for (int i = 2; i < p->pool.n; i++)
            if (p->pool.slot[i]->chosen > p->pool.slot[slot]->chosen) slot = i;
        ckpt_free(p->pool.slot[slot]);
    }
    MsCheckpoint *c = calloc(1, sizeof *c);
    *c = w->pending; // takes ownership of pending.sram
    c->slot0 = 0;
    p->pool.slot[slot] = c;
    w->has_pending = 0;
    memset(&w->pending, 0, sizeof w->pending);
    pthread_mutex_unlock(&p->pool.mtx);
    return slot;
}

// m0x_next: promote the pending discovery, then load the next segment start
static void segment_end(MsWorker *w) {
    MsPlay *p = w->p;
    if (w->has_pending) promote_pending(w);
    deposit_seed(w, &w->m);

    // next segment start: 1/restart_rate from power-on, a stagnant session
    // re-seeds from the most productive checkpoint, otherwise the least chosen
    pthread_mutex_lock(&p->pool.mtx);
    int pick = 0;
    if (w->mode == IN_RNG && p->pool.n > 1) {
        if (w->stagnant_epochs >= p->cfg.stall_epochs) {
            int best = -1;
            for (int i = 1; i < p->pool.n; i++) {
                if (best < 0) { best = i; continue; }
                if (p->pool.slot[i]->credit > p->pool.slot[best]->credit ||
                    (p->pool.slot[i]->credit == p->pool.slot[best]->credit &&
                     p->pool.slot[i]->chosen < p->pool.slot[best]->chosen))
                    best = i;
            }
            pick = best;
        } else if (rnd(&w->in.rng) % (uint32_t)p->cfg.restart_rate) {
            uint32_t best = ~0u;
            uint32_t offset = rnd(&w->in.rng) % (uint32_t)p->pool.n;
            for (int k = 0; k < p->pool.n; k++) {
                int i = (int)((k + offset) % (uint32_t)p->pool.n);
                if (p->pool.slot[i]->slot0) continue;
                if (p->pool.slot[i]->chosen < best) { best = p->pool.slot[i]->chosen; pick = i; }
            }
            if (best == ~0u) pick = 0; // only the start exists
        }
    }
    p->pool.slot[pick]->chosen++;
    w->backtracked = (pick != 0);
    ckpt_load(p->pool.slot[pick], &w->m);
    pthread_mutex_unlock(&p->pool.mtx);
    w->seg_left = p->cfg.segment_min + rnd(&w->in.rng) % p->cfg.segment_span;
    w->in.move_left = 0;
    w->in.tap_left = 0;
    w->credit_prev = worker_credit(w);
    worker_reset_run_state(w);
}

// --- the frame loop ------------------------------------------------------------------
static void run_frames(MsWorker *w, uint32_t frames) {
    MsPlay *p = w->p;
    MsResults *res = p->res;
    const MsPlatform *plat = p->plat;
    for (uint32_t f = 0; f < frames; f++) {
        if (p->quit) return;
        // input for this frame
        uint8_t mask;
        if (w->mode == IN_ATTRACT) mask = 0;
        else if (w->mode == IN_FIXED) mask = w->fixed_mask;
        else if (w->mode == IN_HUMAN) mask = w->human_buttons; // set by the GUI thread
        else if (w->mode == IN_SCRIPT)
            mask = script_keys(w->script, w->script_n, &w->script_pos, w->frames_done);
        else mask = input_keys(&w->in);
        ms_gb_set_buttons(&w->m, mask);

        uint32_t budget = 70224;
        int stepped = 0;
        while (budget) {
            MsMachine *m = &w->m;
            if (m->halted) {
                if (ms_service_irq(w->ex, m)) continue; // dispatch wakes the halt
                uint8_t pending = (uint8_t)(m->ie & m->io[0x0F] & 0x1F);
                if (pending) { m->halted = 0; continue; } // !IME halt exit (halt bug ignored)
                uint32_t adv = gb_halt_advance(m);
                if (!adv) { // nothing can ever wake it: dead end
                    if (w->mode == IN_HUMAN) return; // the human sees the freeze and decides
                    segment_end(w);
                    return;
                }
                if (adv > budget) adv = budget;
                if (plat->tick) plat->tick(m, adv);
                budget -= adv;
                continue;
            }
            if (ms_service_irq(w->ex, m)) continue;
            uint8_t op = ms_peek(m, m->pc);
            uint32_t pk = ms_rom_key(m, m->pc);
            if ((res->static_flags[pk] & MS_SF_PAD) && (op == 0x00 || op == 0xFF)) {
                // a garbage return (RETI on an empty stack...) slides through
                // padding: mark nothing, and cut the run once it is clearly a
                // slide — a lone padded op (the boot NOP, a vector) is fine
                if (++w->pad_slide > 4) {
                    if (++w->deaths >= 8) w->dead = 1;
                    segment_end(w);
                    return;
                }
                w->ex->cur_pad = 1;
            } else {
                w->pad_slide = 0;
                w->ex->cur_pad = 0;
            }
            w->ex->cur_bankN = m->win_romN;
            w->ex->cur_irq = (uint8_t)(w->ex->irq_depth > 0);
            ms_pre_step(w->ex, m, op);
            int r = sm83_step(m);
            stepped = 1;
            w->ex->steps_this_path++;
            __atomic_add_fetch(&res->steps_total, 1, __ATOMIC_RELAXED);
            uint32_t cyc = w->ex->last_cycles ? w->ex->last_cycles : 4;
            if (plat->tick) plat->tick(m, cyc);
            budget -= cyc > budget ? budget : cyc;
            if (r != 0) {
                if (r == 2) { // undefined opcode: the game crashed here
                    if (w->mode == IN_HUMAN) return;
                    if (++w->deaths >= 8) w->dead = 1;
                    segment_end(w);
                    return;
                }
                break; // halted/stopped: handled at the loop top
            }
        }
        // a frame spent entirely halted with no dispatch means a dead halt
        if (w->m.halted && !stepped) {
            if (++w->halt_stalls > 8) {
                if (w->mode == IN_HUMAN) return;
                segment_end(w);
                return;
            }
        } else {
            w->halt_stalls = 0;
        }
        w->frames_done++;
        if (w->seg_left) w->seg_left--;
        uint64_t cred = worker_credit(w);
        if (cred > w->credit_prev) {
            observe(w, cred);
            w->credit_prev = cred;
        }
        ms_copy_flush_path(w->ex, &w->ex->ct);
        if (!w->seg_left && w->mode != IN_HUMAN) {
            segment_end(w);
            if (w->dead) return;
        }
    }
}

// --- worker thread ---------------------------------------------------------------------
static void *worker_main(void *arg) {
    MsWorker *w = arg;
    MsPlay *p = w->p;
    for (;;) {
        pthread_mutex_lock(&p->bar_mtx);
        while (p->epoch_no == w->my_epoch && !p->quit)
            pthread_cond_wait(&p->cv, &p->bar_mtx);
        if (p->quit) {
            pthread_mutex_unlock(&p->bar_mtx);
            break;
        }
        w->my_epoch = p->epoch_no;
        pthread_mutex_unlock(&p->bar_mtx);

        if (!w->dead) run_frames(w, p->cfg.epoch_frames);
        // epoch bookkeeping for the scheduler
        pthread_mutex_lock(&p->bar_mtx);
        w->credit_total = worker_credit(w);
        w->lane_frame = w->frames_done;
        w->lane_seg_left = w->seg_left;
        w->lane_pc = w->m.pc;
        w->lane_bank = (uint8_t)(w->m.pc < 0x4000 ? w->m.win_rom0 : w->m.win_romN);
        w->lane_buttons = w->m.buttons;
        w->lane_dead = (uint8_t)w->dead;
        w->lane_backtracked = (uint8_t)w->backtracked;
        p->workers_done++;
        if (p->workers_done >= p->nworkers) pthread_cond_broadcast(&p->cv);
        // wait until the scheduler starts the next epoch
        while (p->workers_done >= p->nworkers && !p->quit)
            pthread_cond_wait(&p->cv, &p->bar_mtx);
        pthread_mutex_unlock(&p->bar_mtx);
    }
    return NULL;
}

// --- persistence -------------------------------------------------------------------------
static void save_machine(FILE *f, const MsMachine *m) {
    MsMachine t = *m;
    // pointers are rebuilt on load
    t.cart = NULL;
    t.plat = NULL;
    t.sram = NULL;
    t.trace = NULL;
    t.trace_ctx = NULL;
    fwrite(&t, sizeof t, 1, f);
}

static int load_machine(FILE *f, MsMachine *m, const MsCart *cart, const MsPlatform *plat,
                        uint8_t **sram_out, uint32_t *sram_size_out) {
    if (fread(m, sizeof *m, 1, f) != 1) return 0;
    uint32_t sz;
    if (fread(&sz, sizeof sz, 1, f) != 1) return 0;
    if (sz > MS_CRAM_PAGES * 256) return 0;
    uint8_t *buf = malloc(sz ? sz : 1);
    if (sz && fread(buf, 1, sz, f) != sz) { free(buf); return 0; }
    m->cart = cart;
    m->plat = plat;
    m->sram = buf;
    m->sram_size = sz;
    m->trace = NULL;
    m->trace_ctx = NULL;
    *sram_out = buf;
    *sram_size_out = sz;
    return 1;
}

int ms_play_save(const MsPlay *p) {
    if (!p->cfg.state_dir) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/pool.bin", p->cfg.state_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const char magic[4] = { 'M', 'S', 'P', 'S' };
    const uint32_t ver = 1;
    fwrite(magic, 1, 4, f);
    fwrite(&ver, sizeof ver, 1, f);
    pthread_mutex_lock(&p->pool.mtx);
    uint32_t n = (uint32_t)p->pool.n;
    fwrite(&n, sizeof n, 1, f);
    for (int i = 0; i < p->pool.n; i++) {
        const MsCheckpoint *c = p->pool.slot[i];
        fwrite(&c->slot0, 1, 1, f);
        fwrite(&c->chosen, sizeof c->chosen, 1, f);
        fwrite(&c->frame, sizeof c->frame, 1, f);
        fwrite(&c->credit, sizeof c->credit, 1, f);
        save_machine(f, &c->m);
        fwrite(&c->m.sram_size, sizeof c->m.sram_size, 1, f);
        if (c->m.sram_size) fwrite(c->sram, 1, c->m.sram_size, f);
    }
    pthread_mutex_unlock(&p->pool.mtx);
    fwrite(&p->epoch, sizeof p->epoch, 1, f);
    fwrite(&p->total_frames, sizeof p->total_frames, 1, f);
    fwrite(&p->stagnation, sizeof p->stagnation, 1, f);
    fclose(f);
    return 1;
}

static int pool_load(MsPlay *p) {
    if (!p->cfg.state_dir) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/pool.bin", p->cfg.state_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    uint32_t ver;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MSPS", 4) || fread(&ver, sizeof ver, 1, f) != 1 ||
        ver != 1) {
        fclose(f);
        return 0;
    }
    uint32_t n;
    if (fread(&n, sizeof n, 1, f) != 1 || n > (uint32_t)p->pool.cap) {
        fclose(f);
        return 0;
    }
    for (int i = 0; i < p->pool.n; i++) ckpt_free(p->pool.slot[i]);
    p->pool.n = 0;
    int ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        MsCheckpoint *c = calloc(1, sizeof *c);
        if (fread(&c->slot0, 1, 1, f) != 1 || fread(&c->chosen, sizeof c->chosen, 1, f) != 1 ||
            fread(&c->frame, sizeof c->frame, 1, f) != 1 ||
            fread(&c->credit, sizeof c->credit, 1, f) != 1) {
            free(c);
            ok = 0;
            break;
        }
        uint32_t sz;
        if (!load_machine(f, &c->m, p->cart, p->plat, &c->sram, &sz)) {
            free(c);
            ok = 0;
            break;
        }
        c->m.sram = c->sram;
        c->m.sram_size = sz;
        p->pool.slot[p->pool.n++] = c;
    }
    if (ok && fread(&p->epoch, sizeof p->epoch, 1, f) != 1) ok = 0;
    if (ok && fread(&p->total_frames, sizeof p->total_frames, 1, f) != 1) ok = 0;
    if (ok && fread(&p->stagnation, sizeof p->stagnation, 1, f) != 1) ok = 0;
    fclose(f);
    if (!ok) {
        for (int i = 0; i < p->pool.n; i++) ckpt_free(p->pool.slot[i]);
        p->pool.n = 0;
        return 0;
    }
    return 1;
}

// --- lifecycle -----------------------------------------------------------------------------
// builds the recording context (explorer + machine + private SRAM) of one
// lane; used for the thread workers and for the human takeover lane
static void worker_setup(MsPlay *p, MsWorker *w) {
    w->p = p;
    MsConfig ecfg = p->fork->cfg;
    ecfg.queue_cap = 1; // session explorers never queue
    w->ex = ms_explorer_new(&ecfg, p->res);
    w->ex->session = 1;
    ms_machine_init(&w->m, p->cart, p->plat);
    w->sram_buf = malloc(p->cart->ram_size ? p->cart->ram_size : 1);
    if (p->cart->ram_size) memcpy(w->sram_buf, p->cart->ram, p->cart->ram_size);
    w->m.sram = w->sram_buf;
    w->m.sram_size = p->cart->ram_size;
    w->m.trace = &w->ex->vt;
    w->m.trace_ctx = w->ex;
    worker_reset_run_state(w);
}

MsPlay *ms_play_new(const MsSessionCfg *cfg, MsResults *res, MsExplorer *fork_ex) {
    MsPlay *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cfg = *cfg;
    if (p->cfg.pool_cap > MS_MAX_POOL) p->cfg.pool_cap = MS_MAX_POOL;
    if (p->cfg.sessions > MS_MAX_SESSIONS) p->cfg.sessions = MS_MAX_SESSIONS;
    if (p->cfg.sessions < 1) p->cfg.sessions = 1;
    p->res = res;
    p->fork = fork_ex;
    p->cart = res->cart;
    p->plat = res->plat;
    pthread_mutex_init(&p->bar_mtx, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_mutex_init(&p->pool.mtx, NULL);
    pthread_mutex_init(&p->inb_mtx, NULL);
    p->inb = calloc(MS_INBOX_CAP, sizeof(MsInboxEnt));
    p->pool.cap = p->cfg.pool_cap;
    p->workers_done = p->cfg.sessions; // the first poll starts epoch 1

    // slot 0: power-on machine
    MsMachine m;
    ms_machine_init(&m, p->cart, p->plat);
    p->pool.slot[p->pool.n++] = ckpt_take(&m, 0, 0, 1);

    if (p->cfg.resume && pool_load(p))
        fprintf(stderr, "memscan: resumed %d checkpoints (epoch %u, %llu frames)\n", p->pool.n, p->epoch,
                (unsigned long long)p->total_frames);
    else if (p->cfg.resume)
        fprintf(stderr, "memscan: no usable pool file, starting fresh\n");

    // workers: 0 attract, then one per replay script, the rest random
    res->mt = 1;
    p->nworkers = p->cfg.sessions;
    for (int i = 0; i < p->nworkers; i++) {
        MsWorker *w = &p->w[i];
        w->p = p;
        w->id = i;
        w->mode = IN_RNG;
        if (i == 0) w->mode = IN_ATTRACT;
        else if (i - 1 < p->cfg.inputs_n && p->cfg.inputs[i - 1]) {
            w->mode = IN_SCRIPT;
            if (!script_load(p->cfg.inputs[i - 1], &w->script, &w->script_n)) {
                fprintf(stderr, "memscan: cannot read input script %s\n", p->cfg.inputs[i - 1]);
                w->mode = IN_RNG;
            }
        }
        if (i < p->cfg.fixed_n && p->cfg.fixed[i]) {
            w->mode = IN_FIXED;
            w->fixed_mask = p->cfg.fixed[i];
        }
        w->in.rng = p->cfg.seed ? p->cfg.seed * 2654435761u + (uint32_t)i * 0x9E3779B9u
                                : 0x9e3779b9u + (uint32_t)i * 0x85EBCA6Bu;
        worker_setup(p, w);
        w->seg_left = p->cfg.segment_min + rnd(&w->in.rng) % p->cfg.segment_span;
        w->my_epoch = 0;
        if (pthread_create(&w->th, NULL, worker_main, w) == 0) w->started = 1;
    }
    // snapshot gallery directory (next to the pool file, or beside the run)
    if (p->cfg.state_dir && *p->cfg.state_dir)
        snprintf(p->snap_dir, sizeof p->snap_dir, "%s/snapshots", p->cfg.state_dir);
    else
        snprintf(p->snap_dir, sizeof p->snap_dir, "memscan-snaps");
    mkdir(p->snap_dir, 0755);
    return p;
}

void ms_play_free(MsPlay *p) {
    if (!p) return;
    ms_play_finish(p); // stop and join the workers, whatever the caller did
    for (int i = 0; i < p->nworkers; i++) {
        MsWorker *w = &p->w[i];
        if (w->started) pthread_join(w->th, NULL);
        ms_explorer_free(w->ex);
        free(w->sram_buf);
        free(w->script);
        free(w->pending.sram);
    }
    if (p->human_on) {
        ms_explorer_free(p->human.ex);
        free(p->human.sram_buf);
        free(p->human.pending.sram);
        p->human_on = 0;
    }
    free(p->human_screen);
    for (int i = 0; i < p->pool.n; i++) ckpt_free(p->pool.slot[i]);
    for (int i = 0; i < p->inb_n; i++) free(p->inb[i].sram);
    free(p->inb);
    p->res->mt = 0;
    pthread_mutex_destroy(&p->bar_mtx);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->pool.mtx);
    pthread_mutex_destroy(&p->inb_mtx);
    free(p);
}

// --- scheduler ------------------------------------------------------------------------------
static void drain_inbox(MsPlay *p) {
    pthread_mutex_lock(&p->inb_mtx);
    int n = p->inb_n;
    p->inb_n = 0;
    pthread_mutex_unlock(&p->inb_mtx);
    for (int i = 0; i < n; i++) {
        MsInboxEnt *e = &p->inb[i];
        // the fork explorer shares cart->ram: import the session's SRAM as
        // the master so seeded paths run in the same save context
        if (p->cart->ram_size && e->m.sram_size) {
            uint32_t n2 = e->m.sram_size < p->cart->ram_size ? e->m.sram_size : p->cart->ram_size;
            memcpy(p->cart->ram, e->m.sram, n2);
        }
        MsMachine m = e->m;
        m.sram = p->cart->ram;
        m.sram_size = p->cart->ram_size;
        ms_explorer_seed(p->fork, &m, 3 /* MS_SEED_POINTER: real play state */, 0);
        free(e->sram);
    }
}

// one scheduler step.  While the workers run, this returns immediately; at an
// epoch boundary it collects the credits, drains the session-seed inbox into
// the fork explorer's queue, gives the fork explorer a slice, re-seeds the
// stagnant sessions (their next segment_end picks the best checkpoint) and
// checks the stop conditions.
int ms_play_poll(MsPlay *p) {
    p->epoch_ran = 0;
    pthread_mutex_lock(&p->bar_mtx);
    if (p->quit || p->workers_done < p->nworkers) {
        pthread_mutex_unlock(&p->bar_mtx);
        return 0;
    }
    // --- epoch complete: the workers wait, this thread owns the results ----
    p->epoch_ran = 1;
    p->epoch++;
    uint64_t glob_new = 0, frames = 0;
    for (int i = 0; i < p->nworkers; i++) {
        MsWorker *w = &p->w[i];
        w->credit_epoch = w->credit_total - w->credit_base;
        w->credit_base = w->credit_total;
        glob_new += w->credit_epoch;
        frames += w->lane_frame - w->seen_frame;
        w->seen_frame = w->lane_frame;
        w->stagnant_epochs = w->credit_epoch == 0 ? w->stagnant_epochs + 1 : 0;
    }
    // the human lane counts too: playing counts as progress, it must never
    // be punished by the stagnation wall
    if (p->human_on) {
        uint64_t hc = worker_credit(&p->human);
        glob_new += hc - p->human.credit_base;
        p->human.credit_base = hc;
        p->stagnation = 0;
    }
    p->total_frames += frames;
    if (getenv("MS_DBG")) fprintf(stderr, "DBG poll epoch=%u frames=+%llu total=%llu glob_new=%llu pool=%d\n",
                                  p->epoch, (unsigned long long)frames, (unsigned long long)p->total_frames,
                                  (unsigned long long)glob_new, p->pool.n);
    if (frames) p->stagnation = glob_new ? 0 : p->stagnation + frames;
    p->unexplained = p->cfg.until_static ? ms_results_unexplained(p->res) : 0;

    // sessions -> fork explorer: real play states become exploration seeds
    drain_inbox(p);
    // the fork advances while the sessions wait; bounded by paths, not time,
    // so a fixed-seed run is reproducible
    for (int i = 0; i < p->cfg.fork_paths_per_epoch && p->fork->q_len && !p->fork->stop; i++)
        ms_explore_step(p->fork);

    // stop conditions
    int reason = -1;
    if (g_sigint) reason = 2;
    else if (p->cfg.max_frames && p->total_frames >= p->cfg.max_frames) reason = 0;
    else if (p->cfg.stagnation_frames && p->stagnation >= p->cfg.stagnation_frames) reason = 1;
    else if (p->cfg.until_static && p->unexplained == 0 && frames) reason = 3;
    if (reason >= 0) {
        p->quit = 1;
        p->stop_reason = reason;
        ms_play_save(p);
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->bar_mtx);
        return 1;
    }
    if (p->cfg.save_epochs && p->epoch % (uint32_t)p->cfg.save_epochs == 0) ms_play_save(p);
    p->workers_done = 0;
    p->epoch_no++;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->bar_mtx);
    return 1;
}

void ms_play_finish(MsPlay *p) {
    if (!p || p->workers_done < 0) return;
    pthread_mutex_lock(&p->bar_mtx);
    p->quit = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->bar_mtx);
    for (int i = 0; i < p->nworkers; i++)
        if (p->w[i].started) pthread_join(p->w[i].th, NULL);
    for (int i = 0; i < p->nworkers; i++) p->w[i].started = 0;
    p->workers_done = -1; // joined
}

int ms_play_run(MsPlay *p) {
    // headless driver: poll until a stop condition, then let the workers exit
    while (!p->quit) {
        if (!ms_play_poll(p)) usleep(500);
    }
    ms_play_finish(p);
    return p->stop_reason;
}

// --- snapshot PNGs (screen + full machine in one file) ---------------------------------
// payload = the exact record the pool file stores per checkpoint
static void payload_ckpt(MsBuf *b, const MsCheckpoint *c) {
    ms_buf_push(b, &c->slot0, 1);
    ms_buf_push(b, &c->chosen, sizeof c->chosen);
    ms_buf_push(b, &c->frame, sizeof c->frame);
    ms_buf_push(b, &c->credit, sizeof c->credit);
    MsMachine t = c->m;
    t.cart = NULL;
    t.plat = NULL;
    t.sram = NULL;
    t.trace = NULL;
    t.trace_ctx = NULL;
    ms_buf_push(b, &t, sizeof t);
    ms_buf_push(b, &c->m.sram_size, sizeof c->m.sram_size);
    if (c->m.sram_size) ms_buf_push(b, c->sram, c->m.sram_size);
}

static MsCheckpoint *ckpt_from_payload(const MsPlay *p, const uint8_t *d, size_t n) {
    size_t need = 1 + sizeof(uint32_t) * 2 + sizeof(uint64_t) + sizeof(MsMachine) + sizeof(uint32_t);
    if (n < need) return NULL;
    MsCheckpoint *c = calloc(1, sizeof *c);
    size_t o = 0;
    memcpy(&c->slot0, d + o, 1); o += 1;
    memcpy(&c->chosen, d + o, sizeof c->chosen); o += sizeof c->chosen;
    memcpy(&c->frame, d + o, sizeof c->frame); o += sizeof c->frame;
    memcpy(&c->credit, d + o, sizeof c->credit); o += sizeof c->credit;
    memcpy(&c->m, d + o, sizeof c->m); o += sizeof c->m;
    uint32_t sz;
    memcpy(&sz, d + o, sizeof sz); o += sizeof sz;
    if (sz > MS_CRAM_PAGES * 256 || n < o + sz) { free(c); return NULL; }
    c->sram = malloc(sz ? sz : 1);
    if (sz) memcpy(c->sram, d + o, sz);
    c->m.sram = c->sram;
    c->m.sram_size = sz;
    c->m.cart = p->cart;
    c->m.plat = p->plat;
    c->m.trace = NULL;
    c->m.trace_ctx = NULL;
    return c;
}

// writes one snapshot PNG: the frame the player would see plus the machine
static int save_png_of(MsPlay *p, const MsCheckpoint *c) {
    static uint8_t gray[MS_GB_SCREEN_W * MS_GB_SCREEN_H];
    static uint8_t rgb[MS_GB_SCREEN_W * MS_GB_SCREEN_H * 3];
    gb_render_frame(&c->m, gray);
    for (int i = 0; i < MS_GB_SCREEN_W * MS_GB_SCREEN_H; i++) {
        uint8_t v = (uint8_t)(255 - gray[i] * 85);
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v;
    }
    MsBuf pay;
    ms_buf_init(&pay);
    payload_ckpt(&pay, c);
    char path[600];
    snprintf(path, sizeof path, "%s/snap-%03u-cred%llu.png", p->snap_dir, p->snap_seq++,
             (unsigned long long)c->credit);
    int ok = ms_png_save(path, MS_GB_SCREEN_W, MS_GB_SCREEN_H, rgb, pay.p, pay.len);
    ms_buf_free(&pay);
    if (ok) snprintf(p->last_snap, sizeof p->last_snap, "%s", path);
    if (ok && p->cfg.verbose) fprintf(stderr, "memscan: snapshot %s\n", path);
    return ok;
}

// --- human takeover -----------------------------------------------------------------------
void ms_play_human_begin(MsPlay *p, int slot) {
    if (!p->human_on) {
        worker_setup(p, &p->human);
        p->human.mode = IN_HUMAN;
        p->human.id = p->nworkers;
        p->human.my_epoch = ~0u; // never at the worker barrier
        p->human.credit_base = 0;
        p->human.seen_frame = 0;
        p->human_screen = malloc(MS_GB_SCREEN_W * MS_GB_SCREEN_H);
        p->human_on = 1;
    }
    const MsCheckpoint *pick = NULL;
    pthread_mutex_lock(&p->pool.mtx);
    if (slot >= 0 && slot < p->pool.n) {
        pick = p->pool.slot[slot];
    } else { // the most productive checkpoint
        for (int i = 0; i < p->pool.n; i++)
            if (!pick || p->pool.slot[i]->credit > pick->credit) pick = p->pool.slot[i];
    }
    if (pick) ckpt_load(pick, &p->human.m);
    pthread_mutex_unlock(&p->pool.mtx);
    worker_reset_run_state(&p->human);
    p->human.credit_prev = worker_credit(&p->human);
    p->human.credit_base = p->human.credit_prev;
    p->human_png_credit = p->human.credit_prev;
}

void ms_play_human_frame(MsPlay *p, uint8_t buttons) {
    if (!p->human_on || p->quit) return;
    p->human.human_buttons = buttons;
    run_frames(&p->human, 1);
    uint64_t cred = worker_credit(&p->human);
    if (cred > p->human.credit_prev) {
        observe(&p->human, cred);
        p->human.credit_prev = cred;
        // every sizeable advance becomes a start: pool, seeds and PNG
        if (cred - p->human_png_credit >= 32) {
            int slot = promote_pending(&p->human);
            if (slot >= 0) save_png_of(p, p->pool.slot[slot]);
            p->human_png_credit = cred;
        }
    }
}

const uint8_t *ms_play_human_screen(MsPlay *p) {
    if (!p->human_on) return NULL;
    gb_render_frame(&p->human.m, p->human_screen);
    return p->human_screen;
}

int ms_play_human_active(const MsPlay *p) { return p->human_on; }

int ms_play_human_snapshot(MsPlay *p) {
    if (!p->human_on) return 0;
    MsCheckpoint *c = ckpt_take(&p->human.m, p->human.frames_done, worker_credit(&p->human), 0);
    if (!c) return 0;
    // the human's own save also joins the pool: it is a start like any other
    pthread_mutex_lock(&p->pool.mtx);
    int slot;
    if (p->pool.n < p->pool.cap) {
        slot = p->pool.n++;
    } else {
        slot = 1;
        for (int i = 2; i < p->pool.n; i++)
            if (p->pool.slot[i]->chosen > p->pool.slot[slot]->chosen) slot = i;
        ckpt_free(p->pool.slot[slot]);
    }
    p->pool.slot[slot] = c;
    pthread_mutex_unlock(&p->pool.mtx);
    return save_png_of(p, c);
}

int ms_play_load_png(MsPlay *p, const char *path) {
    int w, h;
    uint8_t *rgb, *pay;
    size_t pay_n;
    if (!ms_png_load(path, &w, &h, &rgb, &pay, &pay_n)) {
        fprintf(stderr, "memscan: cannot read PNG %s\n", path);
        return -1;
    }
    free(rgb);
    if (!pay) {
        fprintf(stderr, "memscan: %s has no memscan_snapshot payload\n", path);
        return -1;
    }
    MsCheckpoint *c = ckpt_from_payload(p, pay, pay_n);
    free(pay);
    if (!c) {
        fprintf(stderr, "memscan: %s payload unreadable\n", path);
        return -1;
    }
    pthread_mutex_lock(&p->pool.mtx);
    int slot;
    if (p->pool.n < p->pool.cap) {
        slot = p->pool.n++;
    } else {
        slot = 1;
        for (int i = 2; i < p->pool.n; i++)
            if (p->pool.slot[i]->chosen > p->pool.slot[slot]->chosen) slot = i;
        ckpt_free(p->pool.slot[slot]);
    }
    p->pool.slot[slot] = c;
    pthread_mutex_unlock(&p->pool.mtx);
    return slot;
}

int ms_play_pool_count(MsPlay *p) { return p->pool.n; }

uint16_t ms_play_slot_pc(MsPlay *p, int slot) {
    if (slot < 0 || slot >= p->pool.n) return 0;
    return p->pool.slot[slot]->m.pc;
}

const char *ms_play_last_snapshot(const MsPlay *p) { return p->last_snap[0] ? p->last_snap : NULL; }

int ms_play_done(const MsPlay *p) { return p->quit; }
uint64_t ms_play_frames(const MsPlay *p) { return p->total_frames; }

int ms_play_lanes(MsPlay *p, MsSessionLane *out, int max) {
    pthread_mutex_lock(&p->bar_mtx);
    int n = p->nworkers < max ? p->nworkers : max;
    for (int i = 0; i < n; i++) {
        const MsWorker *w = &p->w[i];
        out[i].id = i;
        out[i].mode = (int)w->mode;
        out[i].frame = w->lane_frame;
        out[i].seg_left = w->lane_seg_left;
        out[i].credit_total = w->credit_total;
        out[i].credit_epoch = w->credit_epoch;
        out[i].stagnant_epochs = w->stagnant_epochs;
        out[i].bank = w->lane_bank;
        out[i].pc = w->lane_pc;
        out[i].buttons = w->lane_buttons;
        out[i].dead = w->lane_dead;
        out[i].backtracked = w->lane_backtracked;
    }
    pthread_mutex_unlock(&p->bar_mtx);
    if (p->human_on && n < max) { // the human lane, read directly (same thread)
        const MsWorker *w = &p->human;
        out[n].id = n;
        out[n].mode = (int)w->mode;
        out[n].frame = w->frames_done;
        out[n].seg_left = 0;
        out[n].credit_total = worker_credit(w);
        out[n].credit_epoch = out[n].credit_total - w->credit_base;
        out[n].stagnant_epochs = 0;
        out[n].bank = (uint8_t)(w->m.pc < 0x4000 ? w->m.win_rom0 : w->m.win_romN);
        out[n].pc = w->m.pc;
        out[n].buttons = w->m.buttons;
        out[n].dead = 0;
        out[n].backtracked = 0;
        n++;
    }
    return n;
}
