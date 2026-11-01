// deepeel.c: command line front-end.
//
//   deepeel explore rom.gb [options] [--json f] [--text f] [--cov f] [--gui]
//   deepeel explore rom.gb --play [...]   play sessions (m0boy-style coverage)
//   deepeel map rom.gb [--json f] [--text f]        static map only
//   deepeel regs gb                                 dump the GB register registry
//   deepeel dis rom.gb bank:addr [n]                disassemble n instructions
#include "core/cart.h"
#include "core/machine.h"
#include "core/sta.h"
#include "mach/m0boy.h"
#include "core/snapshot_png.h"
#include "explore/explorer.h"
#include "explore/sessions.h"
#include "explore/tree.h"
#include "report/report.h"
#include "gui/gui.h"
#include "cpu/sm83/sm83.h"

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

static void usage(void) {
    fprintf(stderr,
            "deepeel — ROM memory-map explorer (game boy first)\n\n"
            "  deepeel explore rom.gb [options]\n"
            "    --max-steps N        per-path step budget (default 100000)\n"
            "    --max-paths N        path cap (default 200000)\n"
            "    --max-total N        global step wall (default 20000000)\n"
            "    --queue N            pending-state cap (default 4096)\n"
            "    --queue-max N        adaptive growth ceiling of the pending states\n"
            "                         (default 4x queue): a full queue doubles instead of\n"
            "                         dropping paths, stopped by --mem-cap when set\n"
            "    --mem-cap MB         wall shared by the play and the queue growth:\n"
            "                         stop/grow no more once RSS exceeds MB MiB\n"
            "    --time-limit SEC     play wall clock: stop gracefully (pool saved) after\n"
            "                         SEC seconds, TAS movies included (0 = off)\n"
            "    --loop-cap N         iterations of one backward edge before cut (default 4096)\n"
            "    --seed-cap N         pointer-scan seed cap (default 4096)\n"
            "    --entry bank:addr    extra seed (repeatable)\n"
            "    --seed-banks         also seed every bank at $4000\n"
            "    --no-pointer-scan    disable pointer-table seeding\n"
            "    --no-context-evict   disable same-context eviction\n"
            "    --no-irq-fork        disable IRQ forking\n"
            "    --no-closure         disable the static flow closure (and its seeds)\n"
            "    --closure-rounds N   closure -> seed -> search loops (default 12; each\n"
            "                         round exits early once it seeds nothing)\n"
            "    --no-covered-prune   keep forking sites whose both sides are covered\n"
            "    --covered-keep N     forks such a site still gets (default 2)\n"
            "    --gui                SDL2 live viewer\n"
            "    --watch [FPS]        small window showing the replaying TAS movie, 1..30 img/s (default 30)\n"
            "    --json f / --text f / --cov f / --romdata f / --map f / --html f   outputs\n"
            "                         (--map x.map.png: squares image + embedded .map;\n"
            "                          deepeel mapx x.map.png out.map extracts it)\n"
            "\n"
            "  play sessions (the m0boy coverage machinery: random inputs, attract mode,\n"
            "  checkpoint pool, epoch scheduler, discovery credit):\n"
            "    --play               enable the play sessions (default 10 workers)\n"
            "    --sessions N         parallel sessions (default 10)\n"
            "    --epoch F            frames per epoch (default 480)\n"
            "    --frames F           total frame budget (default unlimited)\n"
            "    --seed N             input RNG base seed\n"
            "    --stagnation F       stop after F frames with no discovery (default never)\n"
            "    --mem-cap MB         stop (gracefully, resumable) once RSS exceeds MB MiB\n"
            "    --until-static       also stop when every byte is explained\n"
            "    --checkpoints N      checkpoint pool size (default 48)\n"
            "    --stall E            empty epochs before re-seeding from the best (default 3)\n"
            "    --state-dir D        persist the pool (pool.bin) and the PNG snapshots\n"
            "    --resume             continue from a saved pool\n"
            "    --input f            replay an m0boy held-named input script (repeatable)\n"
            "    --tas-ckpt N         TAS replay: capture-interval override in frames (default:\n"
            "                         auto, from --tas-ckpt-secs and --tas-ckpt-max)\n"
            "    --tas-ckpt-secs S    TAS replay: a checkpoint snapshot every S seconds of game\n"
            "                         time (default 30, 0 = off)\n"
            "    --tas-ckpt-max N     TAS replay: snapshot cap per movie (default 1024); the\n"
            "                         interval is min(movie/S, S secs), so a long movie stays at\n"
            "                         one snapshot per S seconds\n"
            "    --tas-ckpt-live      feed the TAS snapshots to the fork explorer during the\n"
            "                         replay instead of the default: save them, then seed the\n"
            "                         whole set after the coverage exploration\n"
            "    --tas f              replay a TAS movie: VBA .vmv, BizHawk .bk2 or an\n"
            "                         extracted input log (repeatable, one worker each)\n"
            "    --tas-dir D          discover movies in directory D (up to 8, best title\n"
            "                         match first when there are more) — repeatable\n"
            "    --summary f          compact JSON profile of the run (map summary, full\n"
            "                         residual list, every counter, tree, play facts)\n"
            "    --tas-solo           single mode: only the TAS workers run (one per movie, no\n"
            "                         random/attract crowd), the play stops when the movies end;\n"
            "                         the checkpoint snapshots are kept, then explored after\n"
            "                         the coverage phases\n"
            "    --human [slot]       play a checkpoint yourself in the GUI (pad/keyboard);\n"
            "                         every advance is snapshotted (PNG) and explored\n"
            "    --from-png f         load a snapshot PNG into the pool before starting\n"
            "    --export-playable D  after the play, write the chosen checkpoint as slot 0\n"
            "                         of the pc project D's <SD>.STA (menu -> Charger\n"
            "                         l'Etat) and its .SAV; --export-slot N picks the slot\n"
            "  deepeel map rom.gb     static map only (pointer tables, padding)\n"
            "  deepeel regs gb        dump the register registry\n"
            "  deepeel dis rom.gb bank:addr [n]\n");
}

