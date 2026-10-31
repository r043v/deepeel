// gui.h: the SDL2 live viewer interface.
#ifndef DP_GUI_H
#define DP_GUI_H

#include "../explore/explorer.h"
#include "../explore/sessions.h"
#include "../report/report.h"

// returns 1 when built with SDL2
int dp_gui_available(void);
// runs the real-time viewer; drives exploration (and, when play is given,
// the session scheduler and the human lane) in slices, redraws, returns when
// the window is closed.  finalize() is called before returning.
int dp_gui_run(DpResults *res, DpConfig *cfg, DpExplorer *ex, DpPlay *play);

// --- watch window (--watch): a small window showing the screen of a replaying
// TAS movie, sampled at 1..30 images/s, plus the movie and its real speed.
// It runs in its own thread and never slows the replay.
// returns 1 when built with SDL2
int dp_watch_available(void);
// starts the window thread (enable the feed with dp_play_watch_enable first);
// returns 0 on success
int dp_watch_start(DpPlay *play, int fps);
// closes the window and joins the thread (idempotent)
void dp_watch_stop(void);

#endif
