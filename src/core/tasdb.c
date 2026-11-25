/* tasdb: local TASVideos database built from the TASVideos API (https://tasvideos.org/api/index.html).
 *   deepeel tasdb sync  [--systems A,B] [--dir D]         systems, games (with ROM hashes), publications, submissions
 *   deepeel tasdb emu   ROM FRAMES                         emulator of the movie with that frame count (mandatory for TAS tooling)
 *   deepeel tasdb find  ROM [--fetch] [--dir D]            match the ROM by MD5/SHA-1, list (and download) its movies
 *   deepeel tasdb fetch GAME_ID [--dir D]                  download every movie of a game (publications AND submissions)
 * The database lives in a `tasdb/` directory next to the executable (override with --dir or DEEPEEL_TASDB):
 *   systems.tsv           id, code, name
 *   games/<SYS>.tsv       game id, version id, md5, sha1, region, version, ROM file name, game name
 *   pubs/<SYS>.tsv        publication id, game id, version id, frames, rerecords, emulator, created, movie file, title
 *   subs/<SYS>.tsv        submission id, publication id, game id, version id, status, frames, rerecords, emulator,
 *                         created, extension, ROM name, title
 *   movies/<game>/<id>M|S/  downloaded movie files
 * TSV fields never contain tab or newline (replaced by spaces).  Everything downloaded is untrusted data. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "md5.h"
#include "sha1.h"

unsigned char *dp_tas_http(const char *url, size_t *len);
int dp_tas_unzip(const unsigned char *z, size_t n, const char *dir);

/* ---- minimal JSON DOM ---- */
typedef struct J { char t; char *s; double n; struct J *kids; char **keys; int nk, cap; } J;   /* t: o a s n b 0 */
static void jfree(J *j) { if (!j) return; for (int i = 0; i < j->nk; i++) { jfree(&j->kids[i]); free(j->keys ? j->keys[i] : 0); } free(j->kids); free(j->keys); free(j->s); }
static void jpush(J *p, char *key, J v) {
    if (p->nk == p->cap) { p->cap = p->cap ? p->cap * 2 : 8; p->kids = realloc(p->kids, (size_t)p->cap * sizeof *p->kids); p->keys = realloc(p->keys, (size_t)p->cap * sizeof *p->keys); }
    p->kids[p->nk] = v; p->keys[p->nk++] = key;
}
static void jws(const char **p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++; }
static char *jstr(const char **p) {   /* at opening quote; handles \uXXXX (BMP -> UTF-8) */
    size_t cap = 32, n = 0; char *o = malloc(cap); (*p)++;
    while (**p && **p != '"') {
        unsigned c = (unsigned char)*(*p)++;
        if (c == '\\') { c = (unsigned char)*(*p)++;
            switch (c) { case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'u': { char h[5] = {0}; memcpy(h, *p, 4); *p += 4; c = (unsigned)strtoul(h, 0, 16); break; } default: break; } }
        if (n + 4 >= cap) o = realloc(o, cap *= 2);
        if (c < 0x80) o[n++] = (char)c;
        else if (c < 0x800) { o[n++] = (char)(0xC0 | c >> 6); o[n++] = (char)(0x80 | (c & 63)); }
        else { o[n++] = (char)(0xE0 | c >> 12); o[n++] = (char)(0x80 | ((c >> 6) & 63)); o[n++] = (char)(0x80 | (c & 63)); }
    }
    if (**p == '"') (*p)++;
    o[n] = 0; return o;
}
static int jval(const char **p, J *out, int depth) {
    memset(out, 0, sizeof *out); jws(p);
    if (depth > 64) return 0;
    if (**p == '{' || **p == '[') {
        char close = **p == '{' ? '}' : ']'; out->t = **p == '{' ? 'o' : 'a'; (*p)++; jws(p);
        if (**p == close) { (*p)++; return 1; }
        for (;;) { char *k = 0; jws(p);
            if (out->t == 'o') { if (**p != '"') return 0; k = jstr(p); jws(p); if (*(*p)++ != ':') { free(k); return 0; } }
            J v; if (!jval(p, &v, depth + 1)) { free(k); return 0; }
            jpush(out, k, v); jws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == close) { (*p)++; return 1; }
            return 0; }
    }
    if (**p == '"') { out->t = 's'; out->s = jstr(p); return 1; }
    if (!strncmp(*p, "true", 4)) { out->t = 'b'; out->n = 1; *p += 4; return 1; }
    if (!strncmp(*p, "false", 5)) { out->t = 'b'; *p += 5; return 1; }
    if (!strncmp(*p, "null", 4)) { out->t = '0'; *p += 4; return 1; }
    char *e; out->n = strtod(*p, &e); if (e == *p) return 0; out->t = 'n'; *p = e; return 1;
}
static J *jget(const J *o, const char *k) { if (o->t == 'o') for (int i = 0; i < o->nk; i++) if (!strcmp(o->keys[i], k)) return &o->kids[i]; return 0; }
static const char *jtext(const J *o, const char *k) { J *v = jget(o, k); return v && v->t == 's' ? v->s : ""; }
static long jint(const J *o, const char *k) { J *v = jget(o, k); return v && (v->t == 'n') ? (long)v->n : 0; }

