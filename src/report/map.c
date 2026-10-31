// map.c: the .map export — a memory map covering 100% of the ROM.
// Every byte gets exactly one class. Classes 1..7 are observed by the tool;
// classes 8..10 are INFERRED from the observed neighbours (and always
// labelled so). Numbers are tool observations, not hardware measurements.
#include "report.h"
#include "core/snapshot_png.h"
#include <string.h>
#include <stdlib.h>

const char *dp_map_class_name(int c) {
    static const char *nm[DP_MAP_COUNT] = { "unknown", "code-natural", "code-forced",
        "code-static", "data-copied", "data-read", "table", "padding",
        "inferred-data", "inferred-code", "inferred-padding" };
    return c >= 0 && c < DP_MAP_COUNT ? nm[c] : "?";
}

int dp_map_is_code(int c) { return c == DP_MAP_CODE_NAT || c == DP_MAP_CODE_FORCED || c == DP_MAP_CODE_STATIC || c == DP_MAP_INF_CODE; }
int dp_map_is_data(int c) { return c == DP_MAP_DATA_COPIED || c == DP_MAP_DATA_READ || c == DP_MAP_TABLE || c == DP_MAP_INF_DATA; }

uint8_t *dp_map_build(const DpResults *res, DpMapSummary *sum) {
    size_t n = (size_t)res->banks * 0x4000;
    uint8_t *m = malloc(n ? n : 1);
    for (size_t k = 0; k < n; k++) {
        uint8_t x = res->exec_flags[k], d = res->data_flags[k], s = res->static_flags[k];
        int c;
        if (x & DP_XF_NATURAL) c = DP_MAP_CODE_NAT;
        else if (x & DP_XF_FORCED) c = DP_MAP_CODE_FORCED;
        else if (x & DP_XF_STATIC) c = DP_MAP_CODE_STATIC;
        else if (d & (DP_DF_COPIED | DP_DF_VERBATIM)) c = DP_MAP_DATA_COPIED;
        else if (d & DP_DF_READ) c = DP_MAP_DATA_READ;
        else if (s & DP_SF_POINTER) c = DP_MAP_TABLE;
        else if (s & DP_SF_PAD) c = DP_MAP_PAD;
        else c = DP_MAP_UNKNOWN;
        m[k] = (uint8_t)c;
    }
    // inference over the unknown runs of each bank (never crossing a bank)
    const uint8_t *rom = res->cart ? res->cart->rom : NULL;
    size_t rsz = res->cart ? res->cart->rom_size : 0;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        size_t be = bs + 0x4000;
        for (size_t i = bs; i < be;) {
            if (m[i] != DP_MAP_UNKNOWN) { i++; continue; }
            size_t j = i;
            while (j < be && m[j] == DP_MAP_UNKNOWN) j++;
            int before = i > bs ? m[i - 1] : -1, after = j < be ? m[j] : -1;
            size_t len = j - i;
            // pure filler: every byte 00 or FF (mixed runs happen between tiles)
            int fill = rom && j <= rsz && len >= 4;
            for (size_t k = i; fill && k < j; k++) if (rom[k] != 0x00 && rom[k] != 0xFF) fill = 0;
            int c = DP_MAP_UNKNOWN;
            if (fill) c = DP_MAP_INF_PAD;
            else if (before >= 0 && after >= 0 && dp_map_is_code(before) && dp_map_is_code(after) && len <= 3) c = DP_MAP_INF_CODE;
            else if (before >= 0 && after >= 0 && dp_map_is_data(before) && dp_map_is_data(after) && len <= 1024) c = DP_MAP_INF_DATA;
            if (c != DP_MAP_UNKNOWN) memset(m + i, c, len);
            i = j;
        }
    }
    if (sum) {
        memset(sum, 0, sizeof *sum);
        sum->total = n;
        for (size_t k = 0; k < n; k++) sum->cls[m[k]]++;
        for (int c = 0; c < DP_MAP_COUNT; c++) {
            if (c == DP_MAP_UNKNOWN) sum->unknown += sum->cls[c];
            else sum->known += sum->cls[c];
            if (dp_map_is_code(c)) sum->code += sum->cls[c];
            else if (dp_map_is_data(c)) sum->data += sum->cls[c];
            else if (c == DP_MAP_PAD || c == DP_MAP_INF_PAD) sum->pad += sum->cls[c];
        }
        for (int c = 0; c < DP_MAP_COUNT; c++)
            if (c >= DP_MAP_INF_DATA) sum->inferred += sum->cls[c];
    }
    return m;
}

