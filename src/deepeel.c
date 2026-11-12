// deepeel.c: command line front-end.
//
//   deepeel explore rom.gb [options] [--json f] [--text f] [--cov f] [--gui]
//   deepeel explore rom.gb --play [...]   play sessions (m0boy-style coverage)
//   deepeel map rom.gb [--json f] [--text f]        static map only
//   deepeel regs gb                                 dump the GB register registry
//   deepeel dis rom.gb bank:addr [n]                disassemble n instructions
#include <signal.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "explore/md_ref_feed.h"
#include "core/md5.h"
#include "core/cart.h"
#include "core/cart_md.h"
#include "explore/frontier.h"
#include "explore/md_trace.h"
#include "core/machine.h"
#include "core/sta.h"
#include "mach/m0boy.h"
#include "core/snapshot_png.h"
#include "explore/explorer.h"
#include "explore/coverage_defaults.h"
#include "explore/lots.h"
#include "explore/sessions.h"
#include "explore/tree.h"
#include "report/report.h"
#include "gui/gui.h"
#include "gui/fbwatch.h"
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
#include "platform/nes/tas.h"
#include "platform/pce/tas.h"
#include "explore/tas.h"

#include "explore/core.h"
#include "core/bizhawk.h"
#include "core/desync.h"
static const char *argv_cov(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--cov")) return argv[i + 1];
    return NULL;
}

static const char *argv_opt(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}
// kernel (--core) outputs shared by every platform: --cov, --map (PNG or text), --summary, --text
static void core_outputs(int argc, char **argv, const char *id, const char *rom, const DpCoreResult *r) {
    dp_core_write_cov(argv_cov(argc, argv), id, r);
    if (getenv("DEEPEEL_MAPWIN_DUMP")) {   // kernel --mapwin: the final ROM map (1 px per byte, 256 wide) as a PPM; the live window is the legacy engine's
        FILE *mf = fopen(getenv("DEEPEEL_MAPWIN_DUMP"), "wb");
        if (mf) {
            size_t rows = (r->rom_size + 255) / 256;
            fprintf(mf, "P6\n256\n%zu\n255\n", rows);
            for (size_t i = 0; i < rows * 256; i++) {
                uint8_t f = i < r->rom_size ? r->flags[i] : 0, c[3] = { 0x20, 0x20, 0x28 };
                if (f & DP_CORE_NATURAL) { c[0] = 0x30; c[1] = 0xc0; c[2] = 0x50; }
                else if (f & DP_CORE_FORCED) { c[0] = 0xe0; c[1] = 0x90; c[2] = 0x30; }
                else if (f & DP_CORE_STATIC) { c[0] = 0x40; c[1] = 0x70; c[2] = 0xe0; }
                else if (f & DP_CORE_DATA) { c[0] = 0xd0; c[1] = 0xd0; c[2] = 0x40; }
                fwrite(c, 1, 3, mf);
            }
            fclose(mf);
        }
    }
    if (dp_core_write_map(argv_opt(argc, argv, "--map"), id, rom, r) < 0) fprintf(stderr, "deepeel: cannot write --map\n");
    if (dp_core_write_summary(argv_opt(argc, argv, "--summary"), id, r) < 0) fprintf(stderr, "deepeel: cannot write --summary\n");
    const char *t = argv_opt(argc, argv, "--text");
    if (t) { FILE *f = fopen(t, "w"); if (f) { dp_core_report(f, id, r); fclose(f); } }
}

static uint8_t *nes_read_file(const char *path, size_t *size);
static DpExplorer *volatile g_sig_ex;
static volatile sig_atomic_t g_alarm_fired;   // the wall-clock alarm (not a user SIGINT) stopped the run
static unsigned g_search_limit_secs;          // --search-limit: fresh wall budget for the post-play fork search
// SIGUSR1: one line per RAM-heavy structure (diagnostic, not async-signal-safe on purpose: the run is long-lived
// and this is read by a human). Sizes are the live allocations the explorer holds, in MB.
static DpTree *g_sig_tree;
static void on_explore_usr1(int sig) {
    (void)sig;
    DpExplorer *ex = g_sig_ex;
    if (!ex) return;
    struct mallinfo2 mi = mallinfo2();
    double mb = 1048576.0, img = (double)ex->sb;
    fprintf(stderr, "mem: malloc in-use %.0f MB, arena %.0f MB, mmap %.0f MB\n", mi.uordblks / mb, mi.arena / mb, mi.hblkhd / mb);
    fprintf(stderr, "mem: queue %u/%u, priority %u/%u, state image %u B\n", ex->q_len, ex->q_cap, ex->pq_len, ex->pq_cap, ex->sb);
    if (ex->sh) fprintf(stderr, "mem: shared pool %u/%u (max %u) = up to %.0f MB of images\n", ex->sh->len, ex->sh->cap, ex->sh->max, ex->sh->len * img / mb);
    fprintf(stderr, "mem: deferred %u/%u, exact set %zu keys (%.0f MB)\n", ex->defer_len, ex->defer_cap,
            ex->exact.cnt, ex->exact.cap * 9 / mb);
    if (g_sig_tree) {
        uint32_t res_ch = 0;
        for (uint32_t c = 0; c < g_sig_tree->nch; c++) res_ch += g_sig_tree->ch[c] != NULL;
        fprintf(stderr, "mem: tree %u choices, %u chunks (%u resident = %.0f MB), pin %u open\n", g_sig_tree->n, g_sig_tree->nch,
                res_ch, res_ch * 65536.0 * 20 / mb, g_sig_tree->pin_n);
    }
}

static void on_explore_sigint(int sig) {
    if (sig == SIGALRM) g_alarm_fired = 1;
    dp_explore_interrupt = 1;
    if (g_sig_ex) g_sig_ex->stop = 1;
}

// --jobs auto: one explorer per core left free (all but two), bounded by available RAM at ~1.5 GiB
// per explorer. Decided once at launch (not re-tuned while running); stderr says what was chosen.
static int auto_jobs(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    long n = cpus > 2 ? cpus - 2 : 1;
    FILE *f = fopen("/proc/meminfo", "r");
    char ln[128];
    while (f && fgets(ln, sizeof ln, f)) {
        unsigned long kb;
        if (sscanf(ln, "MemAvailable: %lu kB", &kb) == 1) {
            long byram = (long)(kb / (1536ul * 1024ul));
            if (byram < n) n = byram;
            break;
        }
    }
    if (f) fclose(f);
    if (n < 1) n = 1;
    fprintf(stderr, "deepeel: --jobs auto -> %ld explorers (%ld cores online)\n", n, cpus);
    return (int)n;
}

int dp_lr_command(int argc, char **argv);
int dp_bh_command(int argc, char **argv);
static int cmd_nes(int argc, char **argv);
static int cmd_pce(int argc, char **argv);
static int cmd_maprender(int argc, char **argv);

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include "cli_common.h"
#include "core/dpmachine.h"

