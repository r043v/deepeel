// memscan.c: command line front-end.
//
//   memscan explore rom.gb [options] [--json f] [--text f] [--cov f] [--gui]
//   memscan explore rom.gb --play [...]   play sessions (m0boy-style coverage)
//   memscan map rom.gb [--json f] [--text f]        static map only
//   memscan regs gb                                 dump the GB register registry
//   memscan dis rom.gb bank:addr [n]                disassemble n instructions
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

#include <time.h>

static void usage(void) {
    fprintf(stderr,
            "memscan — ROM memory-map explorer (game boy first)\n\n"
            "  memscan explore rom.gb [options]\n"
            "    --max-steps N        per-path step budget (default 100000)\n"
            "    --max-paths N        path cap (default 200000)\n"
            "    --max-total N        global step wall (default 20000000)\n"
            "    --queue N            pending-state cap (default 2048)\n"
            "    --loop-cap N         iterations of one backward edge before cut (default 65536)\n"
            "    --seed-cap N         pointer-scan seed cap (default 4096)\n"
            "    --entry bank:addr    extra seed (repeatable)\n"
            "    --seed-banks         also seed every bank at $4000\n"
            "    --no-pointer-scan    disable pointer-table seeding\n"
            "    --no-context-evict   disable same-context eviction\n"
            "    --no-irq-fork        disable IRQ forking\n"
            "    --no-closure         disable the static flow closure (and its seeds)\n"
            "    --closure-rounds N   closure -> seed -> search loops (default 4)\n"
            "    --no-covered-prune   keep forking sites whose both sides are covered\n"
            "    --covered-keep N     forks such a site still gets (default 2)\n"
            "    --gui                SDL2 live viewer\n"
            "    --json f / --text f / --cov f / --romdata f / --map f / --html f   outputs\n"
            "                         (--map x.map.png: squares image + embedded .map;\n"
            "                          memscan mapx x.map.png out.map extracts it)\n"
            "\n"
            "  play sessions (the m0boy coverage machinery: random inputs, attract mode,\n"
            "  checkpoint pool, epoch scheduler, discovery credit):\n"
            "    --play               enable the play sessions (default 10 workers)\n"
            "    --sessions N         parallel sessions (default 10)\n"
            "    --epoch F            frames per epoch (default 480)\n"
            "    --frames F           total frame budget (default unlimited)\n"
            "    --seed N             input RNG base seed\n"
            "    --stagnation F       stop after F frames with no discovery (default never)\n"
            "    --until-static       also stop when every byte is explained\n"
            "    --checkpoints N      checkpoint pool size (default 48)\n"
            "    --stall E            empty epochs before re-seeding from the best (default 3)\n"
            "    --state-dir D        persist the pool (pool.bin) and the PNG snapshots\n"
            "    --resume             continue from a saved pool\n"
            "    --input f            replay an m0boy held-named input script (repeatable)\n"
            "    --tas-ckpt N         TAS replay: fork-explorer seed every N frames (default 240, 0 = off)\n"
            "    --tas f              replay a TAS movie: VBA .vmv, BizHawk .bk2 or an\n"
            "                         extracted input log (repeatable, one worker each)\n"
            "    --human [slot]       play a checkpoint yourself in the GUI (pad/keyboard);\n"
            "                         every advance is snapshotted (PNG) and explored\n"
            "    --from-png f         load a snapshot PNG into the pool before starting\n"
            "    --export-playable D  after the play, write the chosen checkpoint as slot 0\n"
            "                         of the pc project D's <SD>.STA (menu -> Charger\n"
            "                         l'Etat) and its .SAV; --export-slot N picks the slot\n"
            "  memscan map rom.gb     static map only (pointer tables, padding)\n"
            "  memscan regs gb        dump the register registry\n"
            "  memscan dis rom.gb bank:addr [n]\n");
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

static void report_lanes(const MsPlay *p, const MsResults *res, MsBuf *out);

static int write_file(const char *path, const MsBuf *b) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "memscan: cannot write %s\n", path);
        return 0;
    }
    fwrite(b->p, 1, b->len, f);
    fclose(f);
    return 1;
}

static void closure_rounds(MsExplorer *ex, MsResults *res);