void dp_map_summary_text(const DpMapSummary *s, DpBuf *out) {
    double t = s->total ? 100.0 / (double)s->total : 0.0;
    dp_buf_fmt(out, "memory map (tool observation, %llu bytes):\n", (unsigned long long)s->total);
    dp_buf_fmt(out, "  known    %5.1f%%   unknown %5.1f%%   (known includes %.1f%% inferred)\n",
               s->known * t, s->unknown * t, s->inferred * t);
    dp_buf_fmt(out, "  code     %5.1f%%   data    %5.1f%%   padding %5.1f%%\n", s->code * t, s->data * t, s->pad * t);
    for (int c = 0; c < DP_MAP_COUNT; c++)
        dp_buf_fmt(out, "    %-17s %8llu  %5.1f%%\n", dp_map_class_name(c), (unsigned long long)s->cls[c], s->cls[c] * t);
}

// the last mile: the byte ranges nothing could observe or infer, largest
// first — the honest boundary of the map
void dp_map_residual_text(const DpResults *res, const uint8_t *m, DpBuf *out) {
    size_t n = (size_t)res->banks * 0x4000;
    struct { unsigned bank, lo, hi; size_t len; } top[8] = {{0}};
    uint32_t total = 0, ranges = 0;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        for (size_t i = bs; i < bs + 0x4000;) {
            if (m[i] != DP_MAP_UNKNOWN) { i++; continue; }
            size_t j = i;
            while (j < bs + 0x4000 && m[j] == DP_MAP_UNKNOWN) j++;
            if (j > i) {
                ranges++;
                total += (uint32_t)(j - i);
                size_t len = j - i;
                for (int t = 0; t < 8; t++) {
                    if (len > top[t].len) {
                        for (int u = 7; u > t; u--) top[u] = top[u - 1];
                        top[t].bank = (unsigned)(bs / 0x4000);
                        top[t].lo = (unsigned)(i - bs);
                        top[t].hi = (unsigned)(j - 1 - bs);
                        top[t].len = len;
                        break;
                    }
                }
            }
            i = j;
        }
    }
    if (!total) {
        dp_buf_str(out, "residual unknown: none\n");
        return;
    }
    dp_buf_fmt(out, "residual unknown: %u bytes in %u ranges, largest:", total, ranges);
    unsigned win = 0;
    for (int t = 0; t < 8 && top[t].len; t++) {
        win = top[t].bank ? 0x4000 : 0;
        dp_buf_fmt(out, " %u:%04x-%04x (%zu)", top[t].bank, win + top[t].lo, win + top[t].hi,
                   top[t].len);
    }
    dp_buf_str(out, "\n");
}

void dp_report_map(const DpResults *res, DpBuf *out) {
    DpMapSummary s;
    uint8_t *m = dp_map_build(res, &s);
    size_t n = s.total;
    dp_buf_fmt(out, "# deepeel map v1: whole-ROM memory map, one class per byte\n");
    dp_buf_fmt(out, "# rom %s, %u banks of 0x4000, %llu bytes\n", res->cart && res->cart->title[0] ? res->cart->title : "?",
               (unsigned)res->banks, (unsigned long long)n);
    dp_buf_fmt(out, "# 'inferred-*' classes are deduced from neighbours, not observed\n");
    dp_buf_str(out, "# ");
    {
        DpBuf tmp;
        dp_buf_init(&tmp);
        dp_map_summary_text(&s, &tmp);
        for (size_t i = 0; i < tmp.len; i++) {
            dp_buf_fmt(out, "%c", tmp.p[i]);
            if (tmp.p[i] == '\n' && i + 1 < tmp.len) dp_buf_str(out, "# ");
        }
        dp_buf_free(&tmp);
    }
    dp_buf_str(out, "# columns: bank start end(inclusive) class\n");
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        for (size_t i = bs; i < bs + 0x4000;) {
            size_t j = i;
            while (j < bs + 0x4000 && m[j] == m[i]) j++;
            unsigned win = bs ? 0x4000 : 0;
            dp_buf_fmt(out, "%u %04x %04x %s\n", (unsigned)(bs / 0x4000), (unsigned)(win + (i - bs)),
                       (unsigned)(win + (j - 1 - bs)), dp_map_class_name(m[i]));
            i = j;
        }
    }
    free(m);
}