static int parse_entry(const char *s, uint8_t *bank, uint16_t *addr) {
    char *end;
    long b = strtol(s, &end, 0);
    if (end == s || *end != ':' || b < 0 || b > 65535) return 0;
    long a = strtol(end + 1, &end, 16); // addresses are hex ($0180 style)
    if (end == end + 1 || a < 0 || a > 0xFFFF) return 0;
    *bank = (uint8_t)b;
    *addr = (uint16_t)a;
    return 1;
}

// --- --tas-dir: movie discovery ------------------------------------------------
static int mov_ext(const char *name) {
    static const char *exts[] = { ".vmv", ".vbm", ".bk2", ".gbm", ".fm2", ".log" };
    size_t n = strlen(name);
    for (int i = 0; i < 6; i++) {
        size_t e = strlen(exts[i]);
        if (n > e && !strcasecmp(name + n - e, exts[i])) return 1;
    }
    return 0;
}

// longest common substring between the lowercase alnum forms of both names
static int title_score(const char *file, const char *title) {
    char a[256], b[64];
    size_t na = 0, nb = 0;
    for (const char *p = file; *p && na < sizeof a - 1; p++)
        if (isalnum((unsigned char)*p)) a[na++] = (char)tolower((unsigned char)*p);
    a[na] = 0;
    for (const char *p = title; *p && nb < sizeof b - 1; p++)
        if (isalnum((unsigned char)*p)) b[nb++] = (char)tolower((unsigned char)*p);
    b[nb] = 0;
    int best = 0;
    for (size_t i = 0; i < nb; i++)
        for (size_t j = 0; j < na; j++) {
            int l = 0;
            while (i + l < nb && j + l < na && b[i + l] == a[j + l]) l++;
            if (l > best) best = l;
        }
    return best;
}

static void tas_collect_dir(const char *dir, const char *title, const char **tas, int *n, int max) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "deepeel: --tas-dir %s: %s\n", dir, strerror(errno));
        return;
    }
    char (*names)[256] = calloc(64, 256);
    if (!names) {
        closedir(d);
        return;
    }
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < 64) {
        if (e->d_name[0] == '.') continue;
        if (!mov_ext(e->d_name)) continue;
        snprintf(names[nn], 256, "%s", e->d_name);
        nn++;
    }
    closedir(d);
    for (int i = 1; i < nn; i++) { // insertion sort, best title match first
        char cur[256];
        snprintf(cur, sizeof cur, "%s", names[i]);
        int cs = title_score(cur, title);
        int j = i - 1;
        while (j >= 0 && title_score(names[j], title) < cs) {
            snprintf(names[j + 1], 256, "%s", names[j]);
            j--;
        }
        snprintf(names[j + 1], 256, "%s", cur);
    }
    for (int i = 0; i < nn && *n < max; i++) {
        char *full = malloc(strlen(dir) + strlen(names[i]) + 2);
        if (!full) break;
        sprintf(full, "%s/%s", dir, names[i]);
        tas[(*n)++] = full;
        fprintf(stderr, "deepeel: --tas-dir: movie %s\n", full);
    }
    free(names);
}

static void report_lanes(const DpPlay *p, const DpResults *res, DpBuf *out);

