// sta.c: see sta.h.  The constants come from the generated project's
// gbl_config.h, so an export always names the file the firmware looks for
// with the format tag the firmware computes — no second copy of the
// generator's decisions here.
#include "sta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC "GBLSTATE"
#define FNV_BASIS 2166136261u

typedef struct {
    char magic[8];
    uint32_t format;
    uint32_t bytes;
    uint32_t checksum;
    uint32_t frame;
    char title[16];
} StaHeader;

static uint32_t fnv(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

// one "#define NAME value" (string or number) out of a generated config
static int define_of(const char *text, const char *name, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "#define %s ", name);
    const char *p = strstr(text, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (p[n] && p[n] != '"' && n < outsz - 1) n++;
    } else {
        while (p[n] && p[n] != '\n' && p[n] != ' ' && p[n] != '\r' && n < outsz - 1) n++;
    }
    if (!n || n >= outsz) return 0;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

int dp_sta_from_project(DpSta *st, const char *project_dir, uint32_t state_bytes, char *err,
                        size_t errsz) {
    char cfgpath[512];
    snprintf(cfgpath, sizeof cfgpath, "%s/gbl_config.h", project_dir);
    FILE *f = fopen(cfgpath, "rb");
    if (!f) { // some project layouts keep the config in src/
        snprintf(cfgpath, sizeof cfgpath, "%s/src/gbl_config.h", project_dir);
        f = fopen(cfgpath, "rb");
    }
    if (!f) {
        snprintf(err, errsz, "no gbl_config.h in %s (not a generated pc project?)", project_dir);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20) {
        fclose(f);
        snprintf(err, errsz, "cannot read %s", cfgpath);
        return 0;
    }
    char *text = malloc((size_t)n + 1);
    fread(text, 1, (size_t)n, f);
    text[n] = 0;
    fclose(f);

    char name[128], version[64], title[64], save[128], num[32];
    int ok = define_of(text, "GBL_STATE_NAME", name, sizeof name) &&
             define_of(text, "GBL_GEN_VERSION", version, sizeof version);
    if (!ok) {
        free(text);
        snprintf(err, errsz, "%s lacks GBL_STATE_NAME / GBL_GEN_VERSION", cfgpath);
        return 0;
    }
    st->slots = 10;
    st->sectors = 0;
    if (define_of(text, "GBL_STATE_SLOTS", num, sizeof num)) st->slots = (uint32_t)strtoul(num, NULL, 0);
    if (define_of(text, "GBL_STATE_SLOT_SECTORS", num, sizeof num))
        st->sectors = (uint32_t)strtoul(num, NULL, 0);
    st->sav_path[0] = 0;
    snprintf(st->sta_path, sizeof st->sta_path, "%s/%s", project_dir, name);
    if (define_of(text, "GBL_SAVE_NAME", save, sizeof save))
        snprintf(st->sav_path, sizeof st->sav_path, "%s/%s", project_dir, save);
    memset(st->title, 0, sizeof st->title);
    if (define_of(text, "GBL_TITLE", title, sizeof title)) {
        size_t tl = strlen(title);
        if (tl > sizeof st->title - 1) tl = sizeof st->title - 1;
        memcpy(st->title, title, tl);
    }
    free(text);

    if (!st->sectors) {
        snprintf(err, errsz, "%s has no GBL_STATE_SLOT_SECTORS (states not offered?)", cfgpath);
        return 0;
    }
    if (state_bytes > (st->sectors - 1u) * 512u) {
        snprintf(err, errsz,
                 "state size %u does not fit the project's %u sectors/slot (variant mismatch?)",
                 state_bytes, st->sectors);
        return 0;
    }
    // m0_state.cpp format_tag(): FNV-1a over the version string, then the
    // little-endian state size
    st->format = fnv(FNV_BASIS, (const uint8_t *)version, (uint32_t)strlen(version));
    uint8_t le[4] = { (uint8_t)state_bytes, (uint8_t)(state_bytes >> 8), (uint8_t)(state_bytes >> 16),
                      (uint8_t)(state_bytes >> 24) };
    st->format = fnv(st->format, le, sizeof le);
    return 1;
}

int dp_sta_write(const DpSta *st, unsigned slot, const uint8_t *img, uint32_t nbytes,
                 uint32_t frame, char *err, size_t errsz) {
    if (slot >= st->slots) {
        snprintf(err, errsz, "slot %u out of range (%u slots)", slot, st->slots);
        return 0;
    }
    if (nbytes > (st->sectors - 1u) * 512u) {
        snprintf(err, errsz, "image of %u bytes does not fit %u sectors", nbytes, st->sectors);
        return 0;
    }
    FILE *f = fopen(st->sta_path, "r+b");
    if (!f) { // first write: the whole slot area, zero-filled (as the pc does)
        f = fopen(st->sta_path, "wb");
        if (!f) {
            snprintf(err, errsz, "cannot create %s", st->sta_path);
            return 0;
        }
        static const uint8_t zero[512];
        for (uint32_t s = 0; s < st->slots * st->sectors; s++)
            if (fwrite(zero, 1, sizeof zero, f) != sizeof zero) {
                fclose(f);
                snprintf(err, errsz, "short write on %s", st->sta_path);
                return 0;
            }
        fclose(f);
        f = fopen(st->sta_path, "r+b");
        if (!f) {
            snprintf(err, errsz, "cannot reopen %s", st->sta_path);
            return 0;
        }
    }
    uint32_t base = slot * st->sectors;
    // the machine image, whole sectors, zero-padded (the firmware's checksum
    // covers exactly nbytes, the padding is never read)
    uint32_t pos = 0;
    uint32_t checksum = FNV_BASIS;
    for (uint32_t sec = 1; pos < nbytes || sec == 1; sec++) {
        uint8_t buf[512];
        uint32_t chunk = nbytes - pos < 512 ? nbytes - pos : 512;
        memset(buf, 0, sizeof buf);
        if (chunk) {
            memcpy(buf, img + pos, chunk);
            checksum = fnv(checksum, buf, chunk); // over exactly the data bytes
        }
        pos += chunk;
        if (fseek(f, (long)(base + sec) * 512, SEEK_SET) ||
            fwrite(buf, 1, sizeof buf, f) != sizeof buf) {
            fclose(f);
            snprintf(err, errsz, "short write on %s", st->sta_path);
            return 0;
        }
        if (!chunk) break; // header-only slot would be invalid anyway
    }
    StaHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, STATE_MAGIC, 8);
    h.format = st->format;
    h.bytes = nbytes;
    h.checksum = checksum;
    h.frame = frame;
    memcpy(h.title, st->title, sizeof h.title);
    int ok = !fseek(f, (long)base * 512, SEEK_SET) && fwrite(&h, 1, sizeof h, f) == sizeof h &&
             fflush(f) == 0;
    fclose(f);
    if (!ok) {
        snprintf(err, errsz, "cannot write the header of %s", st->sta_path);
        return 0;
    }
    return 1;
}
