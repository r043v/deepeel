/* tasvideos_fetch: download every TASVideos movie (publications + submissions) of a game, in C.
 *   deepeel tasfetch GAME_ID|NNNG|URL OUT_DIR [--list] [--max N]
 * Pages come through `curl -sL` (retried: the site answers 503 under load); zip archives are unpacked with
 * zlib (stored/deflate).  Each movie goes in its own fresh sub-directory of OUT_DIR; an existing one is skipped.
 * Prints the --tas arguments for deepeel.  Downloads are untrusted data: only base names of known movie
 * extensions are written, never an archive path.  Linked into deepeel (zlib). */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static const char *EXT[] = {".vbm", ".bk2", ".gbm", ".fm2", ".gmv", ".fm3", ".mc2", ".mmv", ".r08", ".lsmv", 0};

static unsigned char *fetch(const char *url, size_t *len) {
    char cmd[1024]; snprintf(cmd, sizeof cmd, "curl -sSL --fail -A deepeel-tasfetch/3 --max-time 120 '%s'", url);
    for (int t = 0; t < 6; t++) {
        FILE *p = popen(cmd, "r"); if (!p) return 0;
        size_t cap = 1 << 16, n = 0, r; unsigned char *b = malloc(cap);
        while ((r = fread(b + n, 1, cap - n, p)) > 0) { n += r; if (n == cap) b = realloc(b, cap *= 2); }
        if (pclose(p) == 0 && n) { *len = n; return b; }
        free(b); sleep(2u * (t + 1));
    }
    return 0;
}

static int is_movie(const char *n) {
    size_t l = strlen(n);
    for (const char **e = EXT; *e; e++) { size_t k = strlen(*e); if (l >= k && !strcasecmp(n + l - k, *e)) return 1; }
    return 0;
}
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Unpack the movies of a zip held in memory into dir; appends their paths to stdout as --tas arguments. */
static int unzip_movies(const unsigned char *z, size_t n, const char *dir) {
    if (n < 22) return 0;
    size_t e = n - 22; while (e > 0 && le32(z + e) != 0x06054b50u) e--;
    if (le32(z + e) != 0x06054b50u) return 0;
    unsigned count = le16(z + e + 10); size_t c = le32(z + e + 16); int got = 0;
    for (unsigned i = 0; i < count && c + 46 <= n && le32(z + c) == 0x02014b50u; i++) {
        unsigned method = le16(z + c + 10), nl = le16(z + c + 28), xl = le16(z + c + 30), cl = le16(z + c + 32);
        uint32_t csz = le32(z + c + 20), usz = le32(z + c + 24); size_t lh = le32(z + c + 42);
        char name[512]; if (nl >= sizeof name) nl = sizeof name - 1; memcpy(name, z + c + 46, nl); name[nl] = 0;
        c += 46 + nl + xl + cl;
        const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
        if (!*base || !is_movie(base) || strstr(base, "..") || lh + 30 > n) continue;
        size_t data = lh + 30 + le16(z + lh + 26) + le16(z + lh + 28);
        if (data + csz > n || usz > (256u << 20)) continue;
        unsigned char *o = malloc(usz ? usz : 1); int ok = 0;
        if (method == 0 && csz == usz) { memcpy(o, z + data, usz); ok = 1; }
        else if (method == 8) { z_stream s = {0}; if (inflateInit2(&s, -15) == Z_OK) {
            s.next_in = (Bytef *)(z + data); s.avail_in = csz; s.next_out = o; s.avail_out = usz;
            ok = inflate(&s, Z_FINISH) == Z_STREAM_END; inflateEnd(&s); } }
        if (ok) { char fp[1024]; snprintf(fp, sizeof fp, "%s/%s", dir, base); FILE *f = fopen(fp, "wb");
            if (f) { fwrite(o, 1, usz, f); fclose(f); printf("--tas %s ", fp); got++; } }
        free(o);
    }
    return got;
}

/* Collect distinct numbers N of every href="/NNN<suffix>" in html. */
static int ids_of(const char *html, char suffix, long *ids, int max) {
    int k = 0;
    for (const char *p = html; (p = strstr(p, "href=\"/")); p += 7) {
        char *end; long v = strtol(p + 7, &end, 10);
        if (end == p + 7 || *end != suffix || end[1] != '"') continue;
        int dup = 0; for (int i = 0; i < k; i++) dup |= ids[i] == v;
        if (!dup && k < max) ids[k++] = v;
    }
    return k;
}

int dp_tasfetch_command(int argc, char **argv) {   /* argv: deepeel tasfetch GAME OUT [--list] [--max N] */
    const char *target = 0, *out = 0; int list = 0, mx = 1 << 30;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) mx = atoi(argv[++i]);
        else if (!target) target = argv[i]; else if (!out) out = argv[i];
    }
    if (!target || !out) { fprintf(stderr, "usage: deepeel tasfetch GAME_ID|NNNG|URL OUT_DIR [--list] [--max N]\n"); return 2; }
    const char *d = target; while (*d && !isdigit((unsigned char)*d)) d++;
    long gid = strtol(d, 0, 10); if (gid <= 0) { fprintf(stderr, "no game id in '%s'\n", target); return 2; }
    static long mids[4096], sids[4096]; char url[256]; size_t n;
    snprintf(url, sizeof url, "https://tasvideos.org/%ldG", gid);
    unsigned char *h = fetch(url, &n); int nm = 0, ns = 0;
    if (h) { h = realloc(h, n + 1); h[n] = 0; nm = ids_of((char *)h, 'M', mids, 4096); free(h); }
    else fprintf(stderr, "game page %ldG failed\n", gid);
    snprintf(url, sizeof url, "https://tasvideos.org/Subs-List?GameId=%ld", gid);
    h = fetch(url, &n);
    if (h) { h = realloc(h, n + 1); h[n] = 0; ns = ids_of((char *)h, 'S', sids, 4096); free(h); }
    else fprintf(stderr, "Subs-List failed\n");
    fprintf(stderr, "%d publication(s), %d submission(s)\n", nm, ns);
    if (list) { for (int i = 0; i < nm; i++) fprintf(stderr, "  %ldM\n", mids[i]); for (int i = 0; i < ns; i++) fprintf(stderr, "  %ldS\n", sids[i]); return 0; }
    mkdir(out, 0755);
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < (pass ? ns : nm) && mx > 0; i++, mx--) {
        long id = pass ? sids[i] : mids[i]; char dir[1024];
        snprintf(dir, sizeof dir, "%s/%ld%c", out, id, pass ? 'S' : 'M');
        if (mkdir(dir, 0755) && errno == EEXIST) { fprintf(stderr, "  %ld%c: already there, skipped\n", id, pass ? 'S' : 'M'); continue; }
        snprintf(url, sizeof url, "https://tasvideos.org/%ld%c?handler=Download", id, pass ? 'S' : 'M');
        unsigned char *z = fetch(url, &n);
        if (!z) { fprintf(stderr, "  %ld%c: download failed\n", id, pass ? 'S' : 'M'); continue; }
        if (!unzip_movies(z, n, dir)) fprintf(stderr, "  %ld%c: no movie in archive\n", id, pass ? 'S' : 'M');
        free(z);
    }
    printf("\n");
    return 0;
}