static int write_file(const char *path, const DpBuf *b) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "deepeel: cannot write %s\n", path);
        return 0;
    }
    fwrite(b->p, 1, b->len, f);
    fclose(f);
    return 1;
}

static void closure_rounds(DpExplorer *ex, DpResults *res);

static int cmd_explore(int argc, char **argv) {
    const char *json = NULL, *text = NULL, *cov = NULL, *romdata = NULL, *map = NULL, *html = NULL, *path = NULL;
    const char *from_png = NULL, *export_dir = NULL, *summary = NULL;
    const char *tas_dirs[8];
    int tas_dirs_n = 0;
    const char *stop_txt = "explore-only";
    unsigned long long play_frames = 0;
    int watch = 0, gui = 0, play = 0, human = 0, human_slot = -1, export_slot = -1, png_slot = -1;
    DpConfig cfg;
    dp_config_defaults(&cfg);
    DpSessionCfg scfg;
    dp_session_cfg_defaults(&scfg);
    uint8_t eb[32];
    uint16_t ea[32];
    int entries = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(a, "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "--summary") && i + 1 < argc) summary = argv[++i];
        else if (!strcmp(a, "--cov") && i + 1 < argc) cov = argv[++i];
        else if (!strcmp(a, "--romdata") && i + 1 < argc) romdata = argv[++i];
        else if (!strcmp(a, "--map") && i + 1 < argc) map = argv[++i];
        else if (!strcmp(a, "--html") && i + 1 < argc) html = argv[++i];
        else if (!strcmp(a, "--gui")) gui = 1;
        else if (!strcmp(a, "--watch")) {
            watch = 30;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') watch = atoi(argv[++i]);
            if (watch < 1) watch = 1;
            if (watch > 30) watch = 30;
        }
        else if (!strcmp(a, "--max-steps") && i + 1 < argc) cfg.max_steps_path = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-paths") && i + 1 < argc) cfg.max_paths = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-total") && i + 1 < argc) cfg.max_steps_total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue") && i + 1 < argc) cfg.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue-max") && i + 1 < argc) cfg.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--loop-cap") && i + 1 < argc) cfg.loop_back_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--mem-cap") && i + 1 < argc) {
            // one wall for both consumers: the play sessions and the queue growth
            scfg.mem_cap_mb = strtoull(argv[++i], NULL, 0);
            cfg.mem_cap_mb = scfg.mem_cap_mb;
        }
        else if (!strcmp(a, "--time-limit") && i + 1 < argc) scfg.time_limit_secs = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-cap") && i + 1 < argc) cfg.seed_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-banks")) cfg.seed_banks = 1;
        else if (!strcmp(a, "--no-pointer-scan")) cfg.no_pointer_scan = 1;
        else if (!strcmp(a, "--no-context-evict")) cfg.no_context_evict = 1;
        else if (!strcmp(a, "--no-irq-fork")) cfg.no_irq_fork = 1;
        else if (!strcmp(a, "--no-key-fork")) cfg.no_key_fork = 1;
        else if (!strcmp(a, "--no-closure")) cfg.no_closure = 1;
        else if (!strcmp(a, "--closure-rounds") && i + 1 < argc) cfg.closure_rounds = atoi(argv[++i]);
        else if (!strcmp(a, "--no-covered-prune")) cfg.no_covered_prune = 1;
        else if (!strcmp(a, "--covered-keep") && i + 1 < argc) cfg.covered_fork_keep = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--entry") && i + 1 < argc && entries < 32) {
            if (!parse_entry(argv[++i], &eb[entries], &ea[entries])) {
                fprintf(stderr, "deepeel: bad --entry '%s' (bank:addr)\n", argv[i]);
                return 2;
            }
            entries++;
        }
        // play sessions
        else if (!strcmp(a, "--play")) play = 1;
        else if (!strcmp(a, "--sessions") && i + 1 < argc) scfg.sessions = atoi(argv[++i]);
        else if (!strcmp(a, "--epoch") && i + 1 < argc) scfg.epoch_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--frames") && i + 1 < argc) scfg.max_frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed") && i + 1 < argc) scfg.seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--stagnation") && i + 1 < argc) scfg.stagnation_frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--mem-cap") && i + 1 < argc) scfg.mem_cap_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--until-static")) scfg.until_static = 1;
        else if (!strcmp(a, "--checkpoints") && i + 1 < argc) scfg.pool_cap = atoi(argv[++i]);
        else if (!strcmp(a, "--stall") && i + 1 < argc) scfg.stall_epochs = atoi(argv[++i]);
        else if (!strcmp(a, "--state-dir") && i + 1 < argc) scfg.state_dir = argv[++i];
        else if (!strcmp(a, "--resume")) scfg.resume = 1;
        else if (!strcmp(a, "--input") && i + 1 < argc && scfg.inputs_n < 8) scfg.inputs[scfg.inputs_n++] = argv[++i];
        else if (!strcmp(a, "--tas") && i + 1 < argc && scfg.tas_n < 8) scfg.tas[scfg.tas_n++] = argv[++i];
        else if (!strcmp(a, "--tas-dir") && i + 1 < argc && tas_dirs_n < 8) tas_dirs[tas_dirs_n++] = argv[++i];
        else if (!strcmp(a, "--tas-ckpt") && i + 1 < argc) scfg.tas_ckpt_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-ckpt-secs") && i + 1 < argc) scfg.tas_ckpt_secs = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-ckpt-max") && i + 1 < argc) scfg.tas_ckpt_max = atoi(argv[++i]);
        else if (!strcmp(a, "--tas-ckpt-live")) scfg.tas_ckpt_live = 1;
        else if (!strcmp(a, "--tas-solo")) scfg.tas_solo = 1;
        else if (!strcmp(a, "--inputs") && i + 1 < argc && !strcmp(argv[++i], "m0x")) scfg.uniform = 0;
        else if (!strcmp(a, "--uniform-hold") && i + 1 < argc) scfg.uniform_hold = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--fork-paths") && i + 1 < argc) scfg.fork_paths_per_epoch = atoi(argv[++i]);
        else if (!strcmp(a, "--from-png") && i + 1 < argc) from_png = argv[++i];
        else if (!strcmp(a, "--export-playable") && i + 1 < argc) export_dir = argv[++i];
        else if (!strcmp(a, "--export-slot") && i + 1 < argc) export_slot = atoi(argv[++i]);
        else if (!strcmp(a, "--human")) {
            human = 1;
            play = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') human_slot = atoi(argv[++i]);
        } else if (!path) path = a;
        else { fprintf(stderr, "deepeel: unknown option %s\n", a); return 2; }
    }
    if (!path) {
        usage();
        return 2;
    }
    if (human && !gui) {
        fprintf(stderr, "deepeel: --human needs --gui (the human plays with eyes)\n");
        return 2;
    }
    char err[256];
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "deepeel: %s\n", err);
        return 1;
    }
    const DpPlatform *plat = dp_platform_gb();
    // the discovered movies join the explicit --tas ones (the worker list caps at 8)
    for (int i = 0; i < tas_dirs_n; i++)
        tas_collect_dir(tas_dirs[i], cart->title, scfg.tas, &scfg.tas_n, 8);
    DpResults res;
    dp_results_init(&res, cart, plat);
    dp_static_scan(&res);
    int vi = dp_mach_pick_variant(cart->rom);
    if (vi < 0) {
        fprintf(stderr, "deepeel: no machine variant compiled for MBC %s\n", dp_mbc_name(cart->mbc));
        dp_results_free(&res);
        dp_cart_free(cart);
        return 1;
    }
    DpMach *mach = dp_mach_open(vi, 0);
    dp_mach_set_rom(mach, cart->rom, cart->rom_size);
    DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
    // the common exploration tree: every seed, fork and session deposit is
    // one choice; --resume reloads it from tree.bin
    DpTree tree;
    dp_tree_init(&tree);
    ex->tree = &tree;
    if (scfg.resume && scfg.state_dir) {
        char tpath[540];
        snprintf(tpath, sizeof tpath, "%s/tree.bin", scfg.state_dir);
        if (dp_tree_load(&tree, tpath))
            fprintf(stderr, "deepeel: resumed tree (%u choices, depth %u)\n", tree.n - 1,
                    tree.max_depth);
    }
    dp_seed_gb(ex);
    for (int i = 0; i < entries; i++)
        dp_explorer_seed_pc(ex, ea[i] >= 0x4000 ? (eb[i] ? eb[i] : 1) : 0, ea[i], DP_SEED_POINTER, 1);
    DpPlay *play_engine = NULL;
    if (play) {
        play_engine = dp_play_new(&scfg, &res, ex);
        if (from_png) png_slot = dp_play_load_png(play_engine, from_png);
        if (from_png && png_slot < 0) return 1;
    }
    int rc = 0;
    if (gui && dp_gui_available()) {
        if (human) dp_play_human_begin(play_engine, human_slot);
        rc = dp_gui_run(&res, &cfg, ex, play_engine);
    } else if (play) {
        dp_play_install_sigint();
        if (watch && play_engine) {
            if (!dp_play_watch_enable(play_engine, watch))
                fprintf(stderr, "deepeel: --watch needs at least one --tas movie, ignored\n");
            else if (dp_watch_start(play_engine, watch))
                dp_play_watch_disable(play_engine);
        }
        dp_disc_add(&res, "start", 0);
        clock_t t0 = clock();
        int reason = dp_play_run(play_engine);
        dp_watch_stop();
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        static const char *why[7] = { "frame budget reached", "stagnation wall reached",
                                      "interrupted", "every byte explained", "memory cap reached",
                                      "TAS replay done", "time budget reached" };
        fprintf(stderr, "deepeel: play done after %llu frames (%s), %.2fs\n",
                (unsigned long long)dp_play_frames(play_engine),
                reason >= 0 && reason < 7 ? why[reason] : "?", secs);
        stop_txt = reason >= 0 && reason < 7 ? why[reason] : "?";
        play_frames = dp_play_frames(play_engine);
        if (scfg.tas_n) {
            if (scfg.tas_ckpt_live)
                fprintf(stderr, "deepeel: %llu TAS gameplay states handed to the fork explorer\n",
                        (unsigned long long)dp_play_tas_ckpts(play_engine));
            else
                fprintf(stderr, "deepeel: %llu TAS checkpoint snapshots saved (%llu captures)\n",
                        (unsigned long long)dp_play_tas_snap_files(play_engine),
                        (unsigned long long)dp_play_tas_ckpts(play_engine));
        }
        if (scfg.tas_n)
            fprintf(stderr, "deepeel: TAS cuts absorbed (movie kept) %llu, wedged-machine fallbacks %llu\n",
                    (unsigned long long)dp_play_tas_cuts(play_engine),
                    (unsigned long long)dp_play_tas_cut_stuck(play_engine));
        // phase 3, mandatory: the total search drains the frontier the
        // sessions fed, depth-first, until every choice is closed
        if (reason != 2) {
            clock_t t1 = clock();
            ex->cfg.dfs = 1;
            ex->stop = 0;
            ex->steps_own = 0;
            uint32_t k0 = res.disc_n ? res.disc[res.disc_n - 1].known : 0;
            dp_explore(ex);
            dp_disc_add(&res, "total-search", 0);
            if (res.disc_n) res.disc[res.disc_n - 1].credit = res.disc[res.disc_n - 1].known - k0;
            closure_rounds(ex, &res);
            fprintf(stderr, "deepeel: total search done in %.2fs (%llu paths)\n",
                    (double)(clock() - t1) / CLOCKS_PER_SEC, (unsigned long long)res.paths_run);
            // phase 4: the TAS checkpoints were only saved during the replay;
            // now that the whole coverage is drained, every one of them seeds
            // its own pass (dedup keeps the already-known states cheap)
            if (scfg.tas_n && !scfg.tas_ckpt_live && dp_play_tas_snap_files(play_engine)) {
                clock_t t2 = clock();
                dp_disc_add(&res, "tas-shoots", 0);
                uint32_t k1 = res.disc_n ? res.disc[res.disc_n - 1].known : 0;
                int seeds = dp_play_seed_tas_snaps(play_engine);
                fprintf(stderr, "deepeel: TAS shoots: %d snapshots seeded\n", seeds);
                dp_explore(ex);
                dp_disc_add(&res, "tas-shoots-search", 0);
                if (res.disc_n) res.disc[res.disc_n - 1].credit = res.disc[res.disc_n - 1].known - k1;
                closure_rounds(ex, &res);
                fprintf(stderr, "deepeel: TAS shoots explored in %.2fs (%llu paths)\n",
                        (double)(clock() - t2) / CLOCKS_PER_SEC, (unsigned long long)res.paths_run);
            }
            // phase 5: the dispatch targets the JP HL sites actually took —
            // some never ran because their continuation hit a budget wall
            int iseeds = dp_seed_indirect(ex);
            if (iseeds) {
                clock_t t3 = clock();
                dp_disc_add(&res, "indirect", 0);
                uint32_t k2 = res.disc_n ? res.disc[res.disc_n - 1].known : 0;
                fprintf(stderr, "deepeel: indirect: %d JP HL targets seeded\n", iseeds);
                dp_explore(ex);
                dp_disc_add(&res, "indirect-search", 0);
                if (res.disc_n) res.disc[res.disc_n - 1].credit = res.disc[res.disc_n - 1].known - k2;
                closure_rounds(ex, &res);
                fprintf(stderr, "deepeel: indirect seeds explored in %.2fs (%llu paths)\n",
                        (double)(clock() - t3) / CLOCKS_PER_SEC, (unsigned long long)res.paths_run);
            }
        }
        DpBuf out;
        dp_buf_init(&out);
        dp_report_text(&res, &cfg, &out);
        report_lanes(play_engine, &res, &out);
        {
            char tline[512];
            dp_tree_stats(&tree, tline, sizeof tline);
            dp_buf_str(&out, tline);
        }
        fwrite(out.p, 1, out.len, stdout);
        dp_buf_free(&out);
        if (scfg.state_dir) {
            char tpath[540];
            snprintf(tpath, sizeof tpath, "%s/tree.bin", scfg.state_dir);
            dp_tree_save(&tree, tpath);
        }
        // the playable export: the chosen checkpoint becomes slot 0 of the pc
        // project's <SD>.STA, the firmware's menu then loads it; the battery
        // .SAV follows (the state image carries the cartridge RAM too, the
        // .SAV keeps the card in step between sessions)
        if (export_dir) {
            int slot = export_slot >= 0 ? export_slot : (png_slot >= 0 ? png_slot : dp_play_best_slot(play_engine));
            const uint8_t *img = dp_play_slot_img(play_engine, slot);
            DpSta sta;
            if (slot < 0 || !img) {
                fprintf(stderr, "deepeel: export: no checkpoint to export\n");
                rc = 1;
            } else if (!dp_sta_from_project(&sta, export_dir, ex->sb, err, sizeof err)) {
                fprintf(stderr, "deepeel: export: %s\n", err);
                rc = 1;
            } else if (!dp_sta_write(&sta, 0, img, ex->sb, dp_play_slot_frame(play_engine, slot),
                                     err, sizeof err)) {
                fprintf(stderr, "deepeel: export: %s\n", err);
                rc = 1;
            } else {
                fprintf(stderr, "deepeel: exported pool slot %d as state slot 0 of %s (frame %u)\n",
                        slot, sta.sta_path, dp_play_slot_frame(play_engine, slot));
                // the .SAV: load the checkpoint into the idle fork machine and
                // copy its cartridge RAM
                if (sta.sav_path[0] && dp_gbl_variant_eram[vi]) {
                    dp_mach_load(mach, img);
                    FILE *sf = fopen(sta.sav_path, "wb");
                    if (sf) {
                        fwrite(mach->api->eram, 1, dp_gbl_variant_eram[vi], sf);
                        fclose(sf);
                        fprintf(stderr, "deepeel: battery RAM written to %s\n", sta.sav_path);
                    } else {
                        fprintf(stderr, "deepeel: cannot write %s\n", sta.sav_path);
                        rc = 1;
                    }
                }
            }
        }
    } else {
        clock_t t0 = clock();
        dp_explore(ex);
        closure_rounds(ex, &res);
        int iseeds = dp_seed_indirect(ex);
        if (iseeds) {
            dp_disc_add(&res, "indirect", 0);
            dp_explore(ex);
            dp_disc_add(&res, "indirect-search", 0);
            closure_rounds(ex, &res);
        }
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        DpBuf out;
        dp_buf_init(&out);
        dp_report_text(&res, &cfg, &out);
        fwrite(out.p, 1, out.len, stdout);
        dp_buf_free(&out);
        fprintf(stderr, "deepeel: done in %.2fs\n", secs);
    }
    if (json) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_json(&res, &cfg, &out);
        if (!write_file(json, &out)) rc = 1;
        dp_buf_free(&out);
    }
    if (text) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_text(&res, &cfg, &out);
        if (!write_file(text, &out)) rc = 1;
        dp_buf_free(&out);
    }
    if (summary) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_summary(&res, &cfg, &tree, stop_txt, play_frames, scfg.tas_n, &out);
        if (!write_file(summary, &out)) rc = 1;
        dp_buf_free(&out);
    }
    if (cov) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_cov(&res, &out);
        if (!write_file(cov, &out)) rc = 1;
        dp_buf_free(&out);
    }
    if (romdata) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_romdata(&res, &out);
        if (!write_file(romdata, &out)) rc = 1;
        dp_buf_free(&out);
    }
    if (map) {
        DpBuf out;
        dp_buf_init(&out);
        size_t ml = strlen(map);
        if (ml > 4 && !strcmp(map + ml - 4, ".png")) {
            // x.map.png: image of the squares with the .map embedded
            if (!dp_report_map_png(&res, &cfg, path, map)) rc = 1;
        } else {
            dp_report_map(&res, &out);
            if (!write_file(map, &out)) rc = 1;
        }
        dp_buf_free(&out);
    }
    if (html) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_html(&res, &out);
        if (!write_file(html, &out)) rc = 1;
        dp_buf_free(&out);
    }
    {
        DpMapSummary sum;
        uint8_t *cls = dp_map_build(&res, &sum);
        if (cls) {
            DpBuf out;
            dp_buf_init(&out);
            dp_map_summary_text(&sum, &out);
            dp_map_residual_text(&res, cls, &out);
            fwrite(out.p, 1, out.len, stdout);
            dp_buf_free(&out);
            free(cls);
        }
    }
    dp_explorer_free(ex);
    if (play_engine) dp_play_free(play_engine);
    dp_mach_close(mach);
    dp_results_free(&res);
    dp_cart_free(cart);
    return rc;
}