static int cmd_explore(int argc, char **argv) {
    const char *json = NULL, *text = NULL, *cov = NULL, *romdata = NULL, *map = NULL, *html = NULL, *path = NULL;
    const char *from_png = NULL, *export_dir = NULL;
    int gui = 0, play = 0, human = 0, human_slot = -1, export_slot = -1, png_slot = -1;
    MsConfig cfg;
    ms_config_defaults(&cfg);
    MsSessionCfg scfg;
    ms_session_cfg_defaults(&scfg);
    uint8_t eb[32];
    uint16_t ea[32];
    int entries = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(a, "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "--cov") && i + 1 < argc) cov = argv[++i];
        else if (!strcmp(a, "--romdata") && i + 1 < argc) romdata = argv[++i];
        else if (!strcmp(a, "--map") && i + 1 < argc) map = argv[++i];
        else if (!strcmp(a, "--html") && i + 1 < argc) html = argv[++i];
        else if (!strcmp(a, "--gui")) gui = 1;
        else if (!strcmp(a, "--max-steps") && i + 1 < argc) cfg.max_steps_path = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-paths") && i + 1 < argc) cfg.max_paths = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-total") && i + 1 < argc) cfg.max_steps_total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue") && i + 1 < argc) cfg.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--loop-cap") && i + 1 < argc) cfg.loop_back_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
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
                fprintf(stderr, "memscan: bad --entry '%s' (bank:addr)\n", argv[i]);
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
        else if (!strcmp(a, "--until-static")) scfg.until_static = 1;
        else if (!strcmp(a, "--checkpoints") && i + 1 < argc) scfg.pool_cap = atoi(argv[++i]);
        else if (!strcmp(a, "--stall") && i + 1 < argc) scfg.stall_epochs = atoi(argv[++i]);
        else if (!strcmp(a, "--state-dir") && i + 1 < argc) scfg.state_dir = argv[++i];
        else if (!strcmp(a, "--resume")) scfg.resume = 1;
        else if (!strcmp(a, "--input") && i + 1 < argc && scfg.inputs_n < 8) scfg.inputs[scfg.inputs_n++] = argv[++i];
        else if (!strcmp(a, "--tas") && i + 1 < argc && scfg.tas_n < 8) scfg.tas[scfg.tas_n++] = argv[++i];
        else if (!strcmp(a, "--tas-ckpt") && i + 1 < argc) scfg.tas_ckpt_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
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
        else { fprintf(stderr, "memscan: unknown option %s\n", a); return 2; }
    }
    if (!path) {
        usage();
        return 2;
    }
    if (human && !gui) {
        fprintf(stderr, "memscan: --human needs --gui (the human plays with eyes)\n");
        return 2;
    }
    char err[256];
    MsCart *cart = ms_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "memscan: %s\n", err);
        return 1;
    }
    const MsPlatform *plat = ms_platform_gb();
    MsResults res;
    ms_results_init(&res, cart, plat);
    ms_static_scan(&res);
    int vi = ms_mach_pick_variant(cart->rom);
    if (vi < 0) {
        fprintf(stderr, "memscan: no machine variant compiled for MBC %s\n", ms_mbc_name(cart->mbc));
        ms_results_free(&res);
        ms_cart_free(cart);
        return 1;
    }
    MsMach *mach = ms_mach_open(vi, 0);
    ms_mach_set_rom(mach, cart->rom, cart->rom_size);
    MsExplorer *ex = ms_explorer_new(&cfg, &res, mach);
    // the common exploration tree: every seed, fork and session deposit is
    // one choice; --resume reloads it from tree.bin
    MsTree tree;
    ms_tree_init(&tree);
    ex->tree = &tree;
    if (scfg.resume && scfg.state_dir) {
        char tpath[540];
        snprintf(tpath, sizeof tpath, "%s/tree.bin", scfg.state_dir);
        if (ms_tree_load(&tree, tpath))
            fprintf(stderr, "memscan: resumed tree (%u choices, depth %u)\n", tree.n - 1,
                    tree.max_depth);
    }
    ms_seed_gb(ex);
    for (int i = 0; i < entries; i++)
        ms_explorer_seed_pc(ex, ea[i] >= 0x4000 ? (eb[i] ? eb[i] : 1) : 0, ea[i], MS_SEED_POINTER, 1);
    MsPlay *play_engine = NULL;
    if (play) {
        play_engine = ms_play_new(&scfg, &res, ex);
        if (from_png) png_slot = ms_play_load_png(play_engine, from_png);
        if (from_png && png_slot < 0) return 1;
    }
    int rc = 0;
    if (gui && ms_gui_available()) {
        if (human) ms_play_human_begin(play_engine, human_slot);
        rc = ms_gui_run(&res, &cfg, ex, play_engine);
    } else if (play) {
        ms_play_install_sigint();
        ms_disc_add(&res, "start", 0);
        clock_t t0 = clock();
        int reason = ms_play_run(play_engine);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        static const char *why[4] = { "frame budget reached", "stagnation wall reached",
                                      "interrupted", "every byte explained" };
        fprintf(stderr, "memscan: play done after %llu frames (%s), %.2fs\n",
                (unsigned long long)ms_play_frames(play_engine),
                reason >= 0 && reason < 4 ? why[reason] : "?", secs);
        if (scfg.tas_n)
            fprintf(stderr, "memscan: %llu TAS gameplay states handed to the fork explorer\n",
                    (unsigned long long)ms_play_tas_ckpts(play_engine));
        if (scfg.tas_n)
            fprintf(stderr, "memscan: TAS cuts absorbed (movie kept) %llu, wedged-machine fallbacks %llu\n",
                    (unsigned long long)ms_play_tas_cuts(play_engine),
                    (unsigned long long)ms_play_tas_cut_stuck(play_engine));
        // phase 3, mandatory: the total search drains the frontier the
        // sessions fed, depth-first, until every choice is closed
        if (reason != 2) {
            clock_t t1 = clock();
            ex->cfg.dfs = 1;
            ex->stop = 0;
            ex->steps_own = 0;
            uint32_t k0 = res.disc_n ? res.disc[res.disc_n - 1].known : 0;
            ms_explore(ex);
            ms_disc_add(&res, "total-search", 0);
            if (res.disc_n) res.disc[res.disc_n - 1].credit = res.disc[res.disc_n - 1].known - k0;
            closure_rounds(ex, &res);
            fprintf(stderr, "memscan: total search done in %.2fs (%llu paths)\n",
                    (double)(clock() - t1) / CLOCKS_PER_SEC, (unsigned long long)res.paths_run);
        }
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        report_lanes(play_engine, &res, &out);
        {
            char tline[512];
            ms_tree_stats(&tree, tline, sizeof tline);
            ms_buf_str(&out, tline);
        }
        fwrite(out.p, 1, out.len, stdout);
        ms_buf_free(&out);
        if (scfg.state_dir) {
            char tpath[540];
            snprintf(tpath, sizeof tpath, "%s/tree.bin", scfg.state_dir);
            ms_tree_save(&tree, tpath);
        }
        // the playable export: the chosen checkpoint becomes slot 0 of the pc
        // project's <SD>.STA, the firmware's menu then loads it; the battery
        // .SAV follows (the state image carries the cartridge RAM too, the
        // .SAV keeps the card in step between sessions)
        if (export_dir) {
            int slot = export_slot >= 0 ? export_slot : (png_slot >= 0 ? png_slot : ms_play_best_slot(play_engine));
            const uint8_t *img = ms_play_slot_img(play_engine, slot);
            MsSta sta;
            if (slot < 0 || !img) {
                fprintf(stderr, "memscan: export: no checkpoint to export\n");
                rc = 1;
            } else if (!ms_sta_from_project(&sta, export_dir, ex->sb, err, sizeof err)) {
                fprintf(stderr, "memscan: export: %s\n", err);
                rc = 1;
            } else if (!ms_sta_write(&sta, 0, img, ex->sb, ms_play_slot_frame(play_engine, slot),
                                     err, sizeof err)) {
                fprintf(stderr, "memscan: export: %s\n", err);
                rc = 1;
            } else {
                fprintf(stderr, "memscan: exported pool slot %d as state slot 0 of %s (frame %u)\n",
                        slot, sta.sta_path, ms_play_slot_frame(play_engine, slot));
                // the .SAV: load the checkpoint into the idle fork machine and
                // copy its cartridge RAM
                if (sta.sav_path[0] && ms_gbl_variant_eram[vi]) {
                    ms_mach_load(mach, img);
                    FILE *sf = fopen(sta.sav_path, "wb");
                    if (sf) {
                        fwrite(mach->api->eram, 1, ms_gbl_variant_eram[vi], sf);
                        fclose(sf);
                        fprintf(stderr, "memscan: battery RAM written to %s\n", sta.sav_path);
                    } else {
                        fprintf(stderr, "memscan: cannot write %s\n", sta.sav_path);
                        rc = 1;
                    }
                }
            }
        }
    } else {
        clock_t t0 = clock();
        ms_explore(ex);
        closure_rounds(ex, &res);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        fwrite(out.p, 1, out.len, stdout);
        ms_buf_free(&out);
        fprintf(stderr, "memscan: done in %.2fs\n", secs);
    }
    if (json) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_json(&res, &cfg, &out);
        if (!write_file(json, &out)) rc = 1;
        ms_buf_free(&out);
    }
    if (text) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        if (!write_file(text, &out)) rc = 1;
        ms_buf_free(&out);
    }
    if (cov) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_cov(&res, &out);
        if (!write_file(cov, &out)) rc = 1;
        ms_buf_free(&out);
    }
    if (romdata) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_romdata(&res, &out);
        if (!write_file(romdata, &out)) rc = 1;
        ms_buf_free(&out);
    }
    if (map) {
        MsBuf out;
        ms_buf_init(&out);
        size_t ml = strlen(map);
        if (ml > 4 && !strcmp(map + ml - 4, ".png")) {
            // x.map.png: image of the squares with the .map embedded
            if (!ms_report_map_png(&res, map)) rc = 1;
        } else {
            ms_report_map(&res, &out);
            if (!write_file(map, &out)) rc = 1;
        }
        ms_buf_free(&out);
    }
    if (html) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_html(&res, &out);
        if (!write_file(html, &out)) rc = 1;
        ms_buf_free(&out);
    }
    {
        MsMapSummary sum;
        uint8_t *cls = ms_map_build(&res, &sum);
        if (cls) {
            MsBuf out;
            ms_buf_init(&out);
            ms_map_summary_text(&sum, &out);
            fwrite(out.p, 1, out.len, stdout);
            ms_buf_free(&out);
            free(cls);
        }
    }
    ms_explorer_free(ex);
    if (play_engine) ms_play_free(play_engine);
    ms_mach_close(mach);
    ms_results_free(&res);
    ms_cart_free(cart);
    return rc;
}

