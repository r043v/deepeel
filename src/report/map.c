// map.c: the .map export — a memory map covering 100% of the ROM.
// Every byte gets exactly one class. Classes 1..7 are observed by the tool;
// classes 8..10 are INFERRED from the observed neighbours (and always
// labelled so). Numbers are tool observations, not hardware measurements.
#include "report.h"
#include "core/snapshot_png.h"
#include <string.h>
#include <stdlib.h>

const char *ms_map_class_name(int c) {
    static const char *nm[MS_MAP_COUNT] = { "unknown", "code-natural", "code-forced",
        "code-static", "data-copied", "data-read", "table", "padding",
        "inferred-data", "inferred-code", "inferred-padding" };
    return c >= 0 && c < MS_MAP_COUNT ? nm[c] : "?";
}

int ms_map_is_code(int c) { return c == MS_MAP_CODE_NAT || c == MS_MAP_CODE_FORCED || c == MS_MAP_CODE_STATIC || c == MS_MAP_INF_CODE; }
int ms_map_is_data(int c) { return c == MS_MAP_DATA_COPIED || c == MS_MAP_DATA_READ || c == MS_MAP_TABLE || c == MS_MAP_INF_DATA; }

uint8_t *ms_map_build(const MsResults *res, MsMapSummary *sum) {
    size_t n = (size_t)res->banks * 0x4000;
    uint8_t *m = malloc(n ? n : 1);
    for (size_t k = 0; k < n; k++) {
        uint8_t x = res->exec_flags[k], d = res->data_flags[k], s = res->static_flags[k];
        int c;
        if (x & MS_XF_NATURAL) c = MS_MAP_CODE_NAT;
        else if (x & MS_XF_FORCED) c = MS_MAP_CODE_FORCED;
        else if (x & MS_XF_STATIC) c = MS_MAP_CODE_STATIC;
        else if (d & (MS_DF_COPIED | MS_DF_VERBATIM)) c = MS_MAP_DATA_COPIED;
        else if (d & MS_DF_READ) c = MS_MAP_DATA_READ;
        else if (s & MS_SF_POINTER) c = MS_MAP_TABLE;
        else if (s & MS_SF_PAD) c = MS_MAP_PAD;
        else c = MS_MAP_UNKNOWN;
        m[k] = (uint8_t)c;
    }
    // inference over the unknown runs of each bank (never crossing a bank)
    const uint8_t *rom = res->cart ? res->cart->rom : NULL;
    size_t rsz = res->cart ? res->cart->rom_size : 0;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        size_t be = bs + 0x4000;
        for (size_t i = bs; i < be;) {
            if (m[i] != MS_MAP_UNKNOWN) { i++; continue; }
            size_t j = i;
            while (j < be && m[j] == MS_MAP_UNKNOWN) j++;
            int before = i > bs ? m[i - 1] : -1, after = j < be ? m[j] : -1;
            size_t len = j - i;
            int fill = rom && j <= rsz && len >= 4;
            for (size_t k = i; fill && k < j; k++) if (rom[k] != rom[i] || (rom[i] != 0x00 && rom[i] != 0xFF)) fill = 0;
            int c = MS_MAP_UNKNOWN;
            if (fill) c = MS_MAP_INF_PAD;
            else if (before >= 0 && after >= 0 && ms_map_is_code(before) && ms_map_is_code(after) && len <= 3) c = MS_MAP_INF_CODE;
            else if (before >= 0 && after >= 0 && ms_map_is_data(before) && ms_map_is_data(after) && len <= 256) c = MS_MAP_INF_DATA;
            if (c != MS_MAP_UNKNOWN) memset(m + i, c, len);
            i = j;
        }
    }
    if (sum) {
        memset(sum, 0, sizeof *sum);
        sum->total = n;
        for (size_t k = 0; k < n; k++) sum->cls[m[k]]++;
        for (int c = 0; c < MS_MAP_COUNT; c++) {
            if (c == MS_MAP_UNKNOWN) sum->unknown += sum->cls[c];
            else sum->known += sum->cls[c];
            if (ms_map_is_code(c)) sum->code += sum->cls[c];
            else if (ms_map_is_data(c)) sum->data += sum->cls[c];
            else if (c == MS_MAP_PAD || c == MS_MAP_INF_PAD) sum->pad += sum->cls[c];
        }
        for (int c = 0; c < MS_MAP_COUNT; c++)
            if (c >= MS_MAP_INF_DATA) sum->inferred += sum->cls[c];
    }
    return m;
}