static void usage(void) {
    fprintf(stderr,
            "deepeel — ROM memory-map explorer (game boy first)\n\n"
            "  deepeel explore rom.gb [options]\n"
            "  deepeel z80 explore <sg|sms|gg|msx> ROM [--input-total N] [--input-forks N] [--text FILE] [--map FILE] [--evidence FILE]\n"
            "  deepeel z80 dis ROM hex-offset [instruction-count]\n"
            "  deepeel lr explore CORE ROM [--text FILE] [--map FILE] [--path-steps N] [--max-total N] [--queue N]\n"
            "  deepeel md explore ROM [--tas GMV] [--tas-dir DIR] [--max-total N] [--path-steps N] [--queue N] [--queue-max N] [--fork-cap N] [--visit-cap N] [--entry ADDR]... [--watch-write ADDR] [--exclude-read-pc ADDR] [--ctx-cap N] [--ctx-live] [--text FILE] [--map FILE] [--evidence FILE]\n"
            "  deepeel md dis ROM hex-offset [instruction-count]\n"
            "  deepeel regs <gb|sg|sms|gg|msx|md>\n"
            "    --max-steps N        per-path step budget (default 100000)\n"
            "    --max-paths N        path cap (default 200000)\n"
            "    --max-total N        global step wall (default 20000000)\n"
            "    --queue N            pending-state cap (default 4096)\n"
            "    --queue-mem MB       RAM budget of the pending state images (default 1536); the rest goes to the disk spool\n"
            "    --queue-max N        adaptive growth ceiling of the pending states\n"
            "                         (default 4x queue): a full queue doubles instead of\n"
            "                         dropping paths, stopped by --mem-cap when set\n"
            "    --mem-cap MB         wall shared by the play and the queue growth:\n"
            "                         stop/grow no more once RSS exceeds MB MiB\n"
            "    --jobs N|auto        parallel drain: N explorers (own machine each) share\n"
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
            "    --fork-site-cap N    per-branch-site fork cap (default 4, bytes counters)\n"
            "    --gui                SDL2 live viewer\n"
            "    --mapwin [SCALE]     live ROM map window (1 px per byte like the .map.png), redrawn while exploring\n"
            "    --watch [FPS]        small window showing the replaying TAS movie, 1..30 img/s (default 30)\n"
            "    --ram-code f         executed WRAM/HRAM code as byte variants (m0boy: gbrecomp --ram-code f)\n"
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
            "    --attract-frames N   minimum no-input attract run (default 6000 frames)\n"
            "    --frames F           total frame budget (default unlimited)\n"
            "    --seed N             input RNG base seed\n"
            "    --stagnation F       stop after F frames with no discovery (default never)\n"
            "    --mem-cap MB         stop (gracefully, resumable) once RSS exceeds MB MiB\n"
            "    --until-static       also stop when every byte is explained\n"
            "    --search-limit S     after the play budget (--time-limit) ran out, give the fork search\n"
            "                         (frontier drain) S more seconds instead of skipping it\n"
            "    --checkpoints N      checkpoint pool size (default 48)\n"
            "    --stall E            empty epochs before re-seeding from the best (default 3)\n"
            "    --yield-window N     epochs a method's yield is summed over (0 = off, default 0)\n"
            "    --yield-min B        bytes below which a method reallocates, and the whole\n"
            "                         play yields (stop reason \"yield\") when every method does\n"
            "    --state-dir D        persist the pool (pool.bin) and the PNG snapshots\n"
            "    --resume             continue from a saved pool\n"
            "    --input f            replay an m0boy held-named input script (repeatable)\n"
            "    --tas-ckpt N         TAS replay: capture-interval override in frames (default:\n"
            "                         auto, from --tas-ckpt-secs and --tas-ckpt-max)\n"
            "    --tas-ckpt-secs S    TAS replay: a checkpoint snapshot every S seconds of game\n"
            "                         time (default 10, 0 = off)\n"
            "    --tas-ckpt-max N     TAS replay: snapshot cap per movie (default 4096); the\n"
            "                         interval is min(movie/S, S secs), so a long movie stays at\n"
            "                         one snapshot per S seconds\n"
            "    --session-cells      Go-Explore: sessions archive every new background screen as a checkpoint\n"
            "    --no-tas-rng         disable the default: half of the random-key segments restart\n"
            "                         from the TAS checkpoints (long segments, each checkpoint many\n"
            "                         times) to explore the neighbourhood of the near-perfect run\n"
            "    --tas-rng-share N    one random-key segment start in N uses a TAS checkpoint (default 1)\n"
            "    --tas-rng-per N      random bursts started from each TAS checkpoint (default 10)\n"
            "    --tas-rng-frames N   a burst (random keys, no START) lasts at least N frames, until N frames pass without a discovery (default 2000)\n"
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
    static const char *exts[] = { ".vmv", ".vbm", ".bk2", ".gbm", ".fm2", ".log", ".gmv" };
    size_t n = strlen(name);
    for (int i = 0; i < 7; i++) {
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
    char (*names)[256] = calloc(2048, 256);
    if (!names) {
        closedir(d);
        return;
    }
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < 2048) {
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
// the runtime-code archive: one line per distinct instruction run from RAM, input of the recompiler
static void write_rtcode(const DpResults *res, const char *path) {
    if (!path) return;
    uint32_t rn;
    DpRtCode *rt = dp_rtcode_sorted(res, &rn);
    FILE *rf = fopen(path, "w");
    if (rf) {
        fprintf(rf, "# deepeel rtcode v1: pc bytes hits rom_index(-1=generated) producer(-1=none)\n");
        for (uint32_t i = 0; i < rn; i++) {
            fprintf(rf, "%04x ", rt[i].pc);
            for (int k = 0; k < rt[i].len; k++) fprintf(rf, "%02x", rt[i].b[k]);
            fprintf(rf, " %u %d %d\n", rt[i].hits, rt[i].rom_src, rt[i].derived);
        }
        fclose(rf);
    }
    free(rt);
}

// the executed WRAM/HRAM code as byte variants, input of m0boy's `gbrecomp --ram-code`
static int g_ramcode_forced; // --ram-code-forced: also export code only forced forks reached
static void write_ramcode(const DpResults *res, const char *path) {
    if (!path) return;
    uint32_t nv, ni, ns, nd, nf;
    if (dp_rtcode_export_variants(res, path, g_ramcode_forced, &nv, &ni, &ns, &nd, &nf))
        fprintf(stderr, "deepeel: cannot write %s\n", path);
    else
        printf("ram code: %u variants, %u instructions exported to %s (%u forced-only left out, %u outside WRAM/HRAM, "
               "%u dropped)\n", nv, ni, path, nf, ns, nd);
}

static int cmd_explore(int argc, char **argv) {
    int nocredit_set = 0;
    const char *rtcode_path = NULL, *ramcode_path = NULL;
    const char *json = NULL, *text = NULL, *cov = NULL, *romdata = NULL, *map = NULL, *html = NULL, *path = NULL;
    const char *from_png = NULL, *export_dir = NULL, *summary = NULL;
    const char *tas_dirs[64];
    int tas_dirs_n = 0;
    const char *stop_txt = "explore-only";
    unsigned long long play_frames = 0;
    int mapwin = 0, mapwin_on = 0;
    int watch = 0, gui = 0, play = 0, human = 0, human_slot = -1, export_slot = -1, png_slot = -1;
    DpConfig cfg;
    dp_config_defaults(&cfg);
    DpSessionCfg scfg;
    dp_session_cfg_defaults(&scfg);
    uint8_t tier_limit = 0, defer_all = 0;
    uint8_t eb[32];
    int gb_core = 0; DpCoreConfig gkcfg; dp_core_config_default(&gkcfg); gkcfg.visits_per_pc = 2000000;   // frame waits are long loops   // --core: the generic kernel instead of the GB explorer
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--core")) gb_core = 1;
    int spool_to_frontier = 0;
    uint16_t ea[32];
    int entries = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--core")) { /* kernel engine, see gb_core below */ }
        else if (gb_core && dp_core_parse_option(&gkcfg, argc, argv, i)) { i += dp_core_parse_option(&gkcfg, argc, argv, i) - 1; }   // kernel options first: --attract-frames etc. mean the kernel's here
        else if (!strcmp(a, "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(a, "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "--rtcode") && i + 1 < argc) rtcode_path = argv[++i];
        else if (!strcmp(a, "--ram-code") && i + 1 < argc) ramcode_path = argv[++i];
        else if (!strcmp(a, "--ram-code-forced")) g_ramcode_forced = 1;
        else if (!strcmp(a, "--summary") && i + 1 < argc) summary = argv[++i];
        else if (!strcmp(a, "--cov") && i + 1 < argc) cov = argv[++i];
        else if (!strcmp(a, "--romdata") && i + 1 < argc) romdata = argv[++i];
        else if (!strcmp(a, "--map") && i + 1 < argc) map = argv[++i];
        else if (!strcmp(a, "--html") && i + 1 < argc) html = argv[++i];
        else if (!strcmp(a, "--gui")) gui = 1;
        else if (!strcmp(a, "--mapwin")) {
            mapwin = 0;
            if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '8') mapwin = atoi(argv[++i]);
            mapwin_on = 1;
        }
        else if (!strcmp(a, "--watch")) {
            watch = 30;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') watch = atoi(argv[++i]);
            if (watch < 1) watch = 1;
            if (watch > 30) watch = 30;
        }
        else if (!strcmp(a, "--max-steps") && i + 1 < argc) cfg.max_steps_path = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--nocredit-cut") && i + 1 < argc) { cfg.nocredit_cut = (uint32_t)strtoul(argv[++i], NULL, 0); nocredit_set = 1; }
        else if (!strcmp(a, "--max-paths") && i + 1 < argc) cfg.max_paths = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-total") && i + 1 < argc) cfg.max_steps_total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue") && i + 1 < argc) cfg.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue-max") && i + 1 < argc) cfg.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue-mem") && i + 1 < argc) cfg.queue_mem_mb = (uint32_t)strtoul(argv[++i], NULL, 0);
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
        else if (!strcmp(a, "--search-limit") && i + 1 < argc) g_search_limit_secs = (unsigned)strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--frontier-cap-gb") && i + 1 < argc) cfg.frontier_cap_mb = strtoull(argv[++i], NULL, 0) * 1024;
        else if (!strcmp(a, "--spool-cap-mb") && i + 1 < argc) cfg.spool_cap_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--jobs") && i + 1 < argc) { ++i; cfg.jobs = !strcmp(argv[i], "auto") ? auto_jobs() : atoi(argv[i]); }
        else if (!strcmp(a, "--seed-cap") && i + 1 < argc) cfg.seed_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-banks")) cfg.seed_banks = 1;
        else if (!strcmp(a, "--no-pointer-scan")) cfg.no_pointer_scan = 1;
        else if (!strcmp(a, "--no-context-evict")) cfg.no_context_evict = 1;
        else if (!strcmp(a, "--no-irq-fork")) cfg.no_irq_fork = 1;
        else if (!strcmp(a, "--no-key-fork")) cfg.no_key_fork = 1;
        else if (!strcmp(a, "--no-p1-fork")) cfg.no_p1_fork = 1;
        else if (!strcmp(a, "--no-event-fork")) cfg.no_event_fork = 1;
        else if (!strcmp(a, "--no-closure")) cfg.no_closure = 1;
        else if (!strcmp(a, "--no-provenance")) cfg.no_provenance = 1;
        else if (!strcmp(a, "--no-siblings")) cfg.no_siblings = 1;
        else if (!strcmp(a, "--closure-rounds") && i + 1 < argc) cfg.closure_rounds = atoi(argv[++i]);
        else if (!strcmp(a, "--no-covered-prune")) cfg.no_covered_prune = 1;
        else if (!strcmp(a, "--covered-keep") && i + 1 < argc) cfg.covered_fork_keep = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--fork-site-cap") && i + 1 < argc) {
            cfg.fork_site_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
            if (cfg.fork_site_cap > 255) cfg.fork_site_cap = 255; // per-site counters are bytes
        }
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
        else if (!strcmp(a, "--attract-frames") && i + 1 < argc) scfg.attract_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--frames") && i + 1 < argc) scfg.max_frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed") && i + 1 < argc) scfg.seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--stagnation") && i + 1 < argc) scfg.stagnation_frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--mem-cap") && i + 1 < argc) scfg.mem_cap_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--until-static")) scfg.until_static = 1;
        else if (!strcmp(a, "--checkpoints") && i + 1 < argc) scfg.pool_cap = atoi(argv[++i]);
        else if (!strcmp(a, "--stall") && i + 1 < argc) scfg.stall_epochs = atoi(argv[++i]);
        else if (!strcmp(a, "--yield-window") && i + 1 < argc) { scfg.yield_window = atoi(argv[++i]); scfg.attract_stall = scfg.yield_window; }
        else if (!strcmp(a, "--yield-min") && i + 1 < argc) scfg.yield_min = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--state-dir") && i + 1 < argc) scfg.state_dir = argv[++i];
        else if (!strcmp(a, "--resume")) scfg.resume = 1;
        else if (!strcmp(a, "--spool-to-frontier")) spool_to_frontier = 1;
        else if (!strcmp(a, "--unbounded")) {
            // coverage first: no per-site fork cap (covered-both-sides pruning stays: it adds no code), no spool or
            // queue ceiling, no memory stop; whatever RAM cannot hold goes to the disk
            cfg.unbounded = scfg.unbounded = 1; cfg.fork_site_cap = 255u;
            cfg.spool_cap_mb = 0; cfg.spool_cap_bytes = 0;
        }
        else if (!strcmp(a, "--input") && i + 1 < argc && scfg.inputs_n < 8) scfg.inputs[scfg.inputs_n++] = argv[++i];
        else if (!strcmp(a, "--tas") && i + 1 < argc && scfg.tas_n < (int)(sizeof scfg.tas / sizeof *scfg.tas)) scfg.tas[scfg.tas_n++] = argv[++i];
        else if (!strcmp(a, "--tas-dir") && i + 1 < argc && tas_dirs_n < 64) tas_dirs[tas_dirs_n++] = argv[++i];
        else if (!strcmp(a, "--tas-ckpt") && i + 1 < argc) scfg.tas_ckpt_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-ckpt-secs") && i + 1 < argc) scfg.tas_ckpt_secs = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-ckpt-max") && i + 1 < argc) scfg.tas_ckpt_max = atoi(argv[++i]);
        else if (!strcmp(a, "--tas-ckpt-live")) scfg.tas_ckpt_live = 1;
        else if (!strcmp(a, "--no-tas-rng")) scfg.tas_rng = 0;
        else if (!strcmp(a, "--tas-rng-share") && i + 1 < argc) scfg.tas_rng_share = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-rng-per") && i + 1 < argc) scfg.tas_rng_per = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--tas-rng-frames") && i + 1 < argc) scfg.tas_rng_min = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--session-cells")) scfg.cells = 1;
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
        tas_collect_dir(tas_dirs[i], cart->title, scfg.tas, &scfg.tas_n, (int)(sizeof scfg.tas / sizeof *scfg.tas));
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
        dp_mapwin_stop();
        dp_results_free(&res);
        dp_cart_free(cart);
        return 1;
    }
    {
        const char *note = dp_mach_mapper_note(cart->rom);
        if (note) fprintf(stderr, "deepeel: warning: %s\n", note);
    }
    DpMach *mach = dp_mach_open(vi, 0);
    dp_mach_set_rom(mach, cart->rom, cart->rom_size);
    if (gb_core) {
        gkcfg.max_total = cfg.max_steps_total;
        gkcfg.max_path_steps = getenv("DEEPEEL_GB_PATH_STEPS") ? strtoull(getenv("DEEPEEL_GB_PATH_STEPS"), NULL, 0) : 1000;   // short paths, many entries: Zelda 39 M steps 4x the natural bytes of 100000-step paths
        gkcfg.jobs = cfg.jobs > 1 ? (unsigned)cfg.jobs : 1;
        for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--jobs") && !strcmp(argv[i + 1], "auto")) { gkcfg.jobs = (unsigned)auto_jobs(); gkcfg.adaptive = 1; }
        gkcfg.hooks = dp_machine_gb_hooks();
        dp_mach_reset(mach);   // power-on state
        DpMachine *km = dp_machine_gb(mach);
        int rc = 1;
        if (km) {
            dp_machine_gb_source(km, cart->rom, cart->rom_size);
            DpTas gt; memset(&gt, 0, sizeof gt);
            uint8_t *gmasks = NULL;
            if (scfg.tas_n >= 1) {   // GB pad (a1 b2 select4 start8 right16 left32 up64 down128) -> kernel mask; recorder sync profiles are not applied here
                char terr[160] = "";
                if (dp_tas_load(scfg.tas[0], &gt, terr, sizeof terr) && gt.frames) {
                    char why[256];
                    int match = dp_tas_rom_match(&gt, cart->rom, cart->rom_size, why, sizeof why);
                    if (match == 0 && !scfg.tas_any_rom) {   // another revision desyncs by construction
                        fprintf(stderr, "deepeel: TAS %s refused: recorded on another ROM (%s); --tas-any-rom forces it\n", gt.source, why);
                        dp_tas_free(&gt); memset(&gt, 0, sizeof gt);
                    } else if (match == 1) fprintf(stderr, "deepeel: TAS %s: ROM identity verified (%s)\n", gt.source, why);
                }
                if (gt.frames && (gmasks = malloc(gt.frames))) {
                    for (uint32_t f = 0; f < gt.frames; f++) {
                        uint8_t b = gt.masks[f], k = 0;
                        if (b & 64) k |= 0x01;
                        if (b & 128) k |= 0x02;
                        if (b & 32) k |= 0x04;
                        if (b & 16) k |= 0x08;
                        if (b & 1) k |= 0x10;
                        if (b & 2) k |= 0x20;
                        if (b & 8) k |= 0x40;
                        if (b & 4) k |= 0x80;   // select
                        gmasks[f] = k;
                    }
                    if (getenv("DEEPEEL_TAS_MASKS")) {   // the movie as tools/lr_dsy masks (U D L R B A Select Start), for the reference side
                        FILE *mf = fopen(getenv("DEEPEEL_TAS_MASKS"), "wb");
                        if (mf) {
                            for (uint32_t f = 0; f < gt.frames; f++) {
                                uint8_t k = gmasks[f], r = (uint8_t)(k & 0x0F);
                                if (k & 0x10) r |= 0x20; if (k & 0x20) r |= 0x10; if (k & 0x40) r |= 0x80; if (k & 0x80) r |= 0x40;
                                fputc(r, mf);
                            }
                            fclose(mf);
                        }
                    }
                    gkcfg.tas_masks = gmasks; gkcfg.tas_frames = gt.frames;
                    dp_machine_gb_set_tas(km, &gt);   // the recorder's sync profile
                } else fprintf(stderr, "deepeel: --tas: %s\n", terr);
            }
            uint32_t *gseed_pc = NULL; uint16_t *gseed_bk = NULL;
            size_t gseed_cap = getenv("DEEPEEL_GB_POINTER_SEEDS") ? (size_t)atol(getenv("DEEPEEL_GB_POINTER_SEEDS")) : 40000;   // 0 turns it off
            if (gseed_cap) {   // pointer-table entry discovery (core_gbseeds.c), to match the GB explorer's speculative seeds
                gkcfg.n_seeds = dp_core_gb_pointer_seeds(cart->rom, cart->rom_size, gseed_cap, &gseed_pc, &gseed_bk);
                gkcfg.seeds = gseed_pc; gkcfg.seed_banks = gseed_bk;
            }
            DpCoreResult kr;
            rc = dp_core_explore(km, cart->rom_size, &gkcfg, &kr) ? 0 : 1;
            free(gseed_pc); free(gseed_bk);
            if (!rc) dp_core_report(stdout, "gb", &kr); core_outputs(argc, argv, "gb", path, &kr);
            dp_core_result_free(&kr);
            free(gmasks); dp_tas_free(&gt);
            km->close(km);
        } else fprintf(stderr, "deepeel: --core: cannot wrap the machine\n");
        dp_mapwin_stop();
        dp_results_free(&res);
        dp_cart_free(cart);
        return rc;
    }
    if (cfg.unbounded && !nocredit_set) cfg.nocredit_cut = 20000;   // unbounded still ends a path that only loops over known code
    if (cfg.unbounded) { cfg.max_paths = UINT32_MAX; cfg.max_steps_total = UINT64_MAX; }   // no path/step wall: the run ends when nothing is left
    DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
    // the common exploration tree: every seed, fork and session deposit is
    // one choice; --resume reloads it from tree.bin
    DpTree tree;
    dp_tree_init(&tree);
    ex->tree = &tree;
    if (scfg.state_dir) {                      // closed choices leave RAM: the tree grows on disk
        char spath[540];
        snprintf(spath, sizeof spath, "%s/tree.spill", scfg.state_dir);
        dp_tree_set_spill(&tree, spath);
    }
    if (scfg.resume && scfg.state_dir) {
        char tpath[540];
        snprintf(tpath, sizeof tpath, "%s/tree.bin", scfg.state_dir);
        if (dp_tree_load(&tree, tpath)) {
            fprintf(stderr, "deepeel: resumed tree (%u choices, depth %u)\n", tree.n - 1,
                    tree.max_depth);
        } else if (access(tpath, F_OK) == 0) {
            // unreadable or over the in-RAM cap: never let the next save overwrite it silently
            char kept[560];
            snprintf(kept, sizeof kept, "%s.prev", tpath);
            if (rename(tpath, kept) == 0)
                fprintf(stderr, "deepeel: tree.bin not reloaded (too large or invalid); kept as %s, starting a new tree\n", kept);
            else
                fprintf(stderr, "deepeel: tree.bin not reloaded and could not be set aside (%s)\n", tpath);
        }
    }
    ex->defer_tier_limit = tier_limit;
    ex->defer_all = defer_all;
    // the frontier directory exists whatever the options: next to the outputs
    // when there is no --state-dir
    char fdir_auto[600];
    char ub_dir[600];
    if (cfg.unbounded && !scfg.state_dir) {   // the spool needs a directory: next to the outputs
        const char *b = json ? json : text ? text : html ? html : summary ? summary : "deepeel-unbounded";
        snprintf(ub_dir, sizeof ub_dir, "%s.state", b);
        mkdir(ub_dir, 0777);
        scfg.state_dir = ub_dir;
    }
    if (scfg.state_dir) mkdir(scfg.state_dir, 0777);   // a missing --state-dir must not silently disable the spool
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
        if (scfg.resume) {
            char cpath[540];
            snprintf(cpath, sizeof cpath, "%s/coverage.bin", scfg.state_dir);
            char dpath[540];
            snprintf(dpath, sizeof dpath, "%s/disc.bin", scfg.state_dir);
            long dl = dp_results_disc_load(&res, dpath);
            if (dl >= 0) fprintf(stderr, "deepeel: resumed discovery log (%ld entries)\n", dl);
            long g = dp_results_cov_load(&res, cpath);
            if (g >= 0) fprintf(stderr, "deepeel: resumed coverage map (%ld bytes known again)\n", g);
            char spath[560];
            snprintf(spath, sizeof spath, "%s.sticky", cpath);
            long sg = dp_map_sticky_load(&res, spath);
            if (sg >= 0) fprintf(stderr, "deepeel: resumed inferred map (%ld bytes kept)\n", sg);
        }
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
    // plain fork exploration: SIGINT asks the explorer to stop cleanly (state, spool and report are
    // written) instead of killing the process; --play installs its own cooperative handler below
    g_sig_ex = ex;
    signal(SIGINT, on_explore_sigint);
    g_sig_tree = &tree;
    signal(SIGUSR1, on_explore_usr1);
    if (scfg.time_limit_secs) { // wall-clock limit for the whole run, play or not: same clean stop as SIGINT
        signal(SIGALRM, on_explore_sigint);
        alarm((unsigned)scfg.time_limit_secs);
    }
    int rc = 0;
    if (mapwin_on && play_engine && !gui && dp_play_watch_enable(play_engine, 4)) { /* the game screen beside the map */ }
    else if (mapwin_on && play_engine && !gui) fprintf(stderr, "deepeel: --mapwin: no game screen without a --tas movie\n");
    if (mapwin_on) dp_mapwin_start(&res, mapwin, mapwin_on && play_engine && !gui ? play_engine : NULL);
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
        static const char *why[8] = { "frame budget reached", "stagnation wall reached",
                                      "interrupted", "every byte explained", "memory cap reached",
                                      "TAS replay done", "time budget reached", "yield stop" };
        fprintf(stderr, "deepeel: play done after %llu frames (%s), %.2fs\n",
                (unsigned long long)dp_play_frames(play_engine),
                reason >= 0 && reason < 8 ? why[reason] : "?", secs);
        stop_txt = reason >= 0 && reason < 8 ? why[reason] : "?";
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
        if (scfg.cells)
            fprintf(stderr, "deepeel: %llu screen cells archived into the checkpoint pool\n", (unsigned long long)dp_play_cells(play_engine));
        if (scfg.tas_n)
            fprintf(stderr, "deepeel: TAS cuts absorbed (movie kept) %llu, wedged-machine fallbacks %llu\n",
                    (unsigned long long)dp_play_tas_cuts(play_engine),
                    (unsigned long long)dp_play_tas_cut_stuck(play_engine));
        // the play's cooperative SIGINT handler only stops the sessions: back to the explorer's own
        // handler, or a SIGINT during the total search is never seen (the drain ran on without end)
        signal(SIGINT, on_explore_sigint);
        // phase 3, mandatory: the total search drains the frontier the
        // sessions fed, depth-first, until every choice is closed
        // the play budget ran out by the clock (not a user SIGINT): the fork search keeps its own
        // wall budget instead of inheriting an already-fired alarm (it used to never start)
        if ((g_alarm_fired || reason == 6) && g_search_limit_secs && reason != 2) {
            g_alarm_fired = 0;
            dp_explore_interrupt = 0;
            ex->stop = 0;
            alarm(g_search_limit_secs);
            fprintf(stderr, "deepeel: play budget spent, fork search gets %us\n", g_search_limit_secs);
        }
        if (reason != 2 && !(g_alarm_fired && dp_explore_interrupt && !g_search_limit_secs)) {
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
                fprintf(stderr, "deepeel: TAS shoots: %d snapshots seeded (%llu from residual banks: backsearch first)\n",
                        seeds, (unsigned long long)dp_play_tas_bank_hits(play_engine));
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
        write_rtcode(&res, rtcode_path);
        write_ramcode(&res, ramcode_path);
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
        write_rtcode(&res, rtcode_path);
        write_ramcode(&res, ramcode_path);
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
    if (scfg.state_dir) {
        char cpath[540];
        snprintf(cpath, sizeof cpath, "%s/coverage.bin", scfg.state_dir);
        if (!dp_results_cov_save(&res, cpath)) fprintf(stderr, "deepeel: cannot save %s\n", cpath);
        {
            char spath[560];
            snprintf(spath, sizeof spath, "%s.sticky", cpath);
            DpMapSummary ms;
            free(dp_map_build(&res, &ms)); // refreshes the sticky classes before they are saved
            if (!dp_map_sticky_save(&res, spath)) fprintf(stderr, "deepeel: cannot save %s\n", spath);
        }
        char dpath[540];
        snprintf(dpath, sizeof dpath, "%s/disc.bin", scfg.state_dir);
        if (!dp_results_disc_save(&res, dpath)) fprintf(stderr, "deepeel: cannot save %s\n", dpath);
    }
    if (fdir) {
        if (!scfg.state_dir) mkdir(fdir, 0777);
        int fr = dp_explorer_frontier_save(ex, fdir);
        if (fr) fprintf(stderr, "deepeel: %d continuations/seeds saved in %s/frontier for --resume\n", fr, fdir);
    }
    dp_explorer_free(ex);
    if (play_engine) dp_play_free(play_engine);
    dp_mach_close(mach);
    dp_mapwin_stop();
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
    int saved_png = cv_in != NULL;
    if (ln > 8 && !memcmp(text, "\x89PNG", 4) && !cv_in) {
        fprintf(stderr, "deepeel: %s: PNG without a dpCv class vector\n", inmap);
        free(text);
        return 1;
    }
    // "md map v1" / "nes map v1" (one class digit per ROM byte): rewrite as
    // whole-ROM runs "0 start end class" and let the z80 splitter below window them
    if (!cv_in && ln > 11 && (!strncmp(text, "md map v1\n", 10) || !strncmp(text, "nes map v1\n", 11))) {
        char *d = text + (text[0] == 'm' ? 10 : 11);
        char *dend = memchr(d, '\n', (size_t)(text + ln - d));
        if (!dend) dend = text + ln;
        DpBuf runs;
        dp_buf_init(&runs);
        for (char *q = d; q < dend;) {
            char *r2 = q;
            while (r2 < dend && *r2 == *q) r2++;
            int k = *q - '0';
            dp_buf_fmt(&runs, "0 %x %x %s\n", (unsigned)(q - d), (unsigned)(r2 - d) - 1, dp_map_class_name(k));
            q = r2;
        }
        free(text);
        text = (char *)runs.p;
        ln = (long)runs.len;
        z80_map = 1;
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
    } else if (!strcmp(sysname, "nes")) {
        size_t file_size = 0; char err[256];
        uint8_t *data = nes_read_file(rom, &file_size);
        DpNesMach *machine = data ? dp_nesmach_open(data, file_size, -1, err, sizeof err) : NULL;
        if (!machine) { fprintf(stderr, "deepeel: cannot read NES PRG from %s\n", rom); free(data); free(text); free(cv_in); return 1; }
        rom_size = (unsigned)dp_nesmach_prg_size(machine);
        dp_nesmach_close(machine); free(data);
        rom_banks = (int)((rom_size + 0x3FFF) / 0x4000);
    } else { // raw cartridges: size from the file, banks of 16 KiB
        FILE *rf = fopen(rom, "rb");
        if (!rf) { perror(rom); free(text); return 1; }
        fseek(rf, 0, SEEK_END);
        rom_size = (unsigned)ftell(rf);
        fclose(rf);
        if (!strcmp(sysname, "pce") && rom_size > 512 && rom_size % 0x2000 == 512)
            rom_size -= 512;
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
    int ok = dp_map_png_from_saved(text, (size_t)ln, rom_banks,
                                     title, meta, out, sysname,
                                     strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom,
                                     rom_size, saved_png ? inmap : NULL);
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
                        "deepeel_heat|deepeel_cov|deepeel_rom|dpCv|dpCh|dpFr|dpTb|dpIx|dpMt|dpFi]\n");
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
                        "[--max-total N] [--max-steps N] [--max-paths N] [--queue N] [--queue-max N] [--summary FILE] [--heat FILE] "
                        "[--loop-cap N] [--fork-cap N] [--no-irq-fork] [--entry bank:addr]... [--seed-banks] [--text FILE] [--map FILE] [--evidence FILE] [--cov FILE] [--romdata FILE] [--rtcode FILE]\n");
        return 2;
    }
    const char *path = NULL, *mapper_name = "auto", *nes_heat = NULL;
    DpNesOptions opt = dp_nes_options();   // defer_tiers 0: cuts stay final; --defer-tiers N opts in (sega's CLI defaults to 3, NES measured neutral on DuckTales)
    opt.demo_frames = DP_DEFAULT_ATTRACT_FRAMES;
    DpCliCommon cc = { .jobs = 1 };
    int core_flag = 0; for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--core")) core_flag = 1;
    int use_core = 0; DpCoreConfig kcfg; dp_core_config_default(&kcfg);
    kcfg.no_sessions = 1;   // measured on SMB at 2 M steps: sessions halve the natural path's budget (4708 -> 2063 bytes); --pad-sessions N enables them
    kcfg.visits_per_pc = 50000;   // a frame wait is thousands of iterations (SMB: natural 15 -> 4708 bytes, old engine 4716)
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (0) {}
        else if (!strcmp(a, "--core")) use_core = 1;
        else if (core_flag && dp_core_parse_option(&kcfg, argc, argv, i)) { i += dp_core_parse_option(&kcfg, argc, argv, i) - 1; }   // only with --core: the old engine owns --attract-frames etc.
        DP_CLI_COMMON(opt, cc)
        DP_CLI_PLAYED(opt)
        else if (!strcmp(a, "--play-pool") && i + 1 < argc) opt.play_pool = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--ctx-live")) opt.ctx_live = 1;
        else if (!strcmp(a, "--defer-tiers") && i + 1 < argc) {
            opt.defer_tiers = (unsigned)strtoul(argv[++i], NULL, 0);
            if (opt.defer_tiers > DP_TIER_MAX) opt.defer_tiers = DP_TIER_MAX;
        }
        else if (!strcmp(a, "--heat") && i + 1 < argc) nes_heat = argv[++i];   // --summary/--cov/--romdata come from DP_CLI_COMMON
        else if (!strcmp(a, "--tas") && i + 1 < argc) {
            if (opt.tas_count < 8) snprintf(opt.tas_paths[opt.tas_count++], 512, "%s", argv[++i]);
            else { fprintf(stderr, "deepeel: at most 8 TAS movies\n"); return 2; }
        }
        else if (!strcmp(a, "--mapper") && i + 1 < argc) mapper_name = argv[++i];
        else if (!strcmp(a, "--max-paths") && i + 1 < argc) opt.max_paths = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue") && i + 1 < argc) opt.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--ctx-live")) opt.ctx_live = 1;
        else if (!strcmp(a, "--queue-max") && i + 1 < argc) opt.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--spool") && i + 1 < argc) opt.lot_path = argv[++i];
        else if (!strcmp(a, "--resume") && i + 1 < argc) { opt.lot_path = argv[++i]; opt.lot_drain = 1; }
        else if (!strcmp(a, "--seed-banks")) opt.seed_banks = 1;
        else if (!strcmp(a, "--seed-vectors")) opt.seed_vectors = 1;
        else if (!strcmp(a, "--bank-dispatch")) opt.second_pass = opt.bank_dispatch = 1;
        else if (!strcmp(a, "--no-p1-fork")) opt.no_pad_probe = 1;
        else if (!strcmp(a, "--no-event-probes")) opt.event_probes = 0;
        else if (!strcmp(a, "--pad-full")) opt.pad_full = 1;
        else if (!strcmp(a, "--entry") && i + 1 < argc && opt.entry_count < 32) {
            uint8_t eb; uint16_t ea;
            if (!parse_entry(argv[++i], &eb, &ea)) { fprintf(stderr, "deepeel: bad --entry '%s' (bank:addr)\n", argv[i]); return 2; }
            opt.entry_bank[opt.entry_count] = eb; opt.entry_addr[opt.entry_count++] = ea;
        }
        else if (!path) path = a;
        else { fprintf(stderr, "deepeel: unknown option %s\n", a); return 2; }
    }
    const char *rtcode_path = cc.rtcode; int nes_jobs = cc.jobs, nes_watch = cc.watch; const char *nes_dump = cc.dump;
    const char *text = cc.text, *map = cc.map, *evidence = cc.evidence;
    dp_cli_arm_stop(cc.time_limit);
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
    if (use_core) {
        char err[128] = "";
        DpMachine *km = dp_machine_nes(data, size, mapper, err, sizeof err);
        if (!km) { fprintf(stderr, "deepeel: --core: %s\n", err); free(data); return 1; }
        kcfg.jobs = (unsigned)nes_jobs;
        if (opt.max_total) kcfg.max_total = opt.max_total;
        if (opt.max_steps) kcfg.max_path_steps = opt.max_steps;
        if (opt.fork_cap) kcfg.fork_site_cap = opt.fork_cap;
        if (opt.queue_max) kcfg.queue_max = opt.queue_max;
        kcfg.hooks = dp_machine_nes_hooks();
        DpNesTas ntas; memset(&ntas, 0, sizeof ntas);
        uint8_t *kmasks = NULL;
        if (opt.tas_count > 0) {   // first movie replays in the kernel (m->frame counts the frames; masks go from the NES pad layout to the kernel's)
            char terr[160] = "";
            if (dp_nes_tas_load(opt.tas_paths[0], &ntas, terr, sizeof terr) && (kmasks = malloc(ntas.frames ? ntas.frames : 1))) {
                for (uint32_t f = 0; f < ntas.frames; f++) {
                    uint8_t b = ntas.masks[f], k = 0;
                    if (b & 0x01) k |= 0x10; if (b & 0x02) k |= 0x20; if (b & 0x08) k |= 0x40;
                    if (b & 0x10) k |= 0x01; if (b & 0x20) k |= 0x02; if (b & 0x40) k |= 0x04; if (b & 0x80) k |= 0x08;
                    kmasks[f] = k;
                }
                kcfg.tas_masks = kmasks; kcfg.tas_frames = ntas.frames;
            } else fprintf(stderr, "deepeel: --tas: %s\n", terr);
        }
        DpCoreResult kr;
        size_t prg = dp_machine_nes_prg_size(km);
        int kok = dp_core_explore(km, prg, &kcfg, &kr);
        if (kok) dp_core_report(stdout, "nes", &kr); core_outputs(argc, argv, "nes", path, &kr);
        dp_core_result_free(&kr);
        free(kmasks); dp_nes_tas_free(&ntas);
        km->close(km);
        free(data);
        return kok ? 0 : 1;
    }
    DpNesResults res;
    if (nes_watch) dp_fbwatch_start("deepeel nes watch");
    int nes_ok = dp_nes_explore_jobs(data, size, mapper, &opt, &res, nes_jobs);
    if (nes_watch) dp_fbwatch_stop();
    if (nes_dump && dp_fb_dump_ppm(nes_dump)) fprintf(stderr, "deepeel: no frame to dump\n");
    if (!nes_ok) {
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
        size_t ml = strlen(map);
        if (ml >= 4 && !strcmp(map + ml - 4, ".png")) {
            uint8_t *classes = dp_nes_map_build(&res);
            const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
            if (!classes || !dp_map_png_from_evidence(classes, res.prg_size, res.play_exec_hits,
                                                       res.play_data_hits, res.src_flags, &res.first,
                                                       base, "nes", base, map))
                fprintf(stderr, "deepeel: cannot write %s\n", map);
            free(classes);
        } else {
            FILE *f = fopen(map, "w");
            if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", map); }
            else { dp_nes_map(f, &res); fclose(f); }
        }
    }
    if (evidence) {
        FILE *f = fopen(evidence, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", evidence); }
        else { dp_nes_evidence(f, &res); fclose(f); }
    }
    if (cc.cov) {
        FILE *f = fopen(cc.cov, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", cc.cov); }
        else { dp_nes_cov(f, &res); fclose(f); }
    }
    if (cc.summary && dp_nes_summary(&res, cc.summary)) fprintf(stderr, "deepeel: cannot write %s\n", cc.summary);
    if (nes_heat && dp_nes_heat(&res, nes_heat)) fprintf(stderr, "deepeel: cannot write %s\n", nes_heat);
    if (cc.romdata) {
        FILE *f = fopen(cc.romdata, "w");
        if (!f) { fprintf(stderr, "deepeel: cannot write %s\n", cc.romdata); }
        else { dp_nes_romdata(f, &res); fclose(f); }
    }
    if (rtcode_path) {
        FILE *f = fopen(rtcode_path, "w");
        if (f) { dp_nes_rtcode(f, &res); fclose(f); }
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
                        "[--novelty STEPS] [--novelty-frames N] [--novelty-archive N] [--novelty-cells] [--pad-fork-cap N] [--irq-fork-cap N] [--no-irq-fork] [--no-static] [--no-infer] [--multitap] [--jobs 2] "
                        "[--text FILE] [--map FILE] [--png FILE] [--evidence FILE] [--entry ADDR]... [--ctx-cap N] [--ctx-live] [--summary FILE]\n");
        return 2;
    }
    const char *path = NULL, *png = NULL;
    DpPceOptions opt = dp_pce_options();
    opt.demo_frames = DP_DEFAULT_ATTRACT_FRAMES;
    DpCliCommon cc = { .jobs = 1 };
    int core_flag = 0; for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--core")) core_flag = 1;
    int multitap = 0, use_core = 0; DpCoreConfig kcfg; dp_core_config_default(&kcfg);
    kcfg.visits_per_pc = 50000;   // Raiden at 2 M steps: natural 116 bytes at 64 visits, 2220 at 50000 (frame waits are long loops)
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (0) {}
        else if (!strcmp(a, "--core")) use_core = 1;
        else if (core_flag && dp_core_parse_option(&kcfg, argc, argv, i)) { i += dp_core_parse_option(&kcfg, argc, argv, i) - 1; }   // only with --core: the old engine owns --attract-frames etc.
        DP_CLI_COMMON(opt, cc)
        DP_CLI_PLAYED(opt)
        else if (!strcmp(a, "--ctx-live")) opt.ctx_live = 1;
        else if (!strcmp(a, "--queue-max") && i + 1 < argc) opt.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--play-pool") && i + 1 < argc) opt.play_pool = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-vectors")) opt.seed_vectors = 1;
        else if (!strcmp(a, "--seed-banks")) opt.seed_banks = 1;
        else if (!strcmp(a, "--entry") && i + 1 < argc) {
            if (opt.entry_count >= 16) { fprintf(stderr, "deepeel: at most 16 --entry\n"); return 2; }
            opt.entries[opt.entry_count++] = (uint16_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(a, "--png") && i + 1 < argc) png = argv[++i];   // --summary/--cov/--romdata come from DP_CLI_COMMON
        else if (!strcmp(a, "--tas") && i + 1 < argc) {
            if (opt.tas_count < 8) snprintf(opt.tas_paths[opt.tas_count++], 512, "%s", argv[++i]);
            else { fprintf(stderr, "deepeel: at most 8 TAS movies\n"); return 2; }
        }
        else if (!strcmp(a, "--pad-fork-cap") && i + 1 < argc) opt.pad_fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--defer-tiers") && i + 1 < argc) {
            opt.defer_tiers = (unsigned)strtoul(argv[++i], NULL, 0);
            if (opt.defer_tiers > DP_TIER_MAX) opt.defer_tiers = DP_TIER_MAX;
        }
        else if (!strcmp(a, "--irq-fork-cap") && i + 1 < argc) opt.irq_fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--no-static")) opt.static_closure = 0;
        else if (!strcmp(a, "--no-infer")) opt.infer = 0;
        else if (!strcmp(a, "--multitap")) multitap = 1;
        else if (!strcmp(a, "--bank-dispatch")) opt.bank_dispatch = 1;
        else if (!path) path = a;
        else { fprintf(stderr, "deepeel: unknown option %s\n", a); return 2; }
    }
    const char *rtcode_path = cc.rtcode; int pce_jobs = cc.jobs, pce_watch = cc.watch; const char *pce_dump = cc.dump;
    const char *text = cc.text, *map = cc.map, *pce_ev = cc.evidence;
    dp_cli_arm_stop(cc.time_limit);
    if (opt.fork_cap > 255) opt.fork_cap = 255; // per-site counters are bytes
    if (opt.pad_fork_cap > 255) opt.pad_fork_cap = 255;
    if (opt.irq_fork_cap > 255) opt.irq_fork_cap = 255;
    if (multitap && pce_jobs > 1) { // the scan state is process-global: one instance only
        fprintf(stderr, "deepeel: --multitap keeps its scan state process-wide; running on one instance\n");
        pce_jobs = 1;
    }
    dp_pce_multitap(multitap);
    size_t size;
    uint8_t *data = nes_read_file(path, &size);
    if (!data) { fprintf(stderr, "deepeel: cannot read %s\n", path); return 1; }
    if (use_core) {
        if (!dp_pce_init(data, size)) { fprintf(stderr, "deepeel: --core: not a PCE image\n"); free(data); return 1; }
        DpMachine *km = dp_machine_pce();
        if (km) dp_machine_pce_source(km, data, size);
        kcfg.jobs = (unsigned)pce_jobs;
        kcfg.hooks = dp_machine_pce_hooks();
        if (!km) { free(data); return 1; }
        if (opt.max_total) kcfg.max_total = opt.max_total;
        if (opt.max_steps) kcfg.max_path_steps = opt.max_steps;
        if (opt.fork_cap) kcfg.fork_site_cap = opt.fork_cap;
        if (opt.queue_max) kcfg.queue_max = opt.queue_max;
        DpPceTas ptas; memset(&ptas, 0, sizeof ptas);
        uint8_t *kmasks = NULL;
        if (opt.tas_count > 0) {
            char terr[160] = "";
            if (dp_pce_tas_load(opt.tas_paths[0], &ptas, terr, sizeof terr) && (kmasks = malloc(ptas.frames ? ptas.frames : 1))) {
                for (uint32_t f = 0; f < ptas.frames; f++) {
                    uint8_t b = ptas.pads[f], k = 0;
                    if (b & 0x01) k |= 0x10; if (b & 0x02) k |= 0x20; if (b & 0x08) k |= 0x40;
                    if (b & 0x10) k |= 0x01; if (b & 0x40) k |= 0x02; if (b & 0x80) k |= 0x04; if (b & 0x20) k |= 0x08;
                    kmasks[f] = k;
                }
                kcfg.tas_masks = kmasks; kcfg.tas_frames = ptas.frames;
            } else fprintf(stderr, "deepeel: --tas: %s\n", terr);
        }
        DpCoreResult kr;
        int kok = dp_core_explore(km, dp_pce_rom_size(), &kcfg, &kr);
        free(kmasks); dp_pce_tas_free(&ptas);
        if (kok) dp_core_report(stdout, "pce", &kr); core_outputs(argc, argv, "pce", path, &kr);
        dp_core_result_free(&kr);
        km->close(km);
        free(data);
        return kok ? 0 : 1;
    }
    DpPceResults *res = calloc(1, sizeof *res);
    if (pce_watch) dp_fbwatch_start("deepeel pce watch");
    int pce_ok = res && dp_pce_explore_jobs(data, size, &opt, res, pce_jobs);
    if (pce_watch) dp_fbwatch_stop();
    if (pce_dump && dp_fb_dump_ppm(pce_dump)) fprintf(stderr, "deepeel: no frame to dump\n");
    if (!pce_ok) {
        fprintf(stderr, "deepeel: pce explore failed (%s)\n", path);
        free(res);
        free(data);
        return 1;
    }
    fprintf(stderr, "deepeel: pce explore done — steps %llu, paths %llu, forks %llu, sections %u, copies %u\n",
            (unsigned long long)res->steps, (unsigned long long)res->paths,
            (unsigned long long)res->forks, res->section_count, res->copy_count);
    if (cc.summary && dp_pce_summary(res, cc.summary)) fprintf(stderr, "deepeel: cannot write %s\n", cc.summary);
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
        size_t ml = strlen(map);
        if (ml >= 4 && !strcmp(map + ml - 4, ".png")) {
            uint8_t *classes = dp_pce_map_build(res);
            const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
            if (!classes || !dp_map_png_from_evidence(classes, res->rom_size, res->play_exec_hits,
                                                       res->play_data_hits, res->src_flags, &res->first,
                                                       base, "pce", base, map))
                fprintf(stderr, "deepeel: cannot write %s\n", map);
            free(classes);
        } else {
            FILE *f = fopen(map, "w");
            if (!f) fprintf(stderr, "deepeel: cannot write %s\n", map);
            else { dp_pce_map(f, res); fclose(f); }
        }
    }
    if (pce_ev) {
        FILE *f = fopen(pce_ev, "w");
        if (!f) fprintf(stderr, "deepeel: cannot write %s\n", pce_ev);
        else { dp_pce_evidence(f, res); fclose(f); }
    }
    if (cc.cov) {
        FILE *f = fopen(cc.cov, "w");
        if (!f) fprintf(stderr, "deepeel: cannot write %s\n", cc.cov);
        else { dp_pce_cov(f, res); fclose(f); }
    }
    if (cc.romdata) {
        FILE *f = fopen(cc.romdata, "w");
        if (!f) fprintf(stderr, "deepeel: cannot write %s\n", cc.romdata);
        else { dp_pce_romdata(f, res); fclose(f); }
    }
    if (rtcode_path) {
        FILE *f = fopen(rtcode_path, "w");
        if (f) { dp_pce_rtcode(f, res); fclose(f); }
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
        uint32_t rseeded = dp_residual_target(ex, 4096); // residual ranges: re-proof or forced entry (the tried set bounds reloops)
        uint32_t ref0 = res->closure_seeds_refused;
        int seeded = dp_closure_seed(ex, seed_total - *seed_spent) + (int)rseeded; // the cascade solde
        *seed_spent += (uint32_t)seeded;
        uint32_t refused = res->closure_seeds_refused - ref0;
        fprintf(stderr, "deepeel: closure round %d: +%u static bytes, %d entries seeded (%u residual-targeted), %u refused\n",
                r + 1, marked, seeded, res->residual_seeded, refused);
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

/* per-movie output path in a --tas-dir chain: <dir-of-base>/<i>-<movie name><ext of base> */
static void md_movie_output_path(char *out, size_t cap, const char *base, int idx, const char *movie) {
    const char *ext = strrchr(base, '.'), *b = strrchr(movie, '/');
    b = b ? b + 1 : movie;
    if (ext && ext != base)
        snprintf(out, cap, "%.*s/%02d-%s%s", (int)(ext - base), base, idx + 1, b, ext);
    else
        snprintf(out, cap, "%s/%02d-%s", base, idx + 1, b);
}

/* the reference tile: tools/md_ref_live (ClownMDEmu, the reference core of
 * tools/md_compare_reference) replays the same movie into the shared feed
 * (DEEPEEL_MD_REF_FEED, the GB VBA harness pattern); spawned per movie, killed
 * when the explorer moves on — the two cores run at their own pace and the
 * window shows each tile's speed separately */
static const char *md_ref_live_path(void) {
    const char *env = getenv("DP_MD_REF_LIVE");
    if (env && *env) return env;
    static char p[600];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n > 0) {
        p[n] = 0;
        char *s = strrchr(p, '/');
        if (s) { snprintf(s + 1, (size_t)(sizeof p - (size_t)(s + 1 - p)), "md_ref_live"); if (!access(p, X_OK)) return p; }
    }
    snprintf(p, sizeof p, "build/md-reference/md_ref_live");
    if (!access(p, X_OK)) return p;
    return NULL;
}