static void report_lanes(const MsPlay *p, const MsResults *res, MsBuf *out) {
    MsSessionLane lanes[MS_MAX_SESSIONS + 1];
    int n = ms_play_lanes((MsPlay *)p, lanes, MS_MAX_SESSIONS + 1);
    static const char *mode[6] = { "rng", "attract", "script", "tas", "fixed", "human" };
    ms_buf_str(out, "\nsessions (final):\n");
    for (int i = 0; i < n; i++) {
        ms_buf_fmt(out, "  s%d %-7s frames=%-7u epoch-credit=%-6llu total=%-8llu b%d:%04x%s%s\n",
                   lanes[i].id, mode[lanes[i].mode & 7], lanes[i].frame,
                   (unsigned long long)lanes[i].credit_epoch, (unsigned long long)lanes[i].credit_total,
                   lanes[i].bank, lanes[i].pc, lanes[i].dead ? " dead" : "",
                   lanes[i].backtracked ? " backtracked" : "");
    }
    uint64_t b[MS_CLS_COUNT], tot = 0;
    ms_results_breakdown(res, b);
    for (int i = 0; i < MS_CLS_COUNT; i++) tot += b[i];
    static const char *nm[MS_CLS_COUNT] = { "executed (natural)", "executed (forced only)",
        "static flow only", "data copied/verbatim", "data read", "pointer/padding", "unexplained" };
    ms_buf_str(out, "\nper-byte classification (exclusive, tool observation):\n");
    for (int i = 0; i < MS_CLS_COUNT; i++)
        ms_buf_fmt(out, "  %-24s %7llu  %5.1f%%\n", nm[i], (unsigned long long)b[i],
                   tot ? 100.0 * (double)b[i] / (double)tot : 0.0);
}

