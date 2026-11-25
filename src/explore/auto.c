// auto.c: the autonomous coverage campaign.  `deepeel ROM [OVERFLOW_DIR]` is the whole interface: this
// supervisor fetches the TAS movies of the ROM (TASVideos API through the tasdb module), replays them in
// parallel longest first (each replay = a child process: `explore --tas M --replay-only`, with machine snapshots
// and a stagnation detector that logs desynced/incompatible movies), then explores from every snapshot (random
// x10 + both sides of every conditional, coarse-to-fine order), merges the coverage bytes of every child and
// writes the final PNG.  CPU workers, RAM and swap, disk and overflow are managed here, with no script.
// Only stagnation stops a search (kernel side: --tas-stall / --random-stall / --stall-time; here: a run of
// explorations that add no ROM byte to the union).
#define _GNU_SOURCE
#include "auto.h"
#include "core.h"
#include "tas.h"
#include "../core/shquote.h"
#include <dirent.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "campaign.h"
#include "../report/report.h"
#include "../gui/livewin.h"
#define CHILD_RSS_CAP CAMP_CHILD_RSS_CAP
#define HUGE_STEPS "4611686018427387904"
#define LOCAL_MIN_FREE CAMP_LOCAL_MIN_FREE
#define SHED_FREE CAMP_SHED_FREE
#define HARD_MIN_FREE CAMP_HARD_MIN_FREE

typedef struct { char path[1100]; unsigned frames; unsigned long size; char emu[48]; uint64_t input; } Movie;   // emu: the source emulator, from the TASVideos API
typedef struct {
    int kind;                      // K_MOVIE, K_FORK, K_BOOT, K_RANDOM
    int movie;                     // the group: index in the movies array, nm for the power-on group
    unsigned frame, first;         // snapshot frame; K_RANDOM: seed rank of the first run
    int slot, paused, collected;   // K_RANDOM: its quarter of the group's tile; K_FORK: stopped under IO pressure; films: snapshots all queued
    char snap[1100];               // kind 2: the snapshot file
    unsigned key;                  // kind 2: coarse-to-fine rank
    pid_t pid; time_t t0; size_t rss_peak;
    char dir[1100], live[1150], log[1150], rep[1150], shot[1150], lfr[1150];
    int end;                       // kind 2: the snapshot closing its movie (post-movie attract)
    int shrink;                    // kind 2: times it was killed over the RSS cap and re-queued with half the queue
    uint64_t fps_frame; double fps_t; unsigned fps;   // live window: emulated frames per second of this child
    int done; time_t over_t;       // when the RSS ceiling was first hit (SIGINT, then SIGKILL after 15 s)
    char ph[12]; unsigned rnd_done, rnd_total; size_t queued;   // from <lfr>.st: current phase, random runs, states queued in the child
    int stalled;                   // kind 1: the replay ended on stagnation
} Task;
enum { K_MOVIE = 1, K_FORK = 2, K_BOOT = 3, K_RANDOM = 4 };
static int is_film(const Task *t) { return t->kind == K_MOVIE || t->kind == K_BOOT; }
static int running_task(const Task *t) { return t->pid && !t->done; }

static void read_phase(Task *t) {
    if (!t->lfr[0]) return;
    char p[1160], ph[12]; snprintf(p, sizeof p, "%s.st", t->lfr);
    FILE *f = fopen(p, "r"); unsigned a, b; size_t q;
    if (!f) return;
    if (fscanf(f, "%11s %u %u %zu", ph, &a, &b, &q) == 4) { memcpy(t->ph, ph, sizeof ph); t->rnd_done = a; t->rnd_total = b; t->queued = q; }
    fclose(f);
}
// the main tile's progress block: one line per technique (grey: nothing yet, white: running, green: all done)
static void draw_phases(DpCanvas *c, int y, Task *tk, size_t nt) {
    size_t mt = 0, md = 0, mr = 0, ms = 0, ar = 0, bt = 0, bd = 0, br = 0, rt = 0, rd = 0, rr = 0, ft = 0, fd = 0, fr = 0, fp = 0, sta = 0, runs = 0, q = 0, snaps = 0;
    for (size_t i = 0; i < nt; i++) {
        Task *t = &tk[i]; int run = running_task(t), att = run && !strcmp(t->ph, "attract");
        if (t->kind == K_MOVIE) { mt++; md += t->done; mr += run && !att; ar += att; ms += t->stalled; }
        else if (t->kind == K_BOOT) { bt++; bd += t->done; br += run; }
        else if (t->kind == K_RANDOM) { rt++; rd += t->done; rr += run; runs += t->rnd_done; }
        else { ft++; fd += t->done; fr += run; snaps += t->snap[0] != 0; if (run) { fp += t->paused; q += t->queued; sta += !strcmp(t->ph, "static"); } }
    }
    enum { WAIT = 0, RUN = 1, DONE = 2 };
    static const uint32_t col[3] = { 0xff808088u, 0xffffffffu, 0xff80e090u };
    char l[200];
    snprintf(l, sizeof l, "TAS        %zu/%zu done, %zu running, %zu stagnation", md, mt, mr, ms);
    dp_canvas_text(c, 2, y, l, 1, col[mr ? RUN : md == mt && mt ? DONE : WAIT]);
    snprintf(l, sizeof l, "attract    boot %s, %zu after a movie", br ? "running" : bd == bt ? "done" : "waiting", ar);
    dp_canvas_text(c, 2, y + 10, l, 1, col[br || ar ? RUN : bd == bt ? DONE : WAIT]);
    snprintf(l, sizeof l, "random     %zu/%zu snapshots, %zu running, %zu runs", rd, rt, rr, runs);
    dp_canvas_text(c, 2, y + 20, l, 1, col[rr ? RUN : rt && rd == rt ? DONE : WAIT]);
    snprintf(l, sizeof l, "forks      %zu/%zu done, %zu running, %zu paused (disk)", fd, ft, fr - fp, fp);
    dp_canvas_text(c, 2, y + 30, l, 1, col[fr ? RUN : ft && fd == ft ? DONE : WAIT]);
    snprintf(l, sizeof l, "states     %zu queued in the running forks, %zu closing", q, sta);
    dp_canvas_text(c, 2, y + 40, l, 1, col[fr ? RUN : WAIT]);
    snprintf(l, sizeof l, "snapshots  %zu saved | static inference: at the end", snaps);
    dp_canvas_text(c, 2, y + 50, l, 1, col[snaps ? DONE : WAIT]);
}

static volatile sig_atomic_t g_stop;
static void on_int(int s) { (void)s; g_stop = 1; }

static uint64_t meminfo(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r"); char l[256]; uint64_t v = 0;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, key, strlen(key))) { v = strtoull(l + strlen(key), NULL, 10) * 1024ull; break; }
    if (f) fclose(f);
    return v;
}
static double io_pressure(void) {   // share of the last 10 s with every task stalled on IO (%), 0 where the kernel does not report it
    FILE *f = fopen("/proc/pressure/io", "r"); char l[256]; double v = 0;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "full avg10=", 11)) v = strtod(l + 11, NULL);
    if (f) fclose(f);
    return v;
}
static uint64_t swap_used(void) { return meminfo("SwapTotal:") - meminfo("SwapFree:"); }
static uint64_t free_bytes(const char *p) { struct statvfs s; return statvfs(p, &s) ? 0 : (uint64_t)s.f_bavail * s.f_frsize; }
static uint64_t proc_spool(pid_t pid) {   // bytes of the child's unlinked spool files (queue + deferred states)
    char d[64]; snprintf(d, sizeof d, "/proc/%d/fd", (int)pid);
    DIR *dir = opendir(d); if (!dir) return 0;
    uint64_t sum = 0; struct dirent *e;
    while ((e = readdir(dir))) {
        char lp[96], tg[600]; snprintf(lp, sizeof lp, "%s/%s", d, e->d_name);
        ssize_t n = readlink(lp, tg, sizeof tg - 1); if (n <= 0) continue; tg[n] = 0;
        if (!strstr(tg, "deepeel-spool-") && !strstr(tg, "deepeel-deferred-")) continue;
        struct stat st; if (stat(lp, &st) == 0) sum += (uint64_t)st.st_size;
    }
    closedir(dir); return sum;
}
static size_t proc_rss(pid_t pid) {
    char p[64]; snprintf(p, sizeof p, "/proc/%d/statm", (int)pid);
    FILE *f = fopen(p, "r"); unsigned long a = 0, b = 0;
    if (f) { if (fscanf(f, "%lu %lu", &a, &b) != 2) b = 0; fclose(f); }
    return (size_t)b * (size_t)sysconf(_SC_PAGESIZE);
}
static void copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL; char buf[1 << 16]; size_t n;
    while (b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}