/* ---- directories ---- */
static char root[PATH_MAX];
static void mk(const char *sub) { char p[PATH_MAX + 64]; snprintf(p, sizeof p, "%s%s%s", root, *sub ? "/" : "", sub); mkdir(p, 0755); }
static void set_root(const char *dir) {
    if (dir) { snprintf(root, sizeof root, "%s", dir); }
    else if (getenv("DEEPEEL_TASDB")) snprintf(root, sizeof root, "%s", getenv("DEEPEEL_TASDB"));
    else { char exe[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) { exe[n] = 0; char *s = strrchr(exe, '/'); if (s) *s = 0; snprintf(root, sizeof root, "%s/tasdb", exe); } else snprintf(root, sizeof root, "tasdb"); }
    mk(""); mk("games"); mk("pubs"); mk("subs"); mk("movies");
}
static void clean(const char *s, FILE *f) { for (; *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f); }
static void field(FILE *f, const char *s) { clean(s, f); fputc('\t', f); }

/* One API page. Returns the parsed array (caller jfree) or 0. */
static int api_page(const char *path, const char *query, int page, J *out, char **buf) {
    char url[1024]; size_t n;
    snprintf(url, sizeof url, "https://tasvideos.org/api/v1/%s?%sPageSize=100&CurrentPage=%d", path, query, page);
    unsigned char *b = dp_tas_http(url, &n); if (!b) return 0;
    b = realloc(b, n + 1); b[n] = 0; const char *p = (char *)b;
    if (!jval(&p, out, 0) || out->t != 'a') { free(b); return 0; }
    *buf = (char *)b; return 1;
}