static int cmd_mapx(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: memscan mapx in.map.png out.map\n"); return 2; }
    size_t n = 0;
    char *t = ms_png_text(argv[2], "memscan_map", &n);
    if (!t) { fprintf(stderr, "no embedded memscan_map in %s\n", argv[2]); return 1; }
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
    MsCart *cart = ms_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "memscan: %s\n", err);
        return 1;
    }
    MsResults res;
    ms_results_init(&res, cart, ms_platform_gb());
    ms_static_scan(&res);
    printf("memscan map — %s (%s, %d banks)\n", cart->title, ms_mbc_name(cart->mbc), cart->rom_banks);
    for (int b = 0; b < res.banks; b++) {
        int ptr = 0, pad = 0;
        for (int i = 0; i < 0x4000; i++) {
            uint8_t f = res.static_flags[(size_t)b * 0x4000 + i];
            ptr += (f & MS_SF_POINTER) != 0;
            pad += (f & MS_SF_PAD) != 0;
        }
        printf("  bank %3d: %5d pointer-table bytes, %5d padding\n", b, ptr, pad);
    }
    MsConfig cfg;
    ms_config_defaults(&cfg);
    if (json) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_json(&res, &cfg, &out);
        write_file(json, &out);
        ms_buf_free(&out);
    }
    if (text) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        write_file(text, &out);
        ms_buf_free(&out);
    }
    ms_results_free(&res);
    ms_cart_free(cart);
    return 0;
}

