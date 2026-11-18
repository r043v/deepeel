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
#include "core/gbs.h"
#include "core/music_index.h"
#include "core/music_index_seeds.h"
#include <zlib.h>   // crc32: the .verified sidecar records the dump CRC the index is keyed by
#include "core/zip.h"   // dp_read_rom_file: the GBS rip may also be read from a zip member
int dp_gbs_fetch(const char *game, const char *dir, char *out_gbs, size_t cap, char *err, size_t errsz);   // core/gbsfetch.c (network, not in the core lib)
#include "explore/frontier.h"
#include "explore/md_trace.h"
#include "core/machine.h"
#include "core/sta.h"
#include "mach/m0boy.h"
#include "core/snapshot_png.h"
#include "explore/results.h"
#include "explore/coverage_defaults.h"
#include "report/report.h"
#include "gui/fbwatch.h"
#include "gui/kmapwin.h"
#include "cpu/sm83/sm83.h"
#include "platform/sega/sega.h"
#include "platform/msx/msx.h"
#include "platform/sega/explore.h"
#include "platform/md/md_registry.h"
#include "mach/mdmach.h"
#include "cpu/m68k/m68k.h"
#include "platform/nes/explore.h"
#include "platform/pce/explore.h"
#include "platform/pce/pce.h"
#include "core/dpmovie.h"
#include "platform/nes/tas.h"
#include "platform/pce/tas.h"
#include "explore/tas.h"

#include "explore/core.h"
#include "explore/seedset.h"
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
static void core_outputs(int argc, char **argv, const char *id, const char *rom, const DpMachine *m, const DpCoreResult *r) {
    if (argv_opt(argc, argv, "--romdata")) fprintf(stderr, "deepeel: --romdata: not produced by the kernel yet (ignored)\n");
    if (argv_opt(argc, argv, "--evidence")) fprintf(stderr, "deepeel: --evidence: not produced by the kernel yet (ignored)\n");
    dp_core_write_cov(argv_cov(argc, argv), id, m, r);
    if (argv_opt(argc, argv, "--rtcode") && r->ra.n) { FILE *rf = fopen(argv_opt(argc, argv, "--rtcode"), "w"); if (rf) { dp_ramarch_write(&r->ra, rf, id); fclose(rf); } }
    if (dp_core_write_map(argv_opt(argc, argv, "--map"), id, rom, r) < 0) fprintf(stderr, "deepeel: cannot write --map\n");
    if (dp_core_write_summary(argv_opt(argc, argv, "--summary"), id, r) < 0) fprintf(stderr, "deepeel: cannot write --summary\n");
    const char *t = argv_opt(argc, argv, "--text");
    if (t) { FILE *f = fopen(t, "w"); if (f) { dp_core_report(f, id, r); fclose(f); } }
}

static uint8_t *nes_read_file(const char *path, size_t *size);
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
// --mapwin [SCALE]: handled once in main() (kernel live map); the per-platform parsers only skip it and its optional scale
static int skip_mapwin(int argc, char **argv, int *i) {
    if (strcmp(argv[*i], "--mapwin")) return 0;
    if (*i + 1 < argc && argv[*i + 1][0] >= '1' && argv[*i + 1][0] <= '8' && !argv[*i + 1][1]) (*i)++;
    return 1;
}
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
            "deepeel — ROM memory-map explorer (generic kernel: GB, NES, PCE, MD, SG/SMS/GG/MSX)\n\n"
            "  deepeel explore rom.gb [options]\n"
            "  deepeel z80 explore <sg|sms|gg|msx> ROM [--mapper NAME] [options]\n"
            "  deepeel nes explore ROM [options]\n"
            "  deepeel pce explore ROM [options]\n"
            "  deepeel md explore ROM [options]\n"
            "  deepeel lr explore CORE ROM [options]       libretro core as machine\n"
            "  deepeel z80|md dis ROM hex-offset [instruction-count]\n"
            "  deepeel dis rom.gb bank:addr [n]\n"
            "  deepeel regs <gb|nes|pce|md|sg|sms|gg|msx>   dump the register registry\n"
            "  deepeel map rom.gb                           static map only (pointer tables, padding)\n"
            "  deepeel maprender rom.gb in.map out.map.png [--meta f]\n"
            "  deepeel mapx x.map.png out [--chunk NAME]    extract one chunk of a map PNG\n\n"
            "common options (every platform):\n"
            "    --max-total N        global step wall\n"
            "    --jobs N|auto        parallel workers sharing one frontier\n"
            "    --unbounded          lift every cap (forks, queue, spool, paths, steps, memory): only exhaustion or SIGINT stops\n"
            "    --state-dir D        persist the frontier/spool/coverage; --resume continues it\n"
            "    --cov-transfer ROM COV   replay another revision's coverage as seeds where the instruction windows match byte for byte\n"
            "    --cov-hits COV      seed the heat from a coverage file's hit counts (the max wins)\n"
            "    --entry bank:addr    extra seed (repeatable);  --seed-banks  seed every bank\n"
            "    --tas f / --tas-dir D   replay movies (VBA .vmv/.vbm, BizHawk .bk2, GMV, FM2, text log); --tas-any-rom accepts another revision\n"
            "    --no-sessions --no-solve --no-closure --no-infer --no-exact-state ...   switch one technique off (see docs/COUVERTURE.fr.md)\n"
            "    --mapwin [SCALE]     live ROM map window;  --watch [FPS]  game screen window (SDL2)\n"
            "    --gbs FILE / --gbs-fetch [NAME] / --music-index [FILE]   seed entry points from music rips\n"
            "    --text f / --json f / --summary f / --cov f / --romdata f / --map f / --html f   outputs\n"
            "                         (--map: PNG with heat and embedded chunks; .map or .txt writes the plain text)\n"
            "retired options of the former GB engine (--play, --queue, --gui, --human ...) are ignored with a notice\n");
}