static int md_has_core(int argc, char **argv) {
    for (int i = 4; i < argc; i++) if (!strcmp(argv[i], "--core")) return 1;
    return 0;
}

static int cmd_md(int argc, char **argv) {
    const char *md_rtcode = NULL;
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
    } else if (!strcmp(argv[2], "explore") && md_has_core(argc, argv)) {
        DpCoreConfig kcfg; dp_core_config_default(&kcfg);
        kcfg.max_path_steps = 50000000;   // boot clears RAM/VRAM with millions of dbf iterations before the first interrupt is unmasked
        kcfg.hooks = dp_machine_md_hooks();
        kcfg.visits_per_pc = 2000000;   // the header checksum loop runs ROM-size/2 iterations (Sonic 1: 262144): the old 262144 cap cut it one short; was 262144 at 2 M steps: natural 32 bytes at 64 visits, 126 at 262144 (old engine ~134)
        const char *md_core_tas = NULL;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--tas") && i + 1 < argc) { md_core_tas = argv[++i]; continue; }
            int used = dp_core_parse_option(&kcfg, argc, argv, i);
            if (used) { i += used - 1; continue; }
            if (!strcmp(argv[i], "--max-total") && i + 1 < argc) kcfg.max_total = strtoull(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--fork-cap") && i + 1 < argc) kcfg.fork_site_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--progress") && i + 1 < argc) kcfg.progress_secs = (unsigned)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) { ++i; if (!strcmp(argv[i], "auto")) { kcfg.jobs = (unsigned)auto_jobs(); kcfg.adaptive = 1; } else kcfg.jobs = (unsigned)strtoul(argv[i], NULL, 0); }
        }
        DpMdMach *mm = dp_mdmach_open(c.rom, c.size);
        if (mm) dp_mdmach_reset(mm);   // power-on state: the 68000 starts from the vectors (without it the path faults)
        DpMachine *km = mm ? dp_machine_md(mm) : NULL;
        if (km) dp_machine_md_source(km, c.rom, c.size);
        if (!km) { fprintf(stderr, "deepeel: --core: cannot open the ROM\n"); rc = 1; }
        else {
            DpMdMovie mv; memset(&mv, 0, sizeof mv);
            uint8_t *kmasks = NULL;
            if (md_core_tas) {   // GMV / BK2 pads (0-3 directions, 16 A, 32 B, 64 C, 128 Start) -> kernel mask
                char terr[160] = "";
                if (dp_md_movie_load(md_core_tas, &mv, terr, sizeof terr) && (kmasks = malloc(mv.frames ? mv.frames : 1))) {
                    for (uint32_t f = 0; f < mv.frames; f++) {
                        uint8_t b = mv.pads[f], k = b & 0x0F;
                        if (b & 0x50) k |= 0x10;   // A or C
                        if (b & 0x20) k |= 0x20;
                        if (b & 0x80) k |= 0x40;
                        kmasks[f] = k;
                    }
                    kcfg.tas_masks = kmasks; kcfg.tas_frames = mv.frames;
                } else fprintf(stderr, "deepeel: --tas: %s\n", terr);
            }
            DpCoreResult kr;
            rc = dp_core_explore(km, c.size, &kcfg, &kr) ? 0 : 1;
            if (!rc) dp_core_report(stdout, "md", &kr); core_outputs(argc, argv, "md", NULL, &kr);
            dp_core_result_free(&kr);
            free(kmasks); dp_md_movie_free(&mv);
            km->close(km);
        }
    } else if (!strcmp(argv[2], "explore")) {
        DpMdOptions o;
        int md_any_rom = 0;
        DpCliCommon cc = { .jobs = 1 };
        int md_unbounded = 0;
        dp_md_options_default(&o);
        o.fork_cap = DP_DEFAULT_BRANCH_FORK_CAP;
        o.demo_frames = DP_DEFAULT_ATTRACT_FRAMES;
        const char *movie_path = NULL, *md_heat = NULL, *md_tas_dir = NULL;
        for (int i = 4; i < argc; i++) {
            const char *a = argv[i];
            if (i + 1 >= argc && strcmp(a, "--lot-drain") && strcmp(a, "--unbounded")) { fprintf(stderr, "deepeel: bad option %s\n", a); rc = 2; break; }
            if (0) {}
            DP_CLI_COMMON(o, cc)
            else if (!strcmp(a, "--path-steps")) o.path_steps = strtoull(argv[++i], NULL, 0);
            else if (!strcmp(a, "--unbounded")) md_unbounded = 1;
            else if (!strcmp(a, "--cut-padding")) o.pad_cut = 1;
            else if (!strcmp(a, "--no-cut-padding")) o.pad_cut = 0;
            else if (!strcmp(a, "--seed-vectors")) o.seed_vectors = 1;
            else if (!strcmp(a, "--bank-dispatch")) o.bank_dispatch = 1;
            else if (!strcmp(a, "--exact-dedup")) o.exact_dedup = 1;
            else if (!strcmp(a, "--ctx-live")) o.ctx_live = 1;
            else if (!strcmp(a, "--defer-tiers") && i + 1 < argc) {
                o.defer_tiers = (unsigned)strtoul(argv[++i], NULL, 0);
                if (o.defer_tiers > DP_TIER_MAX) o.defer_tiers = DP_TIER_MAX;
            }
            else if (!strcmp(a, "--pad-full")) o.pad_full = 1;
            else if (!strcmp(a, "--pad-full6")) o.pad_full = o.pad_full6 = 1;
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
            else if (!strcmp(a, "--tas-dir") && i + 1 < argc) md_tas_dir = argv[++i];
            else if (!strcmp(a, "--demo-frames")) o.demo_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--attract-frames")) o.demo_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--play-frames")) o.play_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--play-sessions")) o.play_sessions = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--play-seed")) o.play_seed = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--play-pool")) o.play_pool = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--shoot-every")) o.shoot_every = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(a, "--tas-any-rom")) md_any_rom = 1;
            else if (!strcmp(a, "--heat")) md_heat = argv[++i];   // --summary/--cov/--romdata come from DP_CLI_COMMON
            else { fprintf(stderr, "deepeel: unknown option %s\n", a); rc = 2; break; }
        }
        const char *text = cc.text, *map = cc.map, *ev = cc.evidence, *md_dump = cc.dump; md_rtcode = cc.rtcode;
        int md_jobs = cc.jobs, md_watch = cc.watch; unsigned md_time_limit = cc.time_limit;
        const char *movies[64];
        int nmovies = 0;
        if (!rc && movie_path) movies[nmovies++] = movie_path;
        if (!rc && md_tas_dir)   // the movie chain: every replay is whole, the window moves to the next when one ends
            tas_collect_dir(md_tas_dir, c.overseas, movies, &nmovies, (int)(sizeof movies / sizeof *movies));
        if (!rc && !nmovies) movies[nmovies++] = NULL;   // plain exploration, no movie: one report as before
        if (nmovies > 1) {   // per-movie outputs live in the directory of the requested file
            mkdir(cc.summary, 0777);   // a missing --summary directory would silently lose the summaries
            if (md_watch && !o.watch_every) o.watch_every = 2;   // the progress caption needs published frames
        }
        const char *reflive = md_watch ? md_ref_live_path() : NULL;
        char feedpath[512];
        if (reflive) {   // the window picks the feed up by name (dp_fb_ref_active)
            { const char *td = getenv("TMPDIR"); snprintf(feedpath, sizeof feedpath, "%s/deepeel-md-ref-%d.bin", td && *td ? td : "/tmp", (int)getpid()); }
            setenv("DEEPEEL_MD_REF_FEED", feedpath, 1);
            {   // the reference core's name with its version (git revision of its tree, next to this checkout)
                char dir[600], cmd[800], rev[40] = "";
                const char *cd = getenv("CLOWN_MD_DIR");
                if (cd) snprintf(dir, sizeof dir, "%s", cd);
                else { snprintf(dir, sizeof dir, "%s", reflive); char *q = strstr(dir, "/build/md-reference/"); if (q) { *q = 0; strncat(dir, "/../clownmdemu-core", sizeof dir - strlen(dir) - 1); } else dir[0] = 0; }
                snprintf(cmd, sizeof cmd, "git -C '%s' rev-parse --short HEAD 2>/dev/null", dir);
                FILE *gp = dir[0] ? popen(cmd, "r") : NULL;
                if (gp) { if (fgets(rev, sizeof rev, gp)) rev[strcspn(rev, "\n")] = 0; pclose(gp); }
                char nm[80]; snprintf(nm, sizeof nm, "ClownMDEmu%s%s", rev[0] ? " " : "", rev);
                setenv("DEEPEEL_MD_REF_NAME", nm, 1);
            }
        } else if (md_watch)
            fprintf(stderr, "deepeel: md watch: no reference tile (build it with tools/md_ref_live.sh)\n");
        if (md_watch) dp_fbwatch_start("deepeel md watch");
        int md_failed = 0;
        for (int mi = 0; !rc && mi < nmovies && !md_failed; mi++) {
            DpMdMovie movie = {0};
            if (movies[mi] && !dp_md_movie_load(movies[mi], &movie, err, sizeof err)) {
                fprintf(stderr, "deepeel: GMV %s: %s\n", movies[mi], err);
                continue;
            }
            if (movie.has_md5) {
                uint8_t h[16]; dp_md5(c.rom, c.size, h);
                /* BizHawk's Genesis header hash is not a reliable ROM digest (a movie that
                 * replays to its last frame can carry a different value): warn, never refuse */
                if (!md_any_rom && memcmp(h, movie.md5, 16))
                    fprintf(stderr, "deepeel: warning: BK2 header hash differs from the ROM MD5 "
                            "(the replay will show whether the movie matches)\n");
            }
            o.movie = &movie;
            pid_t refpid = 0, hawkpid = 0, gpgxpid = 0;
            const char *wf[3] = { NULL, NULL, NULL };   // feeds to wait on: every tile starts together
            if (reflive && movies[mi]) {   // reset the feed, then the reference replays this movie
                dp_md_ref_feed_open(feedpath, 1);
                wf[0] = feedpath;
                refpid = fork();
                if (refpid == 0) {
                    execl(reflive, reflive, argv[3], movies[mi], (char *)NULL);
                    fprintf(stderr, "deepeel: md ref: exec %s failed\n", reflive);
                    _exit(127);
                }
                if (refpid < 0) { fprintf(stderr, "deepeel: md ref: fork failed (no reference tile)\n"); refpid = 0; }
            }
            const char *gpgxexe = md_watch ? getenv("DP_MD_GPGX_LIVE") : NULL;   // tools/gpgx_lr_live: the standalone Genesis Plus GX libretro core
            const char *gpgxcore = getenv("DP_GPGX_LR_CORE");
            if (gpgxexe && gpgxcore && movies[mi]) {
                char f3[560], mk3[560];
                const char *td = getenv("TMPDIR");
                snprintf(f3, sizeof f3, "%s/deepeel-md-gpgx-%d.bin", td && *td ? td : "/tmp", (int)getpid());
                snprintf(mk3, sizeof mk3, "%s.masks", f3);
                FILE *mf = fopen(mk3, "wb");
                if (mf) { fwrite(movie.pads, 1, movie.frames, mf); fclose(mf); }
                dp_md_ref_feed_open(f3, 1);
                setenv("DEEPEEL_MD_REF3_FEED", f3, 1);
                {   // the libretro core's own checkout: its release line and git revision
                    char dir[600], cmd[700], rev[40] = ""; snprintf(dir, sizeof dir, "%s", gpgxcore);
                    char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
                    snprintf(cmd, sizeof cmd, "git -C '%s' rev-parse --short HEAD 2>/dev/null", dir);
                    FILE *gp = popen(cmd, "r");
                    if (gp) { if (fgets(rev, sizeof rev, gp)) rev[strcspn(rev, "\n")] = 0; pclose(gp); }
                    char nm[120]; snprintf(nm, sizeof nm, "Genesis Plus GX libretro%s%s", rev[0] ? " " : "", rev);
                    setenv("DEEPEEL_MD_REF3_NAME", nm, 1);
                }
                wf[2] = f3;
                gpgxpid = fork();
                if (gpgxpid == 0) { execl(gpgxexe, gpgxexe, gpgxcore, argv[3], mk3, f3, (char *)NULL); _exit(127); }
                if (gpgxpid < 0) gpgxpid = 0;
            }
            const char *hawkexe = md_watch ? getenv("DP_MD_BIZHAWK_LIVE") : NULL;   // tools/bizhawk_md_live: BizHawk's GPGX as the second tile
            if (hawkexe && movies[mi]) {
                char f2[560], mk[560];
                const char *td = getenv("TMPDIR");
                snprintf(f2, sizeof f2, "%s/deepeel-md-hawk-%d.bin", td && *td ? td : "/tmp", (int)getpid());
                snprintf(mk, sizeof mk, "%s.masks", f2);
                FILE *mf = fopen(mk, "wb");
                if (mf) { fwrite(movie.pads, 1, movie.frames, mf); fclose(mf); }
                dp_md_ref_feed_open(f2, 1);
                setenv("DEEPEEL_MD_REF2_FEED", f2, 1);
                {   // name the BizHawk checkout it runs (git describe)
                    const char *bd = getenv("DP_BIZHAWK_DIR"); char cmd[600], ver[64] = "";
                    snprintf(cmd, sizeof cmd, "git -C '%s' describe --tags --always 2>/dev/null", bd ? bd : "/home/m/dev/BizHawk");
                    FILE *gp = popen(cmd, "r");
                    if (gp) { if (fgets(ver, sizeof ver, gp)) ver[strcspn(ver, "\n")] = 0; pclose(gp); }
                    char nm[120]; snprintf(nm, sizeof nm, "BizHawk GPGX%s%s", ver[0] ? " " : "", ver);
                    setenv("DEEPEEL_MD_REF2_NAME", nm, 1);
                }
                wf[1] = f2;
                hawkpid = fork();
                if (hawkpid == 0) {
                    const char *bd = getenv("DP_BIZHAWK_DIR");
                    char ld[700]; snprintf(ld, sizeof ld, "%s/Assets/dll", bd ? bd : "/home/m/dev/BizHawk");
                    setenv("LD_LIBRARY_PATH", ld, 1);
                    execlp("mono", "mono", hawkexe, argv[3], mk, f2, (char *)NULL);
                    _exit(127);
                }
                if (hawkpid < 0) hawkpid = 0;
            }
            for (int k = 0; k < 3; k++) {   // the harnesses boot slowly (mono, waterbox): hold our replay until each published its first frame (30 s cap)
                DpMdRefFeed *wfd = wf[k] ? dp_md_ref_feed_open(wf[k], 0) : NULL;
                for (int t = 0; wfd && t < 600 && wfd->vframe == 0; t++) usleep(50000);
            }
            char cap[240], pth[5][600];
            const char *sum = cc.summary, *txt = text, *mp = map, *evp = ev, *heat = md_heat;
            if (md_watch && movies[mi]) {
                const char *b = strrchr(movies[mi], '/'); b = b ? b + 1 : movies[mi];
                snprintf(cap, sizeof cap, "film %d/%d  %s", mi + 1, nmovies, b);
                dp_fbwatch_caption(cap);
                dp_fbwatch_progress(0, movie.frames, 0);
            }
            if (nmovies > 1) {   // suffix every requested output with the movie, none silently overwrites another
                if (sum)  { md_movie_output_path(pth[0], sizeof pth[0], sum, mi, movies[mi]); sum = pth[0]; }
                if (txt)  { md_movie_output_path(pth[1], sizeof pth[1], txt, mi, movies[mi]); txt = pth[1]; }
                if (mp)   { md_movie_output_path(pth[2], sizeof pth[2], mp, mi, movies[mi]); mp = pth[2]; }
                if (evp)  { md_movie_output_path(pth[3], sizeof pth[3], evp, mi, movies[mi]); evp = pth[3]; }
                if (heat) { md_movie_output_path(pth[4], sizeof pth[4], heat, mi, movies[mi]); heat = pth[4]; }
            }
            if (md_unbounded) { // owner-decided exception: coverage over time and resources; the RAM queue stays
                // modest and the excess states go to the disk lot (resumable with --resume); stops on exhaustion,
                // --time-limit or SIGINT
                o.max_total = UINT64_MAX; o.path_steps = UINT64_MAX;
                o.fork_cap = UINT32_MAX; o.visit_cap = UINT32_MAX;
                if (!o.queue_max) o.queue_max = o.queue_cap < 4096 ? 4096 : o.queue_cap;
                static char lotname[600];
                if (!o.lot_path) { snprintf(lotname, sizeof lotname, "%s.lot", text ? text : "md-unbounded"); o.lot_path = lotname; }
                fprintf(stderr, "deepeel: md --unbounded: caps lifted, excess states spooled to %s\n", o.lot_path);
            }
            dp_cli_arm_stop(md_time_limit);
            DpMdResults r;
            int mrc = dp_md_explore_jobs(&c, &o, &r, md_jobs);
            if (md_watch) dp_fbwatch_progress(movie.frames, movie.frames, 1);
            if (md_dump && dp_fb_dump_ppm(md_dump)) fprintf(stderr, "deepeel: no frame to dump (the ROM never completed a frame)\n");
            if (mrc) {
                fprintf(stderr, "deepeel: md explore failed on %s\n", movies[mi]);
                md_failed = 1;
                rc = 1;
            } else {
                FILE *f = txt ? fopen(txt, "w") : stdout;
                if (f) {
                    dp_md_explore_report(&c, &o, &r, f);
                    if (txt) fclose(f);
                }
                if (md_rtcode) dp_md_explore_rtcode(&r, md_rtcode);
                if (mp) {
                    size_t ml = strlen(mp);
                    if (ml >= 4 && !strcmp(mp + ml - 4, ".png")) {
                        uint8_t *classes = dp_md_map_build(&r);
                        const char *base = strrchr(argv[3], '/'); base = base ? base + 1 : argv[3];
                        if (!classes || !dp_map_png_from_evidence(classes, r.rom_size, r.play_exec_hits,
                                                                   r.play_data_hits, r.src_flags, &r.first,
                                                                   base, "md", base, mp))
                            fprintf(stderr, "deepeel: cannot write %s\n", mp);
                        free(classes);
                    } else dp_md_explore_map(&r, mp);
                }
                if (evp) dp_md_explore_evidence(&r, evp);
                if (sum) dp_md_explore_summary(&r, sum);
                if (heat) dp_md_explore_heat(&r, heat);
                if (cc.cov) dp_md_explore_cov(&r, cc.cov);
                if (cc.romdata) dp_md_explore_romdata(&r, cc.romdata);
                dp_md_results_free(&r);
            }
            if (gpgxpid > 0) { kill(gpgxpid, SIGKILL); waitpid(gpgxpid, NULL, 0); }
            if (hawkpid > 0) { kill(hawkpid, SIGKILL); waitpid(hawkpid, NULL, 0); }
            if (refpid > 0) {   // the harness exits by itself at the end of the movie; kill covers an early move-on
                kill(refpid, SIGKILL);
                waitpid(refpid, NULL, 0);
            }
            dp_md_movie_free(&movie);
        }
        if (md_watch) dp_fbwatch_stop();
    } else rc = (usage(), 2);
    dp_md_cart_free(&c);
    return rc;
}


