/* tas.c: TAS movie loading.  See tas.h.  Every format reduces to one mask
 * byte per frame; button names and bit orders of each format are mapped to
 * the deepeel joypad layout at load time. */
#define _GNU_SOURCE
#include "tas.h"
#include "../core/zip.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DP_TAS_MAX_FRAMES (64u * 1024 * 1024) // 64 M frames: 64 MiB of masks

static int emit(DpTas *out, uint8_t mask, char *err, size_t errsz) {
    if (out->frames >= DP_TAS_MAX_FRAMES) {
        snprintf(err, errsz, "movie too long");
        return 0;
    }
    if (out->frames % 65536 == 0) {
        uint8_t *p = realloc(out->masks, out->frames + 65536);
        if (!p) {
            snprintf(err, errsz, "out of memory");
            return 0;
        }
        out->masks = p;
    }
    out->masks[out->frames++] = mask;
    return 1;
}

// --- shared button-name mapping (bk2 log keys and mnemonic strings) ---------
static uint8_t btn_mask(const char *n) {
    if (!strcmp(n, "a") || !strcmp(n, "b")) {
        // single-letter name: ambiguous in text ("A" the button); the caller
        // passes lowercase names only for full words, letters come positionally
    }
    if (!strcmp(n, "up") || !strcmp(n, "u")) return 0x40;
    if (!strcmp(n, "down") || !strcmp(n, "d")) return 0x80;
    if (!strcmp(n, "left") || !strcmp(n, "l")) return 0x20;
    if (!strcmp(n, "right") || !strcmp(n, "r")) return 0x10;
    if (!strcmp(n, "start") || !strcmp(n, "s") || !strcmp(n, "st")) return 0x08;
    if (!strcmp(n, "select") || !strcmp(n, "se")) return 0x04;
    if (!strcmp(n, "b")) return 0x02;
    if (!strcmp(n, "a")) return 0x01;
    return 0; // Power, L/R shoulders, motion sensors: ignored on GB
}

// --- bk2 / plain input log ---------------------------------------------------
static int log_parse(const uint8_t *d, size_t n, DpTas *out, char *err, size_t errsz) {
    const char *p = (const char *)d, *end = (const char *)d + n;
    uint8_t key_mask[64];
    int nkey = 0;
    int in_input = 0, any_frame = 0;
    char line[4096];
    while (p < end) {
        size_t len = 0;
        while (p + len < end && p[len] != '\n' && len < sizeof line - 1) len++;
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (p + len < end ? 1 : 0);
        // strip CR
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
        if (!strncmp(line, "[Input]", 7)) {
            in_input = 1;
            continue;
        }
        if (!strncmp(line, "[/Input]", 8)) {
            in_input = 0;
            continue;
        }
        if (!strncmp(line, "LogKey:", 7)) {
            nkey = 0;
            char *tok = strtok(line + 7, "| \t#");
            while (tok && nkey < 64) {
                for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
                key_mask[nkey++] = btn_mask(tok);
                tok = strtok(NULL, "| \t#");
            }
            continue;
        }
        if (line[0] != '|') continue;
        // a frame line: pipe-separated segments.  Within a segment, one
        // character per button of the LogKey in order (a bk2 frame is
        // typically "|.........|", all buttons of one controller in a single
        // segment); '.' = released, anything else = pressed
        char fl[4096];
        snprintf(fl, sizeof fl, "%s", line + 1);
        uint8_t mask = 0;
        int idx = 0, seen = 0;
        for (char *c = fl; *c; c++) {
            if (*c == '|') { // next controller: restart the LogKey
                idx = 0;
                continue;
            }
            if (*c == '\n' || *c == '\r') break;
            if (nkey && idx < nkey) {
                if (*c != '.') mask |= key_mask[idx];
                idx++;
            } else {
                // no LogKey: classic GB mnemonic order R L U D B A Select Start
                static const uint8_t pos[8] = {0x10, 0x20, 0x40, 0x80, 0x02, 0x01, 0x04, 0x08};
                if (*c != '.') mask |= idx < 8 ? pos[idx] : 0;
                idx++;
            }
            seen = 1;
        }
        if (!seen) continue;
        if (!emit(out, mask, err, errsz)) return 0;
        any_frame = 1;
        (void)in_input;
    }
    if (!any_frame) {
        snprintf(err, errsz, "no input frames found");
        return 0;
    }
    return 1;
}

// --- VBM ---------------------------------------------------------------------
// VBM button bits match the deepeel layout exactly
// (A=1 B=2 Select=4 Start=8 Right=16 Left=32 Up=64 Down=128).

