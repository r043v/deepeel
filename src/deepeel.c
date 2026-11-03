// deepeel.c: command line front-end.
//
//   deepeel explore rom.gb [options] [--json f] [--text f] [--cov f] [--gui]
//   deepeel explore rom.gb --play [...]   play sessions (m0boy-style coverage)
//   deepeel map rom.gb [--json f] [--text f]        static map only
//   deepeel regs gb                                 dump the GB register registry
//   deepeel dis rom.gb bank:addr [n]                disassemble n instructions
#include <sys/stat.h>
#include "core/cart.h"
#include "core/cart_md.h"
#include "explore/md_trace.h"
#include "core/machine.h"
#include "core/sta.h"
#include "mach/m0boy.h"
#include "core/snapshot_png.h"
#include "explore/explorer.h"
#include "explore/lots.h"
#include "explore/sessions.h"
#include "explore/tree.h"
#include "report/report.h"
#include "gui/gui.h"
#include "cpu/sm83/sm83.h"
#include "platform/sega/sega.h"
#include "platform/msx/msx.h"
#include "platform/sega/explore.h"
#include "platform/md/explore.h"
#include "platform/md/md_registry.h"
#include "cpu/m68k/m68k.h"
#include "platform/nes/explore.h"
#include "platform/pce/explore.h"
#include "platform/pce/pce.h"

static uint8_t *nes_read_file(const char *path, size_t *size);
int dp_lr_command(int argc, char **argv);
static int cmd_nes(int argc, char **argv);
static int cmd_pce(int argc, char **argv);
static int cmd_maprender(int argc, char **argv);

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

