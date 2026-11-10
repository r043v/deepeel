// sessions.c: the play-session engine.  See sessions.h for the design; the
// input policy and the checkpoint pool follow m0boy's m0_explore.h (the
// coverage machinery behind its --explore profiles).  A checkpoint is a full
// m0boy state image (cartridge RAM included); discovery credit is measured
// by the same first-flip counters the engine hooks fill.
//
// Threading: every worker owns an DpExplorer (its recording context) and one
// machine instance (instances 1..n; instance 0 is the fork explorer's, the
// human lane borrows it while the fork is idle, the last instance is the
// snapshot preview).  The shared DpResults maps are updated through atomics
// and the results mutexes (res->mt stays 1 for the whole play).  A segment
// ends inside the vblank hook: the next checkpoint loads straight into the
// running machine and play continues without leaving gbl_run.
#include <zlib.h>
#include "gambatte_init.h"
#include "sessions.h"
#include "coverage_defaults.h"
#include "novelty.h"
#include "../core/snapshot_png.h"
#include "ref_feed.h"
#include "../mach/m0boy.h"
#include "tas.h"
#include "../report/report.h"

#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// --- sigint -------------------------------------------------------------------
static volatile sig_atomic_t g_sigint;
static void on_sigint(int sig) { (void)sig; g_sigint = 1; }
void dp_play_install_sigint(void) { signal(SIGINT, on_sigint); }

