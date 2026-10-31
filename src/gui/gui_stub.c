// gui_stub.c: compiled when SDL2 is missing; the CLI reports it gracefully.
#include "gui.h"
#include "../common.h"

int ms_gui_available(void) { return 0; }

int ms_gui_run(MsResults *res, MsConfig *cfg, MsExplorer *ex, MsPlay *play) {
    (void)res;
    (void)cfg;
    (void)ex;
    (void)play;
    fprintf(stderr, "memscan: built without SDL2, --gui unavailable\n");
    return 1;
}

int ms_watch_available(void) { return 0; }

int ms_watch_start(MsPlay *play, int fps) {
    (void)play;
    (void)fps;
    fprintf(stderr, "memscan: built without SDL2, --watch unavailable\n");
    return 1;
}

void ms_watch_stop(void) {}