static void usage(void) {
    fprintf(stderr,
            "deepeel — ROM memory-map explorer (game boy first)\n\n"
            "  deepeel explore rom.gb [options]\n"
            "  deepeel z80 explore <sg|sms|gg|msx> ROM [--input-total N] [--input-forks N] [--text FILE] [--map FILE] [--evidence FILE]\n"
            "  deepeel z80 dis ROM hex-offset [instruction-count]\n"
            "  deepeel lr explore CORE ROM [--text FILE] [--map FILE] [--path-steps N] [--max-total N] [--queue N]\n"
            "  deepeel md explore ROM [--tas GMV] [--max-total N] [--path-steps N] [--queue N] [--queue-max N] [--fork-cap N] [--visit-cap N] [--entry ADDR]... [--watch-write ADDR] [--exclude-read-pc ADDR] [--text FILE] [--map FILE] [--evidence FILE]\n"
            "  deepeel md dis ROM hex-offset [instruction-count]\n"
            "  deepeel regs <gb|sg|sms|gg|msx|md>\n"
            "    --max-steps N        per-path step budget (default 100000)\n"
            "    --max-paths N        path cap (default 200000)\n"
            "    --max-total N        global step wall (default 20000000)\n"
            "    --queue N            pending-state cap (default 4096)\n"
            "    --queue-max N        adaptive growth ceiling of the pending states\n"
            "                         (default 4x queue): a full queue doubles instead of\n"
            "                         dropping paths, stopped by --mem-cap when set\n"
            "    --mem-cap MB         wall shared by the play and the queue growth:\n"
            "                         stop/grow no more once RSS exceeds MB MiB\n"
            "    --jobs N             parallel drain: N explorers (own machine each) share\n"
            "                         one work pool, exact dedup and spool; the RAM wall\n"
            "                         bounds the shared pool, the spool keeps the excess\n"
            "    --spool-cap-mb MB    disk budget of the state spool (default 4096): with\n"
            "                         a --state-dir, states the RAM worklist cannot hold\n"
            "                         wait in <state-dir>/spool.bin and --resume continues\n"
            "                         an interrupted run exactly where it stopped\n"
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
            "    --no-provenance      disable the static HL/A provenance (jp hl targets)\n"
            "    --no-siblings        disable the copy-sibling table inference\n"
            "    --closure-rounds N   closure -> seed -> search loops (default 12; each\n"
            "                         round exits early once it seeds nothing)\n"
            "    --no-covered-prune   keep forking sites whose both sides are covered\n"
            "    --covered-keep N     forks such a site still gets (default 2)\n"
            "    --gui                SDL2 live viewer\n"
            "    --watch [FPS]        small window showing the replaying TAS movie, 1..30 img/s (default 30)\n"
            "    --json f / --text f / --cov f / --romdata f / --map f / --html f   outputs\n"
            "                         (--map: PNG by default, 1 px per ROM byte + played heat,\n"
            "                          the .map, the played .cov and the usage per zone embedded;\n"
            "                          a path ending in .map or .txt writes the plain .map text;\n"
            "                          deepeel mapx x.map.png out [--chunk NAME] extracts one)\n"
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
            "    --tas-any-rom        replay movies recorded on another ROM (revision): by default a\n"
            "                         movie whose SHA-1 (bk2) or checksums (vbm) differ is refused\n"
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
            "  deepeel maprender rom.gb in.map out.map.png [--meta f]\n"
            "                         re-render the squares image from a saved .map\n"
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

static void closure_rounds(DpExplorer *ex, DpResults *res, uint32_t *seed_spent, uint32_t seed_total);

static int cmd_md(int argc, char **argv);
static int g_drain_only; // frontier drain: no root seeding, only the saved frontier runs
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
    uint8_t tier_limit = 0, defer_all = 0;
    uint8_t eb[32];
    int spool_to_frontier = 0;
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
        else if (!strcmp(a, "--tier") && i + 1 < argc) {
            const char *t = argv[++i];
            if (!strcmp(t, "inf")) defer_all = 1; // the last tier: covered forks resume too
            else tier_limit = (uint8_t)atoi(t);
        }
        else if (!strcmp(a, "--time-limit") && i + 1 < argc) scfg.time_limit_secs = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--frontier-cap-gb") && i + 1 < argc) cfg.frontier_cap_mb = strtoull(argv[++i], NULL, 0) * 1024;
        else if (!strcmp(a, "--spool-cap-mb") && i + 1 < argc) cfg.spool_cap_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--jobs") && i + 1 < argc) cfg.jobs = atoi(argv[++i]);
        else if (!strcmp(a, "--seed-cap") && i + 1 < argc) cfg.seed_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-banks")) cfg.seed_banks = 1;
        else if (!strcmp(a, "--no-pointer-scan")) cfg.no_pointer_scan = 1;
        else if (!strcmp(a, "--no-context-evict")) cfg.no_context_evict = 1;
        else if (!strcmp(a, "--no-irq-fork")) cfg.no_irq_fork = 1;
        else if (!strcmp(a, "--no-key-fork")) cfg.no_key_fork = 1;
        else if (!strcmp(a, "--no-p1-fork")) cfg.no_p1_fork = 1;
        else if (!strcmp(a, "--no-closure")) cfg.no_closure = 1;
        else if (!strcmp(a, "--no-provenance")) cfg.no_provenance = 1;
        else if (!strcmp(a, "--no-siblings")) cfg.no_siblings = 1;
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
        else if (!strcmp(a, "--spool-to-frontier")) spool_to_frontier = 1;
        else if (!strcmp(a, "--input") && i + 1 < argc && scfg.inputs_n < 8) scfg.inputs[scfg.inputs_n++] = argv[++i];
        else if (!strcmp(a, "--tas") && i + 1 < argc && scfg.tas_n < 8) scfg.tas[scfg.tas_n++] = argv[++i];
        else if (!strcmp(a, "--tas-dir") && i + 1 < argc && tas_dirs_n < 8) tas_dirs[tas_dirs_n++] = argv[++i];
        else if (!strcmp(a, "--tas-ckpt") && i + 1 < argc) scfg.tas_ckpt_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-ckpt-secs") && i + 1 < argc) scfg.tas_ckpt_secs = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-ckpt-max") && i + 1 < argc) scfg.tas_ckpt_max = atoi(argv[++i]);
        else if (!strcmp(a, "--tas-ckpt-live")) scfg.tas_ckpt_live = 1;
        else if (!strcmp(a, "--tas-solo")) scfg.tas_solo = 1;
        else if (!strcmp(a, "--tas-any-rom")) scfg.tas_any_rom = 1;
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
    {   // Mega Drive ROMs: alias of `deepeel md explore` (forking exploration)
        DpMdCart mc;
        if (dp_md_cart_load(&mc, path, err, sizeof err) == 0) {
            dp_md_cart_free(&mc);
            char tot[32];
            char *av[12] = {"deepeel", "md", "explore", (char *)path};
            int n = 4;
            if (scfg.tas_n > 1) {
                fprintf(stderr, "deepeel: Mega Drive currently accepts one GMV movie\n");
                return 2;
            }
            if (scfg.tas_n == 1) { av[n++] = "--tas"; av[n++] = (char *)scfg.tas[0]; }
            if (cfg.max_steps_total) {
                snprintf(tot, sizeof tot, "%llu", (unsigned long long)cfg.max_steps_total);
                av[n++] = "--max-total";
                av[n++] = tot;
            }
            if (text) {
                av[n++] = "--text";
                av[n++] = (char *)text;
            }
            return cmd_md(n, av);
        }
    }
    {   // HuCard images (.pce): alias of `deepeel pce explore`
        size_t pl = strlen(path);
        if (pl > 4 && !strcasecmp(path + pl - 4, ".pce")) {
            char *av[12] = {"deepeel", "pce", "explore", (char *)path};
            int n = 4;
            if (cfg.max_steps_total) {
                static char tot[32];
                snprintf(tot, sizeof tot, "%llu", (unsigned long long)cfg.max_steps_total);
                av[n++] = "--max-total";
                av[n++] = tot;
            }
            if (text) { av[n++] = "--text"; av[n++] = (char *)text; }
            return cmd_pce(n, av);
        }
    }
    {   // iNES images: alias of `deepeel nes explore` (6502 forking exploration)
        size_t nsize = 0;
        uint8_t *ndata = nes_read_file(path, &nsize);
        if (ndata) {
            int ines = nsize >= 16 && !memcmp(ndata, "NES\x1a", 4);
            free(ndata);
            if (ines) {
                char *av[12] = {"deepeel", "nes", "explore", (char *)path};
                int n = 4;
                if (cfg.max_steps_total) {
                    static char tot[32];
                    snprintf(tot, sizeof tot, "%llu", (unsigned long long)cfg.max_steps_total);
                    av[n++] = "--max-total";
                    av[n++] = tot;
                }
                if (text) { av[n++] = "--text"; av[n++] = (char *)text; }
                return cmd_nes(n, av);
            }
        }
    }
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
    // cascading seed budgets (z80 branch): the re-seeding phases share one
    // total (2x the per-phase cap); each phase spends from the solde left by
    // the previous ones, refusals for a spent solde are counted
    uint32_t seed_total = cfg.seed_cap > (1u << 30) ? cfg.seed_cap : cfg.seed_cap * 2;
    uint32_t seed_spent = 0;
    dp_static_scan(&res);
    dp_disc_add(&res, "static-scan", 0); // method shares: the static baseline
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
    ex->defer_tier_limit = tier_limit;
    ex->defer_all = defer_all;
    // the frontier directory exists whatever the options: next to the outputs
    // when there is no --state-dir
    char fdir_auto[600];
    const char *fdir = scfg.state_dir;
    if (!fdir) {
        const char *base = json ? json : text ? text : html ? html : summary ? summary : NULL;
        if (base) { snprintf(fdir_auto, sizeof fdir_auto, "%s.frontier", base); fdir = fdir_auto; }
    }
    // the disk spool: overflow states survive any stop, --resume continues
    // where the previous run stopped (worklist + exact dedup)
    char spool_path[540], exact_path[540];
    int spool_on = 0;
    if (scfg.state_dir) {
        snprintf(spool_path, sizeof spool_path, "%s/spool.bin", scfg.state_dir);
        snprintf(exact_path, sizeof exact_path, "%s/exact.bin", scfg.state_dir);
        spool_on = dp_explorer_spool_attach(ex, spool_path, scfg.resume);
        if (!spool_on)
            fprintf(stderr, "deepeel: spool unavailable (%s), overflow states are refused and counted (spool_refused)\n",
                    spool_path);
        if (scfg.resume) dp_explorer_exact_load(ex, exact_path);
    }
    if (scfg.resume && fdir) {
        int fl = dp_explorer_frontier_load(ex, fdir);
        if (fl) fprintf(stderr, "deepeel: resumed frontier (%d continuations/seeds loaded, %llu more on disk)\n", fl,
                        (unsigned long long)dp_explorer_frontier_disk(ex));
    }
    if (spool_to_frontier && fdir) {
        uint64_t cv = dp_explorer_spool_convert(ex);
        fprintf(stderr, "deepeel: %llu spool records converted to frontier lots in %s/frontier\n", (unsigned long long)cv, fdir);
    }
    if (!g_drain_only) dp_seed_gb(ex); // a drain runs the saved continuations only
    for (int i = 0; !g_drain_only && i < entries; i++)
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
            closure_rounds(ex, &res, &seed_spent, seed_total);
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
                closure_rounds(ex, &res, &seed_spent, seed_total);
                fprintf(stderr, "deepeel: TAS shoots explored in %.2fs (%llu paths)\n",
                        (double)(clock() - t2) / CLOCKS_PER_SEC, (unsigned long long)res.paths_run);
            }
            // phase 5: the dispatch targets the JP HL sites actually took —
            // some never ran because their continuation hit a budget wall
            int iseeds = dp_seed_indirect(ex, seed_total - seed_spent);
            seed_spent += (uint32_t)iseeds;
            if (iseeds) {
                clock_t t3 = clock();
                dp_disc_add(&res, "indirect", 0);
                uint32_t k2 = res.disc_n ? res.disc[res.disc_n - 1].known : 0;
                fprintf(stderr, "deepeel: indirect: %d JP HL targets seeded\n", iseeds);
                dp_explore(ex);
                dp_disc_add(&res, "indirect-search", 0);
                if (res.disc_n) res.disc[res.disc_n - 1].credit = res.disc[res.disc_n - 1].known - k2;
                closure_rounds(ex, &res, &seed_spent, seed_total);
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
        dp_disc_add(&res, "explore", 0); // method shares: the fork explorer
        closure_rounds(ex, &res, &seed_spent, seed_total);
        int iseeds = dp_seed_indirect(ex, seed_total - seed_spent);
        seed_spent += (uint32_t)iseeds;
        if (iseeds) {
            dp_disc_add(&res, "indirect", 0);
            dp_explore(ex);
            dp_disc_add(&res, "indirect-search", 0);
            closure_rounds(ex, &res, &seed_spent, seed_total);
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
        int as_text = (ml > 4 && (!strcmp(map + ml - 4, ".map") || !strcmp(map + ml - 4, ".txt")));
        if (!as_text) {
            // the PNG is the default: 1 px per byte image + the .map, the
            // settings and the played usage embedded (zTXt).  A path with
            // no extension gets ".png".
            const char *base = strrchr(map, '/');
            base = base ? base + 1 : map;
            char *png = NULL;
            if (!strchr(base, '.') && (png = malloc(ml + 5))) snprintf(png, ml + 5, "%s.png", map);
            if (!dp_report_map_png(&res, &cfg, path, png ? png : map)) rc = 1;
            free(png);
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
    if (spool_on) {
        uint64_t pend = dp_explorer_spool_flush(ex);
        if (pend)
            fprintf(stderr, "deepeel: %llu states (%llu MiB) spooled for the next --resume\n",
                    (unsigned long long)(res.spool_writes - res.spool_reads),
                    (unsigned long long)(pend >> 20));
        dp_explorer_exact_save(ex, exact_path);
    }
    if (fdir) {
        if (!scfg.state_dir) mkdir(fdir, 0777);
        int fr = dp_explorer_frontier_save(ex, fdir);
        if (fr) fprintf(stderr, "deepeel: %d continuations/seeds saved in %s/frontier for --resume\n", fr, fdir);
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

// re-render the squares image from a saved .map text — no exploration: the
// map is trusted as written (explore --map or mapx produced it).  --meta
// embeds a file verbatim as deepeel_meta, otherwise a compact identity is
// synthesized (version, rom, source)
static int cmd_maprender(int argc, char **argv) {
    const char *meta_src = NULL, *rom = NULL, *inmap = NULL, *out = NULL;
    const char *sysname = "gb", *title_arg = NULL;
    int z80_map = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--meta") && i + 1 < argc) meta_src = argv[++i];
        else if (!strcmp(argv[i], "--sys") && i + 1 < argc) sysname = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title_arg = argv[++i];
        else if (!strcmp(argv[i], "--z80")) z80_map = 1; // physical-offset map (z80 platforms)
        else if (!rom) rom = argv[i];
        else if (!inmap) inmap = argv[i];
        else if (!out) out = argv[i];
        else return usage(), 2;
    }
    if (!rom || !inmap || !out) {
        fprintf(stderr, "usage: deepeel maprender rom in.map out.map.png [--meta f] [--sys gb] "
                        "[--title t] [--z80]\n");
        return 2;
    }
    FILE *f = fopen(inmap, "rb");
    if (!f) { perror(inmap); return 1; }
    fseek(f, 0, SEEK_END);
    long ln = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)ln + 1);
    if (!text || fread(text, 1, (size_t)ln, f) != (size_t)ln) { fclose(f); free(text); perror(inmap); return 1; }
    text[ln] = 0;
    fclose(f);
    size_t cv_len = 0;
    uint8_t *cv_in = (ln > 8 && !memcmp(text, "\x89PNG", 4)) ? (uint8_t *)dp_png_text(inmap, "dpCv", &cv_len) : NULL;
    if (ln > 8 && !memcmp(text, "\x89PNG", 4) && !cv_in) {
        fprintf(stderr, "deepeel: %s: PNG without a dpCv class vector\n", inmap);
        free(text);
        return 1;
    }
    if (z80_map && !cv_in) {
        // z80 maps list physical 16 KiB banks and whole-ROM offsets; rewrite
        // each run into the windowed "bank start end class" form the renderer
        // expects (bank 0 from 0, the others from 0x4000), splitting at the
        // 16 KiB boundaries
        DpBuf cv;
        dp_buf_init(&cv);
        char *p = text, *end = text + ln;
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) nl = end;
            unsigned b, a, e;
            char cls[32];
            if (sscanf(p, "%x %x %x %31s", &b, &a, &e, cls) == 4) {
                uint32_t lo = (uint32_t)b * 0x4000 + a, hi = (uint32_t)b * 0x4000 + e;
                for (uint32_t s = lo; s <= hi;) {
                    uint32_t bk = s / 0x4000, cut = (s | 0x3FFF) < hi ? (bk + 1) * 0x4000 - 1 : hi;
                    dp_buf_fmt(&cv, "%u %04x %04x %s\n", bk, s % 0x4000 + (bk ? 0x4000u : 0u),
                               cut % 0x4000 + (bk ? 0x4000u : 0u), cls);
                    s = cut + 1;
                }
            } else if (*p != '#') {
                dp_buf_fmt(&cv, "%.*s\n", (int)(nl - p), p);
            }
            p = nl < end ? nl + 1 : end;
        }
        free(text);
        text = (char *)cv.p;
        ln = (long)cv.len;
    }
    unsigned rom_size = 0;
    int rom_banks = 0;
    DpCart *cart = NULL;
    char title[17];
    int tl = 0;
    if (!strcmp(sysname, "gb")) {
        char err[256];
        cart = dp_cart_load(rom, err, sizeof err);
        if (!cart) { fprintf(stderr, "deepeel: %s\n", err); free(text); return 1; }
        rom_size = (unsigned)cart->rom_size;
        rom_banks = cart->rom_banks;
    } else { // z80 platforms: no GB header — size from the file, banks of 16 KiB
        FILE *rf = fopen(rom, "rb");
        if (!rf) { perror(rom); free(text); return 1; }
        fseek(rf, 0, SEEK_END);
        rom_size = (unsigned)ftell(rf);
        fclose(rf);
        rom_banks = (int)((rom_size + 0x3FFF) / 0x4000);
    }
    if (cv_in) { // a map PNG as input: its dpCv vector, expanded for the renderer
        DpBuf t2;
        dp_buf_init(&t2);
        if (!dp_map_cv_to_text(cv_in, cv_len, rom_banks, &t2)) {
            fprintf(stderr, "deepeel: %s: dpCv does not match the ROM (%d banks)\n", inmap, rom_banks);
            dp_buf_free(&t2);
            free(cv_in);
            free(text);
            dp_cart_free(cart);
            return 1;
        }
        free(cv_in);
        free(text);
        text = (char *)t2.p;
        ln = (long)t2.len;
    }
    if (title_arg) {
        snprintf(title, sizeof title, "%.16s", title_arg);
        tl = (int)strlen(title);
    } else
    for (const char *ti = cart ? cart->title : ""; *ti && tl < 16; ti++)
        if (*ti == '?') break;
        else if ((unsigned char)*ti >= 0x20 && (unsigned char)*ti < 0x7f) title[tl++] = *ti;
    if (!tl) {
        const char *base = strrchr(inmap, '/');
        base = base ? base + 1 : inmap;
        for (; *base && tl < 16; base++)
            if ((unsigned char)*base >= 0x20 && (unsigned char)*base < 0x7f) title[tl++] = *base;
    }
    title[tl] = 0;
    char meta_syn[512], *meta = NULL;
    if (meta_src) { // embed the given file verbatim (a summary.json, ...)
        FILE *mf = fopen(meta_src, "rb");
        if (!mf) { perror(meta_src); free(text); dp_cart_free(cart); return 1; }
        fseek(mf, 0, SEEK_END);
        long mn = ftell(mf);
        fseek(mf, 0, SEEK_SET);
        meta = malloc((size_t)mn + 1);
        if (!meta || fread(meta, 1, (size_t)mn, mf) != (size_t)mn) mn = 0;
        if (meta) meta[mn] = 0;
        fclose(mf);
        if (!meta) { free(text); dp_cart_free(cart); return 1; }
    } else {
        snprintf(meta_syn, sizeof meta_syn,
                 "{\"deepeel\":\"%s\",\"mode\":\"maprender\",\"source\":\"%s\","
                 "\"rom\":{\"title\":\"%s\",\"banks\":%d,\"size\":%u}}",
                 DEEPEEL_VERSION, inmap, title, rom_banks, rom_size);
        meta = meta_syn;
    }
    int ok = dp_map_png_from_maptext(text, (size_t)ln, rom_banks,
                                     title, meta, out, sysname,
                                     strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom,
                                     rom_size);
    if (!ok) { fprintf(stderr, "deepeel: %s: render failed\n", inmap); }
    if (meta != meta_syn) free(meta);
    free(text);
    dp_cart_free(cart);
    return ok ? 0 : 1;
}