static int parse_entry(const char *s, uint16_t *bank, uint16_t *addr) {
    char *end;
    long b = strtol(s, &end, 0);
    if (end == s || *end != ':' || b < 0 || b > 65535) return 0;
    const char *start = end + 1;
    long a = strtol(start, &end, 16); // addresses are hexadecimal
    if (end == start || *end || a < 0 || a > 0xFFFF) return 0;
    *bank = (uint16_t)b;
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


static int cmd_md(int argc, char **argv);
static int g_drain_only; // frontier drain: no root seeding, only the saved frontier runs
// the runtime-code archive: one line per distinct instruction run from RAM, input of the recompiler

// the executed WRAM/HRAM code as byte variants, input of m0boy's `gbrecomp --ram-code`

// append extra {pc, bank} seeds to the kernel's arrays (the caller's extras are copied);
// 0 ok, -1 on allocation failure (the existing seeds are kept)
static int core_seed_append(uint32_t **pcs, uint16_t **bks, DpSeedRegs **regs, size_t *n,
                            const uint32_t *apcs, const uint16_t *abks, const DpSeedRegs *aregs, size_t an) {
    if (!an) return 0;
    uint32_t *p = realloc(*pcs, (*n + an) * sizeof *p);
    uint16_t *b = realloc(*bks, (*n + an) * sizeof *b);
    if (!p || !b) { free(p); return -1; }
    memcpy(p + *n, apcs, an * sizeof *p);
    memcpy(b + *n, abks, an * sizeof *b);
    *pcs = p; *bks = b;
    if (aregs) {
        DpSeedRegs *r;
        if (!*regs) {   // the seeds already in the array carry no registers
            r = malloc((*n + an) * sizeof *r);
            if (!r) return -1;
            for (size_t i = 0; i < *n; i++) { r[i].a = 0xFF; r[i].sp = 0xFFFF; }
        } else {
            r = realloc(*regs, (*n + an) * sizeof *r);
            if (!r) return -1;
        }
        memcpy(r + *n, aregs, an * sizeof *r);
        *regs = r;
    }
    *n += an;
    return 0;
}

/* append one confirmed entry-point seed (GBS verify or music index); grows the arrays;
 * a/sp are the register seeds (0xFF/0xFFFF: leave A/SP at their start value) */
static int seed_push(uint32_t **pcs, uint16_t **bks, DpSeedRegs **regs, int *n,
                     uint32_t pc, uint16_t bank, uint8_t a, uint16_t sp) {
    uint32_t *np = realloc(*pcs, (*n + 1) * sizeof **pcs);
    if (!np) return -1;
    *pcs = np;
    uint16_t *nb = realloc(*bks, (*n + 1) * sizeof **bks);
    if (!nb) return -1;
    *bks = nb;
    DpSeedRegs *nr = realloc(*regs, (*n + 1) * sizeof **regs);
    if (!nr) return -1;
    *regs = nr;
    (*pcs)[*n] = pc; (*bks)[*n] = bank;
    (*regs)[*n].a = a; (*regs)[*n].sp = sp;
    (*n)++;
    return 0;
}

/* dp_rom_index -> "bank N $ADDR" for the report; bank 0 maps $0000-$3FFF directly */
static void rom_str(char *out, size_t cap, uint32_t rom) {
    if (rom < 0x4000) snprintf(out, cap, "bank 0 $%04x", rom);
    else snprintf(out, cap, "bank %u $%04x", rom >> 14, 0x4000 + (rom & 0x3FFF));
}

// Options of the retired GB engine: accepted and ignored (with one stderr line) so existing scripts keep running;
// the generic kernel has its own budgets (--max-total, --unbounded, --state-dir ...).
static int retired_option(const char *a, int *i, int argc) {
    static const char *const valued[] = { "--queue", "--queue-max", "--queue-mem", "--max-paths", "--loop-cap", "--nocredit-cut",
        "--covered-keep", "--closure-rounds", "--spool-cap-mb", "--frontier-cap-gb", "--seed-cap", "--mem-cap", "--time-limit",
        "--search-limit", "--fork-site-cap", "--tier", "--yield-min", "--yield-window", "--stagnation", "--epoch", "--frames",
        "--seed", "--checkpoints", "--stall", "--tas-ckpt", "--tas-ckpt-secs", "--tas-ckpt-max", "--tas-rng-share", "--tas-rng-per",
        "--tas-rng-frames", "--uniform-hold", "--fork-paths", "--from-png", "--export-playable", "--export-slot", "--inputs",
        "--input", NULL };
    static const char *const flags[] = { "--play", "--seed-banks", "--no-context-evict", "--no-irq-fork", "--no-key-fork",
        "--no-p1-fork", "--no-siblings", "--no-provenance", "--no-covered-prune", "--until-static", "--tas-solo", "--session-cells",
        "--gui", "--human", "--spool-to-frontier", "--ram-code-forced", "--no-tas-rng", "--tas-ckpt-live", NULL };
    for (int k = 0; valued[k]; k++)
        if (!strcmp(a, valued[k]) && *i + 1 < argc) { fprintf(stderr, "deepeel: %s: retired option, ignored\n", a); ++*i; return 1; }
    for (int k = 0; flags[k]; k++)
        if (!strcmp(a, flags[k])) { fprintf(stderr, "deepeel: %s: retired option, ignored\n", a); return 1; }
    return 0;
}

static int cmd_explore(int argc, char **argv) {
    const char *text = NULL, *path = NULL;
    const char *summary = NULL;
    const char *gbs_path = NULL, *gbs_name = NULL, *gbs_dir = NULL;
    int gbs_fetch = 0;
    const char *midx_path = NULL;
    int midx_given = 0;
    const char *tas_dirs[64];
    int tas_dirs_n = 0;
    const char *gb_wdump = NULL;
    int watch = 0;
    DpConfig cfg;
    dp_config_defaults(&cfg);
    struct { const char *tas[8]; int tas_n; int tas_any_rom; } scfg = { {0}, 0, 0 };   // --tas movies (the kernel reads --state-dir/--resume itself)
    uint16_t eb[32];
    DpCoreConfig gkcfg; dp_core_config_default(&gkcfg); gkcfg.visits_per_pc = 2000000;   // frame waits are long loops   // --core: the generic kernel instead of the GB explorer
    uint16_t ea[32];
    int entries = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--core")) { /* accepted, no effect: the kernel is the only engine */ }
        else if (dp_core_parse_option(&gkcfg, argc, argv, i)) { i += dp_core_parse_option(&gkcfg, argc, argv, i) - 1; }   // kernel options first: --attract-frames etc. mean the kernel's here
        else if (!strcmp(a, "--json") && i + 1 < argc) ++i;   // written by core_outputs
        else if (!strcmp(a, "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "--rtcode") && i + 1 < argc) ++i;   // written by core_outputs (the kernel's runtime code archive)
        else if (!strcmp(a, "--ram-code") && i + 1 < argc) ++i;   // accepted for compatibility, no longer produced (use --rtcode)
        else if (!strcmp(a, "--summary") && i + 1 < argc) summary = argv[++i];
        else if (!strcmp(a, "--cov") && i + 1 < argc) ++i;   // written by core_outputs
        else if (!strcmp(a, "--romdata") && i + 1 < argc) ++i;   // accepted for compatibility; core_outputs reports it is unavailable
        else if (!strcmp(a, "--map") && i + 1 < argc) ++i;   // written by core_outputs
        else if (!strcmp(a, "--html") && i + 1 < argc) ++i;   // written by core_outputs
        else if (skip_mapwin(argc, argv, &i)) {}
        else if (!strcmp(a, "--watch-dump") && i + 1 < argc) { gb_wdump = argv[++i]; if (!watch) watch = 30; }
        else if (!strcmp(a, "--watch")) {
            watch = 30;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') watch = atoi(argv[++i]);
            if (watch < 1) watch = 1;
            if (watch > 30) watch = 30;
        }
        else if (!strcmp(a, "--max-steps") && i + 1 < argc) cfg.max_steps_path = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-total") && i + 1 < argc) cfg.max_steps_total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue-max") && i + 1 < argc) gkcfg.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--jobs") && i + 1 < argc) { ++i; cfg.jobs = !strcmp(argv[i], "auto") ? auto_jobs() : atoi(argv[i]); }
        else if (!strcmp(a, "--no-pointer-scan")) cfg.no_pointer_scan = 1;
        else if (!strcmp(a, "--gbs") && i + 1 < argc) gbs_path = argv[++i];
        else if (!strcmp(a, "--gbs-fetch")) {   // optional NAME; when omitted it is derived from the ROM file name
            gbs_fetch = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') gbs_name = argv[++i];
        }
        else if (!strcmp(a, "--gbs-dir") && i + 1 < argc) gbs_dir = argv[++i];
        else if (!strcmp(a, "--music-index")) {   // optional FILE; defaults to output/music_index/music_index.bin
            midx_given = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') midx_path = argv[++i];
        }
        else if (!strcmp(a, "--entry") && i + 1 < argc && entries < 32) {
            if (!parse_entry(argv[++i], &eb[entries], &ea[entries])) {
                fprintf(stderr, "deepeel: bad --entry '%s' (bank:addr)\n", argv[i]);
                return 2;
            }
            entries++;
        }
        // play sessions
        else if (!strcmp(a, "--tas") && i + 1 < argc && scfg.tas_n < (int)(sizeof scfg.tas / sizeof *scfg.tas)) scfg.tas[scfg.tas_n++] = argv[++i];
        else if (!strcmp(a, "--tas-dir") && i + 1 < argc && tas_dirs_n < 64) tas_dirs[tas_dirs_n++] = argv[++i];
        else if (!strcmp(a, "--tas-any-rom")) scfg.tas_any_rom = 1;
        else if (retired_option(a, &i, argc)) {}
        else if (!path) path = a;
        else { fprintf(stderr, "deepeel: unknown option %s\n", a); return 2; }
    }
    if (!path) {
        usage();
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
    // GBS rip: --gbs-fetch downloads the game's rip from gbs.joshw.info, --gbs reads a
    // local file; the data block is verified against this ROM (per-bank best match —
    // rips are patched and trimmed) and its init/play become confirmed seeds, the
    // counterpart of the speculative pointer scan.  Never silent: the full match report.
    DpGbs gbs; DpGbsMatch gmatch; memset(&gmatch, 0, sizeof gmatch);
    uint32_t *gbs_pcs = NULL; uint16_t *gbs_bks = NULL; DpSeedRegs *gbs_regs = NULL; int gbs_seeds = 0;
    if (gbs_path || gbs_fetch) {
        char gpath[1024], gerr[256];
        if (gbs_path) snprintf(gpath, sizeof gpath, "%s", gbs_path);
        else {
            const char *name = gbs_name; char auto_name[256];
            if (!name) {   // ROM file name minus its (...) tags and extension, like tasfetch
                const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
                size_t k = 0; int depth = 0;
                for (; *b && k + 1 < sizeof auto_name; b++) {
                    if (*b == '(') depth++;
                    else if (*b == ')') { if (depth) depth--; }
                    else if (!depth) auto_name[k++] = *b;
                }
                auto_name[k] = 0;
                char *dot = strrchr(auto_name, '.'); if (dot && dot != auto_name) *dot = 0;
                name = auto_name;
            }
            if (dp_gbs_fetch(name, gbs_dir ? gbs_dir : "output/gbs", gpath, sizeof gpath, gerr, sizeof gerr)) {
                fprintf(stderr, "deepeel: --gbs-fetch: %s\n", gerr);
                dp_cart_free(cart);
                return 1;
            }
        }
        size_t gn; uint8_t *gbuf = dp_read_rom_file(gpath, &gn, gerr, sizeof gerr);
        if (!gbuf || dp_gbs_parse(gbuf, gn, &gbs, gerr, sizeof gerr)) {
            fprintf(stderr, "deepeel: --gbs %s: %s\n", gpath, gerr);
            free(gbuf);
            dp_cart_free(cart);
            return 1;
        }
        double thr = 0.9;
        if (getenv("DEEPEEL_GBS_MATCH")) {
            double v = atof(getenv("DEEPEEL_GBS_MATCH"));
            if (v > 0 && v <= 1) thr = v;
        }
        unsigned nmatch = dp_gbs_verify(&gbs, cart->rom, cart->rom_size, thr, &gmatch);
        printf("gbs: \"%s\" songs %u first %u load $%04x init $%04x play $%04x (data %zu bytes, match %u/%u banks, threshold %.0f%%)\n",
               gbs.title, gbs.songs, gbs.first, gbs.load, gbs.init, gbs.play, gbs.data_size, nmatch, gmatch.rip_banks, thr * 100);
        if (gmatch.frag_scanned)   // no bank matched: the rip relocated by fragment vote
            printf("gbs: no byte-identical bank; fragments %u/%u found in ROM, dominant delta $%zx (%u votes)\n",
                   gmatch.frag_matched, gmatch.frag_total, gmatch.frag_dom_delta, gmatch.frag_dom_votes);
        for (unsigned r = 0; r < gmatch.rip_banks; r++) {
            if (!gmatch.bank[r].bytes) continue;
            if (gmatch.bank[r].matched)
                printf("gbs: rip bank %u -> rom bank %u (%u/%zu bytes, %u%%)\n", r, gmatch.bank[r].rom_bank,
                       gmatch.bank[r].best, gmatch.bank[r].bytes, (unsigned)(gmatch.bank[r].best * 100 / gmatch.bank[r].bytes));
            else if (!gmatch.frag_scanned)
                printf("gbs: rip bank %u -> ? (best bank %u, %u%%; second %u%%)\n", r, gmatch.bank[r].rom_bank,
                       (unsigned)(gmatch.bank[r].best * 100 / gmatch.bank[r].bytes),
                       (unsigned)(gmatch.bank[r].second * 100 / gmatch.bank[r].bytes));
        }
        int push_fail = 0;
        if (gmatch.init_bank >= 0) {
            printf("gbs: init -> rom bank %u $%04x\n", gmatch.init_bank, gmatch.init_addr);
            /* the init call carries the first song in A (GBS calling convention);
             * the boot state's stack ($FFFE) is kept — the header's stack byte
             * convention is not settled (see the S_ADDR arbitration note) */
            push_fail |= seed_push(&gbs_pcs, &gbs_bks, &gbs_regs, &gbs_seeds, gmatch.init_addr, (uint16_t)gmatch.init_bank, (uint8_t)gbs.first, 0xFFFF);
        } else printf("gbs: init $%04x not mapped to this ROM (player stub or unmatched bank)\n", gbs.init);
        if (gmatch.play_bank >= 0) {
            printf("gbs: play -> rom bank %u $%04x\n", gmatch.play_bank, gmatch.play_addr);
            push_fail |= seed_push(&gbs_pcs, &gbs_bks, &gbs_regs, &gbs_seeds, gmatch.play_addr, (uint16_t)gmatch.play_bank, 0xFF, 0xFFFF);
        } else printf("gbs: play $%04x not mapped to this ROM (player stub or unmatched bank)\n", gbs.play);
        /* the verified (rip, dump) pair is knowledge: the sidecar line records
         * the ROM mapping the byte-verify just proved (dump CRC32, rip bank ->
         * ROM bank, init/play in ROM space) so the music-index build ingests
         * it and --music-index alone re-seeds this game later — the same facts
         * the BAB rows carry, discovered by the tool instead of a database.
         * Never written when nothing mapped: a failed verify is not knowledge. */
        if (gbs_seeds) {
            char vpath[1100];
            snprintf(vpath, sizeof vpath, "%s.verified", gpath);
            uint32_t dump_crc = (uint32_t)crc32(0, cart->rom, (uInt)cart->rom_size);
            char needle[32];
            snprintf(needle, sizeof needle, "crc=%08x ", dump_crc);
            int seen = 0;
            FILE *rf = fopen(vpath, "r");
            if (rf) {
                char line[800];
                while (fgets(line, sizeof line, rf)) if (strstr(line, needle)) seen = 1;
                fclose(rf);
            }
            if (seen) printf("gbs: verified pair crc=%08x already recorded in %s\n", dump_crc, vpath);
            else {
                FILE *vf = fopen(vpath, "a");
                if (!vf) fprintf(stderr, "deepeel: cannot write %s\n", vpath);
                else {
                    char banks[4 * DP_GBS_MAX_BANKS] = "";
                    size_t bo = 0;
                    for (unsigned b = 0; b < gmatch.rip_banks && bo + 16 < sizeof banks; b++)
                        bo += (size_t)snprintf(banks + bo, sizeof banks - bo, "%s%d", b ? "," : "",
                                               gmatch.bank[b].matched ? (int)gmatch.bank[b].rom_bank : -1);
                    const char *rb = strrchr(path, '/'); rb = rb ? rb + 1 : path;
                    // `rom=` last: the dump file name can carry spaces
                    fprintf(vf, "dump crc=%08x load=%u data_size=%zu banks=%s", dump_crc, gbs.load, gbs.data_size, banks);
                    if (gmatch.init_bank >= 0)
                        fprintf(vf, " init_rom=%u", (unsigned)gmatch.init_bank * 0x4000u + (gmatch.init_addr & 0x3FFF));
                    if (gmatch.play_bank >= 0)
                        fprintf(vf, " play_rom=%u", (unsigned)gmatch.play_bank * 0x4000u + (gmatch.play_addr & 0x3FFF));
                    if (gmatch.frag_scanned && gmatch.frag_dom_votes)
                        fprintf(vf, " frag_delta=%zu frag_votes=%u", gmatch.frag_dom_delta, gmatch.frag_dom_votes);
                    fprintf(vf, " rom=%s\n", rb);
                    fclose(vf);
                    printf("gbs: verified pair recorded in %s (the music-index build ingests it)\n", vpath);
                }
            }
        }
        free(gbuf);
        if (push_fail) {   // same cleanup as the other early exits of the option block
            fprintf(stderr, "deepeel: cannot allocate GBS seeds\n");
            free(gbs_pcs); free(gbs_bks); free(gbs_regs);
            dp_cart_free(cart);
            return 1;
        }
        if (!nmatch && !gbs_seeds) {   // an explicit --gbs that maps nothing is a hard failure, never a silent one
            fprintf(stderr, "deepeel: --gbs: no seed could be mapped to this ROM (threshold %.0f%%)\n", thr * 100);
            dp_cart_free(cart);
            return 1;
        }
    }
    // Music-rip index (DPMDX002, tools/music_index.py export --bin): the shared CLI
    // glue (src/core/music_index_seeds.c) loads the index, runs the three lookup
    // tiers over this dump (CRC32 exact, normalized header title, game identity)
    // and reports the match.  Every seed found joins the GBS seeds with the rip's
    // calling convention (init carries song 0 in A on the rip's stack); the log
    // names the tier that carried each one (identity transfer across a region is
    // weaker evidence than an exact dump CRC — never mixed up silently).
    if (midx_given || access("output/music_index/music_index.bin", F_OK) == 0) {
        static const char *const plats_gb[] = {"GB"};
        char norm[32];
        dp_midx_norm_title(cart->rom + 0x134, norm, sizeof norm);
        DpMusicIndex mix;
        DpMusicSeed *mseeds = NULL; size_t mn = 0; DpMusicSeedStats mst;
        const char *const rip_dirs[] = { "output/gbs", "output/joshw" };
        const char *rb = strrchr(path, '/'); rb = rb ? rb + 1 : path;
        DpMusicSeedCtx mctx = { rip_dirs, 2, cart->rom, cart->rom_size, rb };
        int mrc = dp_music_seeds_cli(midx_given ? (midx_path ? midx_path : "") : NULL,
                                     plats_gb, 1, cart->rom, cart->rom_size, cart->rom_size,
                                     norm, &mseeds, &mn, &mst, &mix, &mctx);
        if ((mrc == DP_MUSIC_SEEDS_LOAD_ERR && midx_given) || mrc == DP_MUSIC_SEEDS_ALLOC) {
            free(gbs_pcs); free(gbs_bks); free(gbs_regs); dp_cart_free(cart);
            return 1;
        }
        if (mrc >= 0) {
            int alloc_fail = 0;
            for (size_t i = 0; i < mn && !alloc_fail; i++) {   // dedup against the GBS seeds already queued
                int dup = 0;
                for (int j = 0; j < gbs_seeds; j++) if (gbs_bks[j] == mseeds[i].bank && gbs_pcs[j] == mseeds[i].pc) dup = 1;
                if (!dup && seed_push(&gbs_pcs, &gbs_bks, &gbs_regs, &gbs_seeds,
                                      mseeds[i].pc, mseeds[i].bank, mseeds[i].a, mseeds[i].sp)) alloc_fail = 1;
            }
            for (size_t i = 0; i < mn; i++) {   // per-entry provenance: the tier that carried it and the rip
                if (mseeds[i].entry == 0xFFFFFFFFu) continue;            // runtime-verified: no index row behind it
                if (i && mseeds[i].entry == mseeds[i - 1].entry) continue;
                DpMusicRec r; dp_midx_rec(&mix, mseeds[i].entry, &r);
                char sinit[20] = "-", splay[20] = "-";
                if (r.init_rom != 0xFFFFFFFFu) rom_str(sinit, sizeof sinit, r.init_rom);
                if (r.play_rom != 0xFFFFFFFFu) rom_str(splay, sizeof splay, r.play_rom);
                uint8_t t = mseeds[i].tier;
                printf("music-index: [%s] %s/%s: init %s, play %s\n",
                       dp_music_seeds_tier_name(t), r.source, r.format, sinit, splay);
            }
            if (alloc_fail) {
                fprintf(stderr, "deepeel: cannot allocate music-index seeds\n");
                free(gbs_pcs); free(gbs_bks); free(gbs_regs);
                free(mseeds);
                dp_midx_free(&mix);
                dp_cart_free(cart);
                return 1;
            }
        }
        free(mseeds);
        if (mrc >= 0) dp_midx_free(&mix);
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
    dp_static_scan(&res);
    dp_disc_add(&res, "static-scan", 0); // method shares: the static baseline
    int vi = dp_mach_pick_variant(cart->rom);
    if (vi < 0) {
        fprintf(stderr, "deepeel: no machine variant compiled for MBC %s\n", dp_mbc_name(cart->mbc));
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
    {
        gkcfg.max_total = cfg.max_steps_total;
        gkcfg.max_path_steps = getenv("DEEPEEL_GB_PATH_STEPS") ? strtoull(getenv("DEEPEEL_GB_PATH_STEPS"), NULL, 0) : 1000;   // short paths, many entries: Zelda 39 M steps 4x the natural bytes of 100000-step paths
        gkcfg.jobs = cfg.jobs > 1 ? (unsigned)cfg.jobs : 1;
        for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--jobs") && !strcmp(argv[i + 1], "auto")) { gkcfg.jobs = (unsigned)auto_jobs(); gkcfg.adaptive = 1; }
        gkcfg.hooks = dp_machine_gb_hooks();
        gkcfg.rom_image = cart->rom; gkcfg.rom_image_size = cart->rom_size;
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
            uint32_t *gseed_pc = NULL; uint16_t *gseed_bk = NULL; DpSeedRegs *gseed_regs = NULL;
            uint64_t gseed_refused = 0;
            size_t gseed_cap = cfg.no_pointer_scan ? 0 :
                (getenv("DEEPEEL_GB_POINTER_SEEDS") ? (size_t)atol(getenv("DEEPEEL_GB_POINTER_SEEDS")) : 40000);
            if (gkcfg.unbounded && gseed_cap) gseed_cap = cart->rom_size;
            if (gseed_cap) {   // pointer-table entry discovery (core_gbseeds.c), to match the GB explorer's speculative seeds
                gkcfg.n_seeds = dp_core_gb_pointer_seeds(cart->rom, cart->rom_size, gseed_cap, &gseed_pc, &gseed_bk, &gseed_refused);
                if (gkcfg.n_seeds == SIZE_MAX) {
                    free(gmasks); dp_tas_free(&gt); km->close(km);
                    dp_results_free(&res); dp_cart_free(cart);
                    fprintf(stderr, "deepeel: cannot allocate pointer seed scan\n");
                    return 1;
                }
                gkcfg.seeds = gseed_pc; gkcfg.seed_banks = gseed_bk;
            }
            if (entries) {
                size_t total = gkcfg.n_seeds + (size_t)entries;
                uint32_t *pcs = malloc(total * sizeof *pcs);
                uint16_t *banks = malloc(total * sizeof *banks);
                DpSeedRegs *regs = malloc(total * sizeof *regs);
                if (!pcs || !banks || !regs) {
                    free(pcs); free(banks); free(regs); free(gseed_pc); free(gseed_bk);
                    free(gmasks); dp_tas_free(&gt); km->close(km);
                    dp_results_free(&res); dp_cart_free(cart);
                    fprintf(stderr, "deepeel: cannot allocate explicit entry seeds\n");
                    return 1;
                }
                if (gkcfg.n_seeds) {
                    memcpy(pcs, gseed_pc, gkcfg.n_seeds * sizeof *pcs);
                    memcpy(banks, gseed_bk, gkcfg.n_seeds * sizeof *banks);
                }
                for (size_t i = 0; i < total; i++) { regs[i].a = 0xFF; regs[i].sp = 0xFFFF; }   // explicit entries carry no registers
                for (int i = 0; i < entries; i++) {
                    pcs[gkcfg.n_seeds + (size_t)i] = ea[i];
                    banks[gkcfg.n_seeds + (size_t)i] = ea[i] >= 0x4000 ?
                        (eb[i] || cart->mbc == DP_MBC5 ? eb[i] : 1) : 0;
                }
                free(gseed_pc); free(gseed_bk);
                gseed_pc = pcs; gseed_bk = banks; gseed_regs = regs;
                gkcfg.n_seeds = total; gkcfg.seeds = pcs; gkcfg.seed_banks = banks; gkcfg.seed_regs = regs;
            }
            if (gbs_seeds) {   // confirmed GBS entry points join the speculative seeds
                if (core_seed_append(&gseed_pc, &gseed_bk, &gseed_regs, &gkcfg.n_seeds, gbs_pcs, gbs_bks, gbs_regs, (size_t)gbs_seeds)) {
                    free(gbs_pcs); free(gbs_bks); free(gbs_regs);
                    free(gseed_pc); free(gseed_bk); free(gseed_regs);
                    free(gmasks); dp_tas_free(&gt); km->close(km);
                    dp_results_free(&res); dp_cart_free(cart);
                    fprintf(stderr, "deepeel: cannot allocate GBS seeds\n");
                    return 1;
                }
                gkcfg.seeds = gseed_pc; gkcfg.seed_banks = gseed_bk; gkcfg.seed_regs = gseed_regs;
            }
            free(gbs_pcs); free(gbs_bks); free(gbs_regs);   // copied into the kernel's seed arrays above
            gbs_pcs = NULL; gbs_bks = NULL; gbs_regs = NULL; gbs_seeds = 0;
            DpCoreResult kr;
            if (watch) { gkcfg.watch_every = (unsigned)(60 / watch); dp_fbwatch_start("deepeel gb watch"); }   // generic screen watch (DpMachine.render)
            rc = dp_core_explore_cli(km, cart->rom_size, &gkcfg, &kr, summary ? summary : text) ? 0 : 1;
            if (watch) dp_fbwatch_stop();
            if (gb_wdump && dp_fb_dump_ppm(gb_wdump)) fprintf(stderr, "deepeel: no frame to dump\n");
            kr.seed_refused_cap = gseed_refused;
            free(gseed_pc); free(gseed_bk); free(gseed_regs);
            if (!rc) { dp_core_report(stdout, "gb", &kr); core_outputs(argc, argv, "gb", path, km, &kr); }
            dp_core_result_free(&kr);
            free(gmasks); dp_tas_free(&gt);
            km->close(km);
        } else fprintf(stderr, "deepeel: --core: cannot wrap the machine\n");
        dp_results_free(&res);
        dp_cart_free(cart);
        return rc;
    }
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
    // any platform's digit map ("gb map v1", "md map v1", "nes map v1", ...):
    // one class digit per ROM byte, rewritten as whole-ROM runs "0 start end
    // class" for the z80 splitter below to window
    if (!cv_in) {
        size_t hl = 0;
        while (hl < (size_t)ln && text[hl] != '\n') hl++;
        if (hl < 9 || hl > 12 || strncmp(text + hl - 7, " map v1", 7) || hl + 1 >= (size_t)ln) goto not_a_digit_map;
        char *d = text + hl + 1;
        char *dend = memchr(d, '\n', (size_t)(text + ln - d));
        if (!dend) dend = text + ln;
        DpBuf runs;
        dp_buf_init(&runs);
        for (char *q = d; q < dend;) {
            char *r2 = q;
            while (r2 < dend && *r2 == *q) r2++;
            int k = *q - '0';
            if (k < 0 || k >= DP_MAP_COUNT) k = DP_MAP_UNKNOWN;   // a future class digit never garbles the runs
            dp_buf_fmt(&runs, "0 %x %x %s\n", (unsigned)(q - d), (unsigned)(r2 - d) - 1, dp_map_class_name(k));
            q = r2;
        }
        free(text);
        text = (char *)runs.p;
        ln = (long)runs.len;
        z80_map = 1;
    }
    not_a_digit_map:;
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
                        "[--loop-cap N] [--fork-cap N] [--no-irq-fork] [--entry bank:addr]... [--seed-banks] [--music-index [FILE]] [--text FILE] [--map FILE] [--evidence FILE] [--cov FILE] [--romdata FILE] [--rtcode FILE]\n");
        return 2;
    }
    const char *path = NULL, *mapper_name = "auto", *nes_heat = NULL, *midx_file = NULL;
    DpNesOptions opt = dp_nes_options();   // defer_tiers 0: cuts stay final; --defer-tiers N opts in (sega's CLI defaults to 3, NES measured neutral on DuckTales)
    opt.demo_frames = DP_DEFAULT_ATTRACT_FRAMES;
    DpCliCommon cc = { .jobs = 1 };
    int core_flag = 1;   // the kernel is the only NES engine
    int use_core = core_flag; DpCoreConfig kcfg; dp_core_config_default(&kcfg);
    kcfg.no_sessions = 0;   // A26: held-pad sessions spend their own budget (held_steps), so they no longer starve the natural path   // measured on SMB at 2 M steps: sessions halve the natural path's budget (4708 -> 2063 bytes); --pad-sessions N enables them
    kcfg.visits_per_pc = 50000;   // a frame wait is thousands of iterations (SMB: natural 15 -> 4708 bytes, old engine 4716)
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (0) {}
        else if (!strcmp(a, "--core")) {}   // accepted: the kernel is the only engine
        else if (core_flag && dp_core_parse_option(&kcfg, argc, argv, i)) { i += dp_core_parse_option(&kcfg, argc, argv, i) - 1; }   // only with --core: the old engine owns --attract-frames etc.
        DP_CLI_COMMON(opt, cc)
        DP_CLI_PLAYED(opt)
        else if (!strcmp(a, "--heat") && i + 1 < argc) nes_heat = argv[++i];   // --summary/--cov/--romdata come from DP_CLI_COMMON
        else if (!strcmp(a, "--tas") && i + 1 < argc) {
            if (opt.tas_count < 8) snprintf(opt.tas_paths[opt.tas_count++], 512, "%s", argv[++i]);
            else { fprintf(stderr, "deepeel: at most 8 TAS movies\n"); return 2; }
        }
        else if (!strcmp(a, "--mapper") && i + 1 < argc) mapper_name = argv[++i];
        else if (!strcmp(a, "--queue") && i + 1 < argc) opt.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue-max") && i + 1 < argc) opt.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--spool") && i + 1 < argc) opt.lot_path = argv[++i];
        else if (!strcmp(a, "--seed-banks")) opt.seed_banks = 1;
        else if (!strcmp(a, "--music-index")) { if (i + 1 < argc && argv[i + 1][0] != '-') midx_file = argv[++i]; else midx_file = ""; }
        else if (!strcmp(a, "--seed-vectors")) opt.seed_vectors = 1;
        else if (!strcmp(a, "--no-p1-fork")) opt.no_pad_probe = 1;
        else if (!strcmp(a, "--no-event-probes")) opt.event_probes = 0;
        else if (!strcmp(a, "--entry") && i + 1 < argc && opt.entry_count < 32) {
            uint16_t eb, ea;
            if (!parse_entry(argv[++i], &eb, &ea) || eb > 255) { fprintf(stderr, "deepeel: bad --entry '%s' (bank:addr)\n", argv[i]); return 2; }
            opt.entry_bank[opt.entry_count] = eb; opt.entry_addr[opt.entry_count++] = ea;
        }
        else if (skip_mapwin(argc, argv, &i)) {}
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
    DpMusicSeed *mseeds = NULL; size_t mn = 0; DpMusicKernelSeeds mks = {0};
    {   // music-rip index: the shared glue reports the match for this dump and
        // runtime-verifies the local rips; the entry points seed both engines
        // (kernel: seeds + banks + regs; legacy: confirmed entries with the
        // rip's calling convention) — never silent
        static const char *const plats[] = {"NES"};
        const char *const rip_dirs[] = { "output/gbs", "output/joshw" };
        const char *rb = strrchr(path, '/'); rb = rb ? rb + 1 : path;
        const uint8_t *img = data; size_t img_size = size;   // the verification reads the image
        if (size > 16 && !memcmp(data, "NES\x1a", 4)) {       // iNES: strip header (+ trainer)
            img_size = data[4] * 0x4000u;
            img = data + 16 + ((data[6] & 0x04) ? 512 : 0);
        }
        DpMusicSeedCtx mctx = { rip_dirs, 2, img, img_size, rb };
        DpMusicSeedStats mst;
        int mrc = dp_music_seeds_cli(midx_file, plats, 1, data, size, img_size, NULL, &mseeds, &mn, &mst, NULL, &mctx);
        if ((mrc == DP_MUSIC_SEEDS_LOAD_ERR && midx_file) || mrc == DP_MUSIC_SEEDS_ALLOC) { free(data); return 1; }
        if (mn)
            fprintf(stderr, "deepeel: nes: %zu music-rip entry point(s) matched — the explorer seeds them (init carries the song in A on the rip's stack)\n", mn);
    }
    opt.music_seeds = mseeds;
    opt.music_seed_count = (unsigned)mn;
    if (use_core) {
        char err[128] = "";
        DpMachine *km = dp_machine_nes(data, size, mapper, err, sizeof err);
        if (!km) { fprintf(stderr, "deepeel: --core: %s\n", err); free(data); return 1; }
        if (mn) {   // each seed's pc is the window this dump already shows (both
            // 8 KiB halves), else re-banked to the $8000 window through bank_view
            if (dp_music_seeds_kernel(&mks, mseeds, mn, 0x8000, km)) { km->close(km); free(mseeds); free(data); return 1; }
            if (mks.n) { kcfg.seeds = mks.pcs; kcfg.n_seeds = mks.n; kcfg.seed_banks = mks.banks; kcfg.seed_regs = mks.regs; }
            if (mks.skipped) fprintf(stderr, "deepeel: nes: %zu music-rip seed(s) outside every PRG window of this dump (another banking)\n", mks.skipped);
        }
        kcfg.jobs = (unsigned)nes_jobs;
        uint32_t epc[32]; uint16_t ebk[32];
        for (unsigned e = 0; e < opt.entry_count; e++) { epc[e] = opt.entry_addr[e]; ebk[e] = opt.entry_bank[e]; }
        DpSeedSet kseeds;
        if (seedset_build(&kcfg, &mks, epc, ebk, opt.entry_count, (opt.seed_banks && km->bank_view) ? 64 : 0, 0x8000, &kseeds)) { km->close(km); free(mseeds); free(data); return 1; }
        if (opt.max_total) kcfg.max_total = opt.max_total;
        if (opt.max_steps) kcfg.max_path_steps = opt.max_steps;
        if (opt.fork_cap) kcfg.fork_site_cap = opt.fork_cap;
        if (opt.queue_max) { kcfg.queue_max = opt.queue_max; kcfg.queue_auto = 0; }
        kcfg.hooks = dp_machine_nes_hooks();
        DpMovie kmov; memset(&kmov, 0, sizeof kmov);
        if (opt.tas_count > 0) {   // first movie replays in the kernel (m->frame counts the frames)
            char terr[160] = "";
            if (dp_movie_load("nes", opt.tas_paths[0], &kmov, terr, sizeof terr)) { kcfg.tas_masks32 = kmov.masks; kcfg.tas_frames = kmov.frames; }
            else fprintf(stderr, "deepeel: --tas: %s\n", terr);
        }
        DpCoreResult kr;
        size_t prg = dp_machine_nes_prg_size(km);
        kcfg.rom_image = dp_machine_nes_prg(km); kcfg.rom_image_size = prg;   // B8/C3: the PRG image for the speculative gaps
        kcfg.watch_every = opt.watch_every;   // --watch: the kernel publishes the replayed movie's screen (DpMachine.render)
        if (nes_watch) dp_fbwatch_start("deepeel nes watch");
        int kok = dp_core_explore_cli(km, prg, &kcfg, &kr, argv_opt(argc, argv, "--summary") ? argv_opt(argc, argv, "--summary") : argv_opt(argc, argv, "--text"));
        if (nes_watch) dp_fbwatch_stop();
        if (nes_dump && dp_fb_dump_ppm(nes_dump)) fprintf(stderr, "deepeel: no frame to dump\n");
        if (kok) { dp_core_report(stdout, "nes", &kr); core_outputs(argc, argv, "nes", path, km, &kr); }
        dp_core_result_free(&kr);
        dp_movie_free(&kmov);
        km->close(km); seedset_free(&kseeds);
        dp_music_seeds_kernel_free(&mks); free(mseeds);
        free(data);
        return kok ? 0 : 1;
    }
    (void)nes_heat; (void)nes_jobs; (void)text; (void)map; (void)evidence; (void)rtcode_path;
    free(data);
    fprintf(stderr, "deepeel: nes: the kernel is the only engine (the historical explorer is retired)\n");
    return 2;
}

