// test_gui.c: runs the SDL2 viewer headlessly (dummy video driver) and checks
// the screenshot hook produced a frame.  The exploration order is identical to
// a headless run: slices only change pacing, never worklist order.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/explore/sessions.h"
#include "mach_util.h"
#include "test_util.h"
#include <SDL.h>
#include <sys/stat.h>

int dp_gui_run(DpResults *res, DpConfig *cfg, DpExplorer *ex, DpPlay *play); // gui.c (not in the stub)

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    char err[256];
    snprintf(path, sizeof path, "%s/fix_branch.gb", dir);
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "cannot load %s: %s\n", path, err);
        return 2;
    }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    const char *shot = "gui_shot.bmp";
    setenv("DEEPEEL_SHOT", shot, 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Quit();

    DpResults res;
    dp_results_init(&res, cart, dp_platform_gb());
    dp_static_scan(&res);
    DpConfig cfg;
    dp_config_defaults(&cfg);
    DpMach *mach = mach_for(cart);
    DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
    dp_seed_gb(ex);
    dp_gui_run(&res, &cfg, ex, NULL);
    dp_explorer_free(ex);
    dp_mach_close(mach);

    struct stat st;
    CHECK(stat(shot, &st) == 0);
    CHECK(st.st_size > 100000); // a real frame was rendered
    // the exploration ran to completion through the GUI loop
    CHECK(res.paths_run > 0);
    CHECK(res.section_count > 0);

    dp_results_free(&res);
    dp_cart_free(cart);
    TEST_DONE("test_gui");
}