static pid_t spawn(const char *exe, char **av, const char *log) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log, 1 | 64 | 512, 0666);   // O_WRONLY|O_CREAT|O_TRUNC
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        setpgid(0, 0);
        setpriority(PRIO_PROCESS, 0, CAMP_NICE);   // the desktop and the supervisor stay responsive
        execv(exe, av);
        _exit(127);
    }
    return pid;
}
static void mkdirs(const char *path) {
    char t[1100]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
    mkdir(t, 0777);
}
static const char *bulk_base(const char *local, const char *overflow) {   // where the next heavy data goes
    uint64_t lf = free_bytes(local);
    const char *cap = getenv("DEEPEEL_LOCAL_FREE_CAP_GB");   // test control: pretend the local disk has at most N GiB free
    if (cap && *cap && ((uint64_t)strtoull(cap, NULL, 10) << 30) < lf) lf = (uint64_t)strtoull(cap, NULL, 10) << 30;
    if (lf >= LOCAL_MIN_FREE || !overflow) return local;
    return free_bytes(overflow) > lf ? overflow : local;
}

static int cmp_movie(const void *a, const void *b) {
    const Movie *x = a, *y = b;
    return x->frames < y->frames ? 1 : x->frames > y->frames ? -1 : strcmp(x->path, y->path);
}
// the movies of the ROM, from the TASVideos-backed database (sync on demand, fetch the files): the `tasdb find
// ROM --fetch` listing gives "game N" and one "  <id>M|S ... N frames" line per publication/submission; the files
// live in <db>/movies/<game>/<id>/
static size_t fetch_movies(const char *exe, const char *rom, Movie **out) {
    size_t n = 0, cap = 0; *out = NULL;
    char db[1100]; snprintf(db, sizeof db, "%s", exe); { char *sl = strrchr(db, '/'); if (sl) *sl = 0; }
    for (int attempt = 0; attempt < 2 && !n; attempt++) {
        if (attempt) {
            char c[2400], qe[2300]; snprintf(c, sizeof c, "%s tasdb sync --systems GB,GBC >&2", dp_shquote(exe, qe, sizeof qe));
            if (system(c)) break;
        }
        // the listing is parsed only once the command is over: its lines come out before their downloads are written
        char c[7000], qe[2300], qr[4400]; snprintf(c, sizeof c, "%s tasdb find %s --fetch 2>&1", dp_shquote(exe, qe, sizeof qe), dp_shquote(rom, qr, sizeof qr));
        FILE *p = tmpfile(), *in = popen(c, "r"); char cp[4096]; size_t got;
        while (p && in && (got = fread(cp, 1, sizeof cp, in)) > 0) fwrite(cp, 1, got, p);
        if (in) pclose(in);
        if (p) rewind(p);
        char *line = NULL; size_t lc = 0; unsigned game = 0, ver = 0; char mname[300] = "";   // ver: the ROM's own version; a movie replays on its own ROM only
        while (p && getline(&line, &lc, p) > 0) {
            unsigned g; if (sscanf(line, "game %u", &g) == 1 || sscanf(line, "ROM matches game %u", &g) == 1) game = g;
            { char *a = strstr(line, "ROM matches game "), *b = a ? strchr(a, '(') : NULL, *e = b ? strstr(b, ", version ") : NULL;
              if (e && (size_t)(e - b - 1) < sizeof mname) { snprintf(mname, sizeof mname, "%.*s", (int)(e - b - 1), b + 1); } }
            { unsigned v; char nm[300]; if (mname[0] && sscanf(line, " version %u: %299[^\n]", &v, nm) == 2) { char *t = strstr(nm, ", region "); if (t) *t = 0; if (!strcmp(nm, mname)) ver = v; } }
            char id[32]; unsigned frames = 0;
            char *fr = strstr(line, " frames");
            if (!game || !fr || sscanf(line, " %31s", id) != 1 || !strchr("MS", id[strlen(id) - 1]) || id[0] < '0' || id[0] > '9') continue;
            char *q = fr; while (q > line && q[-1] >= '0' && q[-1] <= '9') q--;
            frames = (unsigned)strtoul(q, NULL, 10);
            if (ver) { char tag[24]; snprintf(tag, sizeof tag, "[v%u]", ver); if (!strstr(line, tag)) {
                fprintf(stderr, "deepeel auto: %.*s skipped: recorded on another version of the game (a movie replays on its own ROM only)\n", (int)strcspn(line + strspn(line, " "), " "), line + strspn(line, " ")); continue; } }
            char dir[1300]; snprintf(dir, sizeof dir, "%s/tasdb/movies/%u/%s", db, game, id);
            DIR *d = opendir(dir); if (!d) continue;
            struct dirent *de; char best[1100] = ""; off_t bsz = 0;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.') continue;
                char f[1500]; snprintf(f, sizeof f, "%s/%s", dir, de->d_name); struct stat sb;
                if (stat(f, &sb) || !S_ISREG(sb.st_mode)) continue;
                const char *ext = strrchr(de->d_name, '.');
                if (ext && (!strcasecmp(ext, ".bk2") || !strcasecmp(ext, ".vbm") || !strcasecmp(ext, ".vmv") || !strcasecmp(ext, ".gbm")) && sb.st_size > bsz) { snprintf(best, sizeof best, "%.1099s", f); bsz = sb.st_size; }
            }
            closedir(d);
            if (!best[0]) continue;
            uint64_t input = 0;   // FNV-1a of the decoded pad frames
            { DpTas tas; memset(&tas, 0, sizeof tas); char terr[160];
              if (dp_tas_load(best, &tas, terr, sizeof terr)) { input = 0xcbf29ce484222325ull; for (uint32_t i = 0; i < tas.frames; i++) input = (input ^ tas.masks[i]) * 0x100000001b3ull; }
              dp_tas_free(&tas); }
            int dup = 0; for (size_t i = 0; i < n; i++) if ((*out)[i].frames == frames && ((*out)[i].size == (unsigned long)bsz || (input && (*out)[i].input == input))) dup = 1;   // the same movie as publication and submission
            if (dup) continue;
            if (n == cap) { cap = cap ? cap * 2 : 16; *out = realloc(*out, cap * sizeof **out); }
            snprintf((*out)[n].path, sizeof (*out)[n].path, "%s", best); (*out)[n].frames = frames; (*out)[n].size = (unsigned long)bsz; (*out)[n].input = input;
            snprintf((*out)[n].emu, sizeof (*out)[n].emu, "unresolved");
            { char q2[7000]; snprintf(q2, sizeof q2, "%s tasdb emu %s %u --id %s 2>/dev/null", dp_shquote(exe, qe, sizeof qe), dp_shquote(rom, qr, sizeof qr), frames, id);
              FILE *e = popen(q2, "r"); char el[256];
              while (e && fgets(el, sizeof el, e)) if (!strncmp(el, "emulator=", 9)) { char *k = strstr(el, " kind="); if (k) *k = 0; el[strcspn(el, "\n")] = 0; snprintf((*out)[n].emu, sizeof (*out)[n].emu, "%.47s", el + 9); break; }
              if (e) { while (fgets(el, sizeof el, e)) {} pclose(e); } }
            if (!strcmp((*out)[n].emu, "unresolved")) { fprintf(stderr, "deepeel auto: %s skipped: the TASVideos API does not name its emulator\n", best); continue; }
            if (!strncmp((*out)[n].emu, CAMP_SKIP_EMULATOR, strlen(CAMP_SKIP_EMULATOR))) { fprintf(stderr, "deepeel auto: %s skipped: %s movies are disabled for now (desync)\n", best, (*out)[n].emu); continue; }
            n++;
        }
        free(line); if (p) fclose(p);
    }
    if (n) qsort(*out, n, sizeof **out, cmp_movie);
    return n;
}