// PC Engine branch: the HuC6280 forking explorer over m0boy's machine.
static int cmd_pce(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[2], "explore")) {
        fprintf(stderr, "deepeel pce explore ROM [--tas FILE]... [--play FRAMES] [--sessions N] [--seed N] "
                        "[--max-total N] [--max-steps N] [--queue N] [--loop-cap N] [--fork-cap N] "
                        "[--novelty STEPS] [--novelty-frames N] [--novelty-archive N] [--novelty-cells] [--pad-fork-cap N] [--irq-fork-cap N] [--no-irq-fork] [--no-static] [--no-infer] [--multitap] [--jobs 2] "
                        "[--text FILE] [--map FILE] [--png FILE] [--evidence FILE] [--entry ADDR]... [--seed-banks] [--music-index [FILE]] [--ctx-cap N] [--ctx-live] [--summary FILE]\n");
        return 2;
    }
    const char *path = NULL, *png = NULL, *midx_file = NULL;
    DpPceOptions opt = dp_pce_options();
    opt.demo_frames = DP_DEFAULT_ATTRACT_FRAMES;
    DpCliCommon cc = { .jobs = 1 };
    int core_flag = 1;   // the kernel is the only PCE engine
    int multitap = 0, use_core = core_flag; DpCoreConfig kcfg; dp_core_config_default(&kcfg);
    kcfg.visits_per_pc = 50000;   // Raiden at 2 M steps: natural 116 bytes at 64 visits, 2220 at 50000 (frame waits are long loops)
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (0) {}
        else if (!strcmp(a, "--core")) {}   // accepted: the kernel is the only engine
        else if (core_flag && dp_core_parse_option(&kcfg, argc, argv, i)) { i += dp_core_parse_option(&kcfg, argc, argv, i) - 1; }   // only with --core: the old engine owns --attract-frames etc.
        DP_CLI_COMMON(opt, cc)
        DP_CLI_PLAYED(opt)
        else if (!strcmp(a, "--queue-max") && i + 1 < argc) opt.queue_max = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-vectors")) opt.seed_vectors = 1;
        else if (!strcmp(a, "--seed-banks")) opt.seed_banks = 1;
        else if (!strcmp(a, "--music-index")) { if (i + 1 < argc && argv[i + 1][0] != '-') midx_file = argv[++i]; else midx_file = ""; }
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
        else if (!strcmp(a, "--irq-fork-cap") && i + 1 < argc) opt.irq_fork_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--no-static")) opt.static_closure = 0;
        else if (!strcmp(a, "--no-infer")) opt.infer = 0;
        else if (!strcmp(a, "--multitap")) multitap = 1;
        else if (skip_mapwin(argc, argv, &i)) {}
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
    DpMusicSeed *mseeds = NULL; size_t mn = 0; DpMusicKernelSeeds mks = {0};
    {   // music-rip index: the shared glue reports the match for this dump and
        // runtime-verifies the local rips; the entry points seed both engines
        // (kernel: seeds + banks + regs; legacy: confirmed entries with the
        // rip's calling convention) — never silent
        static const char *const plats[] = {"PCE"};
        const char *const rip_dirs[] = { "output/gbs", "output/joshw" };
        const char *rb = strrchr(path, '/'); rb = rb ? rb + 1 : path;
        const uint8_t *img = data; size_t img_size = size;   // the verification reads the image
        if ((size & 0x1FFF) == 512) { img = data + 512; img_size = size - 512; }   // copier header
        DpMusicSeedCtx mctx = { rip_dirs, 2, img, img_size, rb };
        DpMusicSeedStats mst;
        int mrc = dp_music_seeds_cli(midx_file, plats, 1, data, size, img_size, NULL, &mseeds, &mn, &mst, NULL, &mctx);
        if ((mrc == DP_MUSIC_SEEDS_LOAD_ERR && midx_file) || mrc == DP_MUSIC_SEEDS_ALLOC) { free(data); return 1; }
        if (mn)
            fprintf(stderr, "deepeel: pce: %zu music-rip entry point(s) matched — the explorer seeds them (init carries the song in A on the rip's stack)\n", mn);
    }
    opt.music_seeds = mseeds;
    opt.music_seed_count = (unsigned)mn;
    if (use_core) {
        if (!dp_pce_init(data, size)) { fprintf(stderr, "deepeel: --core: not a PCE image\n"); free(data); return 1; }
        DpMachine *km = dp_machine_pce();
        if (km) dp_machine_pce_source(km, data, size);
        size_t hdr = size % 0x2000 == 512 ? 512 : 0;   // B8: the whole HuCard, pages scanned at their journaled windows
        kcfg.rom_image = data + hdr; kcfg.rom_image_size = size - hdr;
        kcfg.jobs = (unsigned)pce_jobs;
        uint32_t pepc[16]; for (int e = 0; e < opt.entry_count; e++) pepc[e] = opt.entries[e];
        if (!km) { free(data); return 1; }
        if (mn) {   // each seed's pc is the window this dump's MPR already shows, else re-banked (bank_view)
            if (dp_music_seeds_kernel(&mks, mseeds, mn, 0x4000, km)) { km->close(km); free(mseeds); free(data); return 1; }
            if (mks.skipped) fprintf(stderr, "deepeel: pce: %zu music-rip seed(s) outside every MPR window of this dump (another banking)\n", mks.skipped);
        }
        DpSeedSet kseeds;
        // P1: --seed-banks was parsed but never passed on; every 16 KiB block of the image is
        // seeded at the $4000-$7FFF window (the one dp_pce_bank16 programs, P16)
        unsigned pbanks = (opt.seed_banks && km->bank_view) ? (unsigned)((size - hdr) / 0x4000) : 0;
        if (seedset_build(&kcfg, &mks, pepc, NULL, (size_t)opt.entry_count, pbanks, 0x4000, &kseeds)) { free(data); return 1; }
        kcfg.hooks = dp_machine_pce_hooks();
        if (opt.max_total) kcfg.max_total = opt.max_total;
        if (opt.max_steps) kcfg.max_path_steps = opt.max_steps;
        if (opt.fork_cap) kcfg.fork_site_cap = opt.fork_cap;
        if (opt.queue_max) { kcfg.queue_max = opt.queue_max; kcfg.queue_auto = 0; }
        DpMovie kmov; memset(&kmov, 0, sizeof kmov);
        if (opt.tas_count > 0) {
            char terr[160] = "";
            if (dp_movie_load("pce", opt.tas_paths[0], &kmov, terr, sizeof terr)) { kcfg.tas_masks32 = kmov.masks; kcfg.tas_frames = kmov.frames; }
            else fprintf(stderr, "deepeel: --tas: %s\n", terr);
        }
        DpCoreResult kr;
        kcfg.watch_every = opt.watch_every;
        if (pce_watch) dp_fbwatch_start("deepeel pce watch");
        int kok = dp_core_explore_cli(km, dp_pce_rom_size(), &kcfg, &kr, argv_opt(argc, argv, "--summary") ? argv_opt(argc, argv, "--summary") : argv_opt(argc, argv, "--text"));
        dp_movie_free(&kmov);
        if (pce_watch) dp_fbwatch_stop();
        if (pce_dump && dp_fb_dump_ppm(pce_dump)) fprintf(stderr, "deepeel: no frame to dump\n");
        if (kok) { dp_core_report(stdout, "pce", &kr); core_outputs(argc, argv, "pce", path, km, &kr); }
        dp_core_result_free(&kr);
        km->close(km); seedset_free(&kseeds);
        dp_music_seeds_kernel_free(&mks); free(mseeds);
        free(data);
        return kok ? 0 : 1;
    }
    free(data);
    fprintf(stderr, "deepeel: pce: the kernel is the only engine (the historical explorer is retired)\n");
    return 2;
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
    uint16_t bank;
    uint16_t addr;
    if (!parse_entry(argv[3], &bank, &addr)) {
        fprintf(stderr, "deepeel: bad address '%s' (bank:addr)\n", argv[3]);
        dp_cart_free(cart);
        return 2;
    }
    if ((size_t)bank * 0x4000 >= cart->rom_size) {
        fprintf(stderr, "deepeel: bank %u outside ROM\n", (unsigned)bank);
        dp_cart_free(cart);
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

static void md_regs_print(void) {
    printf("megadrive register registry (%d entries):\n", dp_md_reg_count);
    for (int i = 0; i < dp_md_reg_count; i++)
        printf("  %06x-%06x  %-12s %s\n", dp_md_regs[i].lo, dp_md_regs[i].hi, dp_md_regs[i].name,
               dp_md_regs[i].category);
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
    } else if (!strcmp(argv[2], "explore")) {
        DpCoreConfig kcfg; dp_core_config_default(&kcfg);
        kcfg.max_path_steps = 50000000;   // boot clears RAM/VRAM with millions of dbf iterations before the first interrupt is unmasked
        kcfg.hooks = dp_machine_md_hooks();
        kcfg.visits_per_pc = 2000000;   // the header checksum loop runs ROM-size/2 iterations (Sonic 1: 262144): the old 262144 cap cut it one short; was 262144 at 2 M steps: natural 32 bytes at 64 visits, 126 at 262144 (old engine ~134)
        const char *md_core_tas = NULL, *md_kdump = NULL; int md_kwatch = 0; uint32_t md_entry[16]; size_t md_ne = 0;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--tas") && i + 1 < argc) { md_core_tas = argv[++i]; continue; }
            if (!strcmp(argv[i], "--entry") && i + 1 < argc) {   // an extra root, a 24-bit address
                if (md_ne >= 16) { fprintf(stderr, "deepeel: at most 16 --entry\n"); rc = 2; break; }
                md_entry[md_ne++] = (uint32_t)strtoul(argv[++i], NULL, 0); continue;
            }
            int used = dp_core_parse_option(&kcfg, argc, argv, i);
            if (used) { i += used - 1; continue; }
            if (!strcmp(argv[i], "--watch-dump") && i + 1 < argc) { md_kdump = argv[++i]; if (!kcfg.watch_every) kcfg.watch_every = 2; continue; }
            if (!strcmp(argv[i], "--watch")) {   // the kernel publishes the replayed movie's screen at 1..60 img/s (default 30)
                int fps = 30; if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') fps = atoi(argv[++i]);
                if (fps < 1) fps = 1; if (fps > 60) fps = 60;
                md_kwatch = 1; kcfg.watch_every = (unsigned)(60 / fps);
            }
            else if (!strcmp(argv[i], "--max-total") && i + 1 < argc) kcfg.max_total = strtoull(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--fork-cap") && i + 1 < argc) kcfg.fork_site_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--progress") && i + 1 < argc) kcfg.progress_secs = (unsigned)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) { ++i; if (!strcmp(argv[i], "auto")) { kcfg.jobs = (unsigned)auto_jobs(); kcfg.adaptive = 1; } else kcfg.jobs = (unsigned)strtoul(argv[i], NULL, 0); }
        }
        DpMdMach *mm = dp_mdmach_open(c.rom, c.size);
        if (mm) dp_mdmach_reset(mm);   // power-on state: the 68000 starts from the vectors (without it the path faults)
        DpMachine *km = mm ? dp_machine_md(mm) : NULL;
        if (km) dp_machine_md_source(km, c.rom, c.size);
        kcfg.rom_image = c.rom; kcfg.rom_image_size = c.size;   // B8 on the linear ROM
        DpMusicKernelSeeds mks = {0}; DpSeedSet kseeds = {0};
        if (!rc && km && seedset_build(&kcfg, &mks, md_entry, NULL, md_ne, 0, 0, &kseeds)) { km->close(km); km = NULL; }
        if (rc) { if (km) km->close(km); }
        else if (!km) { fprintf(stderr, "deepeel: md: cannot open the ROM\n"); rc = 1; }
        else {
            DpMovie kmov; memset(&kmov, 0, sizeof kmov);
            if (md_core_tas) {   // GMV / BK2 pads -> kernel mask
                char terr[160] = "";
                if (dp_movie_load("md", md_core_tas, &kmov, terr, sizeof terr)) { kcfg.tas_masks32 = kmov.masks; kcfg.tas_frames = kmov.frames; }
                else fprintf(stderr, "deepeel: --tas: %s\n", terr);
            }
            DpCoreResult kr;
            if (md_kwatch) dp_fbwatch_start("deepeel md watch");
            rc = dp_core_explore_cli(km, c.size, &kcfg, &kr, argv_opt(argc, argv, "--summary") ? argv_opt(argc, argv, "--summary") : argv_opt(argc, argv, "--text")) ? 0 : 1;
            if (md_kwatch) dp_fbwatch_stop();
            if (md_kdump && dp_fb_dump_ppm(md_kdump)) fprintf(stderr, "deepeel: no frame to dump\n");
            if (!rc) { dp_core_report(stdout, "md", &kr); core_outputs(argc, argv, "md", NULL, km, &kr); }
            dp_core_result_free(&kr);
            dp_movie_free(&kmov);
            km->close(km);
            seedset_free(&kseeds);
        }
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

