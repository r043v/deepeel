// livewin.h: the one live window of the campaign: it shows a DpCanvas (tiles composed by the caller). Same sink for every
// machine; without SDL2 or a display the calls are harmless no-ops.
#ifndef DP_LIVEWIN_H
#define DP_LIVEWIN_H
#include "canvas.h"
int dp_livewin_show(const DpCanvas *c, const char *title);   // creates the window on first use, uploads the canvas; returns 0 while the window lives
void dp_livewin_close(void);
#endif
