// sessions.h: the play sessions — the m0boy coverage machinery ported into
// deepeel.  N pthread workers each play the game with a plausible random
// hand (direction held a while, buttons held or tapped, START now and then),
// an always-alive no-input session, or replayed m0boy input scripts.  Their
// discoveries (executed code, data reads, verbatim copies, registers, bank
// switches) feed the same DpResults maps as the forking explorer, and their
// interesting machine states become both new session checkpoints and seeds
// for the fork explorer.  The scheduler runs in epochs: sessions that stop
// discovering are re-seeded from the most productive checkpoint; runs are
// persistable and resumable, so a future run continues where this one left
// off instead of falling back into a closed path.
#ifndef DP_SESSIONS_H
#define DP_SESSIONS_H

#include "explorer.h"

#define DP_MAX_SESSIONS 64
#define DP_MAX_POOL 256

typedef struct DpSessionCfg {
    int sessions;               // parallel workers (default 10)
    uint32_t epoch_frames;      // frames per scheduler epoch (default 480)
    uint32_t segment_min;       // play segment length (m0x: 480)
    uint32_t segment_span;      // + random below (m0x: 1920)
    int pool_cap;               // checkpoint pool (m0x: 48)
    int stall_epochs;           // empty epochs before a session re-seeds from the best
    int restart_rate;           // 1/n segments restart from power-on (m0x: 8)
    uint32_t seed;              // input RNG base
    uint64_t max_frames;        // stop after this many frames total (0 = unlimited)
    uint64_t stagnation_frames; // stop after this many frames with no discovery (0 = never)
    int unbounded;              // --unbounded: the memory cap no longer stops the run (overflow goes to disk)
    uint64_t mem_cap_mb;        // stop (gracefully) once resident memory exceeds this (0 = off)
    uint64_t time_limit_secs;   // stop (gracefully) after this many wall-clock seconds of
                                // play (0 = off) — a campaign wall: it outranks even the
                                // TAS priority, the pool stays saved and resumable
    int until_static;           // also stop when no byte is left unexplained
    int save_epochs;            // persist the checkpoint pool every n epochs
    const char *state_dir;      // persistence directory (pool file inside)
    int resume;                 // load the pool file and continue
    const char *inputs[8];      // m0boy held-named input scripts to replay
    int inputs_n;
    const char *tas[1024];         // TAS movie files (VBA .vmv, BizHawk .bk2, plain
    int tas_n;                  // input log): one worker each, frame by frame
    int tas_any_rom;            // 1: replay a movie whose recorded ROM identity differs
                                //    from this ROM (default: refused, counted)
    int tas_solo;               // 1: only the TAS workers run (no attract/random crowd);
                                // the play stops once every movie has ended — the
                                // checkpoints are kept for the post-play phases
    uint32_t tas_ckpt_frames;   // TAS replay: capture interval override, in frames (0 = auto:
                                // derived from tas_ckpt_secs and tas_ckpt_max)
    uint32_t tas_ckpt_secs;     // TAS replay: save a checkpoint snapshot every n seconds of game
                                // time (0 = off; default 30)
    int cells;                  // Go-Explore screen cells (J4): a never-seen background fingerprint joins the pool
    int tas_ckpt_max;           // TAS replay: checkpoint snapshots kept per movie (default 1024)
    int tas_rng;                // 1 (default): random-key sessions also start long segments from the TAS
                                // checkpoints (the neighbourhood of the near-perfect run), many times each
    uint32_t tas_rng_share;     // 1 segment start in this many is drawn from the TAS checkpoints (default 2)
    uint32_t tas_rng_per;       // segments started from each TAS checkpoint before the share stops (default 8)
    uint32_t tas_rng_min;       // frames of such a segment: min + random below 2*min (default 3600)
    int tas_ckpt_live;          // 1: the old behavior — the captures feed the fork explorer's
                                // inbox during the replay instead of waiting for the post-play
                                // phase (default 0)
    int uniform;                // 1 (default): uniform random keys (one of the
                                // 8 buttons, held 1..uniform_hold frames); 0:
                                // m0boy's weighted generator (m0x)
    uint32_t uniform_hold;      // max hold of one uniform key (default 64)
    uint8_t fixed[DP_MAX_SESSIONS]; // fixed pad masks (tests); 0 = not fixed
    int fixed_n;
    int fork_paths_per_epoch;   // fork-explorer paths drained per epoch (0 = none)
    int verbose;
} DpSessionCfg;

void dp_session_cfg_defaults(DpSessionCfg *cfg);

// read-only view of one session lane, for the GUI and the reports
typedef struct DpSessionLane {
    int id;
    int mode;                   // 0 random, 1 attract (no keys), 2 script, 3 fixed
    uint32_t frame, seg_left;   // frames played, frames left in the segment
    uint64_t credit_total, credit_epoch;
    int stagnant_epochs;
    uint8_t bank;
    uint16_t pc;
    uint8_t buttons;
    uint8_t dead;               // ended on a fatal state (bad opcode...)
    uint8_t backtracked;        // currently resuming from a checkpoint
} DpSessionLane;

typedef struct DpPlay DpPlay;