int dp_tasfetch_command(int argc, char **argv);

static void kmapwin_cb(void *u, const DpCoreResult *r) { dp_kmapwin_start(r, "deepeel ROM map", (int)(intptr_t)u); }
// ---- `deepeel ROM [options]`: the zero-argument entry -------------------------------------------------
// The platform comes from the file (extension, then header), the outputs go to a dedicated directory
// (<rom stem>.deepeel/ unless --out DIR), a saved analysis there is resumed on its own, and the
// worker count and queue size follow the free RAM.  Any explicit option still wins (it comes last).
typedef enum { AR_NONE, AR_GB, AR_NES, AR_PCE, AR_MD, AR_SMS, AR_GG, AR_SG, AR_MSX } ArSystem;

static ArSystem ar_detect(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return AR_NONE;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) { fclose(f); return AR_NONE; }
    uint8_t h[0x8000 + 16]; size_t n = fread(h, 1, sizeof h, f); fclose(f);
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    char ext[8] = ""; if (dot && (!slash || dot > slash) && strlen(dot) < sizeof ext) for (size_t i = 0; dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    if (n >= 4 && !memcmp(h, "NES\x1a", 4)) return AR_NES;
    if (!strcmp(ext, ".nes")) return AR_NES;
    if (!strcmp(ext, ".gb") || !strcmp(ext, ".gbc")) return AR_GB;
    if (!strcmp(ext, ".pce")) return AR_PCE;
    if (!strcmp(ext, ".gg")) return AR_GG;
    if (!strcmp(ext, ".sms")) return AR_SMS;
    if (!strcmp(ext, ".sg") || !strcmp(ext, ".sc")) return AR_SG;
    if (!strcmp(ext, ".mx1") || !strcmp(ext, ".mx2")) return AR_MSX;
    if (!strcmp(ext, ".md") || !strcmp(ext, ".gen") || !strcmp(ext, ".smd")) return AR_MD;
    if (n >= 0x108 && !memcmp(h + 0x100, "SEGA", 4)) return AR_MD;                       // Mega Drive header
    if (n >= 0x108 && !memcmp(h + 0x104, "\xCE\xED\x66\x66", 4)) return AR_GB;            // Nintendo logo
    static const size_t tmr[3] = { 0x7FF0, 0x3FF0, 0x1FF0 };                              // Master System / Game Gear "TMR SEGA"
    for (int i = 0; i < 3; i++)
        if (n >= tmr[i] + 16 && !memcmp(h + tmr[i], "TMR SEGA", 8)) return (h[tmr[i] + 15] >> 4) >= 5 ? AR_GG : AR_SMS;
    if (n >= 2 && h[0] == 'A' && h[1] == 'B') return AR_MSX;                              // MSX cartridge header
    return AR_NONE;
}