void ms_map_summary_text(const MsMapSummary *s, MsBuf *out) {
    double t = s->total ? 100.0 / (double)s->total : 0.0;
    ms_buf_fmt(out, "memory map (tool observation, %llu bytes):\n", (unsigned long long)s->total);
    ms_buf_fmt(out, "  known    %5.1f%%   unknown %5.1f%%   (known includes %.1f%% inferred)\n",
               s->known * t, s->unknown * t, s->inferred * t);
    ms_buf_fmt(out, "  code     %5.1f%%   data    %5.1f%%   padding %5.1f%%\n", s->code * t, s->data * t, s->pad * t);
    for (int c = 0; c < MS_MAP_COUNT; c++)
        ms_buf_fmt(out, "    %-17s %8llu  %5.1f%%\n", ms_map_class_name(c), (unsigned long long)s->cls[c], s->cls[c] * t);
}

void ms_report_map(const MsResults *res, MsBuf *out) {
    MsMapSummary s;
    uint8_t *m = ms_map_build(res, &s);
    size_t n = s.total;
    ms_buf_fmt(out, "# memscan map v1: whole-ROM memory map, one class per byte\n");
    ms_buf_fmt(out, "# rom %s, %u banks of 0x4000, %llu bytes\n", res->cart && res->cart->title[0] ? res->cart->title : "?",
               (unsigned)res->banks, (unsigned long long)n);
    ms_buf_fmt(out, "# 'inferred-*' classes are deduced from neighbours, not observed\n");
    ms_buf_str(out, "# ");
    {
        MsBuf tmp;
        ms_buf_init(&tmp);
        ms_map_summary_text(&s, &tmp);
        for (size_t i = 0; i < tmp.len; i++) {
            ms_buf_fmt(out, "%c", tmp.p[i]);
            if (tmp.p[i] == '\n' && i + 1 < tmp.len) ms_buf_str(out, "# ");
        }
        ms_buf_free(&tmp);
    }
    ms_buf_str(out, "# columns: bank start end(inclusive) class\n");
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        for (size_t i = bs; i < bs + 0x4000;) {
            size_t j = i;
            while (j < bs + 0x4000 && m[j] == m[i]) j++;
            unsigned win = bs ? 0x4000 : 0;
            ms_buf_fmt(out, "%u %04x %04x %s\n", (unsigned)(bs / 0x4000), (unsigned)(win + (i - bs)),
                       (unsigned)(win + (j - 1 - bs)), ms_map_class_name(m[i]));
            i = j;
        }
    }
    free(m);
}

// colour of each class in the .map.png (same palette idea as the HTML page)
static const uint8_t map_rgb[MS_MAP_COUNT][3] = {
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
int ms_report_map_png(const MsResults *res, const char *path) {
    enum { SQ = 6, GRID = 16, BANK_PX = SQ * GRID, GAP = 6, PER_ROW = 8 };
    MsMapSummary s;
    uint8_t *m = ms_map_build(res, &s);
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
            unsigned cnt[MS_MAP_COUNT] = { 0 };
            const uint8_t *blk = m + (size_t)b * 0x4000 + (size_t)q * 64;
            for (int k = 0; k < 64; k++) cnt[blk[k]]++;
            int best = 0;
            for (int c = 1; c < MS_MAP_COUNT; c++) if (cnt[c] > cnt[best]) best = c;
            for (int y = 0; y < SQ - 1; y++)
                for (int x = 0; x < SQ - 1; x++) {
                    uint8_t *px = rgb + ((size_t)(oy + (q / GRID) * SQ + y) * w + ox + (q % GRID) * SQ + x) * 3;
                    px[0] = map_rgb[best][0]; px[1] = map_rgb[best][1]; px[2] = map_rgb[best][2];
                }
        }
    }
    free(m);
    MsBuf txt;
    ms_buf_init(&txt);
    ms_report_map(res, &txt);
    int ok = ms_png_save_text(path, w, h, rgb, "memscan_map", (const char *)txt.p, txt.len);
    ms_buf_free(&txt);
    free(rgb);
    return ok;
}