// colour of each class in the .map.png (same palette idea as the HTML page)
static const uint8_t map_rgb[DP_MAP_COUNT][3] = {
    { 0xd0, 0x30, 0x30 }, // unknown
    { 0x2e, 0x7d, 0xd1 }, // code-natural
    { 0x6a, 0xa8, 0xe8 }, // code-forced
    { 0x9c, 0xc4, 0xee }, // code-static
    { 0x2e, 0xa0, 0x4f }, // data-copied
    { 0x62, 0xc2, 0x7a }, // data-read
    { 0xc9, 0xa2, 0x27 }, // table
    { 0x70, 0x70, 0x70 }, // padding
    { 0x8f, 0xd6, 0xa0 }, // inferred-data
    { 0xb5, 0xd3, 0xf3 }, // inferred-code
    { 0xa0, 0xa0, 0xa0 }, // inferred-padding
};

// One SQ x SQ square per 64 ROM bytes (dominant class), 16x16 squares per bank,
// banks laid out in rows of up to 8. The full .map text rides in the PNG.
int dp_report_map_png(const DpResults *res, const char *path) {
    enum { SQ = 6, GRID = 16, BANK_PX = SQ * GRID, GAP = 6, PER_ROW = 8 };
    DpMapSummary s;
    uint8_t *m = dp_map_build(res, &s);
    if (!m) return 0;
    int banks = (int)res->banks;
    int cols = banks < PER_ROW ? banks : PER_ROW;
    int rows = (banks + PER_ROW - 1) / PER_ROW;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    int w = GAP + cols * (BANK_PX + GAP), h = GAP + rows * (BANK_PX + GAP);
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) { free(m); return 0; }
    for (size_t i = 0; i < (size_t)w * h; i++) { rgb[i * 3] = 0x18; rgb[i * 3 + 1] = 0x18; rgb[i * 3 + 2] = 0x1c; }
    for (int b = 0; b < banks; b++) {
        int ox = GAP + (b % PER_ROW) * (BANK_PX + GAP), oy = GAP + (b / PER_ROW) * (BANK_PX + GAP);
        for (int q = 0; q < GRID * GRID; q++) {
            unsigned cnt[DP_MAP_COUNT] = { 0 };
            const uint8_t *blk = m + (size_t)b * 0x4000 + (size_t)q * 64;
            for (int k = 0; k < 64; k++) cnt[blk[k]]++;
            int best = 0;
            for (int c = 1; c < DP_MAP_COUNT; c++) if (cnt[c] > cnt[best]) best = c;
            for (int y = 0; y < SQ - 1; y++)
                for (int x = 0; x < SQ - 1; x++) {
                    uint8_t *px = rgb + ((size_t)(oy + (q / GRID) * SQ + y) * w + ox + (q % GRID) * SQ + x) * 3;
                    px[0] = map_rgb[best][0]; px[1] = map_rgb[best][1]; px[2] = map_rgb[best][2];
                }
        }
    }
    free(m);
    DpBuf txt;
    dp_buf_init(&txt);
    dp_report_map(res, &txt);
    int ok = dp_png_save_text(path, w, h, rgb, "deepeel_map", (const char *)txt.p, txt.len);
    dp_buf_free(&txt);
    free(rgb);
    return ok;
}
