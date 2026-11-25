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
#include <dirent.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
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
#include "../gui/livewin.h"
#define CHILD_RSS_CAP CAMP_CHILD_RSS_CAP
#define HUGE_STEPS "4611686018427387904"
#define LOCAL_MIN_FREE CAMP_LOCAL_MIN_FREE
#define SHED_FREE CAMP_SHED_FREE
#define HARD_MIN_FREE CAMP_HARD_MIN_FREE

typedef struct { char path[1100]; unsigned frames; unsigned long size; } Movie;
typedef struct {
    int kind;                      // 1 movie replay, 2 snapshot exploration
    int movie;                     // index in the movies array
    char snap[1100];               // kind 2: the snapshot file
    unsigned key;                  // kind 2: coarse-to-fine rank
    pid_t pid; time_t t0; size_t rss_peak;
    char dir[1100], live[1150], log[1150], rep[1150], shot[1150], lfr[1150];
    int done; time_t over_t;       // when the RSS ceiling was first hit (SIGINT, then SIGKILL after 15 s)
} Task;

static volatile sig_atomic_t g_stop;
static void on_int(int s) { (void)s; g_stop = 1; }

static uint64_t meminfo(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r"); char l[256]; uint64_t v = 0;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, key, strlen(key))) { v = strtoull(l + strlen(key), NULL, 10) * 1024ull; break; }
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
static void mkdirs(const char *path) {
    char t[1100]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
    mkdir(t, 0777);
}
static const char *bulk_base(const char *local, const char *overflow) {   // where the next heavy data goes
    if (free_bytes(local) >= LOCAL_MIN_FREE || !overflow) return local;
    return free_bytes(overflow) > free_bytes(local) ? overflow : local;
}

static int cmp_movie(const void *a, const void *b) {
    const Movie *x = a, *y = b;
    return x->frames < y->frames ? 1 : x->frames > y->frames ? -1 : strcmp(x->path, y->path);
}
static int cmp_task(const void *a, const void *b) {
    const Task *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->movie - y->movie;
}

// the movies of the ROM, from the TASVideos-backed database (sync on demand, fetch the files): the `tasdb find
// ROM --fetch` listing gives "game N" and one "  <id>M|S ... N frames" line per publication/submission; the files
// live in <db>/movies/<game>/<id>/
static size_t fetch_movies(const char *exe, const char *rom, Movie **out) {
    size_t n = 0, cap = 0; *out = NULL;
    char db[1100]; snprintf(db, sizeof db, "%s", exe); { char *sl = strrchr(db, '/'); if (sl) *sl = 0; }
    for (int attempt = 0; attempt < 2 && !n; attempt++) {
        if (attempt) {
            char c[2400]; snprintf(c, sizeof c, "'%s' tasdb sync --systems GB,GBC >&2", exe);
            if (system(c)) break;
        }
        char c[2400]; snprintf(c, sizeof c, "'%s' tasdb find '%s' --fetch 2>&1", exe, rom);
        FILE *p = popen(c, "r"); char *line = NULL; size_t lc = 0; unsigned game = 0, ver = 0; char mname[300] = "";   // ver: the ROM's own version, other versions' movies do not replay on it
        while (p && getline(&line, &lc, p) > 0) {
            unsigned g; if (sscanf(line, "game %u", &g) == 1 || sscanf(line, "ROM matches game %u", &g) == 1) game = g;
            { char *a = strstr(line, "ROM matches game "), *b = a ? strchr(a, '(') : NULL, *e = b ? strstr(b, ", version ") : NULL;
              if (e && (size_t)(e - b - 1) < sizeof mname) { snprintf(mname, sizeof mname, "%.*s", (int)(e - b - 1), b + 1); } }
            { unsigned v; char nm[300]; if (mname[0] && sscanf(line, " version %u: %299[^,]", &v, nm) == 2 && !strcmp(nm, mname)) ver = v; }
            char id[32]; unsigned frames = 0;
            char *fr = strstr(line, " frames");
            if (!game || !fr || sscanf(line, " %31s", id) != 1 || !strchr("MS", id[strlen(id) - 1]) || id[0] < '0' || id[0] > '9') continue;
            char *q = fr; while (q > line && q[-1] >= '0' && q[-1] <= '9') q--;
            frames = (unsigned)strtoul(q, NULL, 10);
            if (ver) { char tag[24]; snprintf(tag, sizeof tag, "[v%u]", ver); if (!strstr(line, tag)) continue; }
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
            int dup = 0; for (size_t i = 0; i < n; i++) if ((*out)[i].frames == frames && (*out)[i].size == (unsigned long)bsz) dup = 1;   // the same movie as publication and submission
            if (dup) continue;
            if (n == cap) { cap = cap ? cap * 2 : 16; *out = realloc(*out, cap * sizeof **out); }
            snprintf((*out)[n].path, sizeof (*out)[n].path, "%s", best); (*out)[n].frames = frames; (*out)[n].size = (unsigned long)bsz; n++;
        }
        free(line); if (p) pclose(p);
    }
    if (n) qsort(*out, n, sizeof **out, cmp_movie);
    return n;
}

static void copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL; char buf[1 << 16]; size_t n;
    if (b) while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a); if (b) fclose(b);
}