static int ar_has(int argc, char **argv, const char *opt) {
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], opt)) return 1;
    return 0;
}

static int ar_is_subcommand(const char *a) {
    static const char *const names[] = { "frontier", "tasfetch", "lr", "bh", "md", "nes", "pce", "z80", "explore", "mapx", "maprender", "tas", "desync", "map", "regs", "dis", NULL };
    for (int i = 0; names[i]; i++) if (!strcmp(a, names[i])) return 1;
    return 0;
}

static int auto_explore(int argc, char **argv, ArSystem sys) {
    static const char *const names[] = { "", "gb", "nes", "pce", "md", "sms", "gg", "sg", "msx" };
    const char *rom = argv[1];
    char stem[256] = "rom"; { const char *b = strrchr(rom, '/'); b = b ? b + 1 : rom; snprintf(stem, sizeof stem, "%s", b); char *d = strrchr(stem, '.'); if (d && d != stem) *d = 0; }
    char outdir[1024]; snprintf(outdir, sizeof outdir, "%s.deepeel", stem);
    const char *user[512]; int nu = 0;
    for (int i = 2; i < argc && nu < 500; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) { snprintf(outdir, sizeof outdir, "%s", argv[++i]); continue; }
        user[nu++] = argv[i];
    }
    for (char *p = outdir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(outdir, 0777); *p = '/'; }
    if (mkdir(outdir, 0777) != 0 && errno != EEXIST) { fprintf(stderr, "deepeel: cannot create %s: %s\n", outdir, strerror(errno)); return 1; }
    static char sdir[1100], fmap[1100], ftxt[1100], fsum[1100], fcov[1100], fjobs[16];
    snprintf(sdir, sizeof sdir, "%s/state", outdir);
    snprintf(fmap, sizeof fmap, "%s/%s.map.png", outdir, stem);
    snprintf(ftxt, sizeof ftxt, "%s/%s.txt", outdir, stem);
    snprintf(fsum, sizeof fsum, "%s/%s.json", outdir, stem);
    snprintf(fcov, sizeof fcov, "%s/%s.cov", outdir, stem);
    char probe[1200]; snprintf(probe, sizeof probe, "%s/identity.bin", sdir);
    int resume = access(probe, R_OK) == 0;
    int jobs = auto_jobs(); snprintf(fjobs, sizeof fjobs, "%d", jobs);
    const char *av[640]; int n = 0;
    av[n++] = "deepeel";
    if (sys == AR_GB) av[n++] = "explore";
    else if (sys == AR_SMS || sys == AR_GG || sys == AR_SG || sys == AR_MSX) { av[n++] = "z80"; av[n++] = "explore"; av[n++] = names[sys]; }
    else { av[n++] = names[sys]; av[n++] = "explore"; }
    av[n++] = rom;
    av[n++] = "--state-dir"; av[n++] = sdir;
    if (resume) av[n++] = "--resume";
    av[n++] = "--jobs"; av[n++] = fjobs;
    av[n++] = "--max-total"; av[n++] = "4611686018427387904";   // no step wall (a huge budget: the sega parser reads 0 as "default"): the run ends on exhaustion, SIGINT, or 15 minutes without a new ROM byte; the queue is bounded by the RAM (dp_mem_budget)
    av[n++] = "--stall-time"; av[n++] = "900";
    av[n++] = "--map"; av[n++] = fmap;
    av[n++] = "--text"; av[n++] = ftxt;
    av[n++] = "--summary"; av[n++] = fsum;
    av[n++] = "--cov"; av[n++] = fcov;
    if (sys == AR_GB) { static char frt[1100]; snprintf(frt, sizeof frt, "%s/%s.rtcode", outdir, stem); av[n++] = "--rtcode"; av[n++] = frt; }
    for (int i = 0; i < nu; i++) av[n++] = user[i];
    av[n] = NULL;
    fprintf(stderr, "deepeel: %s -> %s, outputs in %s/, %s, %d worker(s)\n", rom, names[sys], outdir, resume ? "resuming the saved analysis" : "fresh analysis", jobs);
    switch (sys) {
        case AR_GB:  return cmd_explore(n, (char **)av);
        case AR_NES: return cmd_nes(n, (char **)av);
        case AR_PCE: return cmd_pce(n, (char **)av);
        case AR_MD:  return cmd_md(n, (char **)av);
        default:     return dp_sega_explore_command(n, (char **)av);
    }
}