static void report_lanes(const DpPlay *p, const DpResults *res, DpBuf *out) {
    DpSessionLane lanes[DP_MAX_SESSIONS + 1];
    int n = dp_play_lanes((DpPlay *)p, lanes, DP_MAX_SESSIONS + 1);
    static const char *mode[6] = { "rng", "attract", "script", "tas", "fixed", "human" };
    dp_buf_str(out, "\nsessions (final):\n");
    for (int i = 0; i < n; i++) {
        dp_buf_fmt(out, "  s%d %-7s frames=%-7u epoch-credit=%-6llu total=%-8llu b%d:%04x%s%s\n",
                   lanes[i].id, mode[lanes[i].mode & 7], lanes[i].frame,
                   (unsigned long long)lanes[i].credit_epoch, (unsigned long long)lanes[i].credit_total,
                   lanes[i].bank, lanes[i].pc, lanes[i].dead ? " dead" : "",
                   lanes[i].backtracked ? " backtracked" : "");
    }
    uint64_t b[DP_CLS_COUNT], tot = 0;
    dp_results_breakdown(res, b);
    for (int i = 0; i < DP_CLS_COUNT; i++) tot += b[i];
    static const char *nm[DP_CLS_COUNT] = { "executed (natural)", "executed (forced only)",
        "static flow only", "data copied/verbatim", "data read", "pointer/padding", "unexplained" };
    dp_buf_str(out, "\nper-byte classification (exclusive, tool observation):\n");
    for (int i = 0; i < DP_CLS_COUNT; i++)
        dp_buf_fmt(out, "  %-24s %7llu  %5.1f%%\n", nm[i], (unsigned long long)b[i],
                   tot ? 100.0 * (double)b[i] / (double)tot : 0.0);
}