typedef struct { uint8_t *u; size_t n; uint8_t *inf, *prov; } Union;   // observed flags + the two suspect classes
static size_t merge_flags(Union *un, const char *path) {   // header "COV1" + size, one flag byte per ROM index, then optional sections
    FILE *f = fopen(path, "rb"); uint32_t hd[2];
    if (!f) return 0;
    if (fread(hd, 4, 2, f) != 2 || hd[0] != 0x31564F43u || !hd[1] || (un->n && hd[1] != un->n)) { fclose(f); return 0; }
    if (!un->n) {   // the kernel's compact ROM size, learned from the first child
        un->u = calloc(hd[1], 1); un->inf = calloc(hd[1], 1); un->prov = calloc(hd[1], 1);
        if (!un->u || !un->inf || !un->prov) { fclose(f); return 0; }
        un->n = hd[1];
    }
    uint8_t *b = malloc(un->n); size_t added = 0;
    if (b && fread(b, 1, un->n, f) == un->n) {
        for (size_t i = 0; i < un->n; i++) { uint8_t v = b[i] & ~un->u[i]; if (v) { if (!un->u[i]) added++; un->u[i] |= b[i]; } }
        // a finished child's coverage.bin appends its suspects: "INF1" (static inference) and "PRV1" (probable-code walks)
        uint32_t sh[2];
        while (fread(sh, 4, 2, f) == 2 && sh[1] == un->n) {
            uint8_t *dst = sh[0] == 0x31464E49u ? un->inf : sh[0] == 0x31565250u ? un->prov : NULL;
            if (dst) { if (fread(b, 1, un->n, f) != un->n) break; for (size_t i = 0; i < un->n; i++) if (!dst[i]) dst[i] = b[i]; continue; }
            long skip = sh[0] == 0x31524353u || sh[0] == 0x31544948u ? 4l * (long)un->n : (long)un->n;   // SCR1, HIT1 are words
            if (fseek(f, skip, SEEK_CUR)) break;
        }
    }
    free(b); fclose(f);
    return added;
}
// Each ROM byte counts once.  Observed while running: code (executed), else data (read, copied, decompressed).  Proven
// without running: code reached by direct transfers from executed code.  Suspect: code (probable walk, inferred), data
// (inferred data, tables), padding (filler runs and the gaps against them).  known = all of them.
typedef struct { size_t code, data, proven, scode, sdata, pad, known; } CovStats;
static CovStats union_stats(const Union *un) {
    CovStats c = { 0, 0, 0, 0, 0, 0, 0 };
    for (size_t i = 0; i < un->n; i++) {
        uint8_t f = un->u[i], in = un->inf[i];
        if (f & (DP_CORE_NATURAL | DP_CORE_FORCED)) c.code++;
        else if (f & (DP_CORE_COPIED | DP_CORE_COMPRESSED | DP_CORE_DATA)) c.data++;
        else if (f & DP_CORE_STATIC) c.proven++;
        else if (un->prov[i] || in == DP_MAP_INF_CODE) c.scode++;
        else if (in == DP_MAP_PAD || in == DP_MAP_INF_PAD) c.pad++;
        else if (in) c.sdata++;
    }
    c.known = c.code + c.data + c.proven + c.scode + c.sdata + c.pad;
    return c;
}
static void cov_line(const Union *un, char *out, size_t n) {
    CovStats c = union_stats(un); double t = un->n ? 100.0 / (double)un->n : 0.0;
    snprintf(out, n, "observed: code %.1f%% data %.1f%% | proven static code %.1f%% | suspect: code %.1f%% data %.1f%% padding %.1f%% | known %.1f%% of %zu bytes",
             c.code * t, c.data * t, c.proven * t, c.scode * t, c.sdata * t, c.pad * t, c.known * t, un->n);
}
static void save_union(const Union *un, const char *path) {   // "COV1" flags, then the suspects: "INF1", "PRV1"; atomic
    if (!un->n) return;
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    uint32_t hd[2] = { 0x31564F43u, (uint32_t)un->n }, is[2] = { 0x31464E49u, (uint32_t)un->n }, ps[2] = { 0x31565250u, (uint32_t)un->n };
    int ok = fwrite(hd, 4, 2, f) == 2 && fwrite(un->u, 1, un->n, f) == un->n && fwrite(is, 4, 2, f) == 2 && fwrite(un->inf, 1, un->n, f) == un->n &&
             fwrite(ps, 4, 2, f) == 2 && fwrite(un->prov, 1, un->n, f) == un->n;
    if (fclose(f) || !ok || rename(tmp, path)) unlink(tmp);
}
static int seed_state(const Union *un, const char *sdir) {   // a child resumes from everything observed so far: "new" means new for the whole campaign
    if (!un->n) return 0;
    mkdirs(sdir);
    char cp[1300]; snprintf(cp, sizeof cp, "%s/coverage.bin", sdir);
    FILE *cf = fopen(cp, "wb"); uint32_t hd[2] = { 0x31564F43u, (uint32_t)un->n };
    if (!cf) return 0;
    int ok = fwrite(hd, 4, 2, cf) == 2 && fwrite(un->u, 1, un->n, cf) == un->n;
    return fclose(cf) == 0 && ok;
}
// stops a child's process group; after `secs` without an exit it is killed (the live coverage it published is kept)
static void stop_child(pid_t pid, int secs) {
    kill(-pid, SIGCONT); kill(-pid, SIGINT);
    for (int i = 0; i < secs * 20; i++) { if (waitpid(pid, NULL, WNOHANG) != 0) return; usleep(50000); }
    kill(-pid, SIGKILL); waitpid(pid, NULL, 0);
}
// the static inference (padding, gaps, tables, probable code) runs once, on the merged coverage: a child's partial view
// would leave guesses the rest of the campaign contradicts
static void infer_union(const char *exe, const char *rom, const char *local, Union *un) {
    if (!un->n) return;
    char dir[1200], sdir[1250], log[1250], cov[1300];
    snprintf(dir, sizeof dir, "%s/final", local); snprintf(sdir, sizeof sdir, "%s/state", dir); snprintf(log, sizeof log, "%s/log.txt", dir); snprintf(cov, sizeof cov, "%s/coverage.bin", sdir);
    mkdirs(dir);
    if (!seed_state(un, sdir)) return;
    fprintf(stderr, "deepeel auto: static inference on the merged coverage\n");
    char *av[] = { (char *)exe, "explore", (char *)rom, "--jobs", "1", "--state-dir", sdir, "--no-frontier", "--resume", "--observe", "--no-start-attract", NULL };
    pid_t pid = spawn(exe, av, log); int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "deepeel auto: static inference failed, see %s\n", log); return; }
    memset(un->inf, 0, un->n);
    merge_flags(un, cov);
}

static void write_png(const Union *un, const char *png, const char *id, const char *name) {
    if (!un->n) return;
    DpCoreResult r; memset(&r, 0, sizeof r);
    r.rom_size = un->n; r.flags = un->u; r.inferred = un->inf; r.prov = un->prov;
    snprintf(r.rom_title, sizeof r.rom_title, "%s", name);
    dp_core_write_map(png, id, name, &r);
}