static int sync_system(const char *code) {
    char path[PATH_MAX + 64], q[160]; FILE *f; int total;
    snprintf(q, sizeof q, "Systems=%s&", code);
    /* games + versions (ROM hashes) */
    snprintf(path, sizeof path, "%s/games/%s.tsv.part", root, code); f = fopen(path, "w"); if (!f) return 0; total = 0;
    for (int page = 1;; page++) {
        J a; char *b; if (!api_page("games", q, page, &a, &b)) { fclose(f); fprintf(stderr, "  %s games page %d failed\n", code, page); return 0; }
        for (int i = 0; i < a.nk; i++) { J *g = &a.kids[i]; long gid = jint(g, "id"); J *vs = jget(g, "versions"); if (gid < 0 || !vs) continue;
            for (int k = 0; k < vs->nk; k++) { J *v = &vs->kids[k];
                fprintf(f, "%ld\t%ld\t", gid, jint(v, "id")); field(f, jtext(v, "md5")); field(f, jtext(v, "sha1")); field(f, jtext(v, "region"));
                field(f, jtext(v, "version")); field(f, jtext(v, "name")); clean(jtext(g, "displayName"), f); fputc('\n', f); } }
        total += a.nk; int last = a.nk < 100; jfree(&a); free(b); if (last) break;
    }
    fclose(f); char fin[PATH_MAX + 64]; snprintf(fin, sizeof fin, "%s/games/%s.tsv", root, code); rename(path, fin);
    fprintf(stderr, "  %s: %d game(s)\n", code, total);
    /* publications */
    snprintf(path, sizeof path, "%s/pubs/%s.tsv.part", root, code); f = fopen(path, "w"); if (!f) return 0; total = 0;
    snprintf(q, sizeof q, "Systems=%s&ShowObsoleted=true&", code);
    for (int page = 1;; page++) {
        J a; char *b; if (!api_page("publications", q, page, &a, &b)) { fclose(f); fprintf(stderr, "  %s publications page %d failed\n", code, page); return 0; }
        for (int i = 0; i < a.nk; i++) { J *p = &a.kids[i];
            fprintf(f, "%ld\t%ld\t%ld\t%ld\t%ld\t", jint(p, "id"), jint(p, "gameId"), jint(p, "gameVersionId"), jint(p, "frames"), jint(p, "rerecordCount"));
            field(f, jtext(p, "emulatorVersion")); field(f, jtext(p, "createTimestamp")); field(f, jtext(p, "movieFileName")); clean(jtext(p, "title"), f); fputc('\n', f); }
        total += a.nk; int last = a.nk < 100; jfree(&a); free(b); if (last) break;
    }
    fclose(f); snprintf(fin, sizeof fin, "%s/pubs/%s.tsv", root, code); rename(path, fin);
    fprintf(stderr, "  %s: %d publication(s)\n", code, total);
    /* submissions (every status) */
    snprintf(path, sizeof path, "%s/subs/%s.tsv.part", root, code); f = fopen(path, "w"); if (!f) return 0; total = 0;
    snprintf(q, sizeof q, "Systems=%s&", code);
    for (int page = 1;; page++) {
        J a; char *b; if (!api_page("submissions", q, page, &a, &b)) { fclose(f); fprintf(stderr, "  %s submissions page %d failed\n", code, page); return 0; }
        for (int i = 0; i < a.nk; i++) { J *s = &a.kids[i];
            fprintf(f, "%ld\t%ld\t%ld\t%ld\t", jint(s, "id"), jint(s, "publicationId"), jint(s, "gameId"), jint(s, "gameVersionId"));
            field(f, jtext(s, "status")); fprintf(f, "%ld\t%ld\t", jint(s, "frames"), jint(s, "rerecordCount"));
            field(f, jtext(s, "emulatorVersion")); field(f, jtext(s, "createTimestamp")); field(f, jtext(s, "movieExtension"));
            field(f, jtext(s, "romName")); clean(jtext(s, "title"), f); fputc('\n', f); }
        total += a.nk; int last = a.nk < 100; jfree(&a); free(b); if (last) break;
    }
    fclose(f); snprintf(fin, sizeof fin, "%s/subs/%s.tsv", root, code); rename(path, fin);
    fprintf(stderr, "  %s: %d submission(s)\n", code, total);
    return 1;
}

static int cmd_sync(const char *only) {
    char url[128], path[PATH_MAX + 64]; size_t n;
    snprintf(url, sizeof url, "https://tasvideos.org/api/v1/systems");
    unsigned char *b = dp_tas_http(url, &n); if (!b) { fprintf(stderr, "systems: request failed\n"); return 1; }
    b = realloc(b, n + 1); b[n] = 0; const char *p = (char *)b; J a;
    if (!jval(&p, &a, 0) || a.t != 'a') { fprintf(stderr, "systems: bad JSON\n"); free(b); return 1; }
    snprintf(path, sizeof path, "%s/systems.tsv", root); FILE *f = fopen(path, "w");
    for (int i = 0; i < a.nk; i++) { fprintf(f, "%ld\t", jint(&a.kids[i], "id")); field(f, jtext(&a.kids[i], "code")); clean(jtext(&a.kids[i], "displayName"), f); fputc('\n', f); }
    fclose(f); int fails = 0;
    for (int i = 0; i < a.nk; i++) { const char *code = jtext(&a.kids[i], "code");
        if (only) { int hit = 0; for (const char *s = only; *s; ) { size_t l = strcspn(s, ","); if (l == strlen(code) && !strncasecmp(s, code, l)) hit = 1; s += l + (s[l] ? 1 : 0); } if (!hit) continue; }
        fprintf(stderr, "%s\n", code); if (!sync_system(code)) fails++; }
    jfree(&a); free(b); fprintf(stderr, "database: %s%s\n", root, fails ? " (some systems failed, rerun)" : ""); return fails != 0;
}