static int cmd_regs(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[2], "gb")) {
        fprintf(stderr, "memscan regs <platform> — platforms: gb\n");
        return 2;
    }
    const MsPlatform *p = ms_platform_gb();
    printf("%s register registry (%d entries, %d irqs):\n", p->name, p->io_reg_count, p->irq_count);
    for (int i = 0; i < p->io_reg_count; i++) {
        const MsIoReg *r = &p->io_regs[i];
        printf("  %04x-%04x  %-8s %-10s %s%s%s", r->lo, r->hi, r->name, r->category,
               (r->rw & 1) ? "r" : "-", (r->rw & 2) ? "w" : "-", (r->rw & 3) ? " " : "-");
        if (r->impl) {
            printf("  impl:");
            if (r->impl & MS_IMPL_BANK_LO) printf(" bank-lo");
            if (r->impl & MS_IMPL_BANK_HI) printf(" bank-hi");
            if (r->impl & MS_IMPL_RAM_BANK) printf(" ram-bank");
            if (r->impl & MS_IMPL_RAMG) printf(" ramg");
            if (r->impl & MS_IMPL_MODE) printf(" mode");
            if (r->impl & MS_IMPL_DMA_SRC) printf(" dma");
            if (r->impl & MS_IMPL_HDMA) printf(" hdma");
            if (r->impl & MS_IMPL_IRQ) printf(" irq");
            if (r->impl & MS_IMPL_EVENT) printf(" event");
            if (r->impl & MS_IMPL_WINDOW) printf(" window");
            if (r->impl & MS_IMPL_MODELLED) printf(" modelled");
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
    MsCart *cart = ms_cart_load(argv[2], err, sizeof err);
    if (!cart) {
        fprintf(stderr, "memscan: %s\n", err);
        return 1;
    }
    uint8_t bank;
    uint16_t addr;
    if (!parse_entry(argv[3], &bank, &addr)) {
        fprintf(stderr, "memscan: bad address '%s' (bank:addr)\n", argv[3]);
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
    ms_cart_free(cart);
    return 0;
}

// closure rounds: the static flow proposes the unvisited entries of known
// code, the machine disposes (a fresh total search from each), until a round
// seeds nothing or the round cap is reached.  Each search gets a fresh wall.
static void closure_rounds(MsExplorer *ex, MsResults *res) {
    if (ex->cfg.no_closure) return;
    for (int r = 0; r < ex->cfg.closure_rounds; r++) {
        uint32_t marked = ms_static_closure(res);
        int seeded = ms_closure_seed(ex);
        fprintf(stderr, "memscan: closure round %d: +%u static bytes, %d entries seeded\n", r + 1,
                marked, seeded);
        if (!seeded) return;
        ex->stop = 0;
        ex->steps_own = 0;
        ex->cfg.dfs = 1;
        uint32_t k0 = res->disc_n ? res->disc[res->disc_n - 1].known : 0;
        ms_explore(ex);
        {
            char nm[48];
            snprintf(nm, sizeof nm, "closure-%d", r + 1);
            ms_disc_add(res, nm, 0);
            if (res->disc_n) res->disc[res->disc_n - 1].credit = res->disc[res->disc_n - 1].known - k0;
        }
    }
    ms_static_closure(res); // the last search may have opened more direct flow
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