// deepeel tas info MOVIE | deepeel tas diff A B : generic BizHawk movie reader (any core)
static int bz_open(const char *path, DpBzMovie *m) {
    size_t n = 0;
    uint8_t *d = nes_read_file(path, &n);
    char err[160] = "";
    if (!d) { fprintf(stderr, "deepeel: cannot read %s\n", path); return 0; }
    int ok = dp_bz_load(d, n, m, err, sizeof err);
    free(d);
    if (!ok) fprintf(stderr, "deepeel: %s: %s\n", path, err);
    return ok;
}
// deepeel desync A.dsy B.dsy [--window N] [--offset-a N] [--offset-b N] [--show K] [--ignore block:lo-hi]... [--ring DIR]
// compares two per-frame state streams (src/core/desync.h) and reports the first divergence with the last N frames
static int cmd_desync(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: deepeel desync A.dsy B.dsy [--window N] [--offset-a N] [--offset-b N] [--show K] [--ignore block:lo-hi] [--ring DIR]\n"); return 2; }
    DpDsyOptions o; DpDsyIgnore ign[32];
    memset(&o, 0, sizeof o);
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--window") && i + 1 < argc) o.window = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--offset-a") && i + 1 < argc) o.offset_a = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--offset-b") && i + 1 < argc) o.offset_b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) o.show = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ring") && i + 1 < argc) o.ring_dir = argv[++i];
        else if (!strcmp(argv[i], "--ignore") && i + 1 < argc && o.nignore < 32) {
            char *a = argv[++i], *c = strchr(a, ':');
            DpDsyIgnore *g = &ign[o.nignore];
            if (!c) { fprintf(stderr, "--ignore wants block:lo-hi (hex)\n"); return 2; }
            snprintf(g->block, sizeof g->block, "%.*s", (int)(c - a), a);
            g->lo = (uint32_t)strtoul(c + 1, &c, 16);
            g->hi = *c == '-' ? (uint32_t)strtoul(c + 1, NULL, 16) : g->lo;
            o.nignore++;
        } else { fprintf(stderr, "desync: unknown option %s\n", argv[i]); return 2; }
    }
    o.ignore = ign;
    uint32_t n = 0;
    int rc = dp_dsy_compare(argv[2], argv[3], &o, stdout, &n);
    return rc < 0 ? 2 : rc;
}