/* Calls cb for each line of <sub>/SYS.tsv whose tab field `col` equals val (case-insensitive). */
typedef void (*line_cb)(const char *sys, const char *line, void *u);
static int scan(const char *sub, int col, const char *val, line_cb cb, void *u) {
    char d[PATH_MAX + 64]; snprintf(d, sizeof d, "%s/%s", root, sub); int hits = 0;
    DIR *dir = opendir(d); if (!dir) return 0; struct dirent *e;
    while ((e = readdir(dir))) { size_t l = strlen(e->d_name); if (l < 5 || strcmp(e->d_name + l - 4, ".tsv")) continue;
        char fp[PATH_MAX + 300]; snprintf(fp, sizeof fp, "%s/%s", d, e->d_name); FILE *f = fopen(fp, "r"); if (!f) continue;
        char sys[64]; snprintf(sys, sizeof sys, "%.*s", (int)(l - 4 < 63 ? l - 4 : 63), e->d_name); static char line[8192];
        while (fgets(line, sizeof line, f)) { const char *s = line;
            for (int c = 0; c < col && s; c++) { s = strchr(s, '\t'); if (s) s++; }
            if (!s) continue; size_t fl = strcspn(s, "\t\n");
            if (strlen(val) == fl && !strncasecmp(s, val, fl)) { cb(sys, line, u); hits++; } }
        fclose(f); }
    closedir(dir); return hits;
}
static const char *col_of(const char *line, int col, char *out, size_t cap) {
    const char *s = line; for (int c = 0; c < col && s; c++) { s = strchr(s, '\t'); if (s) s++; }
    if (!s) { out[0] = 0; return out; } size_t l = strcspn(s, "\t\n"); if (l >= cap) l = cap - 1; memcpy(out, s, l); out[l] = 0; return out;
}

typedef struct { long gids[64]; int n; const char *md5, *sha1; } Match;
static void on_game(const char *sys, const char *line, void *u) {
    Match *m = u; char gid[32], sha[64], md[64], name[256], ver[64]; (void)sys;
    col_of(line, 0, gid, sizeof gid); col_of(line, 3, sha, sizeof sha); col_of(line, 2, md, sizeof md);
    col_of(line, 6, name, sizeof name); col_of(line, 5, ver, sizeof ver);
    if (strcasecmp(sha, m->sha1) && strcasecmp(md, m->md5)) return;
    long g = atol(gid); for (int i = 0; i < m->n; i++) if (m->gids[i] == g) return;
    if (m->n < 64) m->gids[m->n++] = g;
    fprintf(stderr, "ROM matches game %ld (%s, version %s) [%s]\n", g, name, ver, sys);
}

