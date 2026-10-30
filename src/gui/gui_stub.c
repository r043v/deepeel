// gui_stub.c: compiled when SDL2 is missing; the CLI reports it gracefully.
#include "gui.h"
#include "../common.h"

int ms_gui_available(void) { return 0; }

int ms_gui_run(MsResults *res, MsConfig *cfg, MsExplorer *ex) {
    (void)res;
    (void)cfg;
    (void)ex;
    fprintf(stderr, "memscan: built without SDL2, --gui unavailable\n");
    return 1;
}