static int cmd_mapx(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: deepeel mapx in.map.png out.map\n"); return 2; }
    size_t n = 0;
    char *t = dp_png_text(argv[2], "deepeel_map", &n);
    if (!t) { fprintf(stderr, "no embedded deepeel_map in %s\n", argv[2]); return 1; }
    FILE *f = fopen(argv[3], "wb");
    if (!f) { free(t); perror(argv[3]); return 1; }
    fwrite(t, 1, n, f);
    fclose(f);
    free(t);
    return 0;
}

static int cmd_map(int argc, char **argv) {
    const char *json = NULL, *text = NULL, *path = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) text = argv[++i];
        else if (!path) path = argv[i];
        else return usage(), 2;
    }
    if (!path) return usage(), 2;
    char err[256];
    DpCart *cart = dp_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "deepeel: %s\n", err);
        return 1;
    }
    DpResults res;
    dp_results_init(&res, cart, dp_platform_gb());
    dp_static_scan(&res);
    printf("deepeel map — %s (%s, %d banks)\n", cart->title, dp_mbc_name(cart->mbc), cart->rom_banks);
    for (int b = 0; b < res.banks; b++) {
        int ptr = 0, pad = 0;
        for (int i = 0; i < 0x4000; i++) {
            uint8_t f = res.static_flags[(size_t)b * 0x4000 + i];
            ptr += (f & DP_SF_POINTER) != 0;
            pad += (f & DP_SF_PAD) != 0;
        }
        printf("  bank %3d: %5d pointer-table bytes, %5d padding\n", b, ptr, pad);
    }
    DpConfig cfg;
    dp_config_defaults(&cfg);
    if (json) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_json(&res, &cfg, &out);
        write_file(json, &out);
        dp_buf_free(&out);
    }
    if (text) {
        DpBuf out;
        dp_buf_init(&out);
        dp_report_text(&res, &cfg, &out);
        write_file(text, &out);
        dp_buf_free(&out);
    }
    dp_results_free(&res);
    dp_cart_free(cart);
    return 0;
}