static int download(long id, char suffix, long gid) {
    char dir[PATH_MAX + 128], url[200]; size_t n;
    snprintf(dir, sizeof dir, "%s/movies/%ld", root, gid); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/movies/%ld/%ld%c", root, gid, id, suffix);
    if (mkdir(dir, 0755) && errno == EEXIST) return 1;
    snprintf(url, sizeof url, "https://tasvideos.org/%ld%c?handler=Download", id, suffix);
    unsigned char *z = dp_tas_http(url, &n); if (!z) { fprintf(stderr, "  %ld%c: download failed\n", id, suffix); rmdir(dir); return 0; }
    int got = dp_tas_unzip(z, n, dir); free(z);
    if (!got) { fprintf(stderr, "  %ld%c: no movie in the archive\n", id, suffix); return 0; }
    return 1;
}
typedef struct { long gid; int fetch, np, ns; } Lst;
static void on_pub(const char *sys, const char *line, void *u) {
    Lst *l = u; char id[32], fr[32], rr[32], emu[64], date[40], file[128], title[256]; (void)sys;
    col_of(line, 0, id, sizeof id); col_of(line, 3, fr, sizeof fr); col_of(line, 4, rr, sizeof rr); col_of(line, 5, emu, sizeof emu);
    col_of(line, 6, date, sizeof date); col_of(line, 7, file, sizeof file); col_of(line, 8, title, sizeof title);
    char vid[32]; col_of(line, 2, vid, sizeof vid);
    printf("  %sM  [v%s]  %.10s  %-10s %s frames, %s rerecords  %s\n", id, vid, date, emu, fr, rr, title); l->np++;
    if (l->fetch) download(atol(id), 'M', l->gid);
}
static void on_sub(const char *sys, const char *line, void *u) {
    Lst *l = u; char id[32], st[40], fr[32], emu[64], date[40], ext[16], rom[200], title[256]; (void)sys;
    col_of(line, 0, id, sizeof id); col_of(line, 4, st, sizeof st); col_of(line, 5, fr, sizeof fr); col_of(line, 7, emu, sizeof emu);
    col_of(line, 8, date, sizeof date); col_of(line, 9, ext, sizeof ext); col_of(line, 10, rom, sizeof rom); col_of(line, 11, title, sizeof title);
    char vid[32]; col_of(line, 3, vid, sizeof vid);
    printf("  %sS  [v%s]  %.10s  %-10s %-10s %s frames  ROM \"%s\"  %s\n", id, vid, date, emu, st, fr, rom, title); l->ns++;
    if (l->fetch) download(atol(id), 'S', l->gid);
}
static void on_version(const char *sys, const char *line, void *u) {
    char vid[32], reg[16], ver[32], name[200]; (void)sys; (void)u;
    col_of(line, 1, vid, sizeof vid); col_of(line, 4, reg, sizeof reg); col_of(line, 5, ver, sizeof ver); col_of(line, 6, name, sizeof name);
    char sha[64]; col_of(line, 3, sha, sizeof sha);
    printf("  version %s: %s, region %s, %s, sha1 %s\n", vid, name, reg, ver, sha);
}
static int list_game(long gid, int fetch) {
    char g[32]; snprintf(g, sizeof g, "%ld", gid); Lst l = { gid, fetch, 0, 0 };
    printf("game %ld versions:\n", gid); scan("games", 0, g, on_version, 0);
    printf(" publications:\n"); scan("pubs", 1, g, on_pub, &l);
    printf(" submissions:\n"); scan("subs", 2, g, on_sub, &l);
    fprintf(stderr, "game %ld: %d publication(s), %d submission(s)%s\n", gid, l.np, l.ns, fetch ? " (movies in the database)" : "");
    return l.np + l.ns > 0;
}

/* `tasdb emu ROM FRAMES`: the single, mandatory source of the emulator a movie was made and verified on.  The game is
 * matched by ROM hash, then the publication/submission with the same frame count; stdout carries machine-readable
 * lines `emulator=BizHawk 2.3.2 kind=publication id=4015` (exit 0), nothing otherwise (exit 1: no match, 3: no database). */
typedef struct { long frames; int n; const char *id; } EmuQ;   /* id: NNNN[M|S] (the movie directory name in the database) matches without the frame count */
static void on_emu_pub(const char *sys, const char *line, void *u) {
    EmuQ *q = u; char id[32], fr[32], emu[64]; (void)sys;
    col_of(line, 0, id, sizeof id); col_of(line, 3, fr, sizeof fr); col_of(line, 5, emu, sizeof emu);
    int byid = q->id && !strcmp(q->id + strlen(q->id) - 1, "M") && !strncmp(q->id, id, strlen(q->id) - 1) && strlen(id) == strlen(q->id) - 1;
    if (atol(fr) == q->frames || byid) { printf("emulator=%s kind=publication id=%s\n", emu, id); q->n++; }
}
static void on_emu_sub(const char *sys, const char *line, void *u) {
    EmuQ *q = u; char id[32], fr[32], emu[64]; (void)sys;
    col_of(line, 0, id, sizeof id); col_of(line, 5, fr, sizeof fr); col_of(line, 7, emu, sizeof emu);
    int byid = q->id && !strcmp(q->id + strlen(q->id) - 1, "S") && !strncmp(q->id, id, strlen(q->id) - 1) && strlen(id) == strlen(q->id) - 1;
    if (atol(fr) == q->frames || byid) { printf("emulator=%s kind=submission id=%s\n", emu, id); q->n++; }
}
/* The games holding the ROM, by SHA1 then MD5 of the file; md / sh receive the identity that matched.  TASVideos
 * hashes NES dumps with an iNES 1.0 header: a file whose header differs (NES 2.0 fields, trailing bytes) is tried
 * again with bytes 8-15 cleared. */
