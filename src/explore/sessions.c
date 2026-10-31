// sessions.c: the play-session engine.  See sessions.h for the design; the
// input policy and the checkpoint pool follow m0boy's m0_explore.h (the
// coverage machinery behind its --explore profiles).  A checkpoint is a full
// m0boy state image (cartridge RAM included); discovery credit is measured
// by the same first-flip counters the engine hooks fill.
//
// Threading: every worker owns an MsExplorer (its recording context) and one
// machine instance (instances 1..n; instance 0 is the fork explorer's, the
// human lane borrows it while the fork is idle, the last instance is the
// snapshot preview).  The shared MsResults maps are updated through atomics
// and the results mutexes (res->mt stays 1 for the whole play).  A segment
// ends inside the vblank hook: the next checkpoint loads straight into the
// running machine and play continues without leaving gbl_run.
#include "sessions.h"
#include "../core/snapshot_png.h"
#include "../mach/m0boy.h"
#include "tas.h"

#include <signal.h>
#include <string.h>
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
    cfg->tas_ckpt_frames = 240;
    cfg->uniform = 1;
    cfg->uniform_hold = 64;
}

// --- input generation ----------------------------------------------------------
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
    int uniform;              // draw one of the 8 keys, no human priors
    uint32_t uniform_hold;
} MsInputState;

static uint32_t rnd(uint32_t *rng) {
    uint32_t x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x ? x : 0x9e3779b9u;
    return x;
}

static const uint8_t key_tab[8] = { K_A, K_B, 0x04, K_START, K_RIGHT, K_LEFT, K_UP, K_DOWN };