static int cmd_regs(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[2], "gb")) {
        fprintf(stderr, "deepeel regs <platform> — platforms: gb\n");
        return 2;
    }
    const DpPlatform *p = dp_platform_gb();
    printf("%s register registry (%d entries, %d irqs):\n", p->name, p->io_reg_count, p->irq_count);
    for (int i = 0; i < p->io_reg_count; i++) {
        const DpIoReg *r = &p->io_regs[i];
        printf("  %04x-%04x  %-8s %-10s %s%s%s", r->lo, r->hi, r->name, r->category,
               (r->rw & 1) ? "r" : "-", (r->rw & 2) ? "w" : "-", (r->rw & 3) ? " " : "-");
        if (r->impl) {
            printf("  impl:");
            if (r->impl & DP_IMPL_BANK_LO) printf(" bank-lo");
            if (r->impl & DP_IMPL_BANK_HI) printf(" bank-hi");
            if (r->impl & DP_IMPL_RAM_BANK) printf(" ram-bank");
            if (r->impl & DP_IMPL_RAMG) printf(" ramg");
            if (r->impl & DP_IMPL_MODE) printf(" mode");
            if (r->impl & DP_IMPL_DMA_SRC) printf(" dma");
            if (r->impl & DP_IMPL_HDMA) printf(" hdma");
            if (r->impl & DP_IMPL_IRQ) printf(" irq");
            if (r->impl & DP_IMPL_EVENT) printf(" event");
            if (r->impl & DP_IMPL_WINDOW) printf(" window");
            if (r->impl & DP_IMPL_MODELLED) printf(" modelled");
        }
        printf("\n");
    }
    for (int i = 0; i < p->irq_count; i++)
        printf("  irq %s: vector $%04x, IE bit %d\n", p->irqs[i].name, p->irqs[i].vector, p->irqs[i].bit);
    return 0;
}