static Match match_rom(uint8_t *d, size_t sz, char md[33], char sh[41]) {
    for (int pass = 0;; pass++) {
        uint8_t m[16], s[20]; dp_md5(d, sz, m); dp_sha1(d, sz, s);
        for (int i = 0; i < 16; i++) sprintf(md + 2 * i, "%02x", m[i]); for (int i = 0; i < 20; i++) sprintf(sh + 2 * i, "%02x", s[i]);
        Match mt = { {0}, 0, md, sh }; scan("games", 3, sh, on_game, &mt);
        if (!mt.n) scan("games", 2, md, on_game, &mt);
        if (mt.n || pass || sz < 16 || memcmp(d, "NES\x1a", 4)) return mt;
        if ((d[7] & 0x0C) == 0x08) d[7] &= 0xF3;
        memset(d + 8, 0, 8);
    }
}

static int cmd_emu(const char *rom, long frames, const char *mid) {
    char sysf[PATH_MAX + 64]; snprintf(sysf, sizeof sysf, "%s/systems.tsv", root);
    if (access(sysf, R_OK)) { fprintf(stderr, "tasdb: no database in %s (run `deepeel tasdb sync` first)\n", root); return 3; }
    FILE *f = fopen(rom, "rb"); if (!f) { perror(rom); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz + 1); if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return 2; } fclose(f);
    char md[33], sh[41]; Match mt = match_rom(d, (size_t)sz, md, sh); free(d);
    EmuQ q = { frames, 0, mid };
    for (int i = 0; i < mt.n; i++) { char g[32]; snprintf(g, sizeof g, "%ld", mt.gids[i]); scan("pubs", 1, g, on_emu_pub, &q); scan("subs", 2, g, on_emu_sub, &q); }
    if (!q.n) fprintf(stderr, "tasdb: no publication/submission of this ROM has %ld frames\n", frames);
    return q.n ? 0 : 1;
}

static int cmd_find(const char *rom, int fetch) {
    setvbuf(stdout, NULL, _IOLBF, 0);   // callers merge both streams: whole lines only
    FILE *f = fopen(rom, "rb"); if (!f) { perror(rom); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz + 1); if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return 2; } fclose(f);
    char md[33], sh[41]; Match mt = match_rom(d, (size_t)sz, md, sh);
    fprintf(stderr, "ROM md5 %s sha1 %s\n", md, sh);   // the identity the database knows the ROM under (read by the campaign)
    if (!mt.n && sz % 1024 == 512) {   /* .smd: 512-byte header, interleaved; not the TASVideos hash */
        fprintf(stderr, "no match (a 512-byte header suggests an SMD dump: use a plain .bin/.md)\n"); }
    free(d);
    if (!mt.n) { fprintf(stderr, "no TASVideos game has this ROM hash (run `deepeel tasdb sync` first?)\n"); return 1; }
    for (int i = 0; i < mt.n; i++) list_game(mt.gids[i], fetch);
    return 0;
}

int dp_tasdb_command(int argc, char **argv) {
    const char *dir = 0, *systems = 0, *pos = 0, *pos2 = 0, *mid = 0; int fetch = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--systems") && i + 1 < argc) systems = argv[++i];
        else if (!strcmp(argv[i], "--id") && i + 1 < argc) mid = argv[++i];
        else if (!strcmp(argv[i], "--fetch")) fetch = 1;
        else if (!pos) pos = argv[i];
        else if (!pos2) pos2 = argv[i];
    }
    if (argc < 3) { fprintf(stderr, "usage: deepeel tasdb sync [--systems A,B] | emu ROM FRAMES [--id NNNN[M|S]] | find ROM [--fetch] | fetch GAME_ID   [--dir D]\n"); return 2; }
    set_root(dir);
    if (!strcmp(argv[2], "emu") && pos && pos2) return cmd_emu(pos, atol(pos2), mid);
    if (!strcmp(argv[2], "sync")) return cmd_sync(systems);
    if (!strcmp(argv[2], "find") && pos) return cmd_find(pos, fetch);
    if (!strcmp(argv[2], "fetch") && pos) return list_game(atol(pos), 1) ? 0 : 1;
    fprintf(stderr, "usage: deepeel tasdb sync [--systems A,B] | emu ROM FRAMES [--id NNNN[M|S]] | find ROM [--fetch] | fetch GAME_ID   [--dir D]\n"); return 2;
}