static pid_t spawn(const char *exe, char **av, const char *log) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log, 1 | 64 | 512, 0666);   // O_WRONLY|O_CREAT|O_TRUNC
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        setpgid(0, 0);
        execv(exe, av);
        _exit(127);
    }
    return pid;
}

typedef struct { uint8_t *u; size_t n; } Union;
static size_t merge_flags(Union *un, const char *path) {   // header "COV1" + size, then one flag byte per ROM index
    FILE *f = fopen(path, "rb"); uint32_t hd[2];
    if (!f) return 0;
    if (fread(hd, 4, 2, f) != 2 || hd[0] != 0x31564F43u || !hd[1] || (un->n && hd[1] != un->n)) { fclose(f); return 0; }
    if (!un->n) { un->u = calloc(hd[1], 1); if (!un->u) { fclose(f); return 0; } un->n = hd[1]; }   // the kernel's compact ROM size, learned from the first child
    uint8_t *b = malloc(un->n); size_t added = 0;
    if (b && fread(b, 1, un->n, f) == un->n)
        for (size_t i = 0; i < un->n; i++) { uint8_t v = b[i] & ~un->u[i]; if (v) { if (!un->u[i]) added++; un->u[i] |= b[i]; } }
    free(b); fclose(f);
    return added;
}
static size_t union_count(const Union *un) { size_t c = 0; for (size_t i = 0; i < un->n; i++) c += (un->u[i] & 3) != 0; return c; }