static int cmd_dis(int argc, char **argv) {
    if (argc < 4) return usage(), 2;
    char err[256];
    DpCart *cart = dp_cart_load(argv[2], err, sizeof err);
    if (!cart) {
        fprintf(stderr, "deepeel: %s\n", err);
        return 1;
    }
    uint8_t bank;
    uint16_t addr;
    if (!parse_entry(argv[3], &bank, &addr)) {
        fprintf(stderr, "deepeel: bad address '%s' (bank:addr)\n", argv[3]);
        return 2;
    }
    int n = argc > 4 ? atoi(argv[4]) : 16;
    // disassembly walks one bank linearly through a windowed view
    const uint8_t *view = cart->rom + (size_t)bank * 0x4000;
    uint32_t view_size = cart->rom_size > (uint32_t)bank * 0x4000 ? cart->rom_size - (uint32_t)bank * 0x4000 : 0;
    uint16_t a = addr & 0x3FFF;
    uint16_t win = addr < 0x4000 ? 0 : 0x4000;
    for (int i = 0; i < n; i++) {
        if (a >= view_size) break;
        char txt[64];
        int len = sm83_disasm(view, view_size, a, txt, sizeof txt);
        printf("%2d:%04x  %s\n", bank, a + win, txt);
        if (len <= 0) break;
        a = (uint16_t)(a + len);
    }
    dp_cart_free(cart);
    return 0;
}