// ---- live window: one tile per view (coverage, then one per running movie replay), composed on a canvas ----
// A tile has a fixed size: the source machine's screen (learned from the first frame a child publishes) at zoom 2 (or 1 when the
// window would not fit the display) plus an info strip; the coverage tile spans 2x2 cells.
typedef struct { uint32_t w, h, total; uint64_t frame; uint32_t *px; } LiveFrame;
static int read_live_frame(const char *path, LiveFrame *lf) {
    FILE *f = fopen(path, "rb"); uint32_t hd[2]; if (!f) return 0;
    int ok = fread(hd, 4, 2, f) == 2 && hd[0] && hd[1] && hd[0] <= 512 && hd[1] <= 512 && fread(&lf->frame, 8, 1, f) == 1 && fread(&lf->total, 4, 1, f) == 1;
    if (ok) { lf->w = hd[0]; lf->h = hd[1]; free(lf->px); lf->px = malloc((size_t)hd[0] * hd[1] * 4); ok = lf->px && fread(lf->px, 4, (size_t)hd[0] * hd[1], f) == (size_t)hd[0] * hd[1]; }
    fclose(f); return ok;
}
// emulated frames per second of a child, from the frame counter it publishes, averaged over one second
static unsigned live_fps(Task *t, uint64_t frame) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (!t->fps_t || frame < t->fps_frame) { t->fps_t = now; t->fps_frame = frame; t->fps = 0; }
    else if (now - t->fps_t >= 1.0) { t->fps = (unsigned)((double)(frame - t->fps_frame) / (now - t->fps_t) + 0.5); t->fps_t = now; t->fps_frame = frame; }
    return t->fps;
}
static int draw_live(const Union *un, const char *name, time_t t_start, Task *tk, size_t nt, const Movie *mv, size_t nm, uint32_t *sw, uint32_t *sh_) {
    enum { INFO = 32, COVINFO = 96, GAP = 4, MAXW = 1900, MAXH = 1000, TILES = 64, TEXTW = 400, MAPH = 2 * (2 * 144 + INFO + GAP) - GAP - COVINFO };
    typedef struct { int g; Task *film, *q[CAMP_RANDOM_PARALLEL]; size_t rd, rt; } Tile;
    static LiveFrame lf[TILES][CAMP_RANDOM_PARALLEL]; static Tile tl[TILES]; size_t ns = 0;
    for (size_t i = 0; i < nt; i++) if (running_task(&tk[i])) read_phase(&tk[i]);
    for (size_t k = 0; k <= nm && ns < TILES; k++) {   // the power-on group first, then the movies, longest first
        int g = k ? (int)k - 1 : (int)nm; Tile t = { g, NULL, { NULL }, 0, 0 }; int live = 0;
        for (size_t i = 0; i < nt; i++) {
            Task *x = &tk[i]; if (x->movie != g || x->kind == K_FORK) continue;
            if (x->kind == K_RANDOM) { t.rt++; t.rd += x->done; }
            if (!x->done) live = 1;
            if (!running_task(x)) continue;
            if (is_film(x)) t.film = x; else if (x->slot >= 0 && x->slot < CAMP_RANDOM_PARALLEL) t.q[x->slot] = x;
        }
        if (g == (int)nm && !t.film) live = 0;   // the power-on tile is the attract run only: it leaves when that ends
        if (live) tl[ns++] = t;
    }
    for (size_t k = 0; k < ns; k++) for (int j = 0; j < CAMP_RANDOM_PARALLEL; j++) {
        Task *t = tl[k].film ? (j ? NULL : tl[k].film) : tl[k].q[j];
        if (!t || !read_live_frame(t->lfr, &lf[k][j])) { free(lf[k][j].px); lf[k][j].px = NULL; continue; }
        if (!*sw) { *sw = lf[k][j].w; *sh_ = lf[k][j].h; }
    }
    uint32_t w0 = *sw ? *sw : 160, h0 = *sh_ ? *sh_ : 144;
    // the map on the left, exactly as wide as it draws; the tiles to its right, stacked one by one: top, below it, then
    // the next column to the right; a second band of two starts only once the screen width is reached
    int z = 2, tc = 1, cw, chh, mapw, maph;
    for (;; z--) {   // largest zoom whose two rows of tiles fit the screen width
        cw = (int)w0 * z + GAP; chh = (int)h0 * z + INFO + GAP;
        maph = 2 * chh - GAP - COVINFO; if (maph < MAPH) maph = MAPH;
        mapw = dp_canvas_covmap_width(un->n, 0x4000, maph, maph);
        if (mapw < TEXTW) mapw = TEXTW;   // the text lines under the map
        tc = (MAXW - 2 * GAP - mapw) / cw; if (tc < 1) tc = 1;
        if (z <= 1 || 2 * (size_t)tc >= ns) break;
    }
    size_t band = 2 * (size_t)tc;
    int used = ns >= band ? tc : (int)((ns + 1) / 2);
    int rows = (int)(2 * (ns / band)) + (ns % band > 1 ? 2 : (int)(ns % band));
    int cwid = mapw, chei = maph + COVINFO, height = rows * chh - GAP > chei ? rows * chh - GAP : chei;
    DpCanvas *c = dp_canvas_new(cwid + used * cw + 2 * GAP, height + 2 * GAP, 0xff15151au); if (!c) return 1;
    // coverage tile: redrawn once a second at most (it walks the whole ROM), copied from its cache otherwise
    int cx = GAP, cy = GAP;
    static DpCanvas *cache; static time_t cache_t;
    time_t nowt = time(NULL);
    if (!cache || cache->w != cwid || cache->h != chei || nowt != cache_t) {
        if (cache && (cache->w != cwid || cache->h != chei)) { dp_canvas_free(cache); cache = NULL; }
        if (!cache) cache = dp_canvas_new(cwid, chei, 0xff15151au); else dp_canvas_rect(cache, 0, 0, cwid, chei, 0xff15151au);
        cache_t = nowt;
        if (cache) {
            char l[200]; CovStats cs = union_stats(un); double pc = un->n ? 100.0 / (double)un->n : 0.0;
            size_t block = un->n ? dp_canvas_covmap(cache, 0, 0, maph, maph, un->u, un->n, 0x4000, un->inf, un->prov) : 0;
            long el = (long)(nowt - t_start);
            snprintf(l, sizeof l, "%s  %02ld:%02ld:%02ld  known %.1f%%", name, el / 3600, el / 60 % 60, el % 60, cs.known * pc);
            dp_canvas_text(cache, 2, chei - COVINFO + 3, l, 1, 0xff80e090u);
            snprintf(l, sizeof l, "code %.1f%%  data %.1f%%  proven static %.1f%%", cs.code * pc, cs.data * pc, cs.proven * pc);
            dp_canvas_text(cache, 2, chei - COVINFO + 13, l, 1, 0xff96c8ffu);
            snprintf(l, sizeof l, "suspect: code %.1f%% data %.1f%% padding %.1f%% | 1 square %zu B", cs.scode * pc, cs.sdata * pc, cs.pad * pc, block);
            dp_canvas_text(cache, 2, chei - COVINFO + 23, l, 1, 0xffd0a050u);
            draw_phases(cache, chei - COVINFO + 36, tk, nt);
        }
    }
    if (cache) for (int y = 0; y < chei && cy + y < c->h; y++) memcpy(c->px + (size_t)(cy + y) * c->w + cx, cache->px + (size_t)y * cwid, (size_t)(cx + cwid <= c->w ? cwid : c->w - cx) * 4);
    char l[200];
    // the group tiles
    for (size_t k = 0; k < ns; ) {
        int tx = 2 * GAP + cwid + (int)(k % band / 2) * cw, ty = GAP + (int)(2 * (k / band) + k % 2) * chh, tw = (int)w0 * z, th = (int)h0 * z;
        Tile *g = &tl[k]; LiveFrame *f = lf[k]; k++;
        const char *b = "power-on"; if (g->g < (int)nm) { b = strrchr(mv[g->g].path, '/'); b = b ? b + 1 : mv[g->g].path; }
        dp_canvas_rect(c, tx, ty, tw, th + INFO, 0xff0c0c10u);
        if (g->film) {   // the movie, or an attract run (no input): a full screen
            Task *t = g->film; int att = !strcmp(t->ph, "attract");
            if (f[0].px) dp_canvas_tile(c, tx, ty, tw, th, f[0].px, (int)f[0].w, (int)f[0].h);
            unsigned long long fr = f[0].px ? (unsigned long long)f[0].frame : 0; unsigned fps = f[0].px ? live_fps(t, f[0].frame) : 0;
            snprintf(l, sizeof l, "%.40s", b); dp_canvas_text(c, tx + 2, ty + th + 3, l, 1, 0xffc8c8d0u);
            if (t->kind == K_BOOT) snprintf(l, sizeof l, "attract (no input)  frame %llu  %u fps", fr, fps);
            else if (att) snprintf(l, sizeof l, "movie over, no input  frame %llu  %u fps", fr, fps);
            else snprintf(l, sizeof l, "%llu/%u  %u%%  %u fps", fr, mv[g->g].frames, mv[g->g].frames ? (unsigned)(100ull * fr / mv[g->g].frames) : 0u, fps);
            dp_canvas_text(c, tx + 2, ty + th + 13, l, 1, 0xff96c8ffu);
            if (t->kind == K_MOVIE) { snprintf(l, sizeof l, "source: %.40s", mv[g->g].emu); dp_canvas_text(c, tx + 2, ty + th + 23, l, 1, 0xffd0b060u); }
            continue;
        }
        int hw = tw / 2, hh = th / 2, any = 0;   // its random runs: four at a time, a quarter screen each
        for (int j = 0; j < CAMP_RANDOM_PARALLEL; j++) {
            Task *t = g->q[j]; if (!t) continue;
            any = 1;
            if (f[j].px) dp_canvas_tile(c, tx + (j & 1) * hw, ty + (j >> 1) * hh, hw, hh, f[j].px, (int)f[j].w, (int)f[j].h);
            char from[24]; if (t->snap[0]) snprintf(from, sizeof from, "s%u", t->frame); else snprintf(from, sizeof from, "boot");
            snprintf(l, sizeof l, "%s %u/%u %ufps", from, t->rnd_done < t->rnd_total ? t->rnd_done + 1 : t->rnd_done, t->rnd_total, f[j].px ? live_fps(t, f[j].frame) : 0u);
            dp_canvas_text(c, tx + 2 + (j & 1) * hw, ty + th + 3 + (j >> 1) * 10, l, 1, 0xffc8c8d0u);
        }
        snprintf(l, sizeof l, "%s %zu/%zu %.30s", any ? "random" : "waiting", g->rd, g->rt, b);
        dp_canvas_text(c, tx + 2, ty + th + 23, l, 1, 0xffd0b060u);
    }
    int gone = dp_livewin_show(c, "deepeel - live coverage") && !getenv("DEEPEEL_LIVE_DUMP");   // closed or no display: stop composing
    dp_canvas_free(c);
    return !gone;
}

