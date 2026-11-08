#include "bk2.h"
#include "zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* parse a line-anchored "SHA1 <40 hex>" / "MD5 <32 hex>" identity line */
static void parse_identity(const uint8_t *h, size_t hn, DpBk2 *out) {
    for (size_t i = 0; i + 5 < hn; i++) {
        if ((i && h[i - 1] != '\n')) continue;
        size_t vlen = !strncmp((const char *)h + i, "SHA1 ", 5) ? 40 : (!strncmp((const char *)h + i, "MD5 ", 4) ? 32 : 0);
        if (!vlen) continue;
        const char *v = (const char *)h + i + (vlen == 40 ? 5 : 4);
        uint8_t d[20]; int ok = 1;
        for (size_t k = 0; k < vlen && ok; k++) ok = hexval((unsigned char)v[k]) >= 0;
        if (!ok) continue;
        for (size_t k = 0; k < vlen / 2; k++)
            d[k] = (uint8_t)(hexval((unsigned char)v[2 * k]) * 16 + hexval((unsigned char)v[2 * k + 1]));
        if (vlen == 40) { memcpy(out->sha1, d, 20); out->has_sha1 = 1; }
        else { memcpy(out->md5, d, 16); out->has_md5 = 1; }
        break;   /* first identity line only, as each loader did */
    }
}

int dp_bk2_open(const uint8_t *d, size_t n, DpBk2 *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);
    if (!dp_zip_is(d, n)) { snprintf(err, errsz, "bk2: not a zip container"); return 0; }
    out->header = dp_zip_read(d, n, "header", &out->header_n, NULL);   /* "Header.txt" */
    if (out->header) {
        if (out->header_n >= 25 && strcasestr((const char *)out->header, "startsfromsavestate true")) out->anchored = 1;
        parse_identity(out->header, out->header_n, out);
    }
    out->log = dp_zip_read(d, n, "input log", &out->log_n, NULL);
    if (!out->log) out->log = dp_zip_read(d, n, "log", &out->log_n, NULL);
    if (!out->log) {
        /* last resort: any single readable entry, as the sega loader did */
        size_t an = 0; char name[128];
        out->log = dp_zip_read(d, n, NULL, &an, name);
        out->log_n = out->log ? an : 0;
    }
    if (!out->log) {
        free(out->header); out->header = NULL;
        snprintf(err, errsz, "bk2: no Input Log.txt inside");
        return 0;
    }
    return 1;
}

void dp_bk2_free(DpBk2 *b) {
    if (!b) return;
    free(b->header); free(b->log);
    memset(b, 0, sizeof *b);
}