// closure rounds: the static flow proposes the unvisited entries of known
// code, the machine disposes (a fresh total search from each), until a round
// seeds nothing — accepted or refused — or the round cap is reached.  A
// refused seed is retried the next round, once the search freed queue room.
// Each search gets a fresh wall.
static void closure_rounds(DpExplorer *ex, DpResults *res) {
    if (ex->cfg.no_closure) return;
    for (int r = 0; r < ex->cfg.closure_rounds; r++) {
        uint32_t marked = dp_static_closure(res);
        uint32_t ref0 = res->closure_seeds_refused;
        int seeded = dp_closure_seed(ex);
        uint32_t refused = res->closure_seeds_refused - ref0;
        fprintf(stderr, "deepeel: closure round %d: +%u static bytes, %d entries seeded, %u refused\n",
                r + 1, marked, seeded, refused);
        if (!seeded && !refused) return;
        ex->stop = 0;
        ex->steps_own = 0;
        ex->cfg.dfs = 1;
        uint32_t k0 = res->disc_n ? res->disc[res->disc_n - 1].known : 0;
        dp_explore(ex);
        {
            char nm[48];
            snprintf(nm, sizeof nm, "closure-%d", r + 1);
            dp_disc_add(res, nm, 0);
            if (res->disc_n) res->disc[res->disc_n - 1].credit = res->disc[res->disc_n - 1].known - k0;
        }
    }
    dp_static_closure(res); // the last search may have opened more direct flow
}

int main(int argc, char **argv) {
    if (argc < 2) return usage(), 2;
    if (!strcmp(argv[1], "explore")) return cmd_explore(argc, argv);
    if (!strcmp(argv[1], "mapx")) return cmd_mapx(argc, argv);
    if (!strcmp(argv[1], "map")) return cmd_map(argc, argv);
    if (!strcmp(argv[1], "regs")) return cmd_regs(argc, argv);
    if (!strcmp(argv[1], "dis")) return cmd_dis(argc, argv);
    return usage(), 2;
}