// builds the pool (slot 0 = power-on) and spawns the workers.  Machine
// instances: the fork explorer owns instance 0 (the human lane borrows it
// while the fork is idle), workers take 1..n, the last instance is the
// snapshot preview.  `sessions` is clamped to what the build compiled.
DpPlay *dp_play_new(const DpSessionCfg *cfg, DpResults *res, DpExplorer *fork_ex);
// one scheduler step: at an epoch boundary, collects credits, drains the
// session-seed inbox into the fork explorer's queue, drains the fork queue
// itself (sessions paused), re-seeds stagnant sessions and checks the stop
// conditions.  Non-blocking while the workers run.  Returns 1 when an epoch
// was processed.
int dp_play_poll(DpPlay *p);
// headless driver: poll until a stop condition, join the workers;
// returns the reason (0 frame budget, 1 stagnation, 2 interrupt, 3 static
// map fully explained, 4 memory cap, 5 TAS replay done, 6 time budget)
int dp_play_run(DpPlay *p);
void dp_play_free(DpPlay *p);
// persist the checkpoint pool now (also automatic every save_epochs)
int dp_play_save(const DpPlay *p);
int dp_play_done(const DpPlay *p);
// TAS gameplay states handed to the fork explorer so far
uint64_t dp_play_tas_ckpts(const DpPlay *p);
uint64_t dp_play_tas_rng_starts(const DpPlay *p); // random-key segments started from a TAS checkpoint
// screen cells archived into the pool (--session-cells)
uint64_t dp_play_cells(const DpPlay *p);
// TAS checkpoint snapshots saved on disk during the replays
uint64_t dp_play_tas_snap_files(const DpPlay *p);
// seed the fork explorer from every saved TAS checkpoint snapshot (in movie
// order), the post-play phase; returns the number of seeds deposited
int dp_play_seed_tas_snaps(DpPlay *p);
// fork-explorer cuts absorbed mid-movie / wedged-machine guard hits
uint64_t dp_play_tas_cuts(const DpPlay *p);
uint64_t dp_play_tas_cut_stuck(const DpPlay *p);
uint64_t dp_play_frames(const DpPlay *p);
int dp_play_lanes(DpPlay *p, DpSessionLane *out, int max);
// checkpoint pool access for the playable export (--export-playable): the
// most productive checkpoint and its image / discovery frame
int dp_play_best_slot(const DpPlay *p);
const uint8_t *dp_play_slot_img(const DpPlay *p, int slot);
uint32_t dp_play_slot_frame(const DpPlay *p, int slot);
// stop the workers and join them (idempotent); call before finalizing
void dp_play_finish(DpPlay *p);

// --- human takeover -----------------------------------------------------------
// The interesting checkpoints become the start of a game a human can play
// with a pad and eyes: the tool monitors the played frames, saves every
// advance (a full machine snapshot inside a single PNG — the screen the
// human sees plus the machine, like a PICO-8 cartridge) and launches
// exploration from it: the human unlocks, the tool continues.

// start playing pool slot `slot` (-1 = the most productive checkpoint);
// the human lane is driven by the GUI thread, one frame per call
void dp_play_human_begin(DpPlay *p, int slot);
// one frame of human play with the given pad mask
// (a=1 b=2 select=4 start=8 right=16 left=32 up=64 down=128)
void dp_play_human_frame(DpPlay *p, uint8_t buttons);
// the current human frame as DMG shades 0-3, 160x144 row-major
#define DP_GB_SCREEN_W 160
#define DP_GB_SCREEN_H 144
const uint8_t *dp_play_human_screen(DpPlay *p);
int dp_play_human_active(const DpPlay *p);
// save a PNG snapshot of the current human state now
int dp_play_human_snapshot(DpPlay *p);
// load a snapshot PNG into the pool; returns the pool slot or -1
int dp_play_load_png(DpPlay *p, const char *path);
int dp_play_pool_count(DpPlay *p);

// --- watch window feed (--watch): the screen of a replaying movie, sampled
// at 1..30 images/s without slowing the replay (one frame in 1/fps s is
// rendered, the others are not).
typedef struct {
    int movie, movies;        // shown movie (0-based) and how many movies replay
    uint32_t frame, total;    // movie frame of the sampled image / movie length
    uint32_t live_frame;      // the replay's current frame (for the real speed)
    char name[256];
} DpWatchInfo;
// returns the number of replaying movies (0 = nothing to watch)
int dp_play_watch_enable(DpPlay *p, int fps);
void dp_play_watch_disable(DpPlay *p);
void dp_play_watch_next(DpPlay *p, int movies); // show the next movie
// last sampled image (160x144 DMG shades) + info; 0 until the first sample
int dp_play_watch_read(DpPlay *p, uint8_t *shades, DpWatchInfo *info);
// inspection helpers (tests, tools): pc of pool slot `slot`, -1 if empty,
// and the path of the last PNG snapshot written
uint16_t dp_play_slot_pc(DpPlay *p, int slot);
const char *dp_play_last_snapshot(const DpPlay *p);

// SIGINT: cooperative stop — the scheduler finishes its epoch, saves and exits
void dp_play_install_sigint(void);

#endif
