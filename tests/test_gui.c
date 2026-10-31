// test_gui.c: runs the SDL2 viewer headlessly (dummy video driver) and checks
// the screenshot hook produced a frame.  The exploration order is identical to
// a headless run: slices only change pacing, never worklist order.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/explore/sessions.h"
#include "test_util.h"
#include <SDL.h>
#include <sys/stat.h>

int ms_gui_run(MsResults *res, MsConfig *cfg, MsExplorer *ex, MsPlay *play); // gui.c (not in the stub)

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    char err[256];
    snprintf(path, sizeof path, "%s/fix_branch.gb", dir);
    MsCart *cart = ms_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "cannot load %s: %s\n", path, err);
        return 2;
    }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    const char *shot = "gui_shot.bmp";
    setenv("MEMSCAN_SHOT", shot, 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Quit();

    MsResults res;
    ms_results_init(&res, cart, ms_platform_gb());
    ms_static_scan(&res);
    MsConfig cfg;
    ms_config_defaults(&cfg);
    MsExplorer *ex = ms_explorer_new(&cfg, &res);
    ms_seed_gb(ex);
    ms_gui_run(&res, &cfg, ex, NULL);
    ms_explorer_free(ex);

    struct stat st;
    CHECK(stat(shot, &st) == 0);
    CHECK(st.st_size > 100000); // a real frame was rendered
    // the exploration ran to completion through the GUI loop
    CHECK(res.paths_run > 0);
    CHECK(res.section_count > 0);

    ms_results_free(&res);
    ms_cart_free(cart);
    TEST_DONE("test_gui");
}