// extracts one embedded chunk of a map PNG: deepeel_map (default), deepeel_meta,
// deepeel_heat (played usage per zone), deepeel_cov (played .cov),
// deepeel_rom (played ROM reads per 256-byte page)
static int cmd_mapx(int argc, char **argv) {
    const char *chunk = "deepeel_map";
    if (argc >= 6 && !strcmp(argv[4], "--chunk")) chunk = argv[5];
    if (argc < 4) {
        fprintf(stderr, "usage: deepeel mapx in.map.png out [--chunk deepeel_map|deepeel_meta|"
                        "deepeel_heat|deepeel_cov|deepeel_rom]\n");
        return 2;
    }
    size_t n = 0;
    char *t = dp_png_text(argv[2], chunk, &n);
    if (!t) { fprintf(stderr, "no embedded %s in %s\n", chunk, argv[2]); return 1; }
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

// NES branch: a self-contained 6502 explorer; options mirror the z80 names.
static uint8_t *nes_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 16) { fclose(f); return NULL; }
    uint8_t *data = malloc((size_t)n);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static int cmd_nes(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[2], "explore")) {
        fprintf(stderr, "deepeel nes explore ROM [--tas FILE]... [--play FRAMES] [--sessions N] [--seed N] [--mapper auto|nrom|uxrom] "
                        "[--max-total N] [--max-steps N] [--max-paths N] [--queue N] "
                        "[--loop-cap N] [--fork-cap N] [--no-irq-fork] [--entry bank:addr]... [--seed-banks] [--text FILE] [--map FILE] [--evidence FILE] [--cov FILE] [--romdata FILE]\n");
        return 2;
    }
    const char *path = NULL, *text = NULL, *map = NULL, *evidence = NULL, *cov = NULL, *romdata = NULL, *mapper_name = "auto";
    DpNesOptions opt = dp_nes_options();
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "--map") && i + 1 < argc) map = argv[++i];
        else if (!strcmp(a, "--evidence") && i + 1 < argc) evidence = argv[++i];
        else if (!strcmp(a, "--cov") && i + 1 < argc) cov = argv[++i];
        else if (!strcmp(a, "--romdata") && i + 1 < argc) romdata = argv[++i];
        else if (!strcmp(a, "--tas") && i + 1 < argc) {
            if (opt.tas_count < 8) snprintf(opt.tas_paths[opt.tas_count++], 512, "%s", argv[++i]);
            else { fprintf(stderr, "deepeel: at most 8 TAS movies\n"); return 2; }
        }
        else if (!strcmp(a, "--mapper") && i + 1 < argc) mapper_name = argv[++i];
        else if (!strcmp(a, "--max-total") && i + 1 < argc) opt.max_total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-steps") && i + 1 < argc) opt.max_steps = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-paths") && i + 1 < argc) opt.max_paths = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue") && i + 1 < argc) opt.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--lot") && i + 1 < argc) opt.lot_path = argv[++i];
        else if (!strcmp(a, "--lot-drain")) opt.lot_drain = 1;
        else if (!strcmp(a, "--tree") && i + 1 < argc) opt.tree_path = argv[++i];
        else if (!strcmp(a, "--spool") && i + 1 < argc) opt.lot_path = argv[++i];
        else if (!strcmp(a, "--resume") && i + 1 < argc) { opt.lot_path = argv[++i]; opt.lot_drain = 1; }
        else if (!strcmp(a, "--loop-cap") && i + 1 < argc) opt.loop_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--fork-cap") && i + 1 < argc) opt.fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--no-irq-fork")) opt.seed_irq = 0;
        else if (!strcmp(a, "--seed-banks")) opt.seed_banks = 1;
        else if (!strcmp(a, "--seed-vectors")) opt.seed_vectors = 1;
        else if (!strcmp(a, "--ctx-cap") && i + 1 < argc) opt.ctx_cap = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--second-pass")) opt.second_pass = 1;
        else if (!strcmp(a, "--bank-dispatch")) opt.second_pass = opt.bank_dispatch = 1;
        else if (!strcmp(a, "--no-p1-fork")) opt.no_pad_probe = 1;
        else if (!strcmp(a, "--entry") && i + 1 < argc && opt.entry_count < 32) {
            uint8_t eb; uint16_t ea;
            if (!parse_entry(argv[++i], &eb, &ea)) { fprintf(stderr, "deepeel: bad --entry '%s' (bank:addr)\n", argv[i]); return 2; }
            opt.entry_bank[opt.entry_count] = eb; opt.entry_addr[opt.entry_count++] = ea;
        }
        else if (!strcmp(a, "--play") && i + 1 < argc) opt.play_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--sessions") && i + 1 < argc) opt.play_sessions = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed") && i + 1 < argc) opt.play_seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!path) path = a;
        else { fprintf(stderr, "deepeel: unknown option %s\n", a); return 2; }
    }
    int mapper = -1; /* auto: the iNES header's mapper */
    if (!strcmp(mapper_name, "nrom")) mapper = 0;
    else if (!strcmp(mapper_name, "uxrom")) mapper = 2;
    else if (strcmp(mapper_name, "auto")) {
        fprintf(stderr, "deepeel: unknown mapper %s (the m0boy NES runtime boards NROM and UxROM)\n", mapper_name);
        return 2;
    }
    size_t size;
    uint8_t *data = nes_read_file(path, &size);
    if (!data) { fprintf(stderr, "deepeel: cannot read %s\n", path); return 1; }
    DpNesResults res;
    if (!dp_nes_explore(data, size, mapper, &opt, &res)) {
        fprintf(stderr, "deepeel: not an iNES image the explorer accepts (%s)\n", path);
        free(data);
        return 1;
    }
    fprintf(stderr, "deepeel: nes explore done — steps %llu, paths %llu, forks %llu, "
                    "sections %u, copies %u\n",
            (unsigned long long)res.steps, (unsigned long long)res.paths,
            (unsigned long long)res.forks, res.section_count, res.copy_count);
    if (text) {
        FILE *f = fopen(text, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", text); }
        else { dp_nes_report(f, &res); fclose(f); }
    }
    if (map) {
        FILE *f = fopen(map, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", map); }
        else { dp_nes_map(f, &res); fclose(f); }
    }
    if (evidence) {
        FILE *f = fopen(evidence, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", evidence); }
        else { dp_nes_evidence(f, &res); fclose(f); }
    }
    if (cov) {
        FILE *f = fopen(cov, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", cov); }
        else { dp_nes_cov(f, &res); fclose(f); }
    }
    if (romdata) {
        FILE *f = fopen(romdata, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", romdata); }
        else { dp_nes_romdata(f, &res); fclose(f); }
    }
    dp_nes_results_free(&res);
    free(data);
    return 0;
}

// PC Engine branch: the HuC6280 forking explorer over m0boy's machine.
static int cmd_pce(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[2], "explore")) {
        fprintf(stderr, "deepeel pce explore ROM [--tas FILE]... [--play FRAMES] [--sessions N] [--seed N] "
                        "[--max-total N] [--max-steps N] [--queue N] [--loop-cap N] [--fork-cap N] "
                        "[--novelty STEPS] [--novelty-frames N] [--novelty-archive N] [--novelty-cells] [--pad-fork-cap N] [--irq-fork-cap N] [--no-irq-fork] [--no-static] [--no-infer] [--jobs 2] "
                        "[--text FILE] [--map FILE] [--png FILE] [--evidence FILE] [--entry ADDR]... [--ctx-cap N]\n");
        return 2;
    }
    const char *path = NULL, *text = NULL, *map = NULL, *png = NULL, *pce_ev = NULL;
    DpPceOptions opt = dp_pce_options();
    int pce_jobs = 1;
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "--evidence") && i + 1 < argc) pce_ev = argv[++i];
        else if (!strcmp(a, "--ctx-cap") && i + 1 < argc) opt.ctx_cap = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--entry") && i + 1 < argc) {
            if (opt.entry_count >= 16) { fprintf(stderr, "deepeel: at most 16 --entry\n"); return 2; }
            opt.entries[opt.entry_count++] = (uint16_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(a, "--map") && i + 1 < argc) map = argv[++i];
        else if (!strcmp(a, "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(a, "--tas") && i + 1 < argc) {
            if (opt.tas_count < 8) snprintf(opt.tas_paths[opt.tas_count++], 512, "%s", argv[++i]);
            else { fprintf(stderr, "deepeel: at most 8 TAS movies\n"); return 2; }
        }
        else if (!strcmp(a, "--max-total") && i + 1 < argc) opt.max_total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-steps") && i + 1 < argc) opt.max_steps = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue") && i + 1 < argc) opt.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--lot") && i + 1 < argc) opt.lot_path = argv[++i];
        else if (!strcmp(a, "--lot-drain")) opt.lot_drain = 1;
        else if (!strcmp(a, "--loop-cap") && i + 1 < argc) opt.loop_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--fork-cap") && i + 1 < argc) opt.fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--pad-fork-cap") && i + 1 < argc) opt.pad_fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--irq-fork-cap") && i + 1 < argc) opt.irq_fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--no-irq-fork")) opt.seed_irq = 0;
        else if (!strcmp(a, "--novelty") && i + 1 < argc) opt.novelty_steps = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--novelty-frames") && i + 1 < argc) opt.novelty_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--novelty-cells")) opt.novelty_cells = 1;
        else if (!strcmp(a, "--novelty-archive") && i + 1 < argc) opt.novelty_archive = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--no-static")) opt.static_closure = 0;
        else if (!strcmp(a, "--no-infer")) opt.infer = 0;
        else if (!strcmp(a, "--jobs") && i + 1 < argc) pce_jobs = atoi(argv[++i]);
        else if (!strcmp(a, "--tree") && i + 1 < argc) opt.tree_path = argv[++i];
        else if (!strcmp(a, "--second-pass")) opt.second_pass = 1;
        else if (!strcmp(a, "--bank-dispatch")) opt.bank_dispatch = 1;
        else if (!strcmp(a, "--play") && i + 1 < argc) opt.play_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--sessions") && i + 1 < argc) opt.play_sessions = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed") && i + 1 < argc) opt.play_seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!path) path = a;
        else { fprintf(stderr, "deepeel: unknown option %s\n", a); return 2; }
    }
    if (opt.fork_cap > 255) opt.fork_cap = 255; // per-site counters are bytes
    if (opt.pad_fork_cap > 255) opt.pad_fork_cap = 255;
    if (opt.irq_fork_cap > 255) opt.irq_fork_cap = 255;
    size_t size;
    uint8_t *data = nes_read_file(path, &size);
    if (!data) { fprintf(stderr, "deepeel: cannot read %s\n", path); return 1; }
    DpPceResults *res = calloc(1, sizeof *res);
    if (!res || !dp_pce_explore_jobs(data, size, &opt, res, pce_jobs)) {
        fprintf(stderr, "deepeel: pce explore failed (%s)\n", path);
        free(res);
        free(data);
        return 1;
    }
    fprintf(stderr, "deepeel: pce explore done — steps %llu, paths %llu, forks %llu, sections %u, copies %u\n",
            (unsigned long long)res->steps, (unsigned long long)res->paths,
            (unsigned long long)res->forks, res->section_count, res->copy_count);
    if (text) {
        FILE *f = fopen(text, "w");
        if (!f) fprintf(stderr, "deepeel: cannot write %s\n", text);
        else { dp_pce_report(f, res); fclose(f); }
    } else {
        dp_pce_report(stdout, res);
    }
    char mapbuf[1024];
    if (png && !map) { snprintf(mapbuf, sizeof mapbuf, "%s.map", png); map = mapbuf; }
    if (map) {
        FILE *f = fopen(map, "w");
        if (!f) fprintf(stderr, "deepeel: cannot write %s\n", map);
        else { dp_pce_map(f, res); fclose(f); }
    }
    if (pce_ev) {
        FILE *f = fopen(pce_ev, "w");
        if (!f) fprintf(stderr, "deepeel: cannot write %s\n", pce_ev);
        else { dp_pce_evidence(f, res); fclose(f); }
    }
    dp_pce_results_free(res);
    free(res);
    free(data);
    if (png) {
        char *av[] = {"deepeel", "maprender", (char *)path, (char *)map, (char *)png,
                      "--z80", "--sys", "pce"};
        return cmd_maprender(8, av);
    }
    return 0;
}

