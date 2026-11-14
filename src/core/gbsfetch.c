/* gbsfetch: fetch a game's GBS rip from gbs.joshw.info (the GB set of the
 * NSFE/SPC-style rip archive — the hcs64 community set maintained by JoshW;
 * the rips themselves are community contributions, the music data stays
 * © its original publishers).  The listing of https://gbs.joshw.info/<letter>/ is
 * scanned for a *.7z whose decoded, normalized name contains the game name, the archive
 * is downloaded and unpacked with the 7z binary, and the first *.gbs found is returned.
 * Pages come through `curl -sL` (retried: the site answers 503 under load), the same
 * pattern as tasfetch.c.  Downloaded data is untrusted: archives land in their own
 * sub-directory of the GBS directory and only basenames are read back after extraction.
 *   deepeel explore ROM --gbs-fetch [NAME] --gbs-dir DIR  (DIR defaults to output/gbs) */
#include "gbs.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define URL_MAX 1300   // a joshw listing href with its percent escapes

/* decode the HTML entities an Apache listing escapes (&amp; &lt; &gt; &quot;
 * &#39; &apos;); %XX escapes are left alone — the URL still needs them */
static size_t ent_decode(const char *in, char *out, size_t cap) {
    static const struct { const char *ent; char c; } ents[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"#39;", '\''}, {"apos;", '\''},
    };
    size_t k = 0;
    for (size_t i = 0; in[i] && k + 1 < cap; i++) {
        int done = 0;
        if (in[i] == '&')
            for (size_t e = 0; e < sizeof ents / sizeof *ents && !done; e++) {
                size_t el = strlen(ents[e].ent);
                if (!strncmp(in + i + 1, ents[e].ent, el)) {
                    out[k++] = ents[e].c;
                    i += el;
                    done = 1;
                }
            }
        if (!done) out[k++] = in[i];
    }
    out[k] = 0;
    return k;
}

static unsigned char *fetch(const char *url, size_t *len) {
    /* the rip names carry apostrophes (…Link's Awakening…): a raw ' inside the
     * single-quoted shell command closes it — requote the POSIX way (close,
     * escaped quote, reopen) */
    char q[4 * URL_MAX + 8];
    size_t j = 0;
    for (size_t i = 0; url[i] && j + 4 < sizeof q; i++) {
        if (url[i] == '\'') { q[j++] = '\''; q[j++] = '\\'; q[j++] = '\''; q[j++] = '\''; }
        else q[j++] = url[i];
    }
    q[j] = 0;
    char cmd[4 * URL_MAX + 128]; snprintf(cmd, sizeof cmd, "curl -sSL --fail -A deepeel-gbsfetch/1 --max-time 120 '%s'", q);
    for (int t = 0; t < 6; t++) {
        FILE *p = popen(cmd, "r"); if (!p) return 0;
        size_t cap = 1 << 16, n = 0, r; unsigned char *b = malloc(cap);
        while ((r = fread(b + n, 1, cap - n, p)) > 0) { n += r; if (n == cap) b = realloc(b, cap *= 2); }
        if (pclose(p) == 0 && n) { *len = n; return b; }
        free(b); sleep(2u * (t + 1));
    }
    return 0;
}

/* first *.gbs of dir, sorted readdir order; 0 when none */
static int first_gbs(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; char best[512] = "";
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".gbs") && (!best[0] || strcmp(e->d_name, best) < 0))
            snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(out, cap, "%s/%s", dir, best);
    return 1;
}

/* game name -> index segment: letters lowercase, digits land in the 0-9 dir */
static void letter_dir(const char *norm, char *out, size_t cap) {
    if (norm[0] >= '0' && norm[0] <= '9') snprintf(out, cap, "0-9");
    else snprintf(out, cap, "%c", norm[0] >= 'a' && norm[0] <= 'z' ? norm[0] : 'z');
}

int dp_gbs_fetch(const char *game, const char *dir, char *out_gbs, size_t cap, char *err, size_t errsz) {
    char norm[256]; dp_gbs_norm_name(game, norm, sizeof norm);
    if (!norm[0]) { snprintf(err, errsz, "empty game name"); return -1; }
    char sub[1024]; snprintf(sub, sizeof sub, "%s/%s", dir, norm);
    mkdir(dir, 0755);
    if (mkdir(sub, 0755) && errno != EEXIST) { snprintf(err, errsz, "cannot create %s", sub); return -1; }
    if (first_gbs(sub, out_gbs, cap)) return 0;   // already fetched
    char seg[8]; letter_dir(norm, seg, sizeof seg);
    char url[1300]; snprintf(url, sizeof url, "https://gbs.joshw.info/%s/", seg);
    size_t n; unsigned char *h = fetch(url, &n);
    if (!h) { snprintf(err, errsz, "listing %s failed", url); return -1; }
    h = realloc(h, n + 1); h[n] = 0;
    char href[1024];
    size_t hl = dp_gbs_listing_pick((char *)h, game, href, sizeof href);
    free(h);
    if (!hl) { snprintf(err, errsz, "no gbs rip of '%s' in %s", game, url); return -1; }
    char href2[1024]; ent_decode(href, href2, sizeof href2);   // &amp; in the listing
    char dec[1024]; dp_gbs_url_decode(href2, strlen(href2), dec, sizeof dec);
    const char *base = strrchr(dec, '/'); base = base ? base + 1 : dec;
    fprintf(stderr, "deepeel: gbs: downloading '%s'\n", base);
    char durl[1300]; snprintf(durl, sizeof durl, "https://gbs.joshw.info/%s/%s", seg, href2);
    unsigned char *z = fetch(durl, &n);
    if (!z) { snprintf(err, errsz, "download of %s failed", durl); return -1; }
    char arc[1040]; snprintf(arc, sizeof arc, "%s/rip.7z", sub);
    FILE *f = fopen(arc, "wb");
    if (!f || fwrite(z, 1, n, f) != n) { free(z); snprintf(err, errsz, "cannot write %s", arc); return -1; }
    fclose(f); free(z);
    char cmd[2200]; snprintf(cmd, sizeof cmd, "7z x -y -o'%s' '%s' '*.gbs'", sub, arc);
    int rc = system(cmd);
    if (rc != 0) { snprintf(err, errsz, "7z extraction of %s failed", arc); return -1; }
    unlink(arc);   // the rip stays: the .gbs only
    if (!first_gbs(sub, out_gbs, cap)) { snprintf(err, errsz, "no .gbs file in %s", base); return -1; }
    return 0;
}
