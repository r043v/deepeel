// Verify that packed queued states survive adoption into a disk frontier.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/explore/explorer_priv.h"
#include "mach_util.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char path[512], err[256], dir[] = "/tmp/deepeel-frontier-test-XXXXXX";
    snprintf(path, sizeof path, "%s/fix_branch.gb", argv[1]);
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    CHECK(cart != NULL);
    if (!cart || !mkdtemp(dir)) return 2;
    DpResults res;
    dp_results_init(&res, cart, dp_platform_gb());
    DpMach *mach = mach_for(cart);
    CHECK(mach != NULL);
    if (!mach) return 2;
    DpConfig cfg;
    dp_config_defaults(&cfg);
    DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
    uint8_t *img = calloc(1, ex->sb), *expected = calloc(1, ex->sb);
    img[ex->sb - 1] = expected[ex->sb - 1] = 0x5a;
    CHECK(queue_push(ex, img, 1, DP_PATH_FORK, 1, 257));
    CHECK(ex->queue[ex->q_head].packed == 1);
    CHECK(dp_explorer_frontier_save(ex, dir) == 1);
    dp_explorer_free(ex);
    ex = dp_explorer_new(&cfg, &res, mach);
    CHECK(dp_explorer_frontier_load(ex, dir) == 1);
    CHECK(ex->defer_len == 1);
    if (ex->defer_len == 1) {
        CHECK(ex->defer[0].p.packed == 0);
        CHECK(memcmp(ex->defer[0].p.img, expected, ex->sb) == 0);
    }
    // More than 128 separate code-bounded unknown runs must all be visited.
    // Undefined opcodes keep them unknown and make each candidate refuse cleanly.
    size_t n = (size_t)res.banks * 0x4000;
    memset(cart->rom, 0xd3, n);
    memset(res.exec_flags, DP_XF_NATURAL, n);
    memset(res.data_flags, 0, n);
    memset(res.static_flags, 0, n);
    for (size_t i = 0; i < 140; i++) memset(res.exec_flags + 1 + i * 5, 0, 4);
    dp_residual_target(ex, 200);
    CHECK(res.residual_targets == 140);
    CHECK(res.residual_refused == 140);
    dp_residual_target(ex, 200);
    CHECK(res.residual_targets == 140); // previously visited runs consume no budget
    free(expected);
    dp_explorer_free(ex);
    dp_mach_close(mach);
    dp_results_free(&res);
    dp_cart_free(cart);
    TEST_DONE("test_frontier_packed");
}