static void md_regs_print(void);
static int cmd_regs(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[2], "md")) { md_regs_print(); return 0; }
    if(argc>=3 && !strcmp(argv[2],"msx")) { dp_msx_regs(stdout); return 0; }
    if (argc >= 3 && !strcmp(argv[2], "nes")) { dp_nes_regs(stdout); return 0; }
    if (argc >= 3 && !strcmp(argv[2], "pce")) { dp_pce_regs(stdout); return 0; }
    const DpSegaSystem *sega = argc >= 3 ? dp_sega_system(argv[2]) : NULL;
    if (sega) { dp_sega_regs(stdout, sega); return 0; }
    if (argc < 3 || strcmp(argv[2], "gb")) {
        fprintf(stderr, "deepeel regs <platform> — platforms: gb, sg, sms, gg, msx, nes, pce\n");
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
static void closure_rounds(DpExplorer *ex, DpResults *res, uint32_t *seed_spent,
                           uint32_t seed_total) {
    // --no-closure stops the flow closure and its seeds only: provenance and
    // copy siblings have their own switches
    for (int r = 0; !ex->cfg.no_closure && r < ex->cfg.closure_rounds; r++) {
        uint32_t marked = dp_static_closure(res);
        uint32_t ref0 = res->closure_seeds_refused;
        int seeded = dp_closure_seed(ex, seed_total - *seed_spent); // the cascade solde
        *seed_spent += (uint32_t)seeded;
        uint32_t refused = res->closure_seeds_refused - ref0;
        fprintf(stderr, "deepeel: closure round %d: +%u static bytes, %d entries seeded, %u refused\n",
                r + 1, marked, seeded, refused);
        if (!seeded && !refused) break; // provenance + final closure still run
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
    if (!ex->cfg.no_provenance) {
        dp_provenance(res);
        fprintf(stderr, "deepeel: provenance: %u jp-hl targets, %u data bytes proven (%u partial, %u caps)\n",
                res->prov_jp_hl, res->prov_data, res->prov_unresolved, res->prov_sat);
    }
    if (!ex->cfg.no_siblings) {
        uint32_t sib = dp_copy_siblings(res);
        fprintf(stderr, "deepeel: copy siblings: %u tables, %u ranges, %u bytes inferred (%u refused, %u caps)\n",
                res->sib_tables, res->sib_ranges, sib, res->sib_refused, res->sib_caps);
    }
    // the last search may have opened more direct flow
    if (!ex->cfg.no_closure) dp_static_closure(res);
}

static void md_regs_print(void) {
    printf("megadrive register registry (%d entries):\n", dp_md_reg_count);
    for (int i = 0; i < dp_md_reg_count; i++)
        printf("  %06x-%06x  %-12s %s\n", dp_md_regs[i].lo, dp_md_regs[i].hi, dp_md_regs[i].name,
               dp_md_regs[i].category);
}

static int cmd_md(int argc, char **argv) {
    if (argc < 4) return usage(), 2;
    char err[256];
    DpMdCart c;
    if (dp_md_cart_load(&c, argv[3], err, sizeof err)) {
        fprintf(stderr, "deepeel: %s\n", err);
        return 1;
    }
    int rc = 0;
    if (!strcmp(argv[2], "dis")) {
        if (argc < 5) { dp_md_cart_free(&c); return usage(), 2; }
        uint32_t pc = (uint32_t)strtoul(argv[4], NULL, 16);
        int n = argc > 5 ? atoi(argv[5]) : 16;
        for (int i = 0; i < n && pc < c.size; i++) {
            DpM68kInsn in;
            unsigned len = dp_m68k_decode(c.rom + pc, c.size - pc, pc, &in);
            printf("%06X  %s\n", pc, in.text);
            pc += len ? len : 2;
        }
    } else if (!strcmp(argv[2], "explore")) {
        DpMdOptions o;
        dp_md_options_default(&o);
        const char *text = NULL, *map = NULL, *ev = NULL, *movie_path = NULL;
        for (int i = 4; i < argc; i++) {
            const char *a = argv[i];
            if (i + 1 >= argc && strcmp(a, "--lot-drain")) { fprintf(stderr, "deepeel: bad option %s\n", a); rc = 2; break; }
            if (!strcmp(a, "--max-total")) o.max_total = strtoull(argv[++i], NULL, 0);
            else if (!strcmp(a, "--path-steps")) o.path_steps = strtoull(argv[++i], NULL, 0);
            else if (!strcmp(a, "--queue")) o.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--lot")) o.lot_path = argv[++i];
            else if (!strcmp(a, "--lot-drain")) o.lot_drain = 1;
            else if (!strcmp(a, "--cut-padding")) o.pad_cut = 1;
            else if (!strcmp(a, "--second-pass")) o.second_pass = 1;
            else if (!strcmp(a, "--ctx-cap") && i + 1 < argc) o.ctx_cap = (unsigned)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--tree") && i + 1 < argc) o.tree_path = argv[++i];
            else if (!strcmp(a, "--entry")) {
                if (o.entry_count >= 16) { fprintf(stderr, "deepeel: at most 16 --entry\n"); rc = 2; break; }
                o.entries[o.entry_count++] = (uint32_t)strtoul(argv[++i], NULL, 0);
            }
            else if (!strcmp(a, "--queue-max")) o.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--fork-cap")) o.fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--visit-cap")) o.visit_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--watch-write")) o.watch_write = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--exclude-read-pc")) o.exclude_read_pc = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--tas")) movie_path = argv[++i];
            else if (!strcmp(a, "--text")) text = argv[++i];
            else if (!strcmp(a, "--map")) map = argv[++i];
            else if (!strcmp(a, "--evidence")) ev = argv[++i];
            else { fprintf(stderr, "deepeel: unknown option %s\n", a); rc = 2; break; }
        }
        DpMdMovie movie = {0};
        if (!rc && movie_path) {
            if (!dp_md_movie_load(movie_path, &movie, err, sizeof err)) {
                fprintf(stderr, "deepeel: GMV: %s\n", err); rc = 1;
            } else o.movie = &movie;
        }
        if (!rc) {
            DpMdResults r;
            if (dp_md_explore(&c, &o, &r)) {
                fprintf(stderr, "deepeel: md explore failed\n");
                rc = 1;
            } else {
                FILE *f = text ? fopen(text, "w") : stdout;
                if (f) {
                    dp_md_explore_report(&c, &o, &r, f);
                    if (text) fclose(f);
                }
                if (map) dp_md_explore_map(&r, map);
                if (ev) dp_md_explore_evidence(&r, ev);
                dp_md_results_free(&r);
            }
        }
        dp_md_movie_free(&movie);
    } else rc = (usage(), 2);
    dp_md_cart_free(&c);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage(), 2;
    if (!strcmp(argv[1], "frontier") && argc > 3 && !strcmp(argv[2], "stats")) {
        // pending records by reason, tier and bank (the unread part of each lot)
        char p[700], line[400];
        struct stat lst;
        if (!stat(argv[3], &lst) && S_ISREG(lst.st_mode)) {
            // a portable lot (--lot of NES/PCE/MD/Sega): records and sizes
            FILE *lf = fopen(argv[3], "rb");
            uint32_t h[4]; char pl[5] = {0};
            if (!lf || fread(h, sizeof h, 1, lf) != 1 || h[0] != DP_LF_MAGIC || fread(pl, 1, 4, lf) != 4) {
                printf("frontier: %s is not a lot\n", argv[3]); if (lf) fclose(lf); return 1;
            }
            uint64_t raw = 0, zb = 0, n = 0, off = sizeof h + 4 + h[3];
            uint32_t rh[4];
            while (fseek(lf, (long)off, SEEK_SET) == 0 && fread(rh, sizeof rh, 1, lf) == 1 && rh[0] == DP_LF_REC) {
                raw += rh[2]; zb += rh[3]; off += sizeof rh + rh[1] + rh[3]; n++;
            }
            printf("frontier lot (%s, image %u bytes): %llu records pending (%llu bytes raw, %llu compressed XOR deltas)\n",
                   pl, h[2], (unsigned long long)n, (unsigned long long)raw, (unsigned long long)zb);
            fclose(lf);
            return 0;
        }
        snprintf(p, sizeof p, "%s/frontier/frontier.idx", argv[3]);
        FILE *f = fopen(p, "r");
        if (!f) { printf("frontier: empty (no %s)\n", p); return 0; }
        uint64_t by_reason[16][4] = {{0}}, by_bank[1024] = {0}, total = 0;
        char names[16][24] = {{0}};
        int lots = 0;
        while (fgets(line, sizeof line, f)) {
            char name[96], rs[32];
            unsigned long long off;
            unsigned cnt, t1, t2, t3, rd;
            int bank, sec;
            if (sscanf(line, "lot %95s count=%u off=%llu reason=%31s bank=%d sec=%d t1=%u t2=%u t3=%u read=%u", name,
                       &cnt, &off, rs, &bank, &sec, &t1, &t2, &t3, &rd) != 10) continue;
            lots++;
            uint64_t left = cnt - rd, tn = (uint64_t)t1 + t2 + t3 ? (uint64_t)t1 + t2 + t3 : 1;
            int ri = 0;
            while (ri < 15 && names[ri][0] && strcmp(names[ri], rs)) ri++;
            snprintf(names[ri], sizeof names[ri], "%s", rs);
            // the per-tier split is of the whole lot: scaled to what is still unread
            by_reason[ri][0] += left;
            by_reason[ri][1] += left * t1 / tn;
            by_reason[ri][2] += left * t2 / tn;
            by_reason[ri][3] += left * t3 / tn;
            if (bank >= 0 && bank < 1024) by_bank[bank] += left;
            total += left;
        }
        fclose(f);
        printf("frontier: %d lots, %llu records pending\n", lots, (unsigned long long)total);
        for (int i = 0; i < 16 && names[i][0]; i++)
            printf("  %-11s %8llu   (tier1 %llu, tier2 %llu, tier3 %llu)\n", names[i], (unsigned long long)by_reason[i][0],
                   (unsigned long long)by_reason[i][1], (unsigned long long)by_reason[i][2],
                   (unsigned long long)by_reason[i][3]);
        for (int b = 0; b < 1024; b++)
            if (by_bank[b]) printf("  bank %-4d   %8llu\n", b, (unsigned long long)by_bank[b]);
        snprintf(p, sizeof p, "%s/frontier/seeds.bin", argv[3]);
        struct stat st;
        if (!stat(p, &st)) printf("  held seeds  %8llu\n", (unsigned long long)(st.st_size / 6));
        return 0;
    }
    if (!strcmp(argv[1], "frontier") && argc > 4 && !strcmp(argv[2], "drain")) {
        struct stat dst;
        if (!stat(argv[4], &dst) && S_ISREG(dst.st_mode)) {
            // a portable lot: resume its saved states only on the platform that wrote it
            FILE *lf = fopen(argv[4], "rb");
            uint32_t h[4]; char pl[5] = {0};
            if (!lf || fread(h, sizeof h, 1, lf) != 1 || h[0] != DP_LF_MAGIC || fread(pl, 1, 4, lf) != 4) {
                fprintf(stderr, "deepeel: %s is not a lot\n", argv[4]); if (lf) fclose(lf); return 1;
            }
            fclose(lf);
            const char *sys = NULL;
            for (int i = 5; i + 1 < argc; i++) if (!strcmp(argv[i], "--sys")) sys = argv[i + 1];
            char *av[16]; int n = 0;
            av[n++] = argv[0];
            if (!strcmp(pl, "NES")) { av[n++] = (char *)"nes"; av[n++] = (char *)"explore"; av[n++] = argv[3]; }
            else if (!strcmp(pl, "PCE")) { av[n++] = (char *)"pce"; av[n++] = (char *)"explore"; av[n++] = argv[3]; }
            else if (!strcmp(pl, "MD")) { av[n++] = (char *)"md"; av[n++] = (char *)"explore"; av[n++] = argv[3]; }
            else if (!strcmp(pl, "SEG")) {
                if (!sys) { fprintf(stderr, "deepeel: a Sega/MSX lot needs --sys sms|gg|sg1000|msx\n"); return 2; }
                av[n++] = (char *)"z80"; av[n++] = (char *)"explore"; av[n++] = (char *)sys; av[n++] = argv[3];
            } else { fprintf(stderr, "deepeel: unknown lot platform %s\n", pl); return 2; }
            av[n++] = (char *)"--lot"; av[n++] = argv[4]; av[n++] = (char *)"--lot-drain";
            fprintf(stderr, "deepeel: draining the %s lot %s\n", pl, argv[4]);
            argc = n; argv = av;
            if (!strcmp(argv[1], "md")) return cmd_md(argc, argv);
            if (!strcmp(argv[1], "nes")) return cmd_nes(argc, argv);
            if (!strcmp(argv[1], "pce")) return cmd_pce(argc, argv);
            return dp_sega_explore_command(argc, argv);
        }
        // frontier drain <rom> <state-dir> [--tier N] [--time-limit S]: resume the
        // saved continuations only (an explore --resume run on the same state dir)
        char **av = calloc((size_t)argc + 4, sizeof *av);
        int n = 0;
        av[n++] = argv[0]; av[n++] = (char *)"explore"; av[n++] = argv[3];
        av[n++] = (char *)"--state-dir"; av[n++] = argv[4]; av[n++] = (char *)"--resume";
        for (int i = 5; i < argc; i++) av[n++] = argv[i];
        g_drain_only = 1;
        return cmd_explore(n, av);
    }
    if (!strcmp(argv[1], "lr")) return dp_lr_command(argc, argv);
    if (!strcmp(argv[1], "md")) return cmd_md(argc, argv);
    if (!strcmp(argv[1], "nes")) return cmd_nes(argc, argv);
    if (!strcmp(argv[1], "pce")) return cmd_pce(argc, argv);
    if (!strcmp(argv[1], "z80") && argc > 2 && !strcmp(argv[2], "explore"))
        return dp_sega_explore_command(argc, argv);
    if (!strcmp(argv[1], "z80")) return dp_z80_command(argc, argv);
    if (!strcmp(argv[1], "explore")) return cmd_explore(argc, argv);
    if (!strcmp(argv[1], "mapx")) return cmd_mapx(argc, argv);
    if (!strcmp(argv[1], "maprender")) return cmd_maprender(argc, argv);
    if (!strcmp(argv[1], "map")) return cmd_map(argc, argv);
    if (!strcmp(argv[1], "regs")) return cmd_regs(argc, argv);
    if (!strcmp(argv[1], "dis")) return cmd_dis(argc, argv);
    return usage(), 2;
}