static void next_move(MsInputState *in) {
    if (in->uniform) {
        in->move_keys = key_tab[rnd(&in->rng) % 8]; // one key, uniform
        in->move_left = 1 + rnd(&in->rng) % in->uniform_hold;
        in->tap_left = 0;
        return;
    }
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
    if (!in->uniform) {
        if (in->move_keys & K_START && in->tap_left == 0) in->tap_left = 5;
        if (in->tap_left && !--in->tap_left) in->move_keys &= ~(uint32_t)(K_A | K_B | K_START);
    }
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

// pc of a state image (gbl_cpu_t rides at offset 0 of the gbl state stream)
static uint16_t image_pc(const uint8_t *img) {
    gbl_cpu_t g;
    memcpy(&g, img, sizeof g);
    return g.pc;
}

// --- checkpoint pool -------------------------------------------------------------
typedef struct MsCheckpoint {
    uint8_t *img; // full m0boy state image (cartridge RAM included)
    uint32_t sb;  // image size (variant-shaped)
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
    free(c->img);
    free(c);
}

// --- seed inbox (worker -> fork explorer) ----------------------------------------
typedef struct MsInboxEnt {
    uint8_t *img; // full state image, freed with the entry
} MsInboxEnt;

#define MS_INBOX_CAP 512

// --- play engine ------------------------------------------------------------------
typedef enum { IN_RNG = 0, IN_ATTRACT, IN_SCRIPT, IN_TAS, IN_FIXED, IN_HUMAN } InMode;

typedef struct MsWorker {
    MsPlay *p;
    int id;
    InMode mode;
    pthread_t th;
    int started;
    // recording context: one explorer of our own, sharing the results maps
    MsExplorer *ex;
    MsMach *mach; // instance 1..n (0 for the human lane)
    // input state
    MsInputState in;
    MsScriptEv *script;
    int script_n, script_pos;
    MsTas tas;                 // IN_TAS: the loaded movie (masks per frame)
    uint8_t fixed_mask;
    uint8_t human_buttons; // IN_HUMAN: set by the GUI thread before each frame
    uint8_t applied_mask;  // the mask the current frame applied
    // segment state (m0x)
    uint32_t seg_left;
    uint64_t credit_prev; // credit at the last observe
    int has_pending;
    MsCheckpoint pending;
    // epoch accounting
    uint32_t epoch_target, epoch_done;
    // bookkeeping
    uint32_t frames_done;
    uint32_t seen_frame;      // frames_done as last seen by the scheduler
    uint64_t credit_base;     // credit_total as last seen by the scheduler
    uint64_t credit_epoch;    // credit gained during the current epoch
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
    MsMach *fork_mach;   // instance 0 (human lane borrows it while idle)
    MsMach *preview;     // last instance: renders a checkpoint for PNGs
    pthread_mutex_t preview_mtx;
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
    uint64_t tas_ckpts;
    uint64_t tas_cuts;      // fork-explorer cuts absorbed mid-movie (movie kept)
    uint64_t tas_cut_stuck; // consecutive-cut guard hits (movie then falls to random play)
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

// worker credit: everything its engine hooks discovered so far
static uint64_t worker_credit(const MsWorker *w) {
    return w->ex->credit_exec + w->ex->credit_data + w->ex->credit_copies + w->ex->credit_io +
           w->ex->credit_banks;
}

// fresh per-segment engine bookkeeping (the machine state comes from a load)
static void worker_reset_run_state(MsWorker *w) {
    w->ex->cur_forced = 0;
    w->ex->cur_kind = 3; // MS_SEED_POINTER: real play state, not a fork
    w->ex->cur_from_reset = 0;
    w->ex->irq_depth = 0;
    w->ex->have_last = 0;
    w->ex->cur_sec = 0;
    w->ex->steps_this_path = 0;
    w->ex->pad_slide = 0;
    memset(w->ex->back_ring, 0, sizeof w->ex->back_ring);
    w->ex->back_idx = 0;
    ms_copy_reset(&w->ex->ct);
}

// snapshot the live machine of a worker into a fresh image
static uint8_t *worker_image(MsWorker *w) {
    uint8_t *img = malloc(w->ex->sb);
    if (img) ms_mach_save(w->mach, img);
    return img;
}

// a copy of img into the fork explorer's seed inbox (dropped when full)
static void deposit_img(MsPlay *p, const uint8_t *img) {
    uint8_t *copy = malloc(p->fork->sb);
    if (!copy) return;
    memcpy(copy, img, p->fork->sb);
    pthread_mutex_lock(&p->inb_mtx);
    if (p->inb_n < MS_INBOX_CAP) {
        p->inb[p->inb_n++].img = copy;
    } else {
        free(copy);
        p->inb_dropped++;
    }
    pthread_mutex_unlock(&p->inb_mtx);
}

// m0x_observe: on discovery, keep the current machine as the pending snapshot
static void observe(MsWorker *w, uint64_t credit) {
    if (w->has_pending) free(w->pending.img);
    memset(&w->pending, 0, sizeof w->pending);
    w->pending.img = worker_image(w);
    w->pending.sb = w->ex->sb;
    w->pending.frame = w->frames_done;
    w->pending.credit = credit;
    w->has_pending = w->pending.img != NULL;
}

static int pool_insert(MsPlay *p, MsCheckpoint *c) {
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
    c->slot0 = 0;
    p->pool.slot[slot] = c;
    pthread_mutex_unlock(&p->pool.mtx);
    return slot;
}

// m0x_next's promotion: the pending discovery joins the pool and the seed
// inbox, so every engine (sessions, fork explorer, human gallery) gets it.
// Returns the pool slot it landed in.
static int promote_pending(MsWorker *w) {
    if (!w->has_pending || !w->pending.img) return -1;
    MsPlay *p = w->p;
    deposit_img(p, w->pending.img);
    // the pool owns its own copy: w->pending is reused by the next observe,
    // a pointer into it would be zeroed (img = NULL) under the pool's feet
    MsCheckpoint *c = malloc(sizeof *c);
    if (!c) return -1;
    *c = w->pending; // c takes over pending.img
    int slot = pool_insert(p, c);
    w->has_pending = 0;
    memset(&w->pending, 0, sizeof w->pending);
    return slot;
}

// m0x_next: promote the pending discovery, deposit the end-of-segment state,
// then load the next segment start.  Runs inside the vblank hook (frame
// boundary = instruction boundary) or after a halt cut: the machine keeps
// running from the freshly loaded state.
static void segment_end(MsWorker *w) {
    MsPlay *p = w->p;
    if (w->has_pending) promote_pending(w);
    {
        uint8_t *img = worker_image(w);
        if (img) {
            deposit_img(p, img);
            free(img);
        }
    }

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
    ms_mach_load(w->mach, p->pool.slot[pick]->img);
    pthread_mutex_unlock(&p->pool.mtx);
    w->seg_left = p->cfg.segment_min + rnd(&w->in.rng) % p->cfg.segment_span;
    w->in.move_left = 0;
    w->in.tap_left = 0;
    w->credit_prev = worker_credit(w);
    worker_reset_run_state(w);
}

// --- the vblank hook (one frame) ---------------------------------------------------
static void worker_vblank(MsMach *m) {
    MsWorker *w = ((MsExplorer *)m->owner)->session_ctx;
    MsPlay *p = w->p;
    if (p->quit) {
        ms_mach_cut(m, MS_CUT_QUIT);
        return;
    }
    // input for this frame
    uint8_t mask;
    if (w->mode == IN_ATTRACT) mask = 0;
    else if (w->mode == IN_FIXED) mask = w->fixed_mask;
    else if (w->mode == IN_HUMAN) mask = w->human_buttons; // set by the GUI thread
    else if (w->mode == IN_TAS) {
        if (w->frames_done < w->tas.frames) {
            mask = w->tas.masks[w->frames_done];
        } else { // the movie is over: uniform random play, segments resume
            w->mode = IN_RNG;
            mask = input_keys(&w->in);
            w->seg_left = 1; // rotate away from the movie's end state next frame
        }
    } else if (w->mode == IN_SCRIPT)
        mask = script_keys(w->script, w->script_n, &w->script_pos, w->frames_done);
    else mask = input_keys(&w->in);
    w->applied_mask = mask;
    m->api->set_buttons(mask);

    w->frames_done++;
    w->epoch_done++;
    // TAS gameplay states are the best fork origins there are: populated RAM,
    // real level, real inputs behind them.  The movie's own pace is untouched.
    if (w->mode == IN_TAS && p->cfg.tas_ckpt_frames && w->frames_done % p->cfg.tas_ckpt_frames == 0) {
        uint8_t *img = worker_image(w);
        if (img) {
            deposit_img(p, img);
            free(img);
            __atomic_fetch_add(&p->tas_ckpts, 1, __ATOMIC_RELAXED);
        }
    }
    if (w->seg_left) w->seg_left--;
    uint64_t cred = worker_credit(w);
    if (cred > w->credit_prev) {
        observe(w, cred);
        w->credit_prev = cred;
    }
    ms_copy_flush_path(w->ex, &w->ex->ct);
    if (w->mode == IN_HUMAN) {
        ms_mach_cut(m, MS_CUT_SEGEND); // one frame per call, the human decides
        return;
    }
    if (!w->seg_left) {
        if (w->mode == IN_TAS) {
            // TAS priority: the movie replays in full, no checkpoint rotation
            // mid-movie (the frame index and the machine state must stay in
            // step), no segment budget.  seg_left sits at 0 until the movie
            // ends.
            if (w->epoch_done >= w->epoch_target) ms_mach_cut(m, MS_CUT_SEGEND);
            return;
        }
        segment_end(w); // loads the next checkpoint, play continues
        return;
    }
    if (w->epoch_done >= w->epoch_target) ms_mach_cut(m, MS_CUT_SEGEND);
}

// one scheduler slice: frames of play across as many segments as needed.
// The halt watchdog (m0boy.c) cuts dead halts; the segment continues from a
// fresh checkpoint.
static void run_epoch(MsWorker *w, uint32_t frames) {
    MsPlay *p = w->p;
    w->epoch_target = frames;
    w->epoch_done = 0;
    uint64_t tas_prev_frames = w->frames_done;
    int tas_same = 0;
    while (!p->quit && w->epoch_done < frames) {
        ms_mach_attach(w->mach, w->ex, w->ex->mach->on_step, w->ex->mach->on_read,
                       w->ex->mach->on_write, worker_vblank);
        int cut = ms_mach_run(w->mach);
        // the fork explorer's cuts (pad slide, halt) mean this play state is
        // stuck: rotate to the next checkpoint like a dead halt would.  A TAS
        // mid-replay cannot rewind its movie: it falls through to random play.
        if (cut != MS_CUT_QUIT && cut != MS_CUT_SEGEND) {
            if (w->mode == IN_HUMAN) return; // the human sees the freeze
            if (w->mode == IN_TAS) {
                // TAS priority: the movie is never abandoned for a cut.  The
                // game really is there (halt, padding); resume it, the next
                // vblank feeds the next movie frame.  Guard: if no frame
                // advances over many consecutive cuts the machine is truly
                // wedged — only then fall to random play, counted.
                __atomic_fetch_add(&p->tas_cuts, 1, __ATOMIC_RELAXED);
                if (w->frames_done == tas_prev_frames) {
                    if (++tas_same > 1000) {
                        __atomic_fetch_add(&p->tas_cut_stuck, 1, __ATOMIC_RELAXED);
                        w->mode = IN_RNG;
                        segment_end(w);
                        tas_same = 0;
                    }
                } else tas_same = 0;
                tas_prev_frames = w->frames_done;
                continue;
            }
            segment_end(w); // dead halt or pad slide: rotate the checkpoint
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

        run_epoch(w, p->cfg.epoch_frames);
        // epoch bookkeeping for the scheduler
        pthread_mutex_lock(&p->bar_mtx);
        w->credit_total = worker_credit(w);
        w->lane_frame = w->frames_done;
        w->lane_seg_left = w->seg_left;
        w->lane_pc = w->mach->api->gbr->pc;
        w->lane_bank = (uint8_t)(w->mach->api->gbr->pc < 0x4000
                                     ? 0
                                     : w->mach->api->gbr->rom_bank);
        w->lane_buttons = w->applied_mask;
        w->lane_dead = 0;
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
// pool file v2: every checkpoint is a full state image (variant-shaped), so
// the record is slot0/chosen/frame/credit + the image.  v1 pool files (raw
// MsMachine dumps) are not readable anymore.
int ms_play_save(const MsPlay *p) {
    if (!p->cfg.state_dir) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/pool.bin", p->cfg.state_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const char magic[4] = { 'M', 'S', 'P', 'S' };
    const uint32_t ver = 2;
    uint32_t sb = p->fork->sb;
    fwrite(magic, 1, 4, f);
    fwrite(&ver, sizeof ver, 1, f);
    fwrite(&sb, sizeof sb, 1, f);
    pthread_mutex_lock(&p->pool.mtx);
    uint32_t n = (uint32_t)p->pool.n;
    fwrite(&n, sizeof n, 1, f);
    for (int i = 0; i < p->pool.n; i++) {
        const MsCheckpoint *c = p->pool.slot[i];
        fwrite(&c->slot0, 1, 1, f);
        fwrite(&c->chosen, sizeof c->chosen, 1, f);
        fwrite(&c->frame, sizeof c->frame, 1, f);
        fwrite(&c->credit, sizeof c->credit, 1, f);
        fwrite(c->img, 1, sb, f);
    }
    pthread_mutex_unlock(&p->pool.mtx);
    fwrite(&p->epoch, sizeof p->epoch, 1, f);
    fwrite(&p->total_frames, sizeof p->total_frames, 1, f);
    fwrite(&p->stagnation, sizeof p->stagnation, 1, f);
    fclose(f);
    return 1;
}

static MsCheckpoint *ckpt_read(FILE *f, uint32_t sb) {
    MsCheckpoint *c = calloc(1, sizeof *c);
    if (fread(&c->slot0, 1, 1, f) != 1 || fread(&c->chosen, sizeof c->chosen, 1, f) != 1 ||
        fread(&c->frame, sizeof c->frame, 1, f) != 1 ||
        fread(&c->credit, sizeof c->credit, 1, f) != 1) {
        free(c);
        return NULL;
    }
    c->img = malloc(sb);
    if (!c->img || fread(c->img, 1, sb, f) != sb) {
        ckpt_free(c);
        return NULL;
    }
    c->sb = sb;
    return c;
}

static int pool_load(MsPlay *p) {
    if (!p->cfg.state_dir) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/pool.bin", p->cfg.state_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    uint32_t ver, sb;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MSPS", 4) || fread(&ver, sizeof ver, 1, f) != 1 ||
        ver != 2 || fread(&sb, sizeof sb, 1, f) != 1 || sb != p->fork->sb) {
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
        MsCheckpoint *c = ckpt_read(f, sb);
        if (!c) ok = 0;
        else p->pool.slot[p->pool.n++] = c;
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
// builds the recording context (explorer + machine instance) of one lane;
// used for the thread workers and for the human takeover lane
static void worker_setup(MsPlay *p, MsWorker *w, int instance) {
    w->p = p;
    w->mach = ms_mach_open(p->fork_mach->vi, instance);
    ms_mach_set_rom(w->mach, p->cart->rom, p->cart->rom_size);
    w->mach->api->skip_render[0] = 1;
    MsConfig ecfg = p->fork->cfg;
    ecfg.queue_cap = 1; // session explorers never queue
    w->ex = ms_explorer_new(&ecfg, p->res, w->mach);
    w->ex->session = 1;
    w->ex->session_ctx = w;
    ms_mach_load(w->mach, ms_explorer_seed_base(p->fork)); // power-on state
    worker_reset_run_state(w);
}

MsPlay *ms_play_new(const MsSessionCfg *cfg, MsResults *res, MsExplorer *fork_ex) {
    MsPlay *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cfg = *cfg;
    if (p->cfg.pool_cap > MS_MAX_POOL) p->cfg.pool_cap = MS_MAX_POOL;
    p->res = res;
    p->fork = fork_ex;
    p->fork_mach = fork_ex->mach;
    p->cart = res->cart;
    p->plat = res->plat;
    pthread_mutex_init(&p->bar_mtx, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_mutex_init(&p->pool.mtx, NULL);
    pthread_mutex_init(&p->inb_mtx, NULL);
    pthread_mutex_init(&p->preview_mtx, NULL);
    p->inb = calloc(MS_INBOX_CAP, sizeof(MsInboxEnt));
    p->pool.cap = p->cfg.pool_cap;

    // slot 0: power-on state image
    {
        MsCheckpoint *c = calloc(1, sizeof *c);
        c->img = malloc(fork_ex->sb);
        memcpy(c->img, ms_explorer_seed_base(fork_ex), fork_ex->sb);
        c->sb = fork_ex->sb;
        c->slot0 = 1;
        p->pool.slot[p->pool.n++] = c;
    }

    if (p->cfg.resume && pool_load(p))
        fprintf(stderr, "memscan: resumed %d checkpoints (epoch %u, %llu frames)\n", p->pool.n, p->epoch,
                (unsigned long long)p->total_frames);
    else if (p->cfg.resume)
        fprintf(stderr, "memscan: no usable pool file, starting fresh\n");

    // preview instance: renders checkpoints for the snapshot PNGs
    p->preview = ms_mach_open(fork_ex->mach->vi, MS_GBL_MACHINES - 1);
    ms_mach_set_rom(p->preview, p->cart->rom, p->cart->rom_size);
    p->preview->render = 1;

    // workers: 0 attract, then one per replay script, the rest random
    int maxw = MS_GBL_MACHINES - 2; // instance 0 = fork, last = preview
    if (p->cfg.sessions > maxw) {
        fprintf(stderr, "memscan: %d sessions requested, %d machine instances available: clamping\n",
                p->cfg.sessions, maxw);
        p->cfg.sessions = maxw;
    }
    if (p->cfg.sessions < 1) p->cfg.sessions = 1;
    res->mt = 1;
    p->nworkers = p->cfg.sessions;
    p->workers_done = p->nworkers; // the first poll starts epoch 1
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
        } else if (i - 1 - p->cfg.inputs_n < p->cfg.tas_n && p->cfg.tas[i - 1 - p->cfg.inputs_n]) {
            char terr[256];
            if (ms_tas_load(p->cfg.tas[i - 1 - p->cfg.inputs_n], &w->tas, terr, sizeof terr)) {
                w->mode = IN_TAS;
                fprintf(stderr, "memscan: TAS %s: %u frames%s\n", w->tas.source, w->tas.frames,
                        w->tas.anchored
                            ? " (savestate-anchored: replayed from power-on, best effort)"
                            : "");
            } else {
                fprintf(stderr, "memscan: TAS %s: %s\n", p->cfg.tas[i - 1 - p->cfg.inputs_n], terr);
                w->mode = IN_RNG;
            }
        }
        if (i < p->cfg.fixed_n && p->cfg.fixed[i] && w->mode == IN_RNG) {
            w->mode = IN_FIXED;
            w->fixed_mask = p->cfg.fixed[i];
        }
        w->in.rng = p->cfg.seed ? p->cfg.seed * 2654435761u + (uint32_t)i * 0x9E3779B9u
                                : 0x9e3779b9u + (uint32_t)i * 0x85EBCA6Bu;
        w->in.uniform = p->cfg.uniform;
        w->in.uniform_hold = p->cfg.uniform_hold ? p->cfg.uniform_hold : 64;
        worker_setup(p, w, 1 + i);
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
        ms_mach_close(w->mach);
        free(w->script);
        ms_tas_free(&w->tas);
        free(w->pending.img);
    }
    if (p->human_on) {
        ms_explorer_free(p->human.ex);
        free(p->human.pending.img);
        p->human_on = 0;
    }
    free(p->human_screen);
    for (int i = 0; i < p->pool.n; i++) ckpt_free(p->pool.slot[i]);
    for (int i = 0; i < p->inb_n; i++) free(p->inb[i].img);
    free(p->inb);
    ms_mach_close(p->preview);
    p->res->mt = 0;
    pthread_mutex_destroy(&p->bar_mtx);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->pool.mtx);
    pthread_mutex_destroy(&p->inb_mtx);
    pthread_mutex_destroy(&p->preview_mtx);
    free(p);
}

// --- scheduler ------------------------------------------------------------------------------
static void drain_inbox(MsPlay *p) {
    pthread_mutex_lock(&p->inb_mtx);
    int n = p->inb_n;
    p->inb_n = 0;
    pthread_mutex_unlock(&p->inb_mtx);
    for (int i = 0; i < n; i++) {
        // the image is the state: the fork explorer replays it as deposited
        // (cartridge RAM included), no master-copy dance needed
        ms_explorer_seed(p->fork, p->inb[i].img, 3 /* MS_SEED_POINTER: real play state */, 0);
    }
}

// one scheduler step.  While the workers run, this returns immediately; at an
// epoch boundary it collects the credits, drains the session-seed inbox into
// the fork explorer's queue, gives the fork explorer a slice, re-seeds the
// stagnant sessions (their next segment_end picks the best checkpoint) and
// checks the stop conditions.
uint64_t ms_play_tas_ckpts(const MsPlay *p) { return p->tas_ckpts; }
uint64_t ms_play_tas_cuts(const MsPlay *p) { return p->tas_cuts; }
uint64_t ms_play_tas_cut_stuck(const MsPlay *p) { return p->tas_cut_stuck; }

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
        if (w->credit_epoch) {
            static const char *mn[] = { "rng", "attract", "script", "tas", "fixed", "human" };
            char src[48];
            const char *base = w->tas.source;
            const char *sl = strrchr(base, '/');
            if (sl) base = sl + 1;
            if (w->mode == IN_TAS)
                snprintf(src, sizeof src, "tas:%s", base);
            else
                snprintf(src, sizeof src, "%s s%d", (unsigned)w->mode < 6 ? mn[w->mode] : "?", w->id);
            ms_disc_add(p->res, src, w->credit_epoch);
        }
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
    if (frames) p->stagnation = glob_new ? 0 : p->stagnation + frames;
    p->unexplained = p->cfg.until_static ? ms_results_unexplained(p->res) : 0;

    // sessions -> fork explorer: real play states become exploration seeds
    drain_inbox(p);
    // the fork advances while the sessions wait; bounded by paths, not time,
    // so a fixed-seed run is reproducible
    for (int i = 0; i < p->cfg.fork_paths_per_epoch && p->fork->q_len && !p->fork->stop; i++)
        ms_explore_step(p->fork);

    // stop conditions.  TAS priority: a movie covers a whole playthrough, it
    // outruns every frame budget — max_frames and stagnation wait until every
    // TAS worker has finished its replay (a SIGINT still stops at once).
    int reason = -1;
    int tas_left = 0;
    for (int i = 0; i < p->nworkers; i++)
        if (p->w[i].mode == IN_TAS && p->w[i].frames_done < p->w[i].tas.frames) tas_left = 1;
    if (g_sigint) reason = 2;
    else if (p->cfg.max_frames && !tas_left && p->total_frames >= p->cfg.max_frames) reason = 0;
    else if (p->cfg.stagnation_frames && !tas_left && p->stagnation >= p->cfg.stagnation_frames) reason = 1;
    else if (p->cfg.until_static && !tas_left && p->unexplained == 0 && frames) reason = 3;
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
    ms_buf_push(b, &c->sb, sizeof c->sb);
    ms_buf_push(b, c->img, c->sb);
}

static MsCheckpoint *ckpt_from_payload(const uint8_t *d, size_t n) {
    size_t need = 1 + sizeof(uint32_t) * 3 + sizeof(uint64_t);
    if (n < need) return NULL;
    MsCheckpoint *c = calloc(1, sizeof *c);
    size_t o = 0;
    memcpy(&c->slot0, d + o, 1); o += 1;
    memcpy(&c->chosen, d + o, sizeof c->chosen); o += sizeof c->chosen;
    memcpy(&c->frame, d + o, sizeof c->frame); o += sizeof c->frame;
    memcpy(&c->credit, d + o, sizeof c->credit); o += sizeof c->credit;
    memcpy(&c->sb, d + o, sizeof c->sb); o += sizeof c->sb;
    if (c->sb > MS_CRAM_PAGES * 256 + 0x20000 || n < o + c->sb) {
        free(c);
        return NULL;
    }
    c->img = malloc(c->sb);
    memcpy(c->img, d + o, c->sb);
    return c;
}

// one frame of the preview instance: the checkpoint's state rendered as the
// player would see it
static void preview_vblank(MsMach *m) { ms_mach_cut(m, MS_CUT_SEGEND); }

static void preview_frame(MsPlay *p, const uint8_t *img, uint8_t *gray) {
    pthread_mutex_lock(&p->preview_mtx);
    memset(gray, 3, MS_GB_SCREEN_W * MS_GB_SCREEN_H); // LCD off: dark
    ms_mach_attach(p->preview, NULL, NULL, NULL, NULL, preview_vblank);
    ms_mach_load(p->preview, img);
    p->preview->render = 1;
    p->preview->lines = 0;
    memset(p->preview->screen, 3, sizeof p->preview->screen);
    ms_mach_run(p->preview); // exactly one frame, cut at its vblank
    p->preview->render = 0;
    for (int y = 0; y < MS_GB_SCREEN_H; y++)
        for (int x = 0; x < MS_GB_SCREEN_W; x++)
            gray[y * MS_GB_SCREEN_W + x] = p->preview->screen[y][x];
    pthread_mutex_unlock(&p->preview_mtx);
}

// writes one snapshot PNG: the frame the player would see plus the machine
static int save_png_of(MsPlay *p, const MsCheckpoint *c) {
    static uint8_t gray[MS_GB_SCREEN_W * MS_GB_SCREEN_H];
    static uint8_t rgb[MS_GB_SCREEN_W * MS_GB_SCREEN_H * 3];
    preview_frame(p, c->img, gray);
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
        worker_setup(p, &p->human, 0); // borrows the fork explorer's instance
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
    if (pick) ms_mach_load(p->human.mach, pick->img);
    pthread_mutex_unlock(&p->pool.mtx);
    // the human sees the game: render while the lane plays
    p->human.mach->render = 1;
    p->human.mach->api->skip_render[0] = 0;
    worker_reset_run_state(&p->human);
    p->human.credit_prev = worker_credit(&p->human);
    p->human.credit_base = p->human.credit_prev;
    p->human_png_credit = p->human.credit_prev;
}

void ms_play_human_frame(MsPlay *p, uint8_t buttons) {
    if (!p->human_on || p->quit) return;
    p->human.human_buttons = buttons;
    run_epoch(&p->human, 1);
    if (p->human.mach->lines >= MS_GB_SCREEN_H) {
        for (int y = 0; y < MS_GB_SCREEN_H; y++)
            for (int x = 0; x < MS_GB_SCREEN_W; x++)
                p->human_screen[y * MS_GB_SCREEN_W + x] = p->human.mach->screen[y][x];
        p->human.mach->lines = 0;
    }
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
    return p->human_on ? p->human_screen : NULL;
}

int ms_play_human_active(const MsPlay *p) { return p->human_on; }

int ms_play_human_snapshot(MsPlay *p) {
    if (!p->human_on) return 0;
    MsCheckpoint *c = calloc(1, sizeof *c);
    c->img = worker_image(&p->human);
    if (!c->img) {
        free(c);
        return 0;
    }
    c->sb = p->human.ex->sb;
    c->frame = p->human.frames_done;
    c->credit = worker_credit(&p->human);
    int slot = pool_insert(p, c);
    return save_png_of(p, p->pool.slot[slot]);
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
    MsCheckpoint *c = ckpt_from_payload(pay, pay_n);
    free(pay);
    if (!c) {
        fprintf(stderr, "memscan: %s payload unreadable\n", path);
        return -1;
    }
    if (c->sb != p->fork->sb) {
        fprintf(stderr, "memscan: %s state size does not match this run\n", path);
        ckpt_free(c);
        return -1;
    }
    return pool_insert(p, c);
}

int ms_play_pool_count(MsPlay *p) { return p->pool.n; }

uint16_t ms_play_slot_pc(MsPlay *p, int slot) {
    if (slot < 0 || slot >= p->pool.n) return 0;
    return image_pc(p->pool.slot[slot]->img);
}

const char *ms_play_last_snapshot(const MsPlay *p) { return p->last_snap[0] ? p->last_snap : NULL; }

int ms_play_done(const MsPlay *p) { return p->quit; }
uint64_t ms_play_frames(const MsPlay *p) { return p->total_frames; }

// the checkpoint an export offers: the most productive discovery, the power-on
// slot only when the pool holds nothing else
int ms_play_best_slot(const MsPlay *p) {
    if (p->pool.n <= 0) return -1;
    int best = -1;
    pthread_mutex_lock(&p->pool.mtx);
    for (int i = 0; i < p->pool.n; i++)
        if (p->pool.slot[i]->img &&
            (best < 0 || p->pool.slot[i]->credit > p->pool.slot[best]->credit))
            best = i;
    pthread_mutex_unlock(&p->pool.mtx);
    if (best < 0) best = 0;
    return best;
}

const uint8_t *ms_play_slot_img(const MsPlay *p, int slot) {
    if (slot < 0 || slot >= p->pool.n) return NULL;
    return p->pool.slot[slot]->img;
}

uint32_t ms_play_slot_frame(const MsPlay *p, int slot) {
    if (slot < 0 || slot >= p->pool.n) return 0;
    return p->pool.slot[slot]->frame;
}

int ms_play_lanes(MsPlay *p, MsSessionLane *out, int max) {
    pthread_mutex_lock(&p->bar_mtx);
    int n = p->nworkers < max ? p->nworkers : max;
    for (int i = 0; i < n; i++) {
        const MsWorker *w = &p->w[i];
        out[i].id = i;
        out[i].mode = (int)w->mode; // 0 rng, 1 attract, 2 script, 3 tas, 4 fixed
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
        out[n].bank = (uint8_t)(w->mach->api->gbr->pc < 0x4000
                                    ? 0
                                    : w->mach->api->gbr->rom_bank);
        out[n].pc = w->mach->api->gbr->pc;
        out[n].buttons = w->applied_mask;
        out[n].dead = 0;
        out[n].backtracked = 0;
        n++;
    }
    return n;
}