int deepeel_main(int argc, char **argv) {   // the library entry point; src/deepeel_cli.c is the executable's main
    if (argc < 2) return usage(), 2;
    for (int i = 2; i < argc; i++)   // --mapwin [SCALE]: the kernel's live ROM map, for every platform (the legacy GB explorer keeps its own)
        if (!strcmp(argv[i], "--mapwin")) {
            int sc = i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '8' ? atoi(argv[i + 1]) : 0;
            dp_core_live_default(kmapwin_cb, (void *)(intptr_t)sc);
            atexit(dp_kmapwin_stop);
            break;
        }
    if (!ar_is_subcommand(argv[1]) && argv[1][0] != '-') {   // `deepeel ROM`: explore is the default, the system comes from the file
        ArSystem sys = ar_detect(argv[1]);
        if (sys == AR_NONE) { fprintf(stderr, "deepeel: %s: not a readable ROM, or its system is not recognised (use `deepeel <system> explore ROM`)\n", argv[1]); return 2; }
        return auto_explore(argc, argv, sys);
    }
    if (!strcmp(argv[1], "frontier") && argc > 3 && !strcmp(argv[2], "stats")) {
        char kp[700]; snprintf(kp, sizeof kp, "%s/frontier.bin", argv[3]);   // kernel frontier (--core --state-dir)
        FILE *kf = fopen(kp, "rb");
        if (kf) {
            uint32_t kh[3];
            if (fread(kh, 4, 3, kf) == 3 && kh[0] == 0x31524644u) { printf("frontier: %u kernel states pending (%u bytes each)\n", kh[2], kh[1]); fclose(kf); return 0; }
            fclose(kf);
        }
        char p[700], line[400];
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
    if (!strcmp(argv[1], "tasfetch")) return dp_tasfetch_command(argc, argv);
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
