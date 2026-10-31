// sessions.h: the play sessions — the m0boy coverage machinery ported into
// memscan.  N pthread workers each play the game with a plausible random
// hand (direction held a while, buttons held or tapped, START now and then),
// an always-alive no-input session, or replayed m0boy input scripts.  Their
// discoveries (executed code, data reads, verbatim copies, registers, bank
// switches) feed the same MsResults maps as the forking explorer, and their
// interesting machine states become both new session checkpoints and seeds
// for the fork explorer.  The scheduler runs in epochs: sessions that stop
// discovering are re-seeded from the most productive checkpoint; runs are
// persistable and resumable, so a future run continues where this one left
// off instead of falling back into a closed path.
#ifndef MS_SESSIONS_H
#define MS_SESSIONS_H

#include "explorer.h"

#define MS_MAX_SESSIONS 64
#define MS_MAX_POOL 256

typedef struct MsSessionCfg {
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
    int until_static;           // also stop when no byte is left unexplained
    int save_epochs;            // persist the checkpoint pool every n epochs
    const char *state_dir;      // persistence directory (pool file inside)
    int resume;                 // load the pool file and continue
    const char *inputs[8];      // m0boy held-named input scripts to replay
    int inputs_n;
    uint8_t fixed[MS_MAX_SESSIONS]; // fixed pad masks (tests); 0 = not fixed
    int fixed_n;
    int fork_paths_per_epoch;   // fork-explorer paths drained per epoch (0 = none)
    int verbose;
} MsSessionCfg;

void ms_session_cfg_defaults(MsSessionCfg *cfg);

// read-only view of one session lane, for the GUI and the reports
typedef struct MsSessionLane {
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
} MsSessionLane;

typedef struct MsPlay MsPlay;

// builds the pool (slot 0 = power-on) and spawns the workers
MsPlay *ms_play_new(const MsSessionCfg *cfg, MsResults *res, MsExplorer *fork_ex);
// one scheduler step: at an epoch boundary, collects credits, drains the
// session-seed inbox into the fork explorer's queue, drains the fork queue
// itself (sessions paused), re-seeds stagnant sessions and checks the stop
// conditions.  Non-blocking while the workers run.  Returns 1 when an epoch
// was processed.
int ms_play_poll(MsPlay *p);
// headless driver: poll until a stop condition, join the workers;
// returns the reason (0 frame budget, 1 stagnation, 2 interrupt, 3 static
// map fully explained)
int ms_play_run(MsPlay *p);
void ms_play_free(MsPlay *p);
// persist the checkpoint pool now (also automatic every save_epochs)
int ms_play_save(const MsPlay *p);
int ms_play_done(const MsPlay *p);
uint64_t ms_play_frames(const MsPlay *p);
int ms_play_lanes(MsPlay *p, MsSessionLane *out, int max);
// stop the workers and join them (idempotent); call before finalizing
void ms_play_finish(MsPlay *p);

// --- human takeover -----------------------------------------------------------
// The interesting checkpoints become the start of a game a human can play
// with a pad and eyes: the tool monitors the played frames, saves every
// advance (a full machine snapshot inside a single PNG — the screen the
// human sees plus the machine, like a PICO-8 cartridge) and launches
// exploration from it: the human unlocks, the tool continues.

// start playing pool slot `slot` (-1 = the most productive checkpoint);
// the human lane is driven by the GUI thread, one frame per call
void ms_play_human_begin(MsPlay *p, int slot);
// one frame of human play with the given pad mask
// (a=1 b=2 select=4 start=8 right=16 left=32 up=64 down=128)
void ms_play_human_frame(MsPlay *p, uint8_t buttons);
// the current human frame as DMG shades 0-3, 160x144 row-major
#define MS_GB_SCREEN_W 160
#define MS_GB_SCREEN_H 144
const uint8_t *ms_play_human_screen(MsPlay *p);
int ms_play_human_active(const MsPlay *p);
// save a PNG snapshot of the current human state now
int ms_play_human_snapshot(MsPlay *p);
// load a snapshot PNG into the pool; returns the pool slot or -1
int ms_play_load_png(MsPlay *p, const char *path);
int ms_play_pool_count(MsPlay *p);
// inspection helpers (tests, tools): pc of pool slot `slot`, -1 if empty,
// and the path of the last PNG snapshot written
uint16_t ms_play_slot_pc(MsPlay *p, int slot);
const char *ms_play_last_snapshot(const MsPlay *p);

// SIGINT: cooperative stop — the scheduler finishes its epoch, saves and exits
void ms_play_install_sigint(void);

#endif