static int vbm_parse(const uint8_t *d, size_t n, DpTas *out, char *err, size_t errsz) {
    if (n >= 0x40 && !memcmp(d, "VBM\x1a", 4)) {
        uint8_t start_flags = d[0x14];
        uint8_t ctl = d[0x15] & 0x0F;
        if (!ctl) ctl = 1;
        uint32_t offset = d[0x3C] | (d[0x3D] << 8) | ((uint32_t)(d[0x3E] | d[0x3F] << 8) << 16);
        if (!offset || offset >= n) {
            snprintf(err, errsz, "VBM: no controller data");
            return 0;
        }
        out->anchored = (start_flags & 0x03) != 0;
        int nctl = 0;
        for (int i = 0; i < 4; i++)
            if (ctl & (1 << i)) nctl++;
        size_t fsize = (size_t)nctl * 2;
        size_t avail = n - offset;
        size_t nframes = avail / fsize;
        for (size_t f = 0; f < nframes; f++) {
            const uint8_t *fr = d + offset + f * fsize;
            if (!emit(out, fr[0], err, errsz)) return 0; // controller 1
        }
        return out->frames > 0;
    }
    // text header: "VBM_MOVIE v1\r\n...\r\n\r\n" then 4-byte frames
    if (n > 12 && !memcmp(d, "VBM_MOVIE", 9)) {
        const uint8_t *p = memmem(d, n, "\r\n\r\n", 4);
        if (!p) p = memmem(d, n, "\n\n", 2);
        if (!p) {
            snprintf(err, errsz, "VBM: text header without frame data");
            return 0;
        }
        p += (p[0] == '\r' && p[1] == '\n' && p[2] == '\r') ? 4 : 2;
        size_t avail = n - (size_t)(p - d);
        out->anchored = 1; // text v1 starts from SRAM or savestate; best-effort
        for (size_t off = 0; off + 4 <= avail; off += 4) {
            uint8_t ctl = p[off + 1];
            if (ctl > 1) continue; // controllers 2..4: skip the frame slot
            if (!emit(out, p[off], err, errsz)) return 0;
        }
        return out->frames > 0;
    }
    return 0;
}

// --- dispatch ------------------------------------------------------------------
static int tas_load_buf(uint8_t *d, size_t n, DpTas *out, char *err, size_t errsz, int depth);

// one zip layer: header entry for the savestate flag, log entry for input;
// a zip without a log entry (a site download wrapping the .bk2) recurses on
// its first entry
static int tas_load_zip(uint8_t *d, size_t n, DpTas *out, char *err, size_t errsz, int depth) {
    size_t hn = 0;
    uint8_t *hdr = dp_zip_read(d, n, "header", &hn, NULL);
    if (hdr) {
        if (strcasestr((const char *)hdr, "startsfromsavestate true")) out->anchored = 1;
        free(hdr);
    }
    size_t ln = 0;
    uint8_t *log = dp_zip_read(d, n, "input log", &ln, NULL);
    if (!log) log = dp_zip_read(d, n, "log", &ln, NULL);
    if (!log) log = dp_zip_read(d, n, ".txt", &ln, NULL);
    if (log) {
        int ok = tas_load_buf(log, ln, out, err, errsz, depth + 1);
        free(log);
        return ok;
    }
    size_t an = 0;
    char name[128];
    uint8_t *any = dp_zip_read(d, n, NULL, &an, name);
    if (!any) {
        snprintf(err, errsz, "zip: no readable entry");
        return 0;
    }
    int ok = tas_load_buf(any, an, out, err, errsz, depth + 1);
    free(any);
    return ok;
}

static int tas_load_buf(uint8_t *d, size_t n, DpTas *out, char *err, size_t errsz, int depth) {
    if (depth > 3) {
        snprintf(err, errsz, "too many nested containers");
        return 0;
    }
    if (dp_zip_is(d, n)) return tas_load_zip(d, n, out, err, errsz, depth);
    if (!memcmp(d, "VBM\x1a", 4) || (n > 12 && !memcmp(d, "VBM_MOVIE", 9)))
        return vbm_parse(d, n, out, err, errsz);
    if (memchr(d, '|', n < 4096 ? n : 4096)) return log_parse(d, n, out, err, errsz);
    snprintf(err, errsz, "unknown movie format");
    return 0;
}

int dp_tas_load(const char *path, DpTas *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);
    err[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 30)) {
        fclose(f);
        snprintf(err, errsz, "%s: empty or too large", path);
        return 0;
    }
    uint8_t *d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        free(d);
        fclose(f);
        snprintf(err, errsz, "cannot read %s", path);
        return 0;
    }
    fclose(f);
    snprintf(out->source, sizeof out->source, "%s", path);
    int ok = tas_load_buf(d, (size_t)sz, out, err, errsz, 0);
    free(d);
    if (!ok) dp_tas_free(out);
    return ok;
}

void dp_tas_free(DpTas *t) {
    if (!t) return;
    free(t->masks);
    memset(t, 0, sizeof *t);
}