static void write_png(const Union *un, const char *png, const char *id, const char *name) {
    if (!un->n) return;
    DpCoreResult r; memset(&r, 0, sizeof r);
    r.rom_size = un->n; r.flags = un->u;
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
static void draw_live(const Union *un, const char *name, time_t t_start, Task *tk, size_t nt, const Movie *mv, uint32_t *sw, uint32_t *sh_) {
    enum { INFO = 22, GAP = 4, MAXW = 1900, MAXH = 1000 };
    static LiveFrame lf[64]; size_t shown[64], ns = 0;
    for (size_t i = 0; i < nt && ns < 64; i++) {
        Task *t = &tk[i]; if (t->kind != 1 || !t->pid || t->done || !t->lfr[0]) continue;
        if (read_live_frame(t->lfr, &lf[ns])) { if (!*sw) { *sw = lf[ns].w; *sh_ = lf[ns].h; } shown[ns++] = i; }
    }
    uint32_t w0 = *sw ? *sw : 160, h0 = *sh_ ? *sh_ : 144;
    int z = 2, cols = 0, rows = 0, cw, chh;
    for (; z >= 1; z--) {   // largest zoom whose grid fits the display budget
        cw = (int)w0 * z + GAP; chh = (int)h0 * z + INFO + GAP;
        int n = 4 + (int)ns; cols = 2; rows = 2;
        while (cols * rows < n) { if (cols * cw + cw <= MAXW && (cols <= rows || (rows + 1) * chh > MAXH)) cols++; else rows++; }
        if (cols * cw <= MAXW && rows * chh <= MAXH) break;
    }
    if (z < 1) { z = 1; cw = (int)w0 + GAP; chh = (int)h0 + INFO + GAP; }
    DpCanvas *c = dp_canvas_new(cols * cw + GAP, rows * chh + GAP, 0xff15151au); if (!c) return;
    // coverage tile (2x2 cells)
    size_t cov = union_count(un); char l[200];
    int cx = GAP, cy = GAP, cwid = 2 * cw - GAP, chei = 2 * chh - GAP;
    size_t block = un->n ? dp_canvas_covmap(c, cx, cy, cwid, chei - INFO, un->u, un->n, 0x4000) : 0;
    long el = (long)(time(NULL) - t_start);
    snprintf(l, sizeof l, "%s  coverage %.1f%%  %zu/%zu B  time %02ld:%02ld:%02ld", name, un->n ? 100.0 * (double)cov / (double)un->n : 0.0, cov, un->n, el / 3600, el / 60 % 60, el % 60);
    dp_canvas_text(c, cx + 2, cy + chei - INFO + 3, l, 1, 0xff80e090u);
    snprintf(l, sizeof l, "1 square = %zu bytes (never 1 px = 1 byte unless the ROM is tiny)", block);
    dp_canvas_text(c, cx + 2, cy + chei - INFO + 13, l, 1, 0xffa0a0a8u);
    // movie tiles fill the cells the coverage tile leaves free
    size_t k = 0;
    for (int r = 0; r < rows && k < ns; r++) for (int q = 0; q < cols && k < ns; q++) {
        if (r < 2 && q < 2) continue;
        int tx = GAP + q * cw, ty = GAP + r * chh; LiveFrame *f = &lf[k]; Task *t = &tk[shown[k]]; k++;
        dp_canvas_rect(c, tx, ty, (int)w0 * z, (int)h0 * z + INFO, 0xff0c0c10u);
        dp_canvas_tile(c, tx, ty, (int)w0 * z, (int)h0 * z, f->px, (int)f->w, (int)f->h);
        const char *b = strrchr(mv[t->movie].path, '/'); b = b ? b + 1 : mv[t->movie].path;
        snprintf(l, sizeof l, "%.28s", b); dp_canvas_text(c, tx + 2, ty + (int)h0 * z + 3, l, 1, 0xffc8c8d0u);
        snprintf(l, sizeof l, "%llu/%u  %u%%", (unsigned long long)f->frame, f->total, f->total ? (unsigned)(100ull * f->frame / f->total) : 0u);
        dp_canvas_text(c, tx + 2, ty + (int)h0 * z + 13, l, 1, 0xff96c8ffu);
    }
    dp_livewin_show(c, "deepeel - live coverage");
    dp_canvas_free(c);
    for (size_t i = 0; i < ns; i++) { free(lf[i].px); lf[i].px = NULL; }
}

static unsigned bitrev(unsigned v) { unsigned r = 0; for (int i = 0; i < 16; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int snap_frame(const char *n) { return n[0] == 's' ? atoi(n + 1) : -1; }
static int cmp_snap(const void *a, const void *b) { return snap_frame(*(char *const *)a) - snap_frame(*(char *const *)b); }

int dp_auto_campaign(const char *exe, const char *rom, const char *overflow, size_t rom_size, const char *outdir, const char *name) {
    signal(SIGINT, on_int); signal(SIGTERM, on_int);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 1;
    long cap = ncpu > 3 ? ncpu - 2 : 1;   // leave two cores to the machine
    uint64_t swap0 = swap_used();
    char local[1100]; snprintf(local, sizeof local, "%s/data", outdir); mkdirs(local);
    if (overflow) mkdirs(overflow);
    fprintf(stderr, "deepeel auto: %s | %ld cpu -> up to %ld workers | RAM %llu MiB available | swap in use %llu MiB (never grown) | disk local %llu GiB free%s%s\n",
            name, ncpu, cap, (unsigned long long)(meminfo("MemAvailable:") >> 20), (unsigned long long)(swap0 >> 20),
            (unsigned long long)(free_bytes(local) >> 30), overflow ? ", overflow " : "", overflow ? overflow : "");
    Movie *mv; size_t nm = fetch_movies(exe, rom, &mv);
    fprintf(stderr, "deepeel auto: %zu movie(s) for this ROM%s\n", nm, nm ? ", longest first" : " (no TAS: exploration from the power-on state only)");

    Union un = { rom_size ? calloc(rom_size, 1) : NULL, rom_size };
    Task *tk = calloc(nm + 4096 + 1, sizeof *tk); size_t nt = 0;
    for (size_t m = 0; m < nm; m++) { Task *t = &tk[nt++]; t->kind = 1; t->movie = (int)m; }   // movie replays, longest first
    size_t movie_next = nm;
    char **seen = NULL; size_t nseen = 0, seencap = 0;
    size_t rss_est = 600ull << 20, last_total = 0; int no_gain = 0, explore_done = 0;
    time_t t_start = time(NULL), t_log = 0, t_png = time(NULL), t_win = 0; uint32_t scr_w = 0, scr_h = 0; int win_on = !getenv("DEEPEEL_NO_WINDOW");
    char png[1200]; snprintf(png, sizeof png, "%s/%s.map.png", outdir, name);
    char rep_all[1200]; snprintf(rep_all, sizeof rep_all, "%s/movies.txt", outdir);
    FILE *repf = fopen(rep_all, "w");
    int stopping = 0; unsigned killed_big = 0; time_t last_shed = 0;
    size_t pending_explore_from = 0;   // tasks [pending_explore_from..nt) of kind 2 not yet started
    (void)pending_explore_from;

    for (;;) {
        // 1. reap
        int running = 0, running_x = 0;
        for (size_t i = 0; i < nt; i++) {
            Task *t = &tk[i];
            if (!t->pid || t->done) continue;
            size_t rss = proc_rss(t->pid); if (rss > t->rss_peak) t->rss_peak = rss;
            int st; pid_t w = waitpid(t->pid, &st, WNOHANG);
            if (w == t->pid) {
                t->done = 1;
                if (t->rss_peak > rss_est && t->rss_peak <= CHILD_RSS_CAP) rss_est = t->rss_peak;
                char sc[1250]; snprintf(sc, sizeof sc, "%s/state/coverage.bin", t->dir);
                size_t a = merge_flags(&un, sc); a += merge_flags(&un, t->live);
                if (t->kind == 1) {
                    char rl[400] = ""; FILE *rf = fopen(t->rep, "r"); if (rf) { if (!fgets(rl, sizeof rl, rf)) rl[0] = 0; fclose(rf); }
                    size_t L = strlen(rl); if (L && rl[L - 1] == '\n') rl[L - 1] = 0;
                    int stalled = strstr(rl, "stalled=1") != NULL;
                    fprintf(stderr, "deepeel auto: movie %s %s [%s] +%zu bytes\n", mv[t->movie].path, stalled ? "STAGNATION (desynced or incompatible)" : "done", rl, a);
                    if (t->shot[0]) {   // <out>/summaries/<movie>.png: the replay at a glance
                        char sd[1300], dst[1500]; snprintf(sd, sizeof sd, "%s/summaries", outdir); mkdirs(sd);
                        const char *b = strrchr(mv[t->movie].path, '/'); snprintf(dst, sizeof dst, "%s/%s.png", sd, b ? b + 1 : mv[t->movie].path);
                        copy_file(t->shot, dst);
                    }
                    if (repf) { fprintf(repf, "%s\t%u\t%s\t%s\n", mv[t->movie].path, mv[t->movie].frames, stalled ? "stagnation" : (WIFEXITED(st) && !WEXITSTATUS(st) ? "ok" : "failed"), rl); fflush(repf); }
                } else {
                    explore_done++;
                    no_gain = a ? 0 : no_gain + 1;
                }
                continue;
            }
            running++; running_x += t->kind == 2;
        }
        // 2. collect snapshots not yet scheduled (coarse-to-fine order inside each movie)
        size_t first_new = nt;
        for (size_t m = 0; m < nm && !stopping; m++) {
            char sd[1250];
            for (size_t i = 0; i < nt; i++) if (tk[i].kind == 1 && (size_t)tk[i].movie == m) { snprintf(sd, sizeof sd, "%s/snaps", tk[i].dir); goto have; }
            continue;
        have:;
            DIR *d = opendir(sd); if (!d) continue;
            char **names = NULL; size_t nn = 0, nc = 0; struct dirent *de;
            while ((de = readdir(d))) if (de->d_name[0] == 's' && strstr(de->d_name, ".state") && !strstr(de->d_name, ".tmp")) {
                if (nn == nc) { nc = nc ? nc * 2 : 64; names = realloc(names, nc * sizeof *names); }
                names[nn++] = strdup(de->d_name);
            }
            closedir(d);
            qsort(names, nn, sizeof *names, cmp_snap);
            // a snapshot is final once the next one exists (or its movie finished): the last one may still be written
            int fin = 0; for (size_t i = 0; i < nt; i++) if (tk[i].kind == 1 && (size_t)tk[i].movie == m && tk[i].done) fin = 1;
            for (size_t i = 0; i < nn; i++) {
                if (i + 1 == nn && !fin) continue;
                char full[1400]; snprintf(full, sizeof full, "%s/%s", sd, names[i]);
                int dup = 0; for (size_t s = 0; s < nseen && !dup; s++) dup = !strcmp(seen[s], full);
                if (dup) continue;
                if (nseen == seencap) { seencap = seencap ? seencap * 2 : 256; seen = realloc(seen, seencap * sizeof *seen); }
                seen[nseen++] = strdup(full);
                if (nt >= nm + 4096) continue;
                Task *t = &tk[nt++]; memset(t, 0, sizeof *t);
                t->kind = 2; t->movie = (int)m; snprintf(t->snap, sizeof t->snap, "%s", full); t->key = bitrev((unsigned)i);
            }
            for (size_t i = 0; i < nn; i++) free(names[i]);
            free(names);
        }
        if (nt > first_new) qsort(tk + first_new, nt - first_new, sizeof *tk, cmp_task);
        (void)cmp_str;

        // 3. resources, then start what fits
        uint64_t avail = meminfo("MemAvailable:"), tot = meminfo("MemTotal:");
        uint64_t reserve = tot / 8 > (1500ull << 20) ? tot / 8 : (1500ull << 20);
        int swap_grew = swap_used() > swap0 + (128ull << 20);
        const char *base = bulk_base(local, overflow);
        uint64_t dfree = free_bytes(base);
        if (dfree < HARD_MIN_FREE && !stopping) { fprintf(stderr, "deepeel auto: disk full (%llu MiB free): clean stop, nothing is lost\n", (unsigned long long)(dfree >> 20)); stopping = 1; }
        if (dfree < SHED_FREE && dfree >= HARD_MIN_FREE && !stopping && time(NULL) - last_shed >= 90) {   // disk pressure: finish the child with the biggest spool (frontier saved, coverage merged at reap)
            size_t bi = (size_t)-1; uint64_t bs = 0;
            for (size_t i = 0; i < nt; i++) if (tk[i].pid && !tk[i].done && tk[i].kind == 2) { uint64_t sp = proc_spool(tk[i].pid); if (sp > bs) { bs = sp; bi = i; } }
            if (bi != (size_t)-1) {
                kill(-tk[bi].pid, SIGINT); last_shed = time(NULL);
                fprintf(stderr, "deepeel auto: disk %llu GiB free: %s (spool %llu MiB) stopped, frontier saved\n", (unsigned long long)(dfree >> 30), tk[bi].snap, (unsigned long long)(bs >> 20));
            }
        }
        if ((avail < reserve / 2 || swap_grew) && running > 1) {   // memory pressure: stop the newest child (it is re-run later from its start), never swap
            for (size_t i = nt; i-- > 0;) if (tk[i].pid && !tk[i].done) {
                kill(-tk[i].pid, SIGINT); fprintf(stderr, "deepeel auto: RAM %llu MiB available%s: %s stopped, re-queued\n", (unsigned long long)(avail >> 20), swap_grew ? ", swap growing" : "", tk[i].kind == 1 ? mv[tk[i].movie].path : tk[i].snap);
                waitpid(tk[i].pid, NULL, 0); tk[i].done = 1;
                if (tk[i].rss_peak > rss_est) rss_est = tk[i].rss_peak;
                if (nt < nm + 4096) { Task *r = &tk[nt++]; memset(r, 0, sizeof *r); r->kind = tk[i].kind; r->movie = tk[i].movie; memcpy(r->snap, tk[i].snap, sizeof r->snap); r->key = tk[i].key; }
                running--; break;
            }
        }
        long room = cap - running;
        time_t tnow = time(NULL); int ramping = 0; uint64_t headroom = 0;   // memory the running children are still expected to take
        for (size_t i = 0; i < nt; i++) if (tk[i].pid && !tk[i].done) {
            ramping |= tnow - tk[i].t0 < 8;
            size_t cur = proc_rss(tk[i].pid); if (cur < rss_est) headroom += rss_est - cur;
        }
        int mem_ok = avail > reserve + rss_est + headroom && !ramping && !swap_grew && dfree > 2 * LOCAL_MIN_FREE / 4;
        for (; room > 0 && mem_ok && !g_stop && !stopping; room--) {
            Task *t = NULL;
            for (size_t i = 0; i < nt && !t; i++) if (tk[i].kind == 1 && !tk[i].pid && !tk[i].done) t = &tk[i];   // movies first
            for (size_t i = 0; i < nt && !t; i++) if (tk[i].kind == 2 && !tk[i].pid && !tk[i].done) t = &tk[i];
            if (!t && nm == 0 && !explore_done && !running) {   // no movie: one exploration from power-on
                t = &tk[nt++]; memset(t, 0, sizeof *t); t->kind = 2; t->movie = 0; t->snap[0] = 0;
            }
            if (t && t->kind == 2 && no_gain >= (cap * 2 > 8 ? cap * 2 : 8)) t = NULL;   // the union stagnates over a whole wave of explorations
            if (!t) break;
            char id[64]; snprintf(id, sizeof id, "%s%zu", t->kind == 1 ? "m" : "x", (size_t)(t - tk));
            snprintf(t->dir, sizeof t->dir, "%s/%s", base, id); mkdirs(t->dir);
            snprintf(t->live, sizeof t->live, "%s/live.bin", t->dir); snprintf(t->log, sizeof t->log, "%s/log.txt", t->dir);
            snprintf(t->rep, sizeof t->rep, "%s/report.txt", t->dir);
            char sdir[1250], snaps[1250], jobs[8] = "1", spool_arg[32];
            {   // disk budget per child: what is free beyond the shed line, shared by the workers, 2 GiB at least
                uint64_t fb = free_bytes(base), per = fb > SHED_FREE ? (fb - SHED_FREE) / (cap ? cap : 1) : 0;
                snprintf(spool_arg, sizeof spool_arg, "%llu", (unsigned long long)(per > CAMP_SPOOL_MIN ? per : CAMP_SPOOL_MIN));
            }
            snprintf(sdir, sizeof sdir, "%s/state", t->dir); snprintf(snaps, sizeof snaps, "%s/snaps", t->dir);
            char *av[48]; int n = 0;
            av[n++] = (char *)exe; av[n++] = "explore"; av[n++] = (char *)rom;
            av[n++] = "--jobs"; av[n++] = jobs;
            av[n++] = "--max-total"; av[n++] = HUGE_STEPS; av[n++] = "--queue-max"; av[n++] = t->kind == 1 ? CAMP_STR(CAMP_QUEUE_MOVIE) : CAMP_STR(CAMP_QUEUE_EXPLORE);   // RAM share of one child: the rest spills to its disk spool
            av[n++] = "--state-dir"; av[n++] = sdir; av[n++] = "--no-frontier";
            av[n++] = "--spool-max"; av[n++] = spool_arg;
            av[n++] = "--live-flags"; av[n++] = t->live;
            if (t->kind == 1) {
                mkdirs(snaps);
                av[n++] = "--tas"; av[n++] = mv[t->movie].path; av[n++] = "--replay-only";
                av[n++] = "--snap-every"; av[n++] = CAMP_STR(CAMP_SNAP_EVERY); av[n++] = "--snap-dir"; av[n++] = snaps;
                av[n++] = "--tas-stall"; av[n++] = CAMP_STR(CAMP_MOVIE_STALL); av[n++] = "--tas-report"; av[n++] = t->rep;
                snprintf(t->shot, sizeof t->shot, "%s/summary.png", t->dir); av[n++] = "--tas-summary"; av[n++] = t->shot;
                snprintf(t->lfr, sizeof t->lfr, "%s/frame.bin", t->dir); av[n++] = "--live-frame"; av[n++] = t->lfr;   // one-glance picture of the replay
            } else {
                if (t->snap[0]) { av[n++] = "--from-state"; av[n++] = t->snap; }
                if (un.n && un.u) {   // seed the child with everything proved so far: it explores beyond the union, and its stagnation means "nothing new for the whole campaign"
                    mkdirs(sdir);
                    char cp[1300]; snprintf(cp, sizeof cp, "%s/coverage.bin", sdir);
                    FILE *cf = fopen(cp, "wb"); uint32_t hd[2] = { 0x31564F43u, (uint32_t)un.n };
                    if (cf) { int ok = fwrite(hd, 4, 2, cf) == 2 && fwrite(un.u, 1, un.n, cf) == un.n; if (fclose(cf) == 0 && ok) av[n++] = "--resume"; }
                }
                av[n++] = "--random"; av[n++] = CAMP_STR(CAMP_RANDOM_RUNS); av[n++] = "--random-stall"; av[n++] = CAMP_STR(CAMP_RANDOM_STALL);
                av[n++] = "--random-frames"; av[n++] = CAMP_STR(CAMP_RANDOM_MAX);
                av[n++] = "--stall-time"; av[n++] = CAMP_STR(CAMP_EXPLORE_STALL_SECS);
                if (CAMP_EXPLORE_MAX_SECS) { av[n++] = "--max-time"; av[n++] = CAMP_STR(CAMP_EXPLORE_MAX_SECS); }
            }
            av[n] = NULL;
            t->t0 = time(NULL); t->pid = spawn(exe, av, t->log);
            if (t->pid < 0) { t->pid = 0; t->done = 1; }
            else { running++; running_x += t->kind == 2; }
            break;   // one start per tick: the next child waits for this one to ramp up
        }

        // 4. monitor, periodic PNG
        time_t now = time(NULL);
        for (size_t i = 0; i < nt; i++) if (tk[i].pid && !tk[i].done) merge_flags(&un, tk[i].live);
        size_t total = union_count(&un);
        if (win_on && now - t_win >= CAMP_WINDOW_SECS) { t_win = now; draw_live(&un, name, t_start, tk, nt, mv, &scr_w, &scr_h); }
        if (now - t_log >= CAMP_LOG_SECS) {
            t_log = now;
            size_t queued = 0; for (size_t i = 0; i < nt; i++) queued += tk[i].kind == 2 && !tk[i].pid && !tk[i].done;
            fprintf(stderr, "[%ldm%02lds] ROM bytes covered %zu/%zu (%.1f%%) | workers %d/%ld (movies %zu/%zu started, explorations running %d, queued %zu, done %d) | RAM avail %llu MiB, child peak %zu MiB | swap +%lld MiB | disk %s %llu GiB free | stagnation streak %d\n",
                    (long)(now - t_start) / 60, (long)(now - t_start) % 60, total, un.n, un.n ? 100.0 * total / un.n : 0.0, running, cap, movie_next, nm, running_x, queued, explore_done,
                    (unsigned long long)(avail >> 20), rss_est >> 20, (long long)((int64_t)swap_used() - (int64_t)swap0) >> 20, base == local ? "local" : "overflow", (unsigned long long)(dfree >> 30), no_gain);
        }
        if (now - t_png >= CAMP_PNG_SECS || total != last_total) { if (now - t_png >= CAMP_PNG_SECS) { write_png(&un, png, "auto", name); t_png = now; } }
        last_total = total;
        // 5. end: nothing running and nothing left to start (or told to stop)
        int left = 0;
        for (size_t i = 0; i < nt; i++) if (!tk[i].done) left = 1;
        int more = movie_next < nm || (!stopping && !g_stop && no_gain < (cap * 2 > 8 ? cap * 2 : 8) && 0);
        if (g_stop || stopping) {
            for (size_t i = 0; i < nt; i++) if (tk[i].pid && !tk[i].done) kill(-tk[i].pid, SIGINT);
            if (!running) break;
        } else if (!left && !more) {
            // all current tasks done: pick up late snapshots once more, else finish
            int fresh = 0; for (size_t i = 0; i < nt; i++) fresh += tk[i].kind == 2 && !tk[i].pid && !tk[i].done;
            if (!fresh) break;
        } else if (!running && no_gain >= (cap * 2 > 8 ? cap * 2 : 8) && movie_next >= nm) break;
        for (int sl = 0; sl < 8; sl++) {   // fast RSS watch between two supervision ticks
            usleep(250000);
            for (size_t i = 0; i < nt; i++) if (tk[i].pid && !tk[i].done && !tk[i].over_t) {
                size_t rss = proc_rss(tk[i].pid); if (rss > tk[i].rss_peak) tk[i].rss_peak = rss;
                if (rss > CHILD_RSS_CAP) {
                    tk[i].over_t = time(NULL); kill(-tk[i].pid, SIGKILL); killed_big++;
                    fprintf(stderr, "deepeel auto: child over %llu MiB (%zu MiB), killed (coverage so far kept, not re-queued): %s\n", (unsigned long long)(CHILD_RSS_CAP >> 20), rss >> 20, tk[i].kind == 1 ? mv[tk[i].movie].path : tk[i].snap);
                }
            }
        }
    }
    for (size_t i = 0; i < nt; i++) if (tk[i].pid && !tk[i].done) { kill(-tk[i].pid, SIGINT); waitpid(tk[i].pid, NULL, 0); }
    for (size_t i = 0; i < nt; i++) if (tk[i].pid) { char sc[1250]; snprintf(sc, sizeof sc, "%s/state/coverage.bin", tk[i].dir); merge_flags(&un, sc); merge_flags(&un, tk[i].live); }
    size_t total = union_count(&un);
    write_png(&un, png, "auto", name);
    char cb[1200]; snprintf(cb, sizeof cb, "%s/coverage.bin", outdir);
    FILE *cf = fopen(cb, "wb");
    if (cf) { uint32_t hd[2] = { 0x31564F43u, (uint32_t)un.n }; fwrite(hd, 4, 2, cf); if (un.u) fwrite(un.u, 1, un.n, cf); fclose(cf); }
    if (repf) fclose(repf);
    fprintf(stderr, "deepeel auto: finished, %zu/%zu ROM bytes covered (%.1f%%), observations of the tool; map %s, movies report %s, merged coverage %s\n",
            total, un.n, un.n ? 100.0 * total / un.n : 0.0, png, rep_all, cb);
    free(un.u); free(mv); free(tk);
    return 0;
}
