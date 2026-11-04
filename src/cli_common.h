// cli_common.h: the explore options shared by every platform command (nes, pce, md).
// Each option struct names these fields identically; the macro expands to `else if`
// clauses and expects `a`, `i`, `argc`, `argv` in scope and a DpCliCommon `cc`.
#ifndef DP_CLI_COMMON_H
#define DP_CLI_COMMON_H
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "explore/stop.h"

typedef struct {
    int jobs, watch;
    unsigned time_limit;
    const char *dump, *text, *map, *evidence, *rtcode;
} DpCliCommon;

static void dp_cli_on_stop(int sig) { (void)sig; dp_stop = 1; }

// SIGINT and the --time-limit alarm ask the explorer to stop cleanly (the report is still written)
static inline void dp_cli_arm_stop(unsigned time_limit) {
    signal(SIGINT, dp_cli_on_stop);
    if (time_limit) { signal(SIGALRM, dp_cli_on_stop); alarm(time_limit); }
}

#define DP_CLI_U(opt, flag, field, T) \
    else if (!strcmp(a, flag) && i + 1 < argc) (opt).field = (T)strtoull(argv[++i], NULL, 0)

#define DP_CLI_COMMON(opt, cc) \
    else if (!strcmp(a, "--rtcode") && i + 1 < argc) (cc).rtcode = argv[++i]; \
    else if (!strcmp(a, "--text") && i + 1 < argc) (cc).text = argv[++i]; \
    else if (!strcmp(a, "--map") && i + 1 < argc) (cc).map = argv[++i]; \
    else if (!strcmp(a, "--evidence") && i + 1 < argc) (cc).evidence = argv[++i]; \
    DP_CLI_U(opt, "--max-total", max_total, uint64_t); \
    DP_CLI_U(opt, "--queue", queue_cap, uint32_t); \
    DP_CLI_U(opt, "--novelty", novelty_steps, uint64_t); \
    DP_CLI_U(opt, "--novelty-frames", novelty_frames, uint32_t); \
    DP_CLI_U(opt, "--novelty-archive", novelty_archive, uint32_t); \
    DP_CLI_U(opt, "--ctx-cap", ctx_cap, unsigned); \
    DP_CLI_U(cc, "--time-limit", time_limit, unsigned); \
    else if (!strcmp(a, "--novelty-cells")) (opt).novelty_cells = 1; \
    else if (!strcmp(a, "--second-pass")) (opt).second_pass = 1; \
    else if (!strcmp(a, "--lot") && i + 1 < argc) (opt).lot_path = argv[++i]; \
    else if (!strcmp(a, "--lot-drain")) (opt).lot_drain = 1; \
    else if (!strcmp(a, "--tree") && i + 1 < argc) (opt).tree_path = argv[++i]; \
    else if (!strcmp(a, "--jobs") && i + 1 < argc) (cc).jobs = atoi(argv[++i]); \
    else if (!strcmp(a, "--export-state") && i + 1 < argc) (opt).export_state = argv[++i]; \
    else if (!strcmp(a, "--watch-dump") && i + 1 < argc) { (cc).dump = argv[++i]; if (!(opt).watch_every) (opt).watch_every = 2; } \
    else if (!strcmp(a, "--watch")) { \
        int fps_ = 30; \
        if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') fps_ = atoi(argv[++i]); \
        if (fps_ < 1) fps_ = 1; \
        if (fps_ > 30) fps_ = 30; \
        (opt).watch_every = (unsigned)(60 / fps_); \
        (cc).watch = 1; \
    }

// options of the two frame-driven platforms (nes, pce): per-path budget, loop/fork caps, random play
#define DP_CLI_PLAYED(opt) \
    DP_CLI_U(opt, "--max-steps", max_steps, uint32_t); \
    DP_CLI_U(opt, "--loop-cap", loop_cap, uint32_t); \
    DP_CLI_U(opt, "--fork-cap", fork_cap, uint32_t); \
    DP_CLI_U(opt, "--play", play_frames, uint32_t); \
    DP_CLI_U(opt, "--sessions", play_sessions, uint32_t); \
    DP_CLI_U(opt, "--seed", play_seed, uint32_t); \
    else if (!strcmp(a, "--no-irq-fork")) (opt).seed_irq = 0;
#endif
