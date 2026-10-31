// gui_stub.c: compiled when SDL2 is missing; the CLI reports it gracefully.
#include "gui.h"
#include "../common.h"

int dp_gui_available(void) { return 0; }

int dp_gui_run(DpResults *res, DpConfig *cfg, DpExplorer *ex, DpPlay *play) {
    (void)res;
    (void)cfg;
    (void)ex;
    (void)play;
    fprintf(stderr, "deepeel: built without SDL2, --gui unavailable\n");
    return 1;
}

int dp_watch_available(void) { return 0; }

int dp_watch_start(DpPlay *play, int fps) {
    (void)play;
    (void)fps;
    fprintf(stderr, "deepeel: built without SDL2, --watch unavailable\n");
    return 1;
}

void dp_watch_stop(void) {}
