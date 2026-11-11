// Generic, platform-independent map / summary output of the kernel (dp_core_explore).
// One class vector built from the kernel's per-ROM-byte flags feeds the shared squares PNG
// (dp_map_png_from_evidence), the text map and the JSON summary, on every ISA.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "../report/report.h"

// DP_MAP_* class of one ROM byte (same precedence as the NES projection)
uint8_t *dp_core_map_build(const DpCoreResult *r) {
    if (!r || !r->flags || !r->rom_size) return NULL;
    uint8_t *m = malloc(r->rom_size);
    if (!m) return NULL;
    for (size_t i = 0; i < r->rom_size; i++) {
        uint8_t f = r->flags[i], c;
        if (f & DP_CORE_COPIED) c = DP_MAP_DATA_COPIED;
        else if (f & DP_CORE_NATURAL) c = DP_MAP_CODE_NAT;
        else if (f & DP_CORE_FORCED) c = DP_MAP_CODE_FORCED;
        else if (f & DP_CORE_STATIC) c = DP_MAP_CODE_STATIC;
        else if (f & (DP_CORE_DATA | DP_CORE_COMPRESSED)) c = DP_MAP_DATA_READ;
        else c = DP_MAP_UNKNOWN;
        m[i] = c;
    }
    return m;
}

// --map FILE: PNG when the name ends in .png, else the "map v1" digit text
int dp_core_write_map(const char *path, const char *id, const char *rom_name, const DpCoreResult *r) {
    if (!path || !path[0]) return 0;
    uint8_t *m = dp_core_map_build(r);
    if (!m) return -1;
    size_t pl = strlen(path);
    int ok;
    if (pl >= 4 && !strcmp(path + pl - 4, ".png")) {
        DpFirstLog fl; dp_first_init(&fl, (uint32_t)(r->rom_size < 0x400000 ? r->rom_size : 0x400000));
        for (size_t i = 0; i < r->rom_size && r->first_seen; i++) {
            if (!r->first_seen[i]) continue;
            uint32_t sf = r->src_flags ? r->src_flags[i] : 0;
            uint32_t bit = sf ? (sf & (~sf + 1)) : DP_SRC_FORK_NATURAL;
            if (fl.n == fl.cap) {
                uint32_t nc = fl.cap ? fl.cap * 2 : 1024;
                DpFirstSeen *g = realloc(fl.e, sizeof *g * nc);
                if (!g) break;
                fl.e = g; fl.cap = nc;
            }
            if (fl.n < fl.max) fl.e[fl.n++] = (DpFirstSeen){ (uint32_t)i, bit, r->first_seen[i], 0 };
        }
        const char *base = rom_name ? strrchr(rom_name, '/') : NULL;
        base = base ? base + 1 : (rom_name ? rom_name : id);
        ok = dp_map_png_from_evidence(m, r->rom_size, NULL, NULL, r->src_flags, &fl, base, id, base, path);
        dp_first_free(&fl);
    } else {
        FILE *f = fopen(path, "w");
        ok = f != NULL;
        if (f) {
            fprintf(f, "%s map v1\n", id);
            for (size_t i = 0; i < r->rom_size; i++) fputc('0' + m[i], f);
            fputc('\n', f);
            fclose(f);
        }
    }
    free(m);
    return ok ? 0 : -1;
}

// --summary FILE: compact JSON, same shape as the NES summary
int dp_core_write_summary(const char *path, const char *id, const DpCoreResult *r) {
    if (!path || !path[0]) return 0;
    uint8_t *m = dp_core_map_build(r);
    if (!m) return -1;
    uint64_t cls[DP_MAP_COUNT] = { 0 };
    for (size_t i = 0; i < r->rom_size; i++) cls[m[i] < DP_MAP_COUNT ? m[i] : 0]++;
    free(m);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    #define U(x) ((unsigned long long)(x))
    fprintf(f, "{\"deepeel\":\"%s\",\"platform\":\"%s\",\"engine\":\"kernel\",\"rom_size\":%zu,\"classes\":{",
            DEEPEEL_VERSION, id, r->rom_size);
    static const char *const names[DP_MAP_COUNT] = { "unknown", "code-natural", "code-forced", "code-static",
        "data-copied", "data-read", "data-static", "table", "pad", "inferred-data", "inferred-code",
        "inferred-pad", "probable-code", "probable-data" };
    for (int k = 0; k < DP_MAP_COUNT; k++) fprintf(f, "%s\"%s\":%llu", k ? "," : "", names[k], U(cls[k]));
    fprintf(f, "},\"known_pct\":%.3f,\"counters\":{\"steps\":%llu,\"paths\":%llu,\"forks\":%llu,"
               "\"saturation\":%llu,\"closure_roots\":%llu,\"closure_capped\":%llu,\"event_probes\":%llu,"
               "\"cells\":%llu,\"compressed_bytes\":%llu,\"copy_bytes\":%llu}}\n",
            r->rom_size ? 100.0 * (double)(r->rom_size - cls[0]) / (double)r->rom_size : 0.0,
            U(r->steps), U(r->paths), U(r->forks), U(r->sat_total), U(r->closure_roots), U(r->closure_capped),
            U(r->event_probes), U(r->cells), U(r->compressed_bytes), U(r->copy_bytes));
    #undef U
    fclose(f);
    return 0;
}