static unsigned bitrev(unsigned v) { unsigned r = 0; for (int i = 0; i < 16; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static int snap_frame(const char *n) { return n[0] == 's' ? atoi(n + 1) : -1; }
static int cmp_snap(const void *a, const void *b) { return snap_frame(*(char *const *)a) - snap_frame(*(char *const *)b); }

static Task *add_task(Task *tk, size_t *nt, size_t max, int kind, int group) {
    if (*nt >= max) return NULL;
    Task *t = &tk[(*nt)++]; memset(t, 0, sizeof *t); t->kind = kind; t->movie = group; t->slot = -1;
    return t;
}
static void requeue(Task *tk, size_t *nt, size_t max, const Task *o, int shrink) {
    Task *r = add_task(tk, nt, max, o->kind, o->movie); if (!r) return;
    memcpy(r->snap, o->snap, sizeof r->snap); r->key = o->key; r->frame = o->frame; r->first = o->first; r->shrink = shrink;
}
// The next child, by technique: the films (power-on attract, then the movies, longest first); then the random runs of a
// finished film, CAMP_RANDOM_PARALLEL per group (spread coarse-to-fine over the movie); then the fork explorations.
static Task *next_task(Task *tk, size_t nt, size_t ng, long cap, int forks_ok, int *slot) {
    for (size_t i = 0; i < nt; i++) if (is_film(&tk[i]) && !tk[i].pid && !tk[i].done) return &tk[i];
    static char busy[1024]; static unsigned used[1024];   // per group: its film is not over; the quarters taken by running random children
    if (ng > 1024) ng = 1024;
    memset(busy, 0, ng); memset(used, 0, ng * sizeof *used);
    long rnd = 0, forks = 0, films = 0; int forks_wait = 0;
    for (size_t i = 0; i < nt; i++) {
        Task *t = &tk[i];
        if (is_film(t) && !t->done && (size_t)t->movie < ng) busy[t->movie] = 1;
        films += is_film(t) && running_task(t); forks += t->kind == K_FORK && running_task(t);
        if (t->kind == K_RANDOM && running_task(t)) { rnd++; if (t->slot >= 0 && (size_t)t->movie < ng) used[t->movie] |= 1u << t->slot; }
        forks_wait |= t->kind == K_FORK && !t->pid && !t->done;
    }
    long rnd_cap = forks_ok && forks_wait ? cap - CAMP_FORK_RESERVE : cap; if (rnd_cap < 1) rnd_cap = 1;
    Task *best = NULL; int bn = 99;
    for (size_t i = 0; i < nt && rnd < rnd_cap; i++) {
        Task *t = &tk[i]; if (t->kind != K_RANDOM || t->pid || t->done || (size_t)t->movie >= ng || busy[t->movie]) continue;
        int n = __builtin_popcount(used[t->movie]); if (n >= CAMP_RANDOM_PARALLEL) continue;
        if (!best || n < bn || (n == bn && t->key < best->key)) { best = t; bn = n; }
    }
    if (best) { int s = 0; while (used[best->movie] & (1u << s)) s++; *slot = s; return best; }
    // while films run, the forks hold a few workers only: the random runs of a film start as soon as it ends
    if (films && forks >= (cap / CAMP_FORK_SHARE > CAMP_FORK_RESERVE ? cap / CAMP_FORK_SHARE : CAMP_FORK_RESERVE)) forks_ok = 0;
    if (forks_ok) for (size_t i = 0; i < nt; i++) if (tk[i].kind == K_FORK && !tk[i].pid && !tk[i].done && (!best || tk[i].key < best->key)) best = &tk[i];
    return best;
}

int dp_auto_campaign(const char *exe, const char *rom, const char *overflow, size_t rom_size, const char *outdir, const char *name) {
    signal(SIGINT, on_int); signal(SIGTERM, on_int);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 1;
    long cap = ncpu > 3 ? ncpu - 2 : 1;   // leave two cores to the machine
    uint64_t swap0 = swap_used(), swap_ref = swap0;   // swap_ref: the level growth is measured from
    char local[1100]; snprintf(local, sizeof local, "%s/data", outdir); mkdirs(local);
    {   // one campaign per output directory: a second one (or one still stopping) would write over the same children
        char lk[1200]; snprintf(lk, sizeof lk, "%s/lock", outdir);
        int fd = open(lk, O_RDWR | O_CREAT | O_CLOEXEC, 0666);   // held until this process exits
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) { fprintf(stderr, "deepeel auto: another campaign is running (or still stopping) in %s\n", outdir); return 1; }
    }
    if (overflow) mkdirs(overflow);
    fprintf(stderr, "deepeel auto: %s | %ld cpu -> up to %ld workers | RAM %llu MiB available | swap in use %llu MiB (never grown) | disk local %llu GiB free%s%s\n",
            name, ncpu, cap, (unsigned long long)(meminfo("MemAvailable:") >> 20), (unsigned long long)(swap0 >> 20),
            (unsigned long long)(free_bytes(local) >> 30), overflow ? ", overflow " : "", overflow ? overflow : "");
    Movie *mv; size_t nm = fetch_movies(exe, rom, &mv);
    fprintf(stderr, "deepeel auto: %zu movie(s) for this ROM%s\n", nm, nm ? ", longest first" : " (no TAS: exploration from the power-on state only)");

    Union un = { rom_size ? calloc(rom_size, 1) : NULL, rom_size, rom_size ? calloc(rom_size, 1) : NULL, rom_size ? calloc(rom_size, 1) : NULL };
    const size_t max_tasks = 2 * nm + 64 + CAMP_SNAPSHOT_TASKS;
    Task *tk = calloc(max_tasks, sizeof *tk); size_t nt = 0;
    unsigned *gsnap = calloc(nm + 1, sizeof *gsnap);   // per group: the last snapshot frame queued
    add_task(tk, &nt, max_tasks, K_BOOT, (int)nm);   // attract from the power-on state, shown like a movie
    for (size_t m = 0; m < nm; m++) add_task(tk, &nt, max_tasks, K_MOVIE, (int)m);   // movie replays, longest first
    add_task(tk, &nt, max_tasks, K_FORK, (int)nm);   // forks from the power-on state
    for (unsigned j = 0; j < CAMP_BOOT_RANDOM_TASKS; j++) { Task *t = add_task(tk, &nt, max_tasks, K_RANDOM, (int)nm); t->first = j * CAMP_RANDOM_RUNS; t->key = j; }   // random runs from power-on
    size_t rss_light = CAMP_LIGHT_RSS, rss_heavy = CAMP_HEAVY_RSS, last_total = 0; int no_gain = 0, forks_done = 0, failed = 0;
    const int gain_limit = cap * 2 > 8 ? (int)cap * 2 : 8;
    time_t t_start = time(NULL), t_log = 0, t_png = time(NULL); uint32_t scr_w = 0, scr_h = 0; int win_on = !getenv("DEEPEEL_NO_WINDOW");
    char png[1200]; snprintf(png, sizeof png, "%s/%s.map.png", outdir, name);
    char cb[1200]; snprintf(cb, sizeof cb, "%s/coverage.bin", outdir);
    char rep_all[1200]; snprintf(rep_all, sizeof rep_all, "%s/movies.txt", outdir);
    FILE *repf = fopen(rep_all, "w");
    int stopping = 0; time_t last_shed = 0, stop_t = 0;

    for (;;) {
        // 1. reap
        int running = 0, paused = 0, run_kind[5] = { 0 };
        static Task *gone[256]; size_t ngone = 0;
        for (size_t i = 0; i < nt; i++) {
            Task *t = &tk[i];
            if (!running_task(t)) continue;
            size_t rss = proc_rss(t->pid); if (rss > t->rss_peak) t->rss_peak = rss;
            int st; pid_t w = ngone < 256 ? waitpid(t->pid, &st, WNOHANG) : 0;
            if (w != t->pid) { running++; paused += t->paused; run_kind[t->kind]++; continue; }
            t->done = 1; t->paused = 0; read_phase(t);
            size_t *est = t->kind == K_FORK ? &rss_heavy : &rss_light;
            if (t->rss_peak > *est && t->rss_peak <= CHILD_RSS_CAP) *est = t->rss_peak;
            char sc[1250]; snprintf(sc, sizeof sc, "%s/state/coverage.bin", t->dir);
            size_t a = merge_flags(&un, sc); a += merge_flags(&un, t->live);
            int ok = WIFEXITED(st) && !WEXITSTATUS(st);
            if (!ok && !t->over_t && !stopping && !g_stop) { failed++; fprintf(stderr, "deepeel auto: child failed (%s %d), see %s\n", WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st), t->log); }
            if (is_film(t) && a) no_gain = 0;
            if (t->kind == K_MOVIE) {
                char rl[400] = ""; FILE *rf = fopen(t->rep, "r"); if (rf) { if (!fgets(rl, sizeof rl, rf)) rl[0] = 0; fclose(rf); }
                size_t L = strlen(rl); if (L && rl[L - 1] == '\n') rl[L - 1] = 0;
                int stalled = strstr(rl, "stalled=1") != NULL; t->stalled = stalled;
                fprintf(stderr, "deepeel auto: movie %s %s [%s] +%zu bytes\n", mv[t->movie].path, stalled ? "STAGNATION (desynced or incompatible)" : *rl ? "done" : "NOT REPLAYED (see its log)", rl, a);
                if (t->shot[0]) {   // <out>/summaries/<movie>.png: the replay at a glance
                    char sd[1300], dst[1500]; snprintf(sd, sizeof sd, "%s/summaries", outdir); mkdirs(sd);
                    const char *b = strrchr(mv[t->movie].path, '/'); snprintf(dst, sizeof dst, "%s/%s.png", sd, b ? b + 1 : mv[t->movie].path);
                    copy_file(t->shot, dst);
                }
                if (repf) { fprintf(repf, "%s\t%s\t%u\t%s\t%s\n", mv[t->movie].path, mv[t->movie].emu, mv[t->movie].frames, stalled ? "stagnation" : !ok ? "failed" : *rl ? "ok" : "not replayed", rl); fflush(repf); }
            } else if (t->kind == K_FORK) { forks_done++; no_gain = a ? 0 : no_gain + 1; }
            char st_[1160]; snprintf(st_, sizeof st_, "%s.st", t->lfr); unlink(st_); unlink(t->lfr);
            if (ok && !is_film(t)) gone[ngone++] = t;
        }
        if (ngone) {   // the merged coverage is on disk before a child's copy of it goes
            save_union(&un, cb);
            struct stat sb;
            if (!stat(cb, &sb)) for (size_t i = 0; i < ngone; i++) {
                static const char *const f[] = { "state/coverage.bin", "state/disc.bin", "state/cells.bin", "live.bin" };
                for (unsigned k = 0; k < 4; k++) { char p[1300]; snprintf(p, sizeof p, "%s/%s", gone[i]->dir, f[k]); unlink(p); }
            }
        }
        // 2. new snapshots: random runs and a fork exploration from each
        for (size_t i = 0, n0 = nt; i < n0 && !stopping; i++) {
            Task *f = &tk[i]; if (f->kind != K_MOVIE || !f->pid || f->collected) continue;
            char sd[1250]; snprintf(sd, sizeof sd, "%s/snaps", f->dir);
            int fin = f->done;
            DIR *d = opendir(sd); if (!d) { f->collected = fin; continue; }
            char **names = NULL; size_t nn = 0, nc = 0; struct dirent *de;
            while ((de = readdir(d))) if (de->d_name[0] == 's' && strstr(de->d_name, ".state") && !strstr(de->d_name, ".tmp") && snap_frame(de->d_name) > (int)gsnap[f->movie]) {
                if (nn == nc) { nc = nc ? nc * 2 : 64; names = realloc(names, nc * sizeof *names); }
                names[nn++] = strdup(de->d_name);
            }
            closedir(d);
            qsort(names, nn, sizeof *names, cmp_snap);
            for (size_t k = 0; k < nn; k++) {
                unsigned fr = (unsigned)snap_frame(names[k]);
                for (int kind = 0; kind < 2; kind++) {
                    Task *t = add_task(tk, &nt, max_tasks, kind ? K_FORK : K_RANDOM, f->movie); if (!t) break;
                    snprintf(t->snap, sizeof t->snap, "%s/%s", sd, names[k]); t->frame = fr; t->key = bitrev(fr / CAMP_SNAP_EVERY);
                }
                gsnap[f->movie] = fr;
                free(names[k]);
            }
            free(names);
            f->collected = fin;
        }

        // 3. resources, then start what fits
        uint64_t avail = meminfo("MemAvailable:"), tot = meminfo("MemTotal:");
        uint64_t reserve = tot / 8 > (1500ull << 20) ? tot / 8 : (1500ull << 20);
        // swapping counts only while memory is short, and once per rise: pages left in swap are not ours to wait for
        uint64_t sw = swap_used(); if (sw < swap_ref) swap_ref = sw;
        int swap_grew = sw > swap_ref + (128ull << 20);
        if (swap_grew) { swap_ref = sw; if (avail >= 2 * reserve) swap_grew = 0; }
        const char *base = bulk_base(local, overflow);
        uint64_t dfree = free_bytes(base);
        double io = io_pressure();
        if (dfree < HARD_MIN_FREE && !stopping) { fprintf(stderr, "deepeel auto: disk full (%llu MiB free): clean stop, nothing is lost\n", (unsigned long long)(dfree >> 20)); stopping = 1; }
        if (dfree < SHED_FREE && dfree >= HARD_MIN_FREE && !stopping && time(NULL) - last_shed >= 90) {   // disk pressure: finish the child with the biggest spool (coverage merged at reap)
            size_t bi = (size_t)-1; uint64_t bs = 0;
            for (size_t i = 0; i < nt; i++) if (running_task(&tk[i]) && tk[i].kind == K_FORK) { uint64_t sp = proc_spool(tk[i].pid); if (sp > bs) { bs = sp; bi = i; } }
            if (bi != (size_t)-1) {
                kill(-tk[bi].pid, SIGCONT); kill(-tk[bi].pid, SIGINT); last_shed = time(NULL);
                fprintf(stderr, "deepeel auto: disk %llu GiB free: %s (spool %llu MiB) stopped\n", (unsigned long long)(dfree >> 30), tk[bi].snap, (unsigned long long)(bs >> 20));
            }
        }
        // IO pressure (a slow disk under the forks' spools freezes the machine): one fork is stopped or resumed per tick
        if (io > CAMP_IO_PAUSE && run_kind[K_FORK] - paused > 1) {
            for (size_t i = nt; i-- > 0;) if (running_task(&tk[i]) && tk[i].kind == K_FORK && !tk[i].paused) { kill(-tk[i].pid, SIGSTOP); tk[i].paused = 1; paused++; break; }
        } else if (paused && (io < CAMP_IO_RESUME || run_kind[K_FORK] == paused)) {
            for (size_t i = 0; i < nt; i++) if (running_task(&tk[i]) && tk[i].paused) { kill(-tk[i].pid, SIGCONT); tk[i].paused = 0; paused--; break; }
        }
        if ((avail < reserve / 2 || swap_grew) && running > 1) {   // memory pressure: kill the newest child (re-run later), never swap
            size_t v = (size_t)-1;
            for (size_t i = 0; i < nt; i++) if (running_task(&tk[i]) && (v == (size_t)-1 || tk[i].t0 > tk[v].t0 || (tk[i].kind == K_FORK && tk[v].kind != K_FORK))) v = i;
            if (v != (size_t)-1) {
                Task *t = &tk[v];
                fprintf(stderr, "deepeel auto: RAM %llu MiB available%s: %s %s killed, re-queued\n", (unsigned long long)(avail >> 20), swap_grew ? ", swap growing" : "", t->kind == K_MOVIE ? mv[t->movie].path : "exploration", t->snap);
                kill(-t->pid, SIGKILL); waitpid(t->pid, NULL, 0); t->done = 1; merge_flags(&un, t->live);
                size_t *est = t->kind == K_FORK ? &rss_heavy : &rss_light; if (t->rss_peak > *est) *est = t->rss_peak;
                requeue(tk, &nt, max_tasks, t, t->shrink);
                running--;
            }
        }
        long room = cap - (running - paused);
        time_t tnow = time(NULL); uint64_t headroom = 0;   // memory the running children are still expected to take
        for (size_t i = 0; i < nt; i++) if (running_task(&tk[i])) {
            size_t cur = proc_rss(tk[i].pid), est = tk[i].kind == K_FORK ? rss_heavy : rss_light; if (cur < est) headroom += est - cur;
        }
        int forks_ok = no_gain < gain_limit;
        for (; room > 0 && !g_stop && !stopping && !swap_grew && dfree > LOCAL_MIN_FREE / 2; room--) {
            int slot = -1;
            Task *t = next_task(tk, nt, nm + 1, cap, forks_ok && io < CAMP_IO_PAUSE && !paused, &slot);
            if (!t) break;
            size_t need = t->kind == K_FORK ? rss_heavy : rss_light;
            if (avail <= reserve + headroom + need) break;
            char id[64]; snprintf(id, sizeof id, "%c%zu", " mxbr"[t->kind], (size_t)(t - tk));
            snprintf(t->dir, sizeof t->dir, "%s/%s", base, id); mkdirs(t->dir);
            snprintf(t->live, sizeof t->live, "%s/live.bin", t->dir); snprintf(t->log, sizeof t->log, "%s/log.txt", t->dir);
            snprintf(t->rep, sizeof t->rep, "%s/report.txt", t->dir);
            char sdir[1250], snaps[1250], spool_arg[32], queue_arg[32], first_arg[16];
            {   // queued states by RAM, not by count: a Colour machine state is five times a monochrome one
                struct stat sb; static size_t state_bytes;
                if (t->snap[0] && !stat(t->snap, &sb) && sb.st_size > 0) state_bytes = (size_t)sb.st_size;
                size_t q = state_bytes ? (size_t)(CAMP_QUEUE_BYTES / state_bytes) : 400;
                if (q > CAMP_QUEUE_EXPLORE) q = CAMP_QUEUE_EXPLORE;
                q >>= t->shrink; if (q < CAMP_QUEUE_MIN) q = CAMP_QUEUE_MIN;
                snprintf(queue_arg, sizeof queue_arg, "%zu", q);
            }
            {   // disk budget per child: what is free beyond the shed line, shared by the workers, 2 GiB at least
                uint64_t fb = free_bytes(base), per = fb > SHED_FREE ? (fb - SHED_FREE) / (cap ? cap : 1) : 0;
                snprintf(spool_arg, sizeof spool_arg, "%llu", (unsigned long long)(per > CAMP_SPOOL_MIN ? per : CAMP_SPOOL_MIN));
            }
            snprintf(sdir, sizeof sdir, "%s/state", t->dir); snprintf(snaps, sizeof snaps, "%s/snaps", t->dir);
            snprintf(t->lfr, sizeof t->lfr, "/dev/shm/deepeel-%d-%zu.frame", (int)getpid(), (size_t)(t - tk));
            char *av[64]; int n = 0;
#define A(x) av[n++] = (char *)(x)
            A(exe); A("explore"); A(rom);
            A("--jobs"); A("1"); A("--max-total"); A(HUGE_STEPS); A("--queue-max"); A(queue_arg);   // RAM share of one child: the rest spills to its disk spool
            A("--state-dir"); A(sdir); A("--no-frontier"); A("--spool-max"); A(spool_arg);
            A("--live-flags"); A(t->live); A("--live-frame"); A(t->lfr); A("--live-frame-ms"); A(CAMP_STR(CAMP_LIVE_FRAME_MS));
            if (t->kind != K_FORK) { A("--observe"); A("--no-closure"); }   // observation: no fork, no spool; what runs is recorded
            A("--no-infer"); A("--no-spec");   // the inference runs once, on the merged coverage
            if (t->snap[0]) { A("--from-state"); A(t->snap); }
            if (t->kind == K_MOVIE) {   // the movie, then no input from its end state (the ending, the credits)
                mkdirs(snaps);
                A("--tas"); A(mv[t->movie].path); A("--no-start-attract");
                A("--snap-every"); A(CAMP_STR(CAMP_SNAP_EVERY)); A("--snap-dir"); A(snaps);
                A("--tas-stall"); A(CAMP_STR(CAMP_MOVIE_STALL)); A("--tas-report"); A(t->rep);
                snprintf(t->shot, sizeof t->shot, "%s/summary.png", t->dir); A("--tas-summary"); A(t->shot);
                A("--attract-frames"); A(CAMP_STR(CAMP_ATTRACT_MAX)); A("--attract-min"); A(CAMP_STR(CAMP_POST_ATTRACT_MIN)); A("--attract-stall"); A(CAMP_STR(CAMP_POST_ATTRACT_STALL));
            } else if (t->kind == K_BOOT) {
                A("--attract-frames"); A(CAMP_STR(CAMP_ATTRACT_MAX)); A("--attract-min"); A(CAMP_STR(CAMP_ATTRACT_MIN)); A("--attract-stall"); A(CAMP_STR(CAMP_ATTRACT_STALL));
            } else {
                A("--no-start-attract");
                if (seed_state(&un, sdir)) A("--resume");
                if (t->kind == K_RANDOM) {
                    snprintf(first_arg, sizeof first_arg, "%u", t->first);
                    A("--random"); A(CAMP_STR(CAMP_RANDOM_RUNS)); A("--random-first"); A(first_arg);
                    A("--random-no-start"); A(CAMP_STR(CAMP_RANDOM_NO_START));
                    A("--random-no-select"); A(CAMP_STR(CAMP_RANDOM_NO_SELECT));
                    A("--random-min"); A(CAMP_STR(CAMP_RANDOM_MIN)); A("--random-stall"); A(CAMP_STR(CAMP_RANDOM_STALL));
                } else {
                    A("--stall-time"); A(CAMP_STR(CAMP_EXPLORE_STALL_SECS));
                    if (CAMP_EXPLORE_MAX_SECS) { A("--max-time"); A(CAMP_STR(CAMP_EXPLORE_MAX_SECS)); }
                }
            }
#undef A
            av[n] = NULL;
            // BizHawk 2.3.2 Gambatte counts its RTC in CPU cycles: the API names the release, the movie gives the divisor offset
            unsetenv("DEEPEEL_TAS_RTCOFF");
            if (t->kind == K_MOVIE && strstr(mv[t->movie].emu, "2.3.2")) {
                DpTas tas; memset(&tas, 0, sizeof tas); char terr[160], off[16];
                if (dp_tas_load(mv[t->movie].path, &tas, terr, sizeof terr)) { snprintf(off, sizeof off, "%d", tas.rtc_div_offset); setenv("DEEPEEL_TAS_RTCOFF", off, 1); }
                dp_tas_free(&tas);
            }
            t->t0 = tnow; t->slot = slot; t->pid = spawn(exe, av, t->log);
            if (t->pid < 0) { t->pid = 0; t->done = 1; }
            else { running++; run_kind[t->kind]++; headroom += need; }
        }

        // 4. monitor, periodic PNG
        time_t now = time(NULL);
        for (size_t i = 0; i < nt; i++) if (running_task(&tk[i])) merge_flags(&un, tk[i].live);
        size_t total = union_stats(&un).known;
        if (now - t_log >= CAMP_LOG_SECS) {
            t_log = now;
            size_t films = 0, films_n = 0, rq = 0, rd = 0, fq = 0;
            for (size_t i = 0; i < nt; i++) {
                Task *t = &tk[i];
                if (is_film(t)) { films_n++; films += t->done; }
                if (t->kind == K_RANDOM) { rq += !t->pid && !t->done; rd += t->done; }
                if (t->kind == K_FORK) fq += !t->pid && !t->done;
            }
            char cl[300]; cov_line(&un, cl, sizeof cl);
            fprintf(stderr, "[%ldm%02lds] ROM %s | workers %d/%ld (%d paused) | films %zu/%zu done, %d running | random %d running, %zu done, %zu queued | forks %d running, %d done, %zu queued, streak %d%s | failed %d | RAM avail %llu MiB | swap %+lld MiB | IO full %.0f%% | disk %s %llu GiB free\n",
                    (long)(now - t_start) / 60, (long)(now - t_start) % 60, cl, running, cap, paused, films, films_n, run_kind[K_MOVIE] + run_kind[K_BOOT], run_kind[K_RANDOM], rd, rq,
                    run_kind[K_FORK], forks_done, fq, no_gain, forks_ok ? "" : " (stagnation: no more forks)", failed, (unsigned long long)(avail >> 20),
                    (long long)((int64_t)swap_used() - (int64_t)swap0) >> 20, io, base == local ? "local" : "overflow", (unsigned long long)(dfree >> 30));
        }
        if (now - t_png >= CAMP_PNG_SECS && total != last_total) { write_png(&un, png, "auto", name); save_union(&un, cb); t_png = now; last_total = total; }
        // 5. end: told to stop, or nothing running and nothing left to start
        if (g_stop || stopping) {
            if (!stop_t) { stop_t = now; for (size_t i = 0; i < nt; i++) if (running_task(&tk[i])) { kill(-tk[i].pid, SIGCONT); kill(-tk[i].pid, SIGINT); tk[i].paused = 0; } }
            if (!running || now - stop_t > CAMP_STOP_SECS) break;
        } else if (!running) {
            int slot; if (!next_task(tk, nt, nm + 1, cap, no_gain < gain_limit, &slot)) break;
        }
        for (int sl = 0; sl < 2000 / CAMP_WINDOW_MS; sl++) {   // fast RSS watch and live window between two supervision ticks (~2 s)
            usleep(CAMP_WINDOW_MS * 1000);
            if (win_on) win_on = draw_live(&un, name, t_start, tk, nt, mv, nm, &scr_w, &scr_h);
            for (size_t i = 0; i < nt; i++) if (running_task(&tk[i]) && !tk[i].over_t) {
                size_t rss = proc_rss(tk[i].pid); if (rss > tk[i].rss_peak) tk[i].rss_peak = rss;
                if (rss > CHILD_RSS_CAP) {
                    tk[i].over_t = time(NULL); kill(-tk[i].pid, SIGKILL); 
                    int again = tk[i].kind == K_FORK && tk[i].shrink < CAMP_RSS_RETRIES;
                    if (again) requeue(tk, &nt, max_tasks, &tk[i], tk[i].shrink + 1);
                    fprintf(stderr, "deepeel auto: child over %llu MiB (%zu MiB), killed, coverage so far kept, %s: %s\n", (unsigned long long)(CHILD_RSS_CAP >> 20), rss >> 20, again ? "re-queued with half the queue" : "not re-queued", tk[i].kind == K_MOVIE ? mv[tk[i].movie].path : tk[i].snap);
                }
            }
        }
    }
    for (size_t i = 0; i < nt; i++) if (running_task(&tk[i])) stop_child(tk[i].pid, 5);
    for (size_t i = 0; i < nt; i++) if (tk[i].pid) { char sc[1250]; snprintf(sc, sizeof sc, "%s/state/coverage.bin", tk[i].dir); merge_flags(&un, sc); merge_flags(&un, tk[i].live); }
    save_union(&un, cb);
    infer_union(exe, rom, local, &un);
    write_png(&un, png, "auto", name);
    save_union(&un, cb);
    if (repf) fclose(repf);
    for (size_t i = 0; i < nt; i++) if (tk[i].lfr[0]) { char st[1160]; snprintf(st, sizeof st, "%s.st", tk[i].lfr); unlink(st); unlink(tk[i].lfr); }   // screens published in /dev/shm
    dp_livewin_close();
    char cl[300]; cov_line(&un, cl, sizeof cl);
    fprintf(stderr, "deepeel auto: finished, ROM %s, observations of the tool; map %s, movies report %s, merged coverage %s\n",
            cl, png, rep_all, cb);
    free(un.u); free(un.inf); free(un.prov); free(mv); free(tk); free(gsnap);
    return 0;
}