// --- config -------------------------------------------------------------------
void dp_session_cfg_defaults(DpSessionCfg *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->sessions = 10;
    cfg->epoch_frames = 480;
    cfg->segment_min = 480;
    cfg->segment_span = 1920;
    cfg->pool_cap = 48;
    cfg->stall_epochs = 3;
    cfg->attract_stall = (DP_DEFAULT_ATTRACT_FRAMES + 479u) / 480u;
    cfg->attract_frames = DP_DEFAULT_ATTRACT_FRAMES;
    cfg->yield_window = 0; // the yield stop is opt-in (--yield-window / --yield-min)
    cfg->restart_rate = 8;
    cfg->save_epochs = 10;
    cfg->fork_paths_per_epoch = 200;
    cfg->tas_ckpt_frames = 0; // auto: derived from tas_ckpt_secs and tas_ckpt_max
    cfg->tas_ckpt_secs = DP_DEFAULT_TAS_CHECKPOINT_SECONDS;
    cfg->tas_ckpt_max = 4096;
    cfg->tas_rng = 1;
    cfg->tas_rng_share = 1; // every segment, until each checkpoint has had tas_rng_per of them
    cfg->tas_rng_per = DP_DEFAULT_TAS_RNG_PER_CHECKPOINT;
    cfg->tas_rng_min = DP_DEFAULT_TAS_RNG_STAGNATION_FRAMES;
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

typedef struct DpInputState {
    uint32_t rng;
    uint32_t move_left, move_keys, tap_left;
    int uniform;              // draw one of the 8 keys, no human priors
    int no_start;             // never draw START (TAS-snapshot bursts)
    uint32_t uniform_hold;
} DpInputState;

static uint32_t rnd(uint32_t *rng) {
    uint32_t x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x ? x : 0x9e3779b9u;
    return x;
}

static const uint8_t key_tab[8] = { K_A, K_B, 0x04, K_START, K_RIGHT, K_LEFT, K_UP, K_DOWN };

static void next_move(DpInputState *in) {
    if (in->uniform) {
        do in->move_keys = key_tab[rnd(&in->rng) % 8]; // one key, uniform
        while (in->no_start && in->move_keys == K_START);
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

static uint8_t input_keys(DpInputState *in) {
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
typedef struct { uint32_t frame; uint8_t mask; } DpScriptEv;

static int parse_button_word(const char *w, uint8_t *mask) {
    static const struct { const char *n; uint8_t b; } tab[] = {
        { "a", K_A }, { "b", K_B }, { "select", 0x04 }, { "start", K_START },
        { "right", K_RIGHT }, { "left", K_LEFT }, { "up", K_UP }, { "down", K_DOWN },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(w, tab[i].n)) { *mask |= tab[i].b; return 1; }
    return 0;
}

static int script_load(const char *path, DpScriptEv **evs, int *n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int cap = 64, cnt = 0;
    DpScriptEv *e = malloc(sizeof(DpScriptEv) * (size_t)cap);
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
            e = realloc(e, sizeof(DpScriptEv) * (size_t)cap);
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

static uint8_t script_keys(const DpScriptEv *evs, int n, int *pos, uint32_t frame) {
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
typedef struct DpCheckpoint {
    uint8_t *img; // full m0boy state image (cartridge RAM included)
    uint32_t sb;  // image size (variant-shaped)
    uint32_t frame;
    uint64_t credit;
    uint32_t chosen;
    uint8_t slot0;
    uint32_t tas; // movie index when captured from a TAS replay, DP_MAX_TAS otherwise
} DpCheckpoint;

typedef struct DpPool {
    pthread_mutex_t mtx;
    DpCheckpoint *slot[DP_MAX_POOL];
    int n, cap;
} DpPool;

static void ckpt_free(DpCheckpoint *c) {
    if (!c) return;
    free(c->img);
    free(c);
}

// --- seed inbox (worker -> fork explorer) ----------------------------------------
typedef struct DpInboxEnt {
    uint8_t *img; // full state image, freed with the entry
} DpInboxEnt;

#define DP_INBOX_CAP 512

// --- TAS checkpoint captures (worker -> snapshot writer) --------------------------
// During a TAS replay the machine is captured on the (derived) interval; the
// scheduler thread turns each capture into a snapshot PNG on disk and keeps
// the record for the post-play seeding phase.  The queue only absorbs the
// jitter between both paces; a full queue drops the capture, never blocks.
typedef struct DpTasSnap {
    uint8_t *img; // full state image, freed after the PNG is written
    uint32_t frame;
    uint64_t credit;
    int tas;      // movie index (the --tas order)
    uint16_t bank; // ROM bank the machine PC ran in at capture (backsearch index)
} DpTasSnap;

#define DP_TSN_CAP 64
#define DP_MAX_TAS 1024 // movies one run can carry (the --tas / --tas-dir list)

typedef struct DpTasSnapRec {
    uint32_t chosen; // random-key segments already started from this snapshot
    uint32_t frame;
    uint64_t credit;
    int tas;
    uint16_t bank; // ROM bank at capture (the on-disk index is the filename)
    char path[600];
} DpTasSnapRec;

// defined near the snapshot machinery, used by the scheduler and the resume
static int save_png_path(DpPlay *p, const DpCheckpoint *c, const char *path);
static DpCheckpoint *tas_snap_load(DpPlay *p, const char *path);

// --- play engine ------------------------------------------------------------------
typedef enum { IN_RNG = 0, IN_ATTRACT, IN_SCRIPT, IN_TAS, IN_FIXED, IN_HUMAN } InMode;

typedef struct DpWorker {
    DpPlay *p;
    int id;
    InMode mode;
    pthread_t th;
    int started;
    // recording context: one explorer of our own, sharing the results maps
    DpExplorer *ex;
    DpMach *mach; // instance 1..n (0 for the human lane)
    // input state
    DpInputState in;
    DpScriptEv *script;
    int script_n, script_pos;
    DpTas tas;                 // IN_TAS: the loaded movie (masks per frame)
    int tas_idx;               // IN_TAS: movie index (the --tas order)
    int was_gbhawk;            // the movie just replaced was a GBHawk one
    int reload_pending;        // next queued movie claimed in the vblank hook; the machine is reloaded outside the run
    uint32_t tas_ckpt_every;   // IN_TAS: capture interval in frames (0 = no capture)
    uint8_t fixed_mask;
    uint8_t human_buttons; // IN_HUMAN: set by the GUI thread before each frame
    uint8_t applied_mask;  // the mask the current frame applied
    // segment state (m0x)
    uint32_t seg_left;
    int burst;                // segment started from a TAS snapshot: random keys, ends on stall
    uint32_t burst_idle, burst_len; // frames without a discovery of this run / frames run
    uint64_t credit_prev; // credit at the last observe
    int has_pending;
    DpCheckpoint pending;
    // epoch accounting
    uint32_t epoch_target, epoch_done;
    // bookkeeping
    uint32_t frames_done;
    uint32_t attract_frames_done;
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
    // watch sampling (IN_TAS only): the screen is rendered one frame out of
    // every 1/fps seconds, the rest of the time rendering stays off
    int watch_armed;
    uint8_t watch_img[DP_GB_SCREEN_W * DP_GB_SCREEN_H]; // last sampled screen of this worker's movie (under watch_mtx)
    int watch_img_film;     // 1-based film the image belongs to (0 = none yet)
    uint32_t watch_img_frame, watch_img_total;
    uint64_t watch_next_ns;
} DpWorker;

struct DpPlay {
    DpSessionCfg cfg;
    DpResults *res;
    DpExplorer *fork;
    DpMach *fork_mach;   // instance 0 (human lane borrows it while idle)
    DpMach *preview;     // last instance: renders a checkpoint for PNGs
    pthread_mutex_t preview_mtx;
    const DpCart *cart;
    const DpPlatform *plat;
    DpPool pool;
    pthread_mutex_t cell_mtx;
    DpKeys cell_keys;      // set of seen screen fingerprints (novelty.h)
    uint64_t cells_archived;
    DpWorker w[DP_MAX_SESSIONS];
    int nworkers;
    int tas_next;              // solo queue: next movie index nobody has claimed yet
    // human takeover lane (driven by the GUI thread, not threaded)
    int human_on;
    DpWorker human;
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
    time_t t_start; // wall clock at play start (the --time-limit wall)
    // seed inbox
    pthread_mutex_t inb_mtx;
    DpInboxEnt *inb;
    int inb_n;
    uint64_t inb_dropped;
    // TAS checkpoint captures waiting for their snapshot PNG
    pthread_mutex_t tsn_mtx;
    DpTasSnap tsn[DP_TSN_CAP];
    int tsn_n;
    int tas_taken[DP_MAX_TAS]; // captures already handed over per movie (the cap)
    DpTasSnapRec *tasnaps;     // the snapshots on disk, in movie then frame order
    int tasnaps_n, tasnaps_cap;
    int tas_resume_rec[DP_MAX_TAS]; // resume: latest snapshot record per movie (-1 = none)
    uint64_t tas_ckpts;
    uint64_t tas_bank_hits; // backsearch: snapshots seeded from a residual bank
    uint64_t tas_rng_starts; // random-key segments started from a TAS checkpoint
    uint64_t attract_retired; // attract workers retired to random play (zero yield)
    uint64_t method_yield[5]; // per-method credit of the current epoch (tas,rng,script/other,attract,fixed/human)
    uint64_t yield_hist[5][32]; // ring of per-epoch per-method credit (yield window)
    uint32_t yield_hn; // epochs recorded (capped at 32)
    uint32_t yield_hi; // ring write index
    uint64_t tas_cuts;      // fork-explorer cuts absorbed mid-movie (movie kept)
    uint64_t tas_cut_stuck; // consecutive-cut guard hits (movie then falls to random play)
    uint32_t tas_rom_refused; // movies refused: recorded on another ROM (identity mismatch)
    // watch window feed (--watch): a sampled screen of one replaying movie
    int watch_fps;          // 0 = off
    int watch_sel;          // which TAS worker is shown (index among the movies)
    pthread_mutex_t watch_mtx;
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
static uint64_t worker_credit(const DpWorker *w) {
    return w->ex->credit_exec + w->ex->credit_data + w->ex->credit_copies + w->ex->credit_io +
           w->ex->credit_banks;
}

// fresh per-segment engine bookkeeping (the machine state comes from a load)
static int worker_next_movie(DpWorker *w);
static void worker_apply_movie(DpWorker *w);

static void worker_reset_run_state(DpWorker *w) {
    w->ex->cur_forced = 0;
    w->ex->cur_kind = 3; // DP_SEED_POINTER: real play state, not a fork
    w->ex->cur_from_reset = 0;
    w->ex->irq_depth = 0;
    w->ex->have_last = 0;
    w->ex->cur_sec = 0;
    w->ex->steps_this_path = 0;
    w->ex->pad_slide = 0;
    memset(w->ex->back_ring, 0, sizeof w->ex->back_ring);
    w->ex->back_idx = 0;
    dp_copy_reset(&w->ex->ct);
}

// snapshot the live machine of a worker into a fresh image
static uint8_t *worker_image(DpWorker *w) {
    uint8_t *img = malloc(w->ex->sb);
    if (img) dp_mach_save(w->mach, img);
    return img;
}

// a copy of img into the fork explorer's seed inbox (dropped when full)
static void deposit_img(DpPlay *p, const uint8_t *img) {
    uint8_t *copy = malloc(p->fork->sb);
    if (!copy) return;
    memcpy(copy, img, p->fork->sb);
    pthread_mutex_lock(&p->inb_mtx);
    if (p->inb_n < DP_INBOX_CAP) {
        p->inb[p->inb_n++].img = copy;
    } else {
        free(copy);
        p->inb_dropped++;
        if (p->res) p->res->inbox_dropped++; // surfaced in the frontier report block
    }
    pthread_mutex_unlock(&p->inb_mtx);
}

// m0x_observe: on discovery, keep the current machine as the pending snapshot
static void observe(DpWorker *w, uint64_t credit) {
    if (w->has_pending) free(w->pending.img);
    memset(&w->pending, 0, sizeof w->pending);
    w->pending.img = worker_image(w);
    w->pending.sb = w->ex->sb;
    w->pending.frame = w->frames_done;
    w->pending.credit = credit;
    w->has_pending = w->pending.img != NULL;
}

static int pool_insert(DpPlay *p, DpCheckpoint *c) {
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
static int promote_pending(DpWorker *w) {
    if (!w->has_pending || !w->pending.img) return -1;
    DpPlay *p = w->p;
    deposit_img(p, w->pending.img);
    // the pool owns its own copy: w->pending is reused by the next observe,
    // a pointer into it would be zeroed (img = NULL) under the pool's feet
    DpCheckpoint *c = malloc(sizeof *c);
    if (!c) return -1;
    *c = w->pending; // c takes over pending.img
    int slot = pool_insert(p, c);
    w->has_pending = 0;
    memset(&w->pending, 0, sizeof w->pending);
    return slot;
}

// 1 while a TAS snapshot still owes its random bursts, or one is running: the stagnation wall waits,
// every snapshot gets its tas_rng_per bursts (the time budget and SIGINT still stop the run)
static int tas_rng_pending(const DpPlay *p) {
    if (!p->cfg.tas_rng || !p->cfg.tas_rng_share) return 0;
    for (int i = 0; i < p->nworkers; i++)
        if (p->w[i].burst) return 1;
    for (int i = 0; i < p->tasnaps_n; i++)
        if (p->tasnaps[i].chosen < p->cfg.tas_rng_per) return 1;
    return 0;
}

// m0x_next: promote the pending discovery, deposit the end-of-segment state,
// then load the next segment start.  Runs inside the vblank hook (frame
// boundary = instruction boundary) or after a halt cut: the machine keeps
// running from the freshly loaded state.
static void segment_end(DpWorker *w) {
    DpPlay *p = w->p;
    if (w->has_pending) promote_pending(w);
    {
        uint8_t *img = worker_image(w);
        if (img) {
            deposit_img(p, img);
            free(img);
        }
    }

    w->burst = 0;
    w->in.no_start = 0;
    w->in.uniform = p->cfg.uniform;
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
    // a share of the random-key segments restarts from a TAS checkpoint: the least started one, so
    // every seed of the near-perfect run gets many long random explorations of its neighbourhood
    char tpath[600];
    int use_tas = 0;
    if (w->mode == IN_RNG && p->cfg.tas_rng && p->tasnaps_n > 0 && p->cfg.tas_rng_share &&
        rnd(&w->in.rng) % p->cfg.tas_rng_share == 0) {
        int bi = -1;
        int off = (int)(rnd(&w->in.rng) % (uint32_t)p->tasnaps_n);
        for (int k = 0; k < p->tasnaps_n; k++) {
            int i = (k + off) % p->tasnaps_n;
            if (bi < 0 || p->tasnaps[i].chosen < p->tasnaps[bi].chosen) bi = i;
        }
        if (p->tasnaps[bi].chosen < p->cfg.tas_rng_per) {
            p->tasnaps[bi].chosen++;
            snprintf(tpath, sizeof tpath, "%s", p->tasnaps[bi].path);
            use_tas = 1;
            __atomic_fetch_add(&p->tas_rng_starts, 1, __ATOMIC_RELAXED);
        }
    }
    if (!use_tas) {
        p->pool.slot[pick]->chosen++;
        w->backtracked = (pick != 0);
        dp_mach_load(w->mach, p->pool.slot[pick]->img);
    }
    pthread_mutex_unlock(&p->pool.mtx);
    w->seg_left = p->cfg.segment_min + rnd(&w->in.rng) % p->cfg.segment_span;
    if (use_tas) {
        DpCheckpoint *tc = tas_snap_load(p, tpath);
        if (tc) {
            dp_mach_load(w->mach, tc->img);
            ckpt_free(tc);
            w->backtracked = 1;
            // burst: at least tas_rng_min frames of uniform random keys (START excluded), carried on
            // for as long as the run keeps discovering (it ends after tas_rng_min frames without one)
            w->burst = 1;
            w->burst_idle = 0;
            w->burst_len = 0;
            w->in.no_start = 1;
            w->in.uniform = 1;
            w->seg_left = p->cfg.tas_rng_min;
        } else { // unreadable: the power-on start, as a normal segment
            pthread_mutex_lock(&p->pool.mtx);
            w->backtracked = 0;
            dp_mach_load(w->mach, p->pool.slot[0]->img);
            pthread_mutex_unlock(&p->pool.mtx);
        }
    }
    w->in.move_left = 0;
    w->in.tap_left = 0;
    w->credit_prev = worker_credit(w);
    worker_reset_run_state(w);
}

// --- watch sampling ----------------------------------------------------------------
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int tas_movie_index(const DpPlay *p, const DpWorker *w) {
    int k = 0;
    for (int i = 0; i < p->nworkers; i++) {
        if (&p->w[i] == w) return k;
        if (p->w[i].tas.masks) k++;
    }
    return -1;
}

// called once per movie frame; costs two loads while the window is off
static void watch_sample(DpPlay *p, DpWorker *w, DpMach *m) {
    int fps = __atomic_load_n(&p->watch_fps, __ATOMIC_RELAXED);
    if (!fps || !w->tas.masks) return;
    if (w->watch_armed) { // the frame just finished was rendered in full
        pthread_mutex_lock(&p->watch_mtx);
        for (int y = 0; y < DP_GB_SCREEN_H; y++)
            for (int x = 0; x < DP_GB_SCREEN_W; x++)
                w->watch_img[y * DP_GB_SCREEN_W + x] = m->screen[y][x];
        w->watch_img_film = w->tas_idx + 1;
        w->watch_img_frame = (uint32_t)w->frames_done;
        w->watch_img_total = (uint32_t)w->tas.frames;
        pthread_mutex_unlock(&p->watch_mtx);
        w->watch_armed = 0;
        m->render = 0;
        m->api->skip_render[0] = 1;
    }
    uint64_t now = mono_ns();
    if (now >= w->watch_next_ns) {
        w->watch_next_ns = now + 1000000000ull / (uint64_t)fps;
        w->watch_armed = 1;
        m->render = 1;
        m->api->skip_render[0] = 0;
    }
}

// --- the vblank hook (one frame) ---------------------------------------------------
// --- Go-Explore screen cells (J4) ---------------------------------------------------
// FNV-1a over the background map the LCDC selects (tile indices only) plus the coarse
// scroll: what the player sees changes it, animation and sprites do not.
enum { CELL_SET = 1 << 16 };
static uint64_t screen_key(const DpGblApi *api) {
    uint8_t lcdc = api->io[0x40];
    if (!(lcdc & 0x80)) return 0; // LCD off: nothing shown
    const uint8_t *map = api->vram + ((lcdc & 8) ? 0x1C00 : 0x1800);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 1024; i++) { h ^= map[i]; h *= 1099511628211ull; }
    h ^= (uint64_t)(api->io[0x43] >> 3); h *= 1099511628211ull;
    h ^= (uint64_t)(api->io[0x42] >> 3); h *= 1099511628211ull;
    return h | 1;
}
// 1 when the key is new (and now recorded)
static int cell_new(DpPlay *p, uint64_t k) {
    int fresh = 0;
    pthread_mutex_lock(&p->cell_mtx);
    if (!p->cell_keys.k) dp_keys_init(&p->cell_keys, CELL_SET);
    if (p->cell_keys.k) fresh = dp_keys_add(&p->cell_keys, k);
    pthread_mutex_unlock(&p->cell_mtx);
    return fresh;
}
uint64_t dp_play_cells(const DpPlay *p) { return p->cells_archived; }

static void worker_vblank(DpMach *m) {
    DpWorker *w = ((DpExplorer *)m->owner)->session_ctx;
    DpPlay *p = w->p;
    if (p->quit) {
        dp_mach_cut(m, DP_CUT_QUIT);
        return;
    }
    // desync diagnostics: DEEPEEL_TAS_DUMP=<file> logs "<frame> <fnv(WRAM $C000..$DFFF)> ..."
    // at every movie vblank (compare against a reference emulator's run);
    // DEEPEEL_TAS_DUMPFRAME=<n> writes WRAM+SRAM+HRAM at that frame to /tmp/dp_tas_wram.bin;
    // DEEPEEL_TAS_TRACEFRAME=<n> traces the next frame
    if (w->mode == IN_TAS && w->tas_idx == 0) {
        static FILE *dumpf;
        static int dump_init;
        if (!dump_init) {
            const char *dn = getenv("DEEPEEL_TAS_DUMP");
            dump_init = 1;
            if (dn) dumpf = fopen(dn, "w");
        }
        // DEEPEEL_TAS_REGS=<file>: "<frame> <pc> <sp> <a> <bc> <de> <hl>" at every movie vblank (the
        // frame number counts vblanks seen, 1-based), the format tools/gbhawk_ref/compare_regs.py reads
        static FILE *regsf;
        static int regs_init;
        if (!regs_init) {
            const char *rn = getenv("DEEPEEL_TAS_REGS");
            regs_init = 1;
            if (rn) regsf = fopen(rn, "w");
        }
        if (regsf) {
            fprintf(regsf, "%u %04x %04x %02x %04x %04x %04x cyc=%u\n", (unsigned)w->frames_done + 1, m->api->gbr->pc,
                    m->api->gbr->sp, m->api->gbr->a, m->api->gbr->bc, m->api->gbr->de, m->api->gbr->hl,
                    (unsigned)m->api->gbr->cyc);
            fflush(regsf);
        }
        // DEEPEEL_TAS_RAW=<file>: the 8 KiB of WRAM at every movie vblank, appended (tools/replay_compare.py
        // diffs it byte by byte against a reference emulator's per-frame WRAM)
        {
            static FILE *rawf;
            static int raw_init;
            if (!raw_init) {
                const char *rn = getenv("DEEPEEL_TAS_RAW");
                raw_init = 1;
                if (rn) rawf = fopen(rn, "wb");
            }
            if (rawf) fwrite(m->api->wram, 1, 0x2000, rawf);
            {   // DEEPEEL_TAS_VRAM=<file>: VRAM bank 0 (8 KiB) + OAM (160) + LCDC,SCY,SCX,LYC,BGP,OBP0,OBP1,WY,WX
                // (16 bytes) per movie vblank, to find the first graphic divergence against a reference
                static FILE *vramf; static int vram_init;
                if (!vram_init) { vram_init = 1; const char *vn = getenv("DEEPEEL_TAS_VRAM"); if (vn) vramf = fopen(vn, "wb"); }
                if (vramf) {
                    const uint8_t *io = m->api->io;
                    uint8_t r[16] = {io[0x40], io[0x42], io[0x43], io[0x45], io[0x47], io[0x48], io[0x49], io[0x4A], io[0x4B]};
                    fwrite(m->api->vram, 1, 0x2000, vramf); fwrite(m->api->oam, 1, 0xA0, vramf); fwrite(r, 1, 16, vramf);
                }
            }
            {   // DEEPEEL_TAS_HRAM=<file>: the 128 HRAM bytes per movie vblank (hidden-state comparison)
                static FILE *hramf; static int hram_init;
                if (!hram_init) { hram_init = 1; const char *hn = getenv("DEEPEEL_TAS_HRAM"); if (hn) hramf = fopen(hn, "wb"); }
                if (hramf) { uint8_t hb[0x80] = {0}; memcpy(hb, m->api->hram, 0x7F); fwrite(hb, 1, 0x80, hramf); }
            }
            {   // DEEPEEL_TAS_HRAM=<file>: the 128 HRAM bytes per movie vblank (hidden-state comparison)
                static FILE *hramf; static int hram_init;
                if (!hram_init) { hram_init = 1; const char *hn = getenv("DEEPEEL_TAS_HRAM"); if (hn) hramf = fopen(hn, "wb"); }
                if (hramf) { uint8_t hb[0x80] = {0}; memcpy(hb, m->api->hram, 0x7F); fwrite(hb, 1, 0x80, hramf); }
            }
        }
        if (dumpf) {
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < 0x2000; i++) h = (h ^ m->api->wram[i]) * 1099511628211ull;
            uint64_t hx = 1469598103934665603ull; // WRAM without the stack page $CFxx and the sound page $DFxx
            for (int i = 0; i < 0x2000; i++)
                if ((i >> 8) != 0x0F && (i >> 8) != 0x1F) hx = (hx ^ m->api->wram[i]) * 1099511628211ull;
            uint32_t hh = 2166136261u;
            for (int i = 0; i < 0x7F; i++) if (i != 0x04 && i != 0x05) hh = (hh ^ m->api->hram[i]) * 16777619u;
            fprintf(dumpf, "%u %016llx pc=%04x ime=%u if=%02x ie=%02x 82=%02x 40=%02x LY=%02x hh=%08x h99=%02x hc0=%02x hc6=%02x h97=%02x s8fb=%02x%02x hx=%016llx\n", (unsigned)w->frames_done, (unsigned long long)h,
                    m->api->gbr->pc, m->api->gbr->ime, m->api->io ? m->api->io[0x0F] : 0, m->api->ie ? *m->api->ie : 0, m->api->hram[0x82 - 0x80 + 0], m->api->io[0x40], m->api->io[0x44], hh, m->api->hram[0x19], m->api->hram[0x40], m->api->hram[0x46], m->api->hram[0x17], m->api->eram[0x8fc], m->api->eram[0x8fb], (unsigned long long)hx);
            fflush(dumpf);
            extern int dp_dbg_trace;
            extern FILE *dp_dbg_trace_f;
            const char *tf = getenv("DEEPEEL_TAS_TRACEFRAME");
            if (tf) {
                if (!dp_dbg_trace_f) dp_dbg_trace_f = fopen("/tmp/dp_tas_trace.txt", "w");
                fflush(dp_dbg_trace_f);
                dp_dbg_trace = (unsigned)atoi(tf) == w->frames_done; // trace the frame that follows
            }
            const char *df = getenv("DEEPEEL_TAS_DUMPFRAME");
            if (df && (unsigned)atoi(df) == w->frames_done) {
                FILE *rf = fopen("/tmp/dp_tas_wram.bin", "wb");
                if (rf) { fwrite(m->api->wram, 1, 0x2000, rf); fwrite(m->api->eram, 1, 0x8000, rf); fwrite(m->api->hram, 1, 0x80, rf); fclose(rf); }
            }
        }
    }
    // input for this frame
    uint8_t mask;
    if (w->mode == IN_ATTRACT) mask = 0;
    else if (w->mode == IN_FIXED) mask = w->fixed_mask;
    else if (w->mode == IN_HUMAN) mask = w->human_buttons; // set by the GUI thread
    else if (w->mode == IN_TAS) {
        if (w->frames_done < w->tas.frames) {
            mask = w->tas.masks[w->frames_done];
        } else if (worker_next_movie(w)) { // movie queue: the next movie starts on this worker
            mask = w->tas.masks[0];
        } else { // the movie is over: uniform random play, segments resume
            w->mode = IN_RNG;
            *m->api->sync_div_skew = 0, *m->api->sync_ly_lag = 0, *m->api->sync_irq_trim = 0, *m->api->sync_halt_snd = 0;
            *m->api->sync_vba_off_clock = 0;
            if (*m->api->sync_echo_old) { *m->api->sync_echo_old = 0; m->api->sync_echo_apply(); }
            if (w->watch_armed) { w->watch_armed = 0; m->render = 0; m->api->skip_render[0] = 1; }
            mask = input_keys(&w->in);
            w->seg_left = 1; // rotate away from the movie's end state next frame
        }
    } else if (w->mode == IN_SCRIPT)
        mask = script_keys(w->script, w->script_n, &w->script_pos, w->frames_done);
    else mask = input_keys(&w->in);
    w->applied_mask = mask;
    m->api->set_buttons(mask);
    // the first-seen rows carry the method: the per-byte attribution of
    // methods_pct reads this tag (0 falls back to the legacy bucket)
    w->ex->first_actor = w->mode == IN_TAS ? DP_ACTOR_TAS
                       : w->mode == IN_ATTRACT ? DP_ACTOR_ATTRACT
                       : w->mode == IN_RNG ? DP_ACTOR_RNG
                       : 0;

    if (w->mode == IN_TAS && w->tas_idx == 0) dp_ref_gate((uint32_t)w->frames_done);
    w->frames_done++;
    if (w->mode == IN_ATTRACT && w->attract_frames_done < UINT32_MAX) w->attract_frames_done++;
    w->epoch_done++;
    if (w->mode == IN_TAS) watch_sample(p, w, m);
    // TAS gameplay states are the best fork origins there are: populated RAM,
    // real level, real inputs behind them.  By default the capture only goes
    // to the snapshot writer (the seeding happens after the whole play);
    // --tas-ckpt-live keeps the old direct hand-over to the fork explorer.
    if (w->mode == IN_TAS && w->tas_ckpt_every && w->frames_done % w->tas_ckpt_every == 0) {
        int take = p->cfg.tas_ckpt_live;
        if (!take) {
            pthread_mutex_lock(&p->tsn_mtx);
            take = p->tas_taken[w->tas_idx] < p->cfg.tas_ckpt_max && p->tsn_n < DP_TSN_CAP;
            if (take) p->tas_taken[w->tas_idx]++;
            pthread_mutex_unlock(&p->tsn_mtx);
        }
        if (take) {
            uint8_t *img = worker_image(w);
            if (img) {
                __atomic_fetch_add(&p->tas_ckpts, 1, __ATOMIC_RELAXED);
                if (p->cfg.tas_ckpt_live) deposit_img(p, img);
                else {
                    pthread_mutex_lock(&p->tsn_mtx);
                    if (p->tsn_n < DP_TSN_CAP) {
                        p->tsn[p->tsn_n++] = (DpTasSnap){
                            img, w->frames_done, worker_credit(w), w->tas_idx,
                            (uint16_t)dp_gbl_bank_of(m->api, m->api->gbr->pc)
                        };
                        img = NULL; // the queue entry owns it now
                    }
                    pthread_mutex_unlock(&p->tsn_mtx);
                }
                free(img);
            }
        }
    }
    if (p->cfg.cells && w->mode == IN_RNG && (w->frames_done & 7) == 0) {
        uint64_t k = screen_key(m->api);
        if (k && cell_new(p, k)) {
            DpCheckpoint *c = calloc(1, sizeof *c);
            if (c) {
                c->img = worker_image(w);
                c->sb = w->ex->sb;
                c->frame = (uint32_t)w->frames_done;
                c->credit = worker_credit(w);
                if (c->img) { pool_insert(p, c); __atomic_fetch_add(&p->cells_archived, 1, __ATOMIC_RELAXED); }
                else free(c);
            }
        }
    }
    if (w->seg_left) w->seg_left--;
    uint64_t cred = worker_credit(w);
    if (cred > w->credit_prev) {
        observe(w, cred);
        w->credit_prev = cred;
        w->burst_idle = 0;
    } else if (w->burst) w->burst_idle++;
    if (w->burst) { // the segment goes on until a full stall window passes without a discovery
        w->burst_len++;
        w->seg_left = w->burst_idle >= p->cfg.tas_rng_min ? 0 : 1;
    }
    dp_copy_flush_path(w->ex, &w->ex->ct);
    if (w->mode == IN_HUMAN) {
        dp_mach_cut(m, DP_CUT_SEGEND); // one frame per call, the human decides
        return;
    }
    if (!w->seg_left) {
        if (w->mode == IN_TAS) {
            // TAS priority: the movie replays in full, no checkpoint rotation
            // mid-movie (the frame index and the machine state must stay in
            // step), no segment budget.  seg_left sits at 0 until the movie
            // ends.
            if (w->epoch_done >= w->epoch_target) dp_mach_cut(m, DP_CUT_SEGEND);
            return;
        }
        segment_end(w); // loads the next checkpoint, play continues
        return;
    }
    if (w->epoch_done >= w->epoch_target) dp_mach_cut(m, DP_CUT_SEGEND);
}

// one scheduler slice: frames of play across as many segments as needed.
// The halt watchdog (m0boy.c) cuts dead halts; the segment continues from a
// fresh checkpoint.
static void run_epoch_body(DpWorker *w, uint32_t frames) {
    DpPlay *p = w->p;
    w->epoch_target = frames;
    w->epoch_done = 0;
    uint64_t tas_prev_frames = w->frames_done;
    int tas_same = 0;
    while (!p->quit && w->epoch_done < frames) {
        dp_mach_attach(w->mach, w->ex, w->ex->mach->on_step, w->ex->mach->on_read,
                       w->ex->mach->on_write, worker_vblank);
        int cut = dp_mach_run(w->mach);
        if (w->reload_pending) { // queued movie claimed in the hook: load it here, outside the run
            w->reload_pending = 0;
            worker_apply_movie(w);
            continue;
        }
        // the fork explorer's cuts (pad slide, halt) mean this play state is
        // stuck: rotate to the next checkpoint like a dead halt would.  A TAS
        // mid-replay cannot rewind its movie: it falls through to random play.
        if (cut != DP_CUT_QUIT && cut != DP_CUT_SEGEND) {
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
                        *w->mach->api->sync_div_skew = 0, *w->mach->api->sync_ly_lag = 0, *w->mach->api->sync_irq_trim = 0, *w->mach->api->sync_halt_snd = 0;
                        *w->mach->api->sync_vba_off_clock = 0;
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
// every path out of an epoch publishes this thread's cached section hits and batched step
// count (a stale cache entry must never outlive its DpResults)
static void run_epoch(DpWorker *w, uint32_t frames) {
    dp_section_tc_begin();
    run_epoch_body(w, frames);
    dp_section_flush(w->p->res);
    dp_explorer_flush_steps(w->ex);
}

static void *worker_main(void *arg) {
    DpWorker *w = arg;
    DpPlay *p = w->p;
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
        w->lane_bank = (uint8_t)dp_gbl_bank_of(w->mach->api, w->mach->api->gbr->pc);
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
// pool file v3: every checkpoint is a full state image (variant-shaped), so
// the record is slot0/chosen/frame/credit/tas + the image; then the snapshot
// records (path + frame/credit/tas) and the TAS replay positions, so a
// resume continues each movie where it stopped.  v1/v2 pool files (raw
// DpMachine dumps, then the pre-TAS layout) are not readable anymore.
int dp_play_save(const DpPlay *p) {
    if (!p->cfg.state_dir) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/pool.bin", p->cfg.state_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const char magic[4] = { 'D', 'P', 'P', 'S' };
    const uint32_t ver = 4; // v4: each TAS snapshot record carries its capture bank
    uint32_t sb = p->fork->sb;
    fwrite(magic, 1, 4, f);
    fwrite(&ver, sizeof ver, 1, f);
    fwrite(&sb, sizeof sb, 1, f);
    pthread_mutex_lock(&p->pool.mtx);
    uint32_t n = (uint32_t)p->pool.n;
    fwrite(&n, sizeof n, 1, f);
    for (int i = 0; i < p->pool.n; i++) {
        const DpCheckpoint *c = p->pool.slot[i];
        fwrite(&c->slot0, 1, 1, f);
        fwrite(&c->chosen, sizeof c->chosen, 1, f);
        fwrite(&c->frame, sizeof c->frame, 1, f);
        fwrite(&c->credit, sizeof c->credit, 1, f);
        fwrite(&c->tas, sizeof c->tas, 1, f);
        fwrite(c->img, 1, sb, f);
    }
    pthread_mutex_unlock(&p->pool.mtx);
    fwrite(&p->epoch, sizeof p->epoch, 1, f);
    fwrite(&p->total_frames, sizeof p->total_frames, 1, f);
    fwrite(&p->stagnation, sizeof p->stagnation, 1, f);
    // the TAS snapshot records (so the post-play seeding survives a stop)
    uint32_t tn = (uint32_t)p->tasnaps_n;
    fwrite(&tn, sizeof tn, 1, f);
    for (int i = 0; i < p->tasnaps_n; i++) {
        const DpTasSnapRec *r = &p->tasnaps[i];
        fwrite(&r->frame, sizeof r->frame, 1, f);
        fwrite(&r->credit, sizeof r->credit, 1, f);
        fwrite(&r->tas, sizeof r->tas, 1, f);
        fwrite(&r->bank, sizeof r->bank, 1, f);
        uint32_t pl = (uint32_t)strlen(r->path);
        fwrite(&pl, sizeof pl, 1, f);
        fwrite(r->path, 1, pl, f);
    }
    // Go-Explore cell keys (J4) as a trailer: a resumed run keeps the cells already seen
    pthread_mutex_lock(&p->cell_mtx);
    uint32_t cn = 0;
    if (p->cell_keys.k) cn = p->cell_keys.n;
    fwrite(&cn, sizeof cn, 1, f);
    if (p->cell_keys.k)
        for (uint32_t i = 0; i < CELL_SET; i++)
            if (p->cell_keys.k[i]) fwrite(&p->cell_keys.k[i], sizeof p->cell_keys.k[i], 1, f);
    pthread_mutex_unlock(&p->cell_mtx);
    fclose(f);
    return 1;
}

static DpCheckpoint *ckpt_read(FILE *f, uint32_t sb) {
    DpCheckpoint *c = calloc(1, sizeof *c);
    c->tas = DP_MAX_TAS;
    if (fread(&c->slot0, 1, 1, f) != 1 || fread(&c->chosen, sizeof c->chosen, 1, f) != 1 ||
        fread(&c->frame, sizeof c->frame, 1, f) != 1 ||
        fread(&c->credit, sizeof c->credit, 1, f) != 1 ||
        fread(&c->tas, sizeof c->tas, 1, f) != 1) {
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

static int pool_load(DpPlay *p) {
    if (!p->cfg.state_dir) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/pool.bin", p->cfg.state_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    uint32_t ver, sb;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "DPPS", 4) || fread(&ver, sizeof ver, 1, f) != 1 ||
        ver != 4 || fread(&sb, sizeof sb, 1, f) != 1 || sb != p->fork->sb) {
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
        DpCheckpoint *c = ckpt_read(f, sb);
        if (!c) ok = 0;
        else p->pool.slot[p->pool.n++] = c;
    }
    if (ok && fread(&p->epoch, sizeof p->epoch, 1, f) != 1) ok = 0;
    if (ok && fread(&p->total_frames, sizeof p->total_frames, 1, f) != 1) ok = 0;
    if (ok && fread(&p->stagnation, sizeof p->stagnation, 1, f) != 1) ok = 0;
    if (ok) { // the TAS snapshot records + the replay positions to resume from
        uint32_t tn = 0;
        if (fread(&tn, sizeof tn, 1, f) != 1 || tn > 64u * 1024u) ok = 0;
        for (uint32_t i = 0; ok && i < tn; i++) {
            DpTasSnapRec r = { .tas = DP_MAX_TAS };
            uint32_t pl = 0;
            if (fread(&r.frame, sizeof r.frame, 1, f) != 1 ||
                fread(&r.credit, sizeof r.credit, 1, f) != 1 ||
                fread(&r.tas, sizeof r.tas, 1, f) != 1 ||
                fread(&r.bank, sizeof r.bank, 1, f) != 1 ||
                fread(&pl, sizeof pl, 1, f) != 1 || pl >= sizeof r.path ||
                fread(r.path, 1, pl, f) != pl) {
                ok = 0;
                break;
            }
            r.path[pl] = 0;
            if (p->tasnaps_n == p->tasnaps_cap) {
                p->tasnaps_cap = p->tasnaps_cap ? p->tasnaps_cap * 2 : 64;
                p->tasnaps = realloc(p->tasnaps, (size_t)p->tasnaps_cap * sizeof *p->tasnaps);
            }
            p->tasnaps[p->tasnaps_n++] = r;
        }
        // the resume point of each movie: its latest snapshot record — the
        // captures live on disk (PNG), not in the pool
        if (ok)
            for (int i = 0; i < p->tasnaps_n; i++) {
                DpTasSnapRec *r = &p->tasnaps[i];
                if (r->tas >= DP_MAX_TAS) continue;
                int cur = p->tas_resume_rec[r->tas];
                if (cur < 0 || r->frame >= p->tasnaps[cur].frame) p->tas_resume_rec[r->tas] = i;
            }
        // the cell-key trailer (absent in older pool files: tolerated)
        uint32_t cn = 0;
        if (ok && fread(&cn, sizeof cn, 1, f) == 1 && cn <= CELL_SET / 2)
            for (uint32_t i = 0; i < cn; i++) {
                uint64_t k;
                if (fread(&k, sizeof k, 1, f) != 1) break;
                cell_new(p, k);
            }
    }
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
static void worker_setup(DpPlay *p, DpWorker *w, int instance) {
    w->p = p;
    w->mach = dp_mach_open(p->fork_mach->vi, instance);
    dp_mach_set_rom(w->mach, p->cart->rom, p->cart->rom_size);
    w->mach->api->skip_render[0] = 1;
    DpConfig ecfg = p->fork->cfg;
    ecfg.queue_cap = 1; // session explorers never queue
    w->ex = dp_explorer_new(&ecfg, p->res, w->mach);
    w->ex->session = 1;
    w->ex->session_ctx = w;
    dp_mach_load(w->mach, dp_explorer_seed_base(p->fork)); // power-on state
    worker_reset_run_state(w);
}

// load movie t into worker w (mode IN_TAS on success, IN_RNG otherwise)
static int worker_load_movie(DpPlay *p, DpWorker *w, int t) {
    char terr[256];
    int loaded = dp_tas_load(p->cfg.tas[t], &w->tas, terr, sizeof terr);
    if (loaded) {
        char why[256];
        int match = dp_tas_rom_match(&w->tas, p->cart->rom, p->cart->rom_size, why, sizeof why);
        if (match == 0 && !p->cfg.tas_any_rom) {
            // a movie for another revision desyncs by construction:
            // refused, reported (never silently replayed)
            fprintf(stderr, "deepeel: TAS %s refused: recorded on another ROM (%s); --tas-any-rom forces it\n",
                    w->tas.source, why);
            p->tas_rom_refused++;
            dp_tas_free(&w->tas);
            loaded = 0;
        } else if (match == 0) {
            fprintf(stderr, "deepeel: TAS %s: recorded on another ROM (%s), replayed anyway (--tas-any-rom)\n",
                    w->tas.source, why);
        } else if (match == 1) {
            fprintf(stderr, "deepeel: TAS %s: ROM identity verified (%s)\n", w->tas.source, why);
        }
    }
    if (loaded) {
        w->mode = IN_TAS;
        w->tas_idx = t;
        // capture interval: both constraints are lower bounds on the
        // interval — the pace (tas_ckpt_secs of game time) and the
        // cap (the whole movie yields at most tas_ckpt_max snapshots)
        w->tas_ckpt_every = p->cfg.tas_ckpt_frames;
        if (!w->tas_ckpt_every && p->cfg.tas_ckpt_secs && p->cfg.tas_ckpt_max > 0) {
            uint32_t by_cap = w->tas.frames / (uint32_t)p->cfg.tas_ckpt_max;
            uint32_t by_secs = p->cfg.tas_ckpt_secs * 60; // 60 fps game time
            w->tas_ckpt_every = by_cap > by_secs ? by_cap : by_secs;
        }
        fprintf(stderr, "deepeel: TAS %s: %u frames%s%s\n", w->tas.source, w->tas.frames,
                w->tas.anchored
                    ? " (savestate-anchored: replayed from power-on, best effort)"
                    : "",
                w->tas_ckpt_every
                    ? ""
                    : " (no checkpoint capture)");
        if (w->tas_ckpt_every)
            fprintf(stderr, "deepeel: TAS %d: checkpoint snapshot every %u frames (max %d)\n",
                    w->tas_idx, w->tas_ckpt_every,
                    p->cfg.tas_ckpt_max > 0 ? p->cfg.tas_ckpt_max : 0);
    } else {
        fprintf(stderr, "deepeel: TAS %s: %s\n", p->cfg.tas[t], terr);
        w->mode = IN_RNG;
    }
    return w->mode == IN_TAS;
}

// per-recorder machine tuning for the movie loaded in w (sync profiles, frame 0 input)
static void worker_tune_movie(DpPlay *p, DpWorker *w) {
    // the recording emulator's timing quirks (gbl.h movie sync
    // knobs), for this worker's machine only and while the movie
    // plays: random play after it runs on hardware timing again
    // BizHawk (Gambatte) powers a cartridge up with blank battery RAM
    // filled with $FF: games that read their save slot (Mario Land 2)
    // branch on it, so a zeroed RAM replays another game
    if ((w->tas.recorder == DP_TAS_REC_BIZHAWK || (w->tas.recorder == DP_TAS_REC_VBA && w->tas.recorder_rev == 0 && !getenv("DEEPEEL_TAS_NOINIT"))) && w->mach->api->eram)
        memset(w->mach->api->eram, 0xFF, dp_gbl_variant_eram[w->mach->api->variant]);
    // VBA-rr's V8 core (VBM minor version 1) powers WRAM up with a fixed
    // pattern ($FF x8 / $0F x8, blocks of $800 alternating), HRAM with $FF
    // (observed on the VBA-rr headless harness); the V7 core (minor 0)
    // powers up zeroed, like m0boy: uninitialised reads depend on it
    if (w->tas.recorder == DP_TAS_REC_VBA && w->tas.recorder_rev >= 1 && !getenv("DEEPEEL_TAS_NOINIT")) {
        for (int i = 0; i < 0x2000; i++)
            w->mach->api->wram[i] = (((i >> 3) ^ (i >> 11)) & 1) ? 0x0F : 0xFF;
        memset(w->mach->api->hram, 0xFF, 0x7F);
    }
    // BizHawk's Gambatte starts from a fixed power-on RAM dump, not zeros
    if (w->tas.recorder == DP_TAS_REC_BIZHAWK && w->tas.gambatte && !getenv("DEEPEEL_TAS_NOINIT")) {
        memcpy(w->mach->api->wram, dp_gambatte_wram, 0x2000);
        memcpy(w->mach->api->hram, dp_gambatte_hram, 0x7F);
    }
    // BizHawk's GBHawk (gbl.h): the pad is latched at the vblank
    // edge inside its fixed frame (65,664 cycles to LY 144); the
    // worker applies the next mask just after the m0boy vblank, so
    // the pad needs a 14,481-cycle delay (measured window
    // 14,470..14,550; the PPU phase below moves the m0boy vblank
    // edge but the pad window keeps 14,481).  The divider phase at
    // $0100 is 0xFE4D, and GBHawk is 405 cycles into line 153
    // there (LY zeroed by its cycle-6 rule; gbl.h, sync_ppu_phase:
    // applied to the loaded state, it makes every LY read match).
    // GBHawk keeps the WRAM zero (no power-on dump) and its frames
    // are always 70224 cycles, like the vblank clock: no mode-3
    // clock change applies.
    if (w->tas.recorder == DP_TAS_REC_BIZHAWK && w->tas.gbhawk) {
        *w->mach->api->sync_pad_delay = getenv("DEEPEEL_TAS_PADDELAY")
            ? (uint32_t)strtoul(getenv("DEEPEEL_TAS_PADDELAY"), NULL, 0) : 14481u;
        *w->mach->api->sync_div_extra = getenv("DEEPEEL_TAS_DIVEXTRA")
            ? (uint16_t)strtoul(getenv("DEEPEEL_TAS_DIVEXTRA"), NULL, 0) : 0xFE4Du;
        *w->mach->api->sync_ppu_phase = getenv("DEEPEEL_TAS_PPUPHASE")
            ? (uint16_t)strtoul(getenv("DEEPEEL_TAS_PPUPHASE"), NULL, 0) : 405u;
        w->mach->api->sync_ppu_apply();
        fprintf(stderr, "deepeel: TAS %s: GBHawk sync profile (pad latch +14481 cycles, DIV phase $FE4D, PPU phase 405)\n",
                w->tas.source);
    }
    // BizHawk's frame clock (gbl.h, sync_vba_off_clock 3): a frame ends at
    // the video frame or when its sample budget runs out
    if (w->tas.recorder == DP_TAS_REC_BIZHAWK && w->tas.gambatte && !w->tas.equal_frames && !getenv("DEEPEEL_TAS_NOMODE3"))
        *w->mach->api->sync_vba_off_clock = 3;
    // Gambatte's DIV phase against the m0boy clock, measured on SML2 (DIV
    // reads that seed its RNG): any skew in 25..64 matches, 40 is mid-range.
    if (w->tas.recorder == DP_TAS_REC_BIZHAWK && w->tas.gambatte)
        *w->mach->api->sync_div_skew = getenv("DEEPEEL_TAS_DIVSKEW") ? (uint8_t)atoi(getenv("DEEPEEL_TAS_DIVSKEW")) : 40;
    if (w->tas.echo_old && !getenv("DEEPEEL_TAS_NOECHO")) {
        *w->mach->api->sync_echo_old = 1;
        w->mach->api->sync_echo_apply();
        fprintf(stderr, "deepeel: TAS %s: VBA echoRAMFix off ($E000-$FDFF is separate memory)\n", w->tas.source);
    }
    int vba_old = w->tas.recorder == DP_TAS_REC_VBA && w->tas.recorder_rev == 0;
    if (vba_old) {
        // VBA 1.7.2 based recorders (VBM minor version 0): input
        // read at each vblank, no frame while the LCD is off, a
        // ten-line line 0 after an enable.  Partial: their CPU
        // timing quirks (cycle table, HALT wake-up) stay hardware
        *w->mach->api->sync_vba_off_clock = getenv("DEEPEEL_TAS_OFFCLOCK")
            ? (uint8_t)atoi(getenv("DEEPEEL_TAS_OFFCLOCK")) : 2;
        // V7 reads LY at the start of the reading instruction (gbl.h, sync_ly_lag)
        *w->mach->api->sync_ly_lag = getenv("DEEPEEL_TAS_LYLAG")
            ? (int16_t)atoi(getenv("DEEPEEL_TAS_LYLAG")) : 1;
        // V7 counts no tick for the interrupt dispatch (20 cycles on the hardware)
        *w->mach->api->sync_irq_trim = getenv("DEEPEEL_TAS_IRQTRIM")
            ? (int16_t)atoi(getenv("DEEPEEL_TAS_IRQTRIM")) : 20;
        // V7 wakes a halted CPU on its 96-cycle sound grid (gbl.h, sync_halt_snd);
        // DEEPEEL_TAS_HALTSND = grid phase in cycles, -1 = hardware wake-up
        {
            const char *hs = getenv("DEEPEEL_TAS_HALTSND");
            *w->mach->api->sync_halt_snd = (int16_t)((hs ? atoi(hs) : 0) + 1);
        }
        fprintf(stderr, "deepeel: TAS %s: VBA 1.7.2 sync profile (input at vblank, no LCD-off frame, "
                        "ten-line enable)\n", w->tas.source);
    } else if (w->tas.recorder == DP_TAS_REC_VBA) {
        *w->mach->api->sync_div_skew = 4;
        *w->mach->api->sync_vba_off_clock = 1;
        fprintf(stderr, "deepeel: TAS %s: VBA sync profile (DIV read +4 cycles, VBA LCD-off frame clock)\n",
                w->tas.source);
    }
    // movie frame 0 is the power-on frame: its input is held from
    // the first instruction (VBA-rr v23.1+ and BizHawk), each vblank
    // then feeds the next frame.  The VBA 1.7.2 cores read the pad
    // at the vblank instead: frame 0 lands at the first one.
    if (w->tas.frames) {
        w->mach->api->set_buttons(w->tas.masks[0]);
        w->applied_mask = w->tas.masks[0];
        w->frames_done = 1;
    }
}

// Movie queue: when a worker's movie is over and unclaimed movies remain, load
// the next one on this same machine (power-on state, fresh sync profile).  Every
// movie of the list gets replayed even when there are more movies than machine
// instances; a movie refused (other ROM, unreadable) is skipped, already reported.
static void worker_apply_movie(DpWorker *w);
static int worker_next_movie(DpWorker *w) {
    DpPlay *p = w->p;
    for (;;) {
        int t = __atomic_fetch_add(&p->tas_next, 1, __ATOMIC_RELAXED);
        if (t >= p->cfg.tas_n) {
            __atomic_store_n(&p->tas_next, p->cfg.tas_n, __ATOMIC_RELAXED);
            return 0;
        }
        w->was_gbhawk = w->tas.recorder == DP_TAS_REC_BIZHAWK && w->tas.gbhawk;
        dp_tas_free(&w->tas);
        memset(&w->tas, 0, sizeof w->tas);
        if (!worker_load_movie(p, w, t)) continue;
        w->tas_idx = t;
        // called from the vblank hook, i.e. in the middle of an instruction:
        // the machine must not be reloaded here.  Cut at the next instruction
        // boundary; run_epoch then calls worker_apply_movie outside the run.
        w->reload_pending = 1;
        w->frames_done = 0;
        w->mach->cut_pending = DP_CUT_SEGEND;
        return 1;
    }
}

// the machine side of a queued movie start: power-on state, fresh sync profile
static void worker_apply_movie(DpWorker *w) {
    DpPlay *p = w->p;
    dp_mach_load(w->mach, dp_explorer_seed_base(p->fork)); // power-on state
    worker_reset_run_state(w);
    *w->mach->api->sync_div_skew = 0, *w->mach->api->sync_ly_lag = 0, *w->mach->api->sync_irq_trim = 0, *w->mach->api->sync_halt_snd = 0;
    *w->mach->api->sync_vba_off_clock = 0;
    if (*w->mach->api->sync_echo_old) { *w->mach->api->sync_echo_old = 0; w->mach->api->sync_echo_apply(); }
    if (w->was_gbhawk) {
        *w->mach->api->sync_pad_delay = 0;
        *w->mach->api->sync_div_extra = 0;
        *w->mach->api->sync_ppu_phase = 0;
    }
    memset(w->mach->api->eram, 0, dp_gbl_variant_eram[w->mach->api->variant]);
    w->frames_done = 0;
    w->applied_mask = 0;
    w->seg_left = 0;
    w->burst = 0;
    worker_tune_movie(p, w);
    fprintf(stderr, "deepeel: TAS queue: worker %d starts movie %d/%d\n", w->id, w->tas_idx + 1, p->cfg.tas_n);
}

DpPlay *dp_play_new(const DpSessionCfg *cfg, DpResults *res, DpExplorer *fork_ex) {
    DpPlay *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cfg = *cfg;
    if (p->cfg.pool_cap > DP_MAX_POOL) p->cfg.pool_cap = DP_MAX_POOL;
    p->res = res;
    p->fork = fork_ex;
    p->fork_mach = fork_ex->mach;
    p->cart = res->cart;
    p->plat = res->plat;
    pthread_mutex_init(&p->bar_mtx, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_mutex_init(&p->pool.mtx, NULL);
    pthread_mutex_init(&p->cell_mtx, NULL);
    pthread_mutex_init(&p->inb_mtx, NULL);
    pthread_mutex_init(&p->preview_mtx, NULL);
    pthread_mutex_init(&p->watch_mtx, NULL);
    pthread_mutex_init(&p->tsn_mtx, NULL);
    for (int k = 0; k < DP_MAX_TAS; k++) p->tas_resume_rec[k] = -1;
    p->t_start = time(NULL);
    p->inb = calloc(DP_INBOX_CAP, sizeof(DpInboxEnt));
    p->pool.cap = p->cfg.pool_cap;

    // slot 0: power-on state image
    {
        DpCheckpoint *c = calloc(1, sizeof *c);
        c->img = malloc(fork_ex->sb);
        memcpy(c->img, dp_explorer_seed_base(fork_ex), fork_ex->sb);
        c->sb = fork_ex->sb;
        c->slot0 = 1;
        p->pool.slot[p->pool.n++] = c;
    }

    if (p->cfg.resume && pool_load(p))
        fprintf(stderr, "deepeel: resumed %d checkpoints (epoch %u, %llu frames)\n", p->pool.n, p->epoch,
                (unsigned long long)p->total_frames);
    else if (p->cfg.resume)
        fprintf(stderr, "deepeel: no usable pool file, starting fresh\n");

    // preview instance: renders checkpoints for the snapshot PNGs
    p->preview = dp_mach_open(fork_ex->mach->vi, DP_GBL_MACHINES - 1);
    dp_mach_set_rom(p->preview, p->cart->rom, p->cart->rom_size);
    p->preview->render = 1;

    // workers: 0 attract, then one per replay script, the rest random —
    // or, tas_solo, only one worker per movie and nobody else
    int maxw = DP_GBL_MACHINES - 2; // instance 0 = fork, last = preview
    if (p->cfg.tas_solo) {
        // solo with no playable movie (every one unreadable or recorded on
        // another ROM) would stop at frame 0: keep the normal crowd instead,
        // reported (the refusals themselves are logged by the workers)
        int playable = 0, refused = 0;
        for (int t = 0; t < p->cfg.tas_n; t++) {
            DpTas probe;
            char terr[256], why[256];
            memset(&probe, 0, sizeof probe);
            if (!p->cfg.tas[t] || !dp_tas_load(p->cfg.tas[t], &probe, terr, sizeof terr)) continue;
            if (!p->cfg.tas_any_rom && dp_tas_rom_match(&probe, p->cart->rom, p->cart->rom_size, why, sizeof why) == 0)
                refused++;
            else
                playable++;
            dp_tas_free(&probe);
        }
        if (!playable) {
            fprintf(stderr, "deepeel: TAS solo: no movie loaded (%d refused), random play until the time limit\n",
                    refused);
            p->cfg.tas_solo = 0;
        }
    }
    if (p->cfg.tas_solo && p->cfg.tas_n > 0) p->cfg.sessions = p->cfg.tas_n;
    if (p->cfg.sessions > maxw) {
        fprintf(stderr, "deepeel: %d sessions requested, %d machine instances available: clamping\n",
                p->cfg.sessions, maxw);
        p->cfg.sessions = maxw;
    }
    if (p->cfg.sessions < 1) p->cfg.sessions = 1;
    res->mt = 1;
    p->nworkers = p->cfg.sessions;
    {   // movies the initial workers take: the rest is queued for the first worker to finish
        int first = p->cfg.tas_solo ? p->nworkers : p->nworkers - 1 - p->cfg.inputs_n;
        if (first < 0) first = 0;
        p->tas_next = first < p->cfg.tas_n ? first : p->cfg.tas_n;
    }
    p->workers_done = p->nworkers; // the first poll starts epoch 1
    for (int i = 0; i < p->nworkers; i++) {
        DpWorker *w = &p->w[i];
        w->p = p;
        w->id = i;
        w->mode = IN_RNG;
        int t = p->cfg.tas_solo ? i : i - 1 - p->cfg.inputs_n; // TAS movie index
        if (i == 0 && !p->cfg.tas_solo) w->mode = IN_ATTRACT;
        else if (!p->cfg.tas_solo && i - 1 < p->cfg.inputs_n && p->cfg.inputs[i - 1]) {
            w->mode = IN_SCRIPT;
            if (!script_load(p->cfg.inputs[i - 1], &w->script, &w->script_n)) {
                fprintf(stderr, "deepeel: cannot read input script %s\n", p->cfg.inputs[i - 1]);
                w->mode = IN_RNG;
            }
        } else if (t >= 0 && t < p->cfg.tas_n && p->cfg.tas[t]) {
            worker_load_movie(p, w, t);
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
        if (w->mode == IN_TAS) {
            worker_tune_movie(p, w);
        }
        // resume: a TAS worker continues its movie at its latest snapshot
        if (w->mode == IN_TAS && p->cfg.resume && p->tas_resume_rec[w->tas_idx] >= 0) {
            const DpTasSnapRec *r = &p->tasnaps[p->tas_resume_rec[w->tas_idx]];
            DpCheckpoint *c = tas_snap_load(p, r->path);
            if (c) {
                dp_mach_load(w->mach, c->img);
                w->frames_done = c->frame;
                fprintf(stderr, "deepeel: TAS %d resumed at frame %u: %s\n", w->tas_idx, c->frame,
                        r->path);
                ckpt_free(c);
            } else {
                fprintf(stderr, "deepeel: TAS %d: resume snapshot unreadable (%s), replay from power-on\n",
                        w->tas_idx, r->path);
            }
        }
        w->seg_left = p->cfg.segment_min + rnd(&w->in.rng) % p->cfg.segment_span;
        w->my_epoch = 0;
        if (pthread_create(&w->th, NULL, worker_main, w) == 0) w->started = 1;
    }
    // snapshot gallery directory (next to the pool file, or beside the run)
    if (p->cfg.state_dir && *p->cfg.state_dir) {
        snprintf(p->snap_dir, sizeof p->snap_dir, "%s/snapshots", p->cfg.state_dir);
        mkdir(p->cfg.state_dir, 0755); // the parent must exist for the mkdir below
        mkdir(p->snap_dir, 0755);
    } else {
        snprintf(p->snap_dir, sizeof p->snap_dir, "deepeel-snaps");
        mkdir(p->snap_dir, 0755);
    }
    return p;
}

void dp_play_free(DpPlay *p) {
    if (!p) return;
    dp_play_finish(p); // stop and join the workers, whatever the caller did
    for (int i = 0; i < p->nworkers; i++) {
        DpWorker *w = &p->w[i];
        if (w->started) pthread_join(w->th, NULL);
        dp_explorer_free(w->ex);
        dp_mach_close(w->mach);
        free(w->script);
        dp_tas_free(&w->tas);
        free(w->pending.img);
    }
    if (p->human_on) {
        dp_explorer_free(p->human.ex);
        free(p->human.pending.img);
        p->human_on = 0;
    }
    free(p->human_screen);
    for (int i = 0; i < p->pool.n; i++) ckpt_free(p->pool.slot[i]);
    for (int i = 0; i < p->inb_n; i++) free(p->inb[i].img);
    free(p->inb);
    for (int i = 0; i < p->tsn_n; i++) free(p->tsn[i].img);
    free(p->tasnaps);
    dp_mach_close(p->preview);
    p->res->mt = 0;
    pthread_mutex_destroy(&p->bar_mtx);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->pool.mtx);
    pthread_mutex_destroy(&p->cell_mtx);
    dp_keys_free(&p->cell_keys);
    pthread_mutex_destroy(&p->inb_mtx);
    pthread_mutex_destroy(&p->preview_mtx);
    pthread_mutex_destroy(&p->watch_mtx);
    pthread_mutex_destroy(&p->tsn_mtx);
    free(p);
}

// --- scheduler ------------------------------------------------------------------------------
static void drain_inbox(DpPlay *p) {
    pthread_mutex_lock(&p->inb_mtx);
    int n = p->inb_n;
    p->inb_n = 0;
    pthread_mutex_unlock(&p->inb_mtx);
    for (int i = 0; i < n; i++) {
        // the image is the state: the fork explorer replays it as deposited
        // (cartridge RAM included), no master-copy dance needed
        dp_explorer_seed(p->fork, p->inb[i].img, 3 /* DP_SEED_POINTER: real play state */, 0);
    }
}

// TAS captures -> snapshot PNGs on disk + records kept for the post-play
// seeding phase (and for the resume point)
static void drain_tas_snaps(DpPlay *p) {
    for (;;) {
        pthread_mutex_lock(&p->tsn_mtx);
        if (!p->tsn_n) {
            pthread_mutex_unlock(&p->tsn_mtx);
            return;
        }
        DpTasSnap s = p->tsn[0];
        memmove(&p->tsn[0], &p->tsn[1], (size_t)(p->tsn_n - 1) * sizeof s);
        p->tsn_n--;
        pthread_mutex_unlock(&p->tsn_mtx);
        const DpCheckpoint c = { s.img, p->fork->sb, s.frame, s.credit, 0, 0, (uint32_t)s.tas };
        char path[600];
        // the bank in the filename is the on-disk bank->states index: a
        // backsearch targeting a residual bank finds its states by name
        snprintf(path, sizeof path, "%s/tas%02d-b%u-f%u.png", p->snap_dir, s.tas, s.bank, s.frame);
        if (save_png_path(p, &c, path)) {
            pthread_mutex_lock(&p->pool.mtx); // the random-key workers read this list
            if (p->tasnaps_n == p->tasnaps_cap) {
                p->tasnaps_cap = p->tasnaps_cap ? p->tasnaps_cap * 2 : 64;
                p->tasnaps = realloc(p->tasnaps, (size_t)p->tasnaps_cap * sizeof *p->tasnaps);
            }
            DpTasSnapRec *r = &p->tasnaps[p->tasnaps_n++];
            r->chosen = 0;
            r->frame = s.frame;
            r->credit = s.credit;
            r->tas = s.tas;
            r->bank = s.bank;
            snprintf(r->path, sizeof r->path, "%s", path);
            pthread_mutex_unlock(&p->pool.mtx);
        } else {
            fprintf(stderr, "deepeel: TAS snapshot save failed: %s\n", path);
        }
        free(s.img);
    }
}

// one scheduler step.  While the workers run, this returns immediately; at an
// epoch boundary it collects the credits, drains the session-seed inbox into
// the fork explorer's queue, gives the fork explorer a slice, re-seeds the
// stagnant sessions (their next segment_end picks the best checkpoint) and
// checks the stop conditions.
uint64_t dp_play_tas_ckpts(const DpPlay *p) { return p->tas_ckpts; }
uint64_t dp_play_tas_bank_hits(const DpPlay *p) { return p->tas_bank_hits; }
uint64_t dp_play_attract_retired(const DpPlay *p) { return p->attract_retired; }
uint64_t dp_play_tas_rng_starts(const DpPlay *p) { return p->tas_rng_starts; }
uint64_t dp_play_tas_snap_files(const DpPlay *p) { return (uint64_t)p->tasnaps_n; }

// the post-play phase: every snapshot saved during the TAS replays becomes a
// fork-explorer seed, in movie then frame order.  The backsearch first seeds
// the states whose capture bank still holds residual unknown (the dispatch
// code the fork explorer never resolved): real play states local to the zone
// start before the rest (priority 1 vs 0), the queue caps order the work.
int dp_play_seed_tas_snaps(DpPlay *p) {
    int n = 0;
    // residual banks: one bit per bank with unknown ranges (static view)
    uint8_t *rbank = calloc((size_t)p->fork->res->banks, 1);
    if (rbank) {
        uint8_t *m = dp_map_build(p->fork->res, &(DpMapSummary){0});
        if (m) {
            int nr = dp_map_residual_list(p->fork->res, m, NULL, 0);
            DpResidualRange *rr = nr > 0 ? malloc((size_t)nr * sizeof *rr) : NULL;
            if (rr && dp_map_residual_list(p->fork->res, m, rr, nr) > 0)
                for (int i = 0; i < nr; i++)
                    if (rr[i].bank < p->fork->res->banks) rbank[rr[i].bank] = 1;
            free(rr);
        }
        free(m);
    }
    for (int pass = 0; pass < 2; pass++) { // 0: residual banks first, 1: the rest
        for (int i = 0; i < p->tasnaps_n; i++) {
            if ((pass == 0) != (rbank && p->tasnaps[i].bank < p->fork->res->banks &&
                                rbank[p->tasnaps[i].bank]))
                continue;
            DpCheckpoint *c = tas_snap_load(p, p->tasnaps[i].path);
            if (!c) {
                fprintf(stderr, "deepeel: TAS snapshot unreadable: %s\n", p->tasnaps[i].path);
                continue;
            }
            if (!dp_explorer_seed_pri(p->fork, c->img, 3 /* DP_SEED_POINTER: real play state */,
                                      pass == 0 ? 1 : 0, 1))
                continue; // dropped: not seeded (the report counts accepted seeds only)
            if (pass == 0) p->tas_bank_hits++;
            n++;
            c->img = NULL; // the fork explorer's queue owns the image now
            ckpt_free(c);
        }
    }
    free(rbank);
    return n;
}
uint64_t dp_play_tas_cuts(const DpPlay *p) { return p->tas_cuts; }
uint64_t dp_play_tas_cut_stuck(const DpPlay *p) { return p->tas_cut_stuck; }

int dp_play_poll(DpPlay *p) {
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
        DpWorker *w = &p->w[i];
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
            dp_disc_add(p->res, src, w->credit_epoch);
            if (p->res->disc_n) p->res->disc[p->res->disc_n - 1].frame = w->lane_frame;
        }
        glob_new += w->credit_epoch;
        { // per-method yield for this epoch (the yield stop reads the window)
            int mi = w->mode == IN_TAS ? 0 : w->mode == IN_ATTRACT ? 3 : w->mode == IN_FIXED ? 4 : 1;
            p->method_yield[mi] += w->credit_epoch;
        }
        // a worker moved on to its next queued movie: its frame counter restarted
        frames += w->lane_frame >= w->seen_frame ? w->lane_frame - w->seen_frame : w->lane_frame;
        w->seen_frame = w->lane_frame;
        w->stagnant_epochs = w->credit_epoch == 0 ? w->stagnant_epochs + 1 : 0;
        // the attract worker retires when its play finds nothing anymore (the
        // attract mode is exhausted): its core goes to random play from the
        // pool — the least chosen checkpoints — like any other rng worker
        if (w->mode == IN_ATTRACT && w->attract_frames_done >= p->cfg.attract_frames &&
            w->stagnant_epochs >= p->cfg.attract_stall) {
            w->mode = IN_RNG;
            w->seg_left = 1; // rotate to a pool checkpoint at the next frame boundary
            w->stagnant_epochs = 0;
            p->attract_retired++;
            fprintf(stderr, "deepeel: attract worker %d retired after %d empty epochs: core to random play\n",
                    w->id, p->cfg.attract_stall);
        }
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
    p->unexplained = p->cfg.until_static ? dp_results_unexplained(p->res) : 0;

    // the window follows a movie that is still playing: a finished one would freeze its last image
    if (__atomic_load_n(&p->watch_fps, __ATOMIC_RELAXED)) {
        int sel = __atomic_load_n(&p->watch_sel, __ATOMIC_RELAXED), alive_sel = 0, other = -1;
        for (int i = 0; i < p->nworkers; i++) {
            DpWorker *w = &p->w[i];
            if (w->mode != IN_TAS || w->frames_done >= w->tas.frames) continue;
            int mi = tas_movie_index(p, w);
            if (mi == sel) alive_sel = 1;
            else if (other < 0) other = mi;
        }
        if (!alive_sel && other >= 0) __atomic_store_n(&p->watch_sel, other, __ATOMIC_RELAXED);
    }
    // sessions -> fork explorer: real play states become exploration seeds
    drain_inbox(p);
    // TAS captures -> snapshot PNGs (the seeding waits for the post-play phase)
    drain_tas_snaps(p);
    // the fork advances while the sessions wait; bounded by paths, not time,
    // so a fixed-seed run is reproducible
    // (--unbounded: the steps run after the workers are released instead, so the sessions
    // keep playing while the fork explores; reproducibility is not promised in that mode)
    // TAS priority: while a movie is still replaying (or queued) the fork waits, the
    // replay runs at machine speed instead of at the pace of the fork's paths; its
    // seeds are explored once every movie is over (the post-play phase)
    int tas_playing = __atomic_load_n(&p->tas_next, __ATOMIC_RELAXED) < p->cfg.tas_n;
    for (int i = 0; i < p->nworkers && !tas_playing; i++)
        if (p->w[i].mode == IN_TAS && p->w[i].frames_done < p->w[i].tas.frames) tas_playing = 1;
    if (!p->cfg.unbounded && !tas_playing)
        for (int i = 0; i < p->cfg.fork_paths_per_epoch && p->fork->q_len && !p->fork->stop; i++)
            dp_explore_step(p->fork);

    // stop conditions.  TAS priority: a movie covers a whole playthrough, it
    // outruns every frame budget — max_frames and stagnation wait until every
    // TAS worker has finished its replay (a SIGINT still stops at once).
    int reason = -1;
    int tas_left = 0;
    for (int i = 0; i < p->nworkers; i++)
        if (p->w[i].mode == IN_TAS && p->w[i].frames_done < p->w[i].tas.frames) tas_left = 1;
    if (__atomic_load_n(&p->tas_next, __ATOMIC_RELAXED) < p->cfg.tas_n) tas_left = 1;
    // the yield stop: per-method credit over the last --yield-window epochs
    if (p->cfg.yield_window > 0) {
        int wn = p->cfg.yield_window > 32 ? 32 : p->cfg.yield_window;
        p->yield_hist[0][p->yield_hi] = p->method_yield[0];
        p->yield_hist[1][p->yield_hi] = p->method_yield[1];
        p->yield_hist[2][p->yield_hi] = p->method_yield[2];
        p->yield_hist[3][p->yield_hi] = p->method_yield[3];
        p->yield_hist[4][p->yield_hi] = p->method_yield[4];
        p->method_yield[0] = p->method_yield[1] = p->method_yield[2] = 0;
        p->method_yield[3] = p->method_yield[4] = 0;
        p->yield_hi = (p->yield_hi + 1) % 32;
        if (p->yield_hn < 32) p->yield_hn++;
        if (p->yield_hn >= (uint32_t)wn) { // full window: judge each method
            uint64_t ysum[5] = {0};
            for (int m = 0; m < 5; m++)
                for (uint32_t k = 0; k < (uint32_t)wn; k++) {
                    int idx = (int)(((p->yield_hi + 32) - 1 - (uint32_t)k) % 32);
                    ysum[m] += p->yield_hist[m][idx];
                }
            // a starved method reallocates: attract workers retire to random
            // play; stagnant rng workers re-seed from the best checkpoint (the
            // existing stall path — force its counter so it fires now)
            if (ysum[3] < p->cfg.yield_min)
                for (int i = 0; i < p->nworkers; i++)
                    if (p->w[i].mode == IN_ATTRACT && p->w[i].attract_frames_done >= p->cfg.attract_frames) {
                        p->w[i].mode = IN_RNG;
                        p->w[i].seg_left = 1; // rotate to a pool checkpoint now
                        p->w[i].stagnant_epochs = 0;
                        p->attract_retired++;
                        fprintf(stderr, "deepeel: attract worker %d retired on the yield threshold: core to random play\n",
                                p->w[i].id);
                    }
            if (ysum[1] < p->cfg.yield_min)
                for (int i = 0; i < p->nworkers; i++)
                    if (p->w[i].mode == IN_RNG) {
                        p->w[i].stagnant_epochs = p->cfg.stall_epochs; // re-seed from the best
                    }
            // every method below the minimum: the whole play yields
            if (!tas_left && ysum[0] < p->cfg.yield_min && ysum[1] < p->cfg.yield_min &&
                ysum[3] < p->cfg.yield_min) {
                fprintf(stderr, "deepeel: yield stop: every method below %u bytes over %d epochs\n",
                        p->cfg.yield_min, wn);
                p->quit = 1;
                p->stop_reason = 7;
                dp_play_save(p);
                pthread_cond_broadcast(&p->cv);
                pthread_mutex_unlock(&p->bar_mtx);
                return 1;
            }
        }
    }
    if (g_sigint) reason = 2;
    else if (p->cfg.max_frames && !tas_left && p->total_frames >= p->cfg.max_frames) reason = 0;
    else if (p->cfg.stagnation_frames && !tas_left && p->stagnation >= p->cfg.stagnation_frames && !tas_rng_pending(p)) reason = 1;
    else if (p->cfg.until_static && !tas_left && p->unexplained == 0 && frames) reason = 3;
    else if (p->cfg.mem_cap_mb && !p->cfg.unbounded && dp_rss_mb() > p->cfg.mem_cap_mb) reason = 4;
    else if (p->cfg.time_limit_secs &&
             (uint64_t)(time(NULL) - p->t_start) >= p->cfg.time_limit_secs) reason = 6;
    else if (p->cfg.tas_solo && !tas_left && p->epoch > 0) reason = 5;
    if (reason >= 0) {
        p->quit = 1;
        p->stop_reason = reason;
        dp_play_save(p);
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->bar_mtx);
        return 1;
    }
    if (p->cfg.save_epochs && p->epoch % (uint32_t)p->cfg.save_epochs == 0) dp_play_save(p);
    p->workers_done = 0;
    p->epoch_no++;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->bar_mtx);
    if (p->cfg.unbounded)   // overlapped with the next epoch of the sessions; it yields as soon as they are
        // done, so the fork never holds a movie back (the barrier waits for this thread)
        for (int i = 0; i < p->cfg.fork_paths_per_epoch && p->fork->q_len && !p->fork->stop &&
                        __atomic_load_n(&p->workers_done, __ATOMIC_ACQUIRE) < p->nworkers; i++)
            dp_explore_step(p->fork);
    return 1;
}

void dp_play_finish(DpPlay *p) {
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

int dp_play_run(DpPlay *p) {
    // headless driver: poll until a stop condition, then let the workers exit
    while (!p->quit) {
        if (!dp_play_poll(p)) usleep(500);
    }
    dp_play_finish(p);
    return p->stop_reason;
}

// --- snapshot PNGs (screen + full machine in one file) ---------------------------------
// payload = the exact record the pool file stores per checkpoint
static void payload_ckpt(DpBuf *b, const DpCheckpoint *c) {
    dp_buf_push(b, &c->slot0, 1);
    dp_buf_push(b, &c->chosen, sizeof c->chosen);
    dp_buf_push(b, &c->frame, sizeof c->frame);
    dp_buf_push(b, &c->credit, sizeof c->credit);
    dp_buf_push(b, &c->sb, sizeof c->sb);
    dp_buf_push(b, c->img, c->sb);
}

static DpCheckpoint *ckpt_from_payload(const uint8_t *d, size_t n) {
    size_t need = 1 + sizeof(uint32_t) * 3 + sizeof(uint64_t);
    if (n < need) return NULL;
    DpCheckpoint *c = calloc(1, sizeof *c);
    c->tas = DP_MAX_TAS;
    size_t o = 0;
    memcpy(&c->slot0, d + o, 1); o += 1;
    memcpy(&c->chosen, d + o, sizeof c->chosen); o += sizeof c->chosen;
    memcpy(&c->frame, d + o, sizeof c->frame); o += sizeof c->frame;
    memcpy(&c->credit, d + o, sizeof c->credit); o += sizeof c->credit;
    memcpy(&c->sb, d + o, sizeof c->sb); o += sizeof c->sb;
    if (c->sb > DP_CRAM_PAGES * 256 + 0x20000 || n < o + c->sb) {
        free(c);
        return NULL;
    }
    c->img = malloc(c->sb);
    memcpy(c->img, d + o, c->sb);
    return c;
}

// one frame of the preview instance: the checkpoint's state rendered as the
// player would see it
static void preview_vblank(DpMach *m) { dp_mach_cut(m, DP_CUT_SEGEND); }

static void preview_frame(DpPlay *p, const uint8_t *img, uint8_t *gray) {
    pthread_mutex_lock(&p->preview_mtx);
    memset(gray, 3, DP_GB_SCREEN_W * DP_GB_SCREEN_H); // LCD off: dark
    dp_mach_attach(p->preview, NULL, NULL, NULL, NULL, preview_vblank);
    dp_mach_load(p->preview, img);
    p->preview->render = 1;
    p->preview->lines = 0;
    memset(p->preview->screen, 3, sizeof p->preview->screen);
    dp_mach_run(p->preview); // exactly one frame, cut at its vblank
    p->preview->render = 0;
    for (int y = 0; y < DP_GB_SCREEN_H; y++)
        for (int x = 0; x < DP_GB_SCREEN_W; x++)
            gray[y * DP_GB_SCREEN_W + x] = p->preview->screen[y][x];
    pthread_mutex_unlock(&p->preview_mtx);
}

// writes one snapshot PNG at the given path: the frame the player would see
// plus the machine
static int save_png_path(DpPlay *p, const DpCheckpoint *c, const char *path) {
    static uint8_t gray[DP_GB_SCREEN_W * DP_GB_SCREEN_H];
    static uint8_t rgb[DP_GB_SCREEN_W * DP_GB_SCREEN_H * 3];
    preview_frame(p, c->img, gray);
    for (int i = 0; i < DP_GB_SCREEN_W * DP_GB_SCREEN_H; i++) {
        uint8_t v = (uint8_t)(255 - gray[i] * 85);
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v;
    }
    DpBuf pay;
    dp_buf_init(&pay);
    payload_ckpt(&pay, c);
    int ok = dp_png_save(path, DP_GB_SCREEN_W, DP_GB_SCREEN_H, rgb, pay.p, pay.len);
    dp_buf_free(&pay);
    if (ok) snprintf(p->last_snap, sizeof p->last_snap, "%s", path);
    if (ok && p->cfg.verbose) fprintf(stderr, "deepeel: snapshot %s\n", path);
    return ok;
}

static int save_png_of(DpPlay *p, const DpCheckpoint *c) {
    char path[600];
    snprintf(path, sizeof path, "%s/snap-%03u-cred%llu.png", p->snap_dir, p->snap_seq++,
             (unsigned long long)c->credit);
    return save_png_path(p, c, path);
}

// reads a snapshot PNG back into a checkpoint (sb-checked against this run)
static DpCheckpoint *tas_snap_load(DpPlay *p, const char *path) {
    int w, h;
    uint8_t *rgb, *pay;
    size_t pay_n;
    if (!dp_png_load(path, &w, &h, &rgb, &pay, &pay_n)) return NULL;
    free(rgb);
    DpCheckpoint *c = pay ? ckpt_from_payload(pay, pay_n) : NULL;
    free(pay);
    if (!c || c->sb != p->fork->sb) {
        ckpt_free(c);
        return NULL;
    }
    return c;
}

// --- human takeover -----------------------------------------------------------------------
void dp_play_human_begin(DpPlay *p, int slot) {
    if (!p->human_on) {
        worker_setup(p, &p->human, 0); // borrows the fork explorer's instance
        p->human.mode = IN_HUMAN;
        p->human.id = p->nworkers;
        p->human.my_epoch = ~0u; // never at the worker barrier
        p->human.credit_base = 0;
        p->human.seen_frame = 0;
        p->human_screen = malloc(DP_GB_SCREEN_W * DP_GB_SCREEN_H);
        p->human_on = 1;
    }
    const DpCheckpoint *pick = NULL;
    pthread_mutex_lock(&p->pool.mtx);
    if (slot >= 0 && slot < p->pool.n) {
        pick = p->pool.slot[slot];
    } else { // the most productive checkpoint
        for (int i = 0; i < p->pool.n; i++)
            if (!pick || p->pool.slot[i]->credit > pick->credit) pick = p->pool.slot[i];
    }
    if (pick) dp_mach_load(p->human.mach, pick->img);
    pthread_mutex_unlock(&p->pool.mtx);
    // the human sees the game: render while the lane plays
    p->human.mach->render = 1;
    p->human.mach->api->skip_render[0] = 0;
    worker_reset_run_state(&p->human);
    p->human.credit_prev = worker_credit(&p->human);
    p->human.credit_base = p->human.credit_prev;
    p->human_png_credit = p->human.credit_prev;
}

void dp_play_human_frame(DpPlay *p, uint8_t buttons) {
    if (!p->human_on || p->quit) return;
    p->human.human_buttons = buttons;
    run_epoch(&p->human, 1);
    if (p->human.mach->lines >= DP_GB_SCREEN_H) {
        for (int y = 0; y < DP_GB_SCREEN_H; y++)
            for (int x = 0; x < DP_GB_SCREEN_W; x++)
                p->human_screen[y * DP_GB_SCREEN_W + x] = p->human.mach->screen[y][x];
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

const uint8_t *dp_play_human_screen(DpPlay *p) {
    return p->human_on ? p->human_screen : NULL;
}

int dp_play_human_active(const DpPlay *p) { return p->human_on; }

int dp_play_human_snapshot(DpPlay *p) {
    if (!p->human_on) return 0;
    DpCheckpoint *c = calloc(1, sizeof *c);
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

int dp_play_load_png(DpPlay *p, const char *path) {
    int w, h;
    uint8_t *rgb, *pay;
    size_t pay_n;
    if (!dp_png_load(path, &w, &h, &rgb, &pay, &pay_n)) {
        fprintf(stderr, "deepeel: cannot read PNG %s\n", path);
        return -1;
    }
    free(rgb);
    if (!pay) {
        fprintf(stderr, "deepeel: %s has no deepeel_snapshot payload\n", path);
        return -1;
    }
    DpCheckpoint *c = ckpt_from_payload(pay, pay_n);
    free(pay);
    if (!c) {
        fprintf(stderr, "deepeel: %s payload unreadable\n", path);
        return -1;
    }
    if (c->sb != p->fork->sb) {
        fprintf(stderr, "deepeel: %s state size does not match this run\n", path);
        ckpt_free(c);
        return -1;
    }
    return pool_insert(p, c);
}

int dp_play_pool_count(DpPlay *p) { return p->pool.n; }

uint16_t dp_play_slot_pc(DpPlay *p, int slot) {
    if (slot < 0 || slot >= p->pool.n) return 0;
    return image_pc(p->pool.slot[slot]->img);
}

const char *dp_play_last_snapshot(const DpPlay *p) { return p->last_snap[0] ? p->last_snap : NULL; }

int dp_play_done(const DpPlay *p) { return p->quit; }
uint64_t dp_play_frames(const DpPlay *p) { return p->total_frames; }

// the checkpoint an export offers: the most productive discovery, the power-on
// slot only when the pool holds nothing else
int dp_play_best_slot(const DpPlay *p) {
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

const uint8_t *dp_play_slot_img(const DpPlay *p, int slot) {
    if (slot < 0 || slot >= p->pool.n) return NULL;
    return p->pool.slot[slot]->img;
}

uint32_t dp_play_slot_frame(const DpPlay *p, int slot) {
    if (slot < 0 || slot >= p->pool.n) return 0;
    return p->pool.slot[slot]->frame;
}

int dp_play_lanes(DpPlay *p, DpSessionLane *out, int max) {
    pthread_mutex_lock(&p->bar_mtx);
    int n = p->nworkers < max ? p->nworkers : max;
    for (int i = 0; i < n; i++) {
        const DpWorker *w = &p->w[i];
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
        const DpWorker *w = &p->human;
        out[n].id = n;
        out[n].mode = (int)w->mode;
        out[n].frame = w->frames_done;
        out[n].seg_left = 0;
        out[n].credit_total = worker_credit(w);
        out[n].credit_epoch = out[n].credit_total - w->credit_base;
        out[n].stagnant_epochs = 0;
        out[n].bank = (uint8_t)dp_gbl_bank_of(w->mach->api, w->mach->api->gbr->pc);
        out[n].pc = w->mach->api->gbr->pc;
        out[n].buttons = w->applied_mask;
        out[n].dead = 0;
        out[n].backtracked = 0;
        n++;
    }
    return n;
}

// --- watch window feed ---------------------------------------------------------------
int dp_play_watch_enable(DpPlay *p, int fps) {
    if (!p) return 0;
    int n = 0;
    for (int i = 0; i < p->nworkers; i++) if (p->w[i].tas.masks) n++;
    if (!n) return 0;
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    __atomic_store_n(&p->watch_fps, fps, __ATOMIC_RELAXED);
    return n;
}

void dp_play_watch_disable(DpPlay *p) {
    if (p) __atomic_store_n(&p->watch_fps, 0, __ATOMIC_RELAXED);
}

void dp_play_watch_next(DpPlay *p, int n) {
    if (!p || n < 1) return;
    __atomic_store_n(&p->watch_sel, (__atomic_load_n(&p->watch_sel, __ATOMIC_RELAXED) + 1) % n,
                     __ATOMIC_RELAXED);
}

int dp_play_watch_tiles(DpPlay *p, DpWatchTile *t, int max) {
    if (!p) return 0;
    int n = 0;
    for (int i = 0; i < p->nworkers && n < max; i++) {
        DpWorker *w = &p->w[i];
        if (!w->tas.masks) continue;
        DpWatchTile *o = &t[n++];
        o->film = w->tas_idx + 1;
        o->films = p->cfg.tas_n;
        o->total = (uint32_t)w->tas.frames;
        o->live_frame = (uint32_t)__atomic_load_n(&w->frames_done, __ATOMIC_RELAXED);
        o->done = o->live_frame >= o->total;
        snprintf(o->name, sizeof o->name, "%s", w->tas.source);
        pthread_mutex_lock(&p->watch_mtx);
        o->have = w->watch_img_film == o->film;
        if (o->have) {
            memcpy(o->shades, w->watch_img, sizeof o->shades);
            o->frame = w->watch_img_frame;
        }
        pthread_mutex_unlock(&p->watch_mtx);
    }
    return n;
}

// the single selected movie (map window): the tile of the sel-th replaying movie
int dp_play_watch_read(DpPlay *p, uint8_t *shades, DpWatchInfo *info) {
    if (!p) return 0;
    int sel = __atomic_load_n(&p->watch_sel, __ATOMIC_RELAXED), k = 0;
    for (int i = 0; i < p->nworkers; i++) {
        DpWorker *w = &p->w[i];
        if (!w->tas.masks) continue;
        if (k++ != sel) continue;
        int ok;
        pthread_mutex_lock(&p->watch_mtx);
        ok = w->watch_img_film == w->tas_idx + 1;
        if (ok) {
            memcpy(shades, w->watch_img, DP_GB_SCREEN_W * DP_GB_SCREEN_H);
            info->frame = w->watch_img_frame;
            info->total = w->watch_img_total;
        }
        pthread_mutex_unlock(&p->watch_mtx);
        if (!ok) return 0;
        info->movie = sel;
        info->film = w->tas_idx + 1;
        info->films = p->cfg.tas_n;
        info->live_frame = (uint32_t)__atomic_load_n(&w->frames_done, __ATOMIC_RELAXED);
        snprintf(info->name, sizeof info->name, "%s", w->tas.source);
        info->movies = 0;
        for (int j = 0; j < p->nworkers; j++) if (p->w[j].tas.masks) info->movies++;
        return 1;
    }
    return 0;
}
