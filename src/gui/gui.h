// gui.h: the SDL2 live viewer interface.
#ifndef MS_GUI_H
#define MS_GUI_H

#include "../explore/explorer.h"
#include "../report/report.h"

// returns 1 when built with SDL2
int ms_gui_available(void);
// runs the real-time viewer; drives ms_explore_slice in slices, redraws,
// returns when the window is closed.  finalize() is called before returning.
int ms_gui_run(MsResults *res, MsConfig *cfg, MsExplorer *ex);

#endif