static int cmd_tas(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[2], "info")) {
        DpBzMovie m;
        if (!bz_open(argv[3], &m)) return 1;
        dp_bz_describe(&m, stdout);
        dp_bz_free(&m);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[2], "diff")) {
        DpBzMovie a, b;
        if (!bz_open(argv[3], &a)) return 1;
        if (!bz_open(argv[4], &b)) { dp_bz_free(&a); return 1; }
        unsigned oa = 0, ob = 0;
        long f = dp_bz_diff(&a, &b, &oa, &ob);
        printf("A: %s %s %u frames; B: %s %s %u frames\n", a.platform, a.core, a.nframes, b.platform, b.core, b.nframes);
        printf("columns only in A: %u, only in B: %u\n", oa, ob);
        if (f < 0) printf("inputs identical over %u common frames\n", a.nframes < b.nframes ? a.nframes : b.nframes);
        else printf("first differing frame: %ld\n", f);
        dp_bz_free(&a); dp_bz_free(&b);
        return f < 0 ? 0 : 3;
    }
    fprintf(stderr, "usage: deepeel tas info MOVIE | deepeel tas diff MOVIE_A MOVIE_B\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage(), 2;
    if (!strcmp(argv[1], "frontier") && argc > 3 && !strcmp(argv[2], "stats")) {
        char kp[700]; snprintf(kp, sizeof kp, "%s/frontier.bin", argv[3]);   // kernel frontier (--core --state-dir)
        FILE *kf = fopen(kp, "rb");
        if (kf) {
            uint32_t kh[3];
            if (fread(kh, 4, 3, kf) == 3 && kh[0] == 0x31524644u) { printf("frontier: %u kernel states pending (%u bytes each)\n", kh[2], kh[1]); fclose(kf); return 0; }
            fclose(kf);
        }
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
        char kp[700]; snprintf(kp, sizeof kp, "%s/frontier.bin", argv[4]);   // kernel frontier: resume it to the end
        if (!access(kp, R_OK)) {
            char *av[12]; int n = 0;
            av[n++] = argv[0]; av[n++] = (char *)"explore"; av[n++] = argv[3]; av[n++] = (char *)"--core";
            av[n++] = (char *)"--state-dir"; av[n++] = argv[4]; av[n++] = (char *)"--resume"; av[n] = NULL;
            return cmd_explore(n, av);
        }
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
    if (!strcmp(argv[1], "bh")) return dp_bh_command(argc, argv);
    if (!strcmp(argv[1], "md")) return cmd_md(argc, argv);
    if (!strcmp(argv[1], "nes")) return cmd_nes(argc, argv);
    if (!strcmp(argv[1], "pce")) return cmd_pce(argc, argv);
    if (!strcmp(argv[1], "z80") && argc > 2 && !strcmp(argv[2], "explore"))
        return dp_sega_explore_command(argc, argv);
    if (!strcmp(argv[1], "z80")) return dp_z80_command(argc, argv);
    if (!strcmp(argv[1], "explore")) return cmd_explore(argc, argv);
    if (!strcmp(argv[1], "mapx")) return cmd_mapx(argc, argv);
    if (!strcmp(argv[1], "maprender")) return cmd_maprender(argc, argv);
    if (!strcmp(argv[1], "tas")) return cmd_tas(argc, argv);
    if (!strcmp(argv[1], "desync")) return cmd_desync(argc, argv);
    if (!strcmp(argv[1], "map")) return cmd_map(argc, argv);
    if (!strcmp(argv[1], "regs")) return cmd_regs(argc, argv);
    if (!strcmp(argv[1], "dis")) return cmd_dis(argc, argv);
    return usage(), 2;
}
