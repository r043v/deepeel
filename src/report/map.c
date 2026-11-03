// map.c: the .map export — a memory map covering 100% of the ROM.
// Every byte gets exactly one class. Classes 1..7 are observed by the tool;
// classes 8..10 are INFERRED from the observed neighbours (and always
// labelled so). Numbers are tool observations, not hardware measurements.
#include "report.h"
#include "core/snapshot_png.h"
#include "../gui/font5x7.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// --- dpCv: the lossless class vector, run-length coded ------------------------------
// (class byte, LEB128 run length) pairs over the whole ROM, one entry per run
void dp_map_cv_encode(const uint8_t *m, size_t n, DpBuf *out) {
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && m[j] == m[i]) j++;
        uint8_t b = m[i];
        dp_buf_push(out, &b, 1);
        for (size_t r = j - i; ; ) {
            uint8_t v = (uint8_t)(r & 0x7F);
            r >>= 7;
            if (r) v |= 0x80;
            dp_buf_push(out, &v, 1);
            if (!r) break;
        }
        i = j;
    }
}

// 1 when the runs fill exactly n bytes of m
int dp_map_cv_decode(const uint8_t *p, size_t len, uint8_t *m, size_t n) {
    size_t pos = 0, i = 0;
    while (pos < len) {
        uint8_t c = p[pos++];
        size_t run = 0;
        int sh = 0;
        for (;;) {
            if (pos >= len || sh > 56) return 0;
            uint8_t v = p[pos++];
            run |= (size_t)(v & 0x7F) << sh;
            sh += 7;
            if (!(v & 0x80)) break;
        }
        if (c >= DP_MAP_COUNT || run > n - i) return 0;
        memset(m + i, c, run);
        i += run;
    }
    return i == n;
}

// pixel budget of the squares image: past it, one pixel covers several bytes
unsigned dp_map_png_budget = 16u << 20;

const char *dp_map_class_name(int c) {
    static const char *nm[DP_MAP_COUNT] = { "unknown", "code-natural", "code-forced",
        "code-static", "data-copied", "data-read", "data-static", "table", "padding",
        "inferred-data", "inferred-code", "inferred-padding" };
    return c >= 0 && c < DP_MAP_COUNT ? nm[c] : "?";
}

int dp_map_is_code(int c) { return c == DP_MAP_CODE_NAT || c == DP_MAP_CODE_FORCED || c == DP_MAP_CODE_STATIC || c == DP_MAP_INF_CODE; }
int dp_map_is_data(int c) { return c == DP_MAP_DATA_COPIED || c == DP_MAP_DATA_READ || c == DP_MAP_DATA_STATIC || c == DP_MAP_TABLE || c == DP_MAP_INF_DATA; }

uint8_t *dp_map_build(const DpResults *res, DpMapSummary *sum) {
    size_t n = (size_t)res->banks * 0x4000;
    uint8_t *m = malloc(n ? n : 1);
    for (size_t k = 0; k < n; k++) {
        uint8_t x = res->exec_flags[k], d = res->data_flags[k], s = res->static_flags[k];
        int c = dp_nature_class(dp_gb_nature(x, d, s));
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

// the last mile: every range nothing could observe or infer, bank by bank
int dp_map_residual_list(const DpResults *res, const uint8_t *m, DpResidualRange *out, int cap) {
    size_t n = (size_t)res->banks * 0x4000;
    int kept = 0;
    uint32_t ranges = 0;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        for (size_t i = bs; i < bs + 0x4000;) {
            if (m[i] != DP_MAP_UNKNOWN) { i++; continue; }
            size_t j = i;
            while (j < bs + 0x4000 && m[j] == DP_MAP_UNKNOWN) j++;
            if (j > i) {
                ranges++;
                if (kept < cap) {
                    out[kept].bank = (uint16_t)(bs / 0x4000);
                    out[kept].lo = (uint16_t)(i - bs);
                    out[kept].hi = (uint16_t)(j - 1 - bs);
                    out[kept].len = (uint32_t)(j - i);
                    kept++;
                }
            }
            i = j;
        }
    }
    return (int)ranges;
}

// mechanical verdict of one residual range: what blocks it, from the class of
// its two edges (the C port of the campaign's residual anatomy).  It explains
// the unknown, it never classifies it — the inference rules stay untouched.
const char *dp_residual_mechanism(const uint8_t *m, const DpResidualRange *r) {
    size_t bs = (size_t)r->bank * 0x4000;
    int before = r->lo > 0 ? m[bs + r->lo - 1] : -1;
    int after = r->hi < 0x3FFF ? m[bs + r->hi + 1] : -1;
    int bc = before >= 0 && dp_map_is_code(before);
    int bd = before >= 0 && dp_map_is_data(before);
    int bp = before == DP_MAP_PAD || before == DP_MAP_INF_PAD;
    int ac = after >= 0 && dp_map_is_code(after);
    int ad = after >= 0 && dp_map_is_data(after);
    int ap = after == DP_MAP_PAD || after == DP_MAP_INF_PAD;
    if (before < 0 && after < 0) return "bank";
    if (before < 0 || after < 0) { // one end is the bank's edge: judge by the other
        if (bp || ap) return "padding-fringe";
        if (bc || ac) return "edge-code";
        if (bd || ad) return "edge-data";
        return "bank";
    }
    if (bp || ap) return "padding-fringe";
    if (bc && ac) return "code-code";  // a dispatch the closure never resolved
    if ((bc && ad) || (bd && ac)) return "code-data"; // mixed edge: never inferred
    if (bd && ad) return "data-data";  // data no route ever read
    return "other";
}

// per-mechanism rollup of the residual unknown: ranges, bytes, where
void dp_residual_anatomy_text(const DpResults *res, const uint8_t *m, DpBuf *out) {
    static const char *mech[7] = { "code-code (dispatch non prouvé)",
                                   "code-data (bord mixte, jamais inféré)",
                                   "data-data (donnée jamais lue)",
                                   "padding-fringe (frange de remplissage)",
                                   "edge-code", "edge-data", "bank/bord de banque" };
    static const char *key[7] = { "code-code", "code-data", "data-data", "padding-fringe",
                                  "edge-code", "edge-data", "bank" };
    uint32_t bytes[7] = { 0 }, biggest[7] = { 0 };
    int ranges[7] = { 0 }, bi[7] = { 0 };
    size_t n = (size_t)res->banks * 0x4000;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        for (size_t i = bs; i < bs + 0x4000;) {
            if (m[i] != DP_MAP_UNKNOWN) { i++; continue; }
            size_t j = i;
            while (j < bs + 0x4000 && m[j] == DP_MAP_UNKNOWN) j++;
            DpResidualRange r = { (uint16_t)(bs / 0x4000), (uint16_t)(i - bs),
                                  (uint16_t)(j - 1 - bs), (uint32_t)(j - i) };
            const char *mk = dp_residual_mechanism(m, &r);
            for (int k = 0; k < 7; k++)
                if (!strcmp(mk, key[k])) {
                    bytes[k] += r.len;
                    ranges[k]++;
                    if (r.len > biggest[k]) { biggest[k] = r.len; bi[k] = r.bank; }
                    break;
                }
            i = j;
        }
    }
    dp_buf_str(out, "residual anatomy (what blocks each unknown range):\n");
    for (int k = 0; k < 7; k++)
        if (ranges[k])
            dp_buf_fmt(out, "  %-42s %5d ranges %8u bytes (biggest %u in bank %d)\n",
                       mech[k], ranges[k], bytes[k], biggest[k], bi[k]);
}

// the honest boundary of the map, largest first
void dp_map_residual_text(const DpResults *res, const uint8_t *m, DpBuf *out) {    DpResidualRange top[8] = {{0, 0, 0, 0}};
    int ranges = dp_map_residual_list(res, m, NULL, 0);
    uint32_t total = 0;
    size_t n = (size_t)res->banks * 0x4000;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        for (size_t i = bs; i < bs + 0x4000;) {
            if (m[i] != DP_MAP_UNKNOWN) { i++; continue; }
            size_t j = i;
            while (j < bs + 0x4000 && m[j] == DP_MAP_UNKNOWN) j++;
            uint32_t len = (uint32_t)(j - i);
            total += len;
            for (int t = 0; t < 8; t++) {
                if (len > top[t].len) {
                    for (int u = 7; u > t; u--) top[u] = top[u - 1];
                    top[t].bank = (uint16_t)(bs / 0x4000);
                    top[t].lo = (uint16_t)(i - bs);
                    top[t].hi = (uint16_t)(j - 1 - bs);
                    top[t].len = len;
                    break;
                }
            }
            i = j;
        }
    }
    if (!total) {
        dp_buf_str(out, "residual unknown: none\n");
        return;
    }
    dp_buf_fmt(out, "residual unknown: %u bytes in %d ranges, largest:", total, ranges);
    for (int t = 0; t < 8 && top[t].len; t++) {
        unsigned win = top[t].bank ? 0x4000 : 0;
        dp_buf_fmt(out, " %u:%04x-%04x (%u)", top[t].bank, win + top[t].lo, win + top[t].hi,
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
    { 0x3a, 0xb8, 0xbf }, // data-static
    { 0xc9, 0xa2, 0x27 }, // table
    { 0x70, 0x70, 0x70 }, // padding
    { 0x8f, 0xd6, 0xa0 }, // inferred-data
    { 0xb5, 0xd3, 0xf3 }, // inferred-code
    { 0xa0, 0xa0, 0xa0 }, // inferred-padding
};

// accented glyphs: the base lowercase letter (its top two rows are free in
// this font) with the mark drawn in rows 0-1 — decoded from UTF-8 input
static uint8_t acc_glyph[4][5]; // é è ê à
static int acc_ready;

static const uint8_t *glyph_of(const char *s, int *adv) {
    unsigned char c = (unsigned char)*s;
    if (c == 0xC3 && (unsigned char)s[1] >= 0x80) {
        if (!acc_ready) {
            memcpy(acc_glyph[0], &dp_font5x7['e' - 0x20][0], 5); // é acute
            acc_glyph[0][3] |= 1 << 0;
            acc_glyph[0][2] |= 3 << 1;
            memcpy(acc_glyph[1], &dp_font5x7['e' - 0x20][0], 5); // è grave
            acc_glyph[1][1] |= 1 << 0;
            acc_glyph[1][2] |= 1 << 1;
            memcpy(acc_glyph[2], &dp_font5x7['e' - 0x20][0], 5); // ê circumflex
            acc_glyph[2][2] |= 1 << 0;
            acc_glyph[2][1] |= 1 << 1;
            acc_glyph[2][3] |= 1 << 1;
            memcpy(acc_glyph[3], &dp_font5x7['a' - 0x20][0], 5); // à grave
            acc_glyph[3][1] |= 1 << 0;
            acc_glyph[3][2] |= 1 << 1;
            acc_ready = 1;
        }
        unsigned char c2 = (unsigned char)s[1];
        if (c2 == 0xA9) { *adv = 2; return acc_glyph[0]; }
        if (c2 == 0xA8) { *adv = 2; return acc_glyph[1]; }
        if (c2 == 0xAA) { *adv = 2; return acc_glyph[2]; }
        if (c2 == 0xA0) { *adv = 2; return acc_glyph[3]; }
    }
    *adv = 1;
    unsigned ch = c;
    if (ch < 0x20 || ch > 0x7e) ch = '?';
    return &dp_font5x7[ch - 0x20][0];
}

// draws a 5x7 glyph string scaled by `scale` (8-bit style small font,
// dp_font5x7, ASCII 0x20-0x7E, column-major)
static void legend_text(uint8_t *rgb, int w, int h, int x, int y, const char *str,
                        const uint8_t *col, int scale) {
    for (; *str; str++, x += 6 * scale) {
        int adv;
        const uint8_t *g = glyph_of(str, &adv);
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 7; cy++) {
                if (!(g[cx] & (1u << cy))) continue;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) {
                        int px = x + cx * scale + dx, py = y + cy * scale + dy;
                        if (px < 0 || px >= w || py < 0 || py >= h) continue;
                        size_t off = ((size_t)py * w + px) * 3;
                        rgb[off] = col[0]; rgb[off + 1] = col[1]; rgb[off + 2] = col[2];
                    }
            }
        str += adv - 1; // the loop's ++ does the last step
    }
}

// rendered width of a string: one 6*scale cell per glyph (accented letters
// are two UTF-8 bytes but one glyph — strlen would overcount them)
static int legend_text_width(const char *str, int scale) {
    int wpx = 0, adv;
    for (; *str; str += adv) {
        glyph_of(str, &adv);
        wpx += 6 * scale;
    }
    return wpx;
}

// largest font scale whose text still fits the image width
static int fit_scale(const char *str, int w, int x, int scale) {
    while (scale > 1 && x + legend_text_width(str, scale) > w) scale--;
    return scale;
}

// draws one legend entry — a square swatch, one text line tall, then the
// text (swatch on the right when right_edge >= 0, the whole entry then
// right-aligned to it).  The scale backs down until the entry fits `avail`.
static void legend_entry(uint8_t *rgb, int w, int h, int y, const char *str,
                         const uint8_t *col, int scale, int avail, int right_edge) {
    int side = 7 * scale, gap = 3 * scale;
    int tw = legend_text_width(str, scale);
    int need = side + gap + tw;
    while (scale > 1 && need > avail) {
        scale--;
        side = 7 * scale; gap = 3 * scale;
        tw = legend_text_width(str, scale);
        need = side + gap + tw;
    }
    int x0 = right_edge >= 0 ? right_edge - need : 10; // grid margin + 4
    int sx = right_edge >= 0 ? x0 + tw + gap : x0;
    for (int yy = 0; yy < side; yy++)
        for (int xx = 0; xx < side; xx++) {
            int px = sx + xx, py = y + yy;
            if (px < 0 || px >= w || py < 0 || py >= h) continue;
            size_t off = ((size_t)py * w + px) * 3;
            rgb[off] = col[0]; rgb[off + 1] = col[1]; rgb[off + 2] = col[2];
        }
    legend_text(rgb, w, h, right_edge >= 0 ? x0 : x0 + side + gap, y, str, col, scale);
}

// One pixel per ROM byte, exact class: each bank is a 128x128 tile (one row
// per 128 bytes), so every zone shows at its real size and place.  With
// per-byte heat, a same-geometry heat tile sits right of each bank (played
// usage, log scale).  The full .map text rides in the PNG (zTXt
// "deepeel_map"), next to the converter's version and settings (zTXt
// "deepeel_meta") and any extra chunks (usage statistics).  Under the grid,
// a legend band: rom name + coverage, then the classes in two justified
// columns, swatch beside each entry.
static int map_png_render(uint8_t *m, int banks, const char *title, const DpMapSummary *s,
                          const char *map_text, size_t map_len, const char *meta,
                          const char *path, const char *sysname, const char *filename,
                          unsigned rom_size, const uint32_t *heat, const DpPngText *extra,
                          int n_extra, const char *methods);
int dp_report_map_png(const DpResults *res, const DpConfig *cfg, const char *rom_path,
                      const char *path) {
    DpMapSummary s;
    uint8_t *m = dp_map_build(res, &s);
    if (!m) return 0;
    char title[17];
    const char *ti = res->cart && res->cart->title[0] ? res->cart->title : NULL;
    int tl = 0;
    if (ti) {
        for (; *ti && tl < 16; ti++)
            if (*ti == '?') break; // cart titles are zero-padded with '?' in places
            else if ((unsigned char)*ti >= 0x20 && (unsigned char)*ti < 0x7f) title[tl++] = *ti;
    }
    if (!tl && rom_path) { // no usable internal name: the file's base name
        const char *base = rom_path + strlen(rom_path);
        while (base > rom_path && base[-1] != '/' && base[-1] != '\\') base--;
        for (; *base && tl < 16; base++)
            if ((unsigned char)*base >= 0x20 && (unsigned char)*base < 0x7f) title[tl++] = *base;
    }
    title[tl] = 0;
    DpBuf txt;
    dp_buf_init(&txt);
    dp_report_map(res, &txt);
    double pct = s.total ? 100.0 / (double)s.total : 0.0;
    // played usage: per-zone ranking, the played .cov, the played ROM reads
    // per page — what m0boy picks its native code and resident ROM by
    DpHeat heat;
    int have_heat = dp_heat_build(res, m, &heat);
    DpBuf heat_txt, cov_txt, rom_txt;
    dp_buf_init(&heat_txt);
    dp_buf_init(&cov_txt);
    dp_buf_init(&rom_txt);
    if (have_heat) dp_heat_text(&heat, &heat_txt);
    dp_report_cov_play(res, &cov_txt);
    dp_report_romdata_play(res, &rom_txt);
    uint32_t *hb = dp_heat_bytes(res);
    // the conversion's identity: version, rom, settings, result — a tEXt
    // chunk next to the map itself
    // what each exploration method added, in % of the whole ROM: the rise of
    // the known-byte count over each discovery entry, credited by source
    double mshare[5] = {0}; // tas, rng, fork, attract, other
    {
        uint32_t prev = 0;
        double tot = (double)res->banks * 0x4000;
        for (uint32_t i = 0; i < (uint32_t)res->disc_n;) {
            // one play epoch adds one entry per worker that found something,
            // all with the epoch's final known count: split the rise by credit
            uint32_t j = i + 1;
            uint64_t csum = res->disc[i].credit;
            if (res->disc[i].credit)
                while (j < (uint32_t)res->disc_n && res->disc[j].credit &&
                       res->disc[j].known == res->disc[i].known)
                    csum += res->disc[j++].credit;
            uint32_t k = res->disc[j - 1].known;
            if (k > prev) {
                for (uint32_t q = i; q < j; q++) {
                    const char *src = res->disc[q].source;
                    int c = !strncmp(src, "tas", 3) && strncmp(src, "tas-shoots", 10) ? 0
                            : !strncmp(src, "rng", 3)                                 ? 1
                            : !strncmp(src, "attract", 7)                             ? 3
                            : (!strcmp(src, "start") || !strncmp(src, "script", 6) ||
                               !strncmp(src, "fixed", 5) || !strncmp(src, "human", 5) ||
                               !strncmp(src, "closure", 7) || !strncmp(src, "static", 6))
                                ? 4
                                : 2;
                    double f = j - i > 1 && csum ? (double)res->disc[q].credit / (double)csum
                                                 : 1.0 / (j - i);
                    mshare[c] += (k - prev) * f * 100.0 / tot;
                }
                prev = k;
            }
            i = j;
        }
        double sum = mshare[0] + mshare[1] + mshare[2] + mshare[3] + mshare[4];
        double rest = s.known * pct - sum; // provenance, final closure, inference
        if (rest > 0) mshare[4] += rest;
    }
    char meth[160];
    snprintf(meth, sizeof meth,
             "apport : tas %.1f%%  aleatoire %.1f%%  fourche %.1f%%  demo %.1f%%  "
             "statique+infere %.1f%%",
             mshare[0], mshare[1], mshare[2], mshare[3], mshare[4]);
    char meta[1280];
    snprintf(meta, sizeof meta,
             "{\"deepeel\":\"%s\",\"rom\":{\"title\":\"%s\",\"banks\":%d,\"size\":%u},"
             "\"config\":{\"max_steps_path\":%u,\"max_paths\":%u,\"max_steps_total\":%llu,"
             "\"queue\":%u,\"queue_max\":%u,\"mem_cap_mb\":%llu,\"loop_cap\":%u,\"seed_cap\":%u,"
             "\"closure_rounds\":%d,\"covered_keep\":%u,\"seed_banks\":%d},"
             "\"map\":{\"known_pct\":%.2f,\"unknown_pct\":%.2f,\"inferred_pct\":%.2f},"
             "\"heat\":{\"exec\":%llu,\"reads\":%llu,\"zones_used\":%d,\"hot_zones\":%d,"
             "\"hot_bytes\":%u,\"hot_share\":0.90},"
             "\"methods_pct\":{\"tas\":%.2f,\"random\":%.2f,\"fork\":%.2f,\"attract\":%.2f,"
             "\"static\":%.2f}}",
             DEEPEEL_VERSION, title, res->banks,
             res->cart ? (unsigned)res->cart->rom_size : 0u,
             cfg ? cfg->max_steps_path : 0u, cfg ? cfg->max_paths : 0u,
             cfg ? (unsigned long long)cfg->max_steps_total : 0ull,
             cfg ? cfg->queue_cap : 0u, cfg ? cfg->queue_max : 0u,
             cfg ? (unsigned long long)cfg->mem_cap_mb : 0ull,
             cfg ? cfg->loop_back_cap : 0u, cfg ? cfg->seed_cap : 0u,
             cfg ? cfg->closure_rounds : 0, cfg ? cfg->covered_fork_keep : 0u,
             cfg ? cfg->seed_banks : 0,
             s.known * pct, s.unknown * pct, s.inferred * pct,
             have_heat ? (unsigned long long)heat.exec_total : 0ull,
             have_heat ? (unsigned long long)heat.read_total : 0ull, have_heat ? heat.used_n : 0,
             have_heat ? heat.hot_n : 0, have_heat ? heat.hot_bytes : 0u,
             mshare[0], mshare[1], mshare[2], mshare[3], mshare[4]);
    char file[64];
    if (rom_path) {
        const char *b = rom_path + strlen(rom_path);
        while (b > rom_path && b[-1] != '/' && b[-1] != '\\') b--;
        snprintf(file, sizeof file, "%s", b);
    } else file[0] = 0;
    DpPngText extra[3] = {
        { "deepeel_heat", heat_txt.p, heat_txt.len, 0 },
        { "deepeel_cov", cov_txt.p, cov_txt.len, 0 },
        { "deepeel_rom", rom_txt.p, rom_txt.len, 0 },
    };
    int ok = map_png_render(m, (int)res->banks, title, &s, (const char *)txt.p, txt.len,
                            meta, path, res->plat ? res->plat->name : "gb", file,
                            res->cart ? (unsigned)res->cart->rom_size : 0u, hb, extra, 3, meth);
    free(hb);
    if (have_heat) dp_heat_free(&heat);
    dp_buf_free(&heat_txt);
    dp_buf_free(&cov_txt);
    dp_buf_free(&rom_txt);
    dp_buf_free(&txt);
    free(m);
    return ok;
}

int dp_map_png_from_maptext(const char *text, size_t len, int banks, const char *title,
                            const char *meta, const char *path, const char *sysname,
                            const char *filename, unsigned rom_size) {
    if (banks <= 0) return 0;
    size_t n = (size_t)banks * 0x4000;
    uint8_t *m = calloc(n ? n : 1, 1); // DP_MAP_UNKNOWN
    if (!m) return 0;
    // parse "bank start end(inclusive) class" — CPU-view addresses (bank 0
    // from 0, the others from 0x4000), one run per line, '#' comments
    DpMapSummary s;
    memset(&s, 0, sizeof s);
    s.total = n;
    const char *p = text, *end = text + len;
    while (p < end) {
        if (*p == '#' || *p == '\n') { // a whole comment line, or a newline
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }
        unsigned bank, a, e;
        char cls[32];
        int got = sscanf(p, "%u %x %x %31s", &bank, &a, &e, cls);
        while (p < end && *p != '\n') p++;
        if (got != 4 || bank >= (unsigned)banks) continue;
        unsigned win = bank ? 0x4000u : 0u;
        if (a < win || e < a) continue;
        size_t lo = bank * 0x4000ull + (a - win), hi = bank * 0x4000ull + (e - win);
        if (lo >= n) continue;
        if (hi >= n) hi = n - 1;
        int c = -1;
        for (int k = 0; k < DP_MAP_COUNT; k++)
            if (!strcmp(cls, dp_map_class_name(k))) { c = k; break; }
        if (c < 0) continue;
        memset(m + lo, c, hi - lo + 1);
    }
    for (size_t k = 0; k < n; k++) {
        int c = m[k];
        s.cls[c]++;
        if (c == DP_MAP_UNKNOWN) s.unknown++; else s.known++;
        if (dp_map_is_code(c)) s.code++;
        else if (dp_map_is_data(c)) s.data++;
        else if (c == DP_MAP_PAD || c == DP_MAP_INF_PAD) s.pad++;
        if (c >= DP_MAP_INF_DATA) s.inferred++;
    }
    return map_png_render(m, banks, title, &s, text, len, meta, path, sysname, filename,
                          rom_size, NULL, NULL, 0, NULL);
}

// the shared renderer: squares grid + legend band + tEXt chunks, from a
// per-byte class array and the .map text to embed
// heat ramp: dark (cold, 0) through purple and red to pale yellow (hottest)
static void heat_rgb(double t, uint8_t *px) {
    static const uint8_t stops[5][3] = {
        { 0x30, 0x1a, 0x50 }, { 0x8a, 0x22, 0x6a }, { 0xd8, 0x3c, 0x3c },
        { 0xf2, 0x98, 0x2a }, { 0xfa, 0xec, 0xa0 } };
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double f = t * 4.0;
    int i = (int)f;
    if (i > 3) i = 3;
    f -= i;
    for (int c = 0; c < 3; c++)
        px[c] = (uint8_t)(stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f + 0.5);
}

static int map_png_render(uint8_t *m, int banks, const char *title, const DpMapSummary *s,
                          const char *map_text, size_t map_len, const char *meta,
                          const char *path, const char *sysname, const char *filename,
                          unsigned rom_size, const uint32_t *heat, const DpPngText *extra,
                          int n_extra, const char *methods) {
    // TILE: one pixel per byte, 128 bytes per row; with heat, a heat tile
    // follows each bank tile (TGAP apart)
    enum { TILE = 128, TGAP = 2, GAP = 6 };
    // the ROM grid, then (with heat) a separate heat grid of the same
    // geometry below it, under its own caption
    const int BANK_PX = TILE;
    const int PER_ROW = 8;
    (void)TGAP;
    // adaptive scale: bytes per pixel (power of two, 1 = the exact render)
    // doubles until the image fits the pixel budget, or one bank tile is a
    // single row (128 bytes per pixel)
    int bpp = 1;
    {
        int bc = banks < PER_ROW ? banks : PER_ROW, br = (banks + PER_ROW - 1) / PER_ROW;
        if (bc < 1) bc = 1;
        if (br < 1) br = 1;
        for (;;) {
            double pw = GAP + bc * (double)(BANK_PX + GAP);
            double ph = GAP + br * (double)(TILE / bpp + GAP);
            if (heat) ph *= 2;
            if (pw * ph <= (double)dp_map_png_budget || bpp >= 128) break;
            bpp <<= 1;
        }
    }
    const int BANK_H = TILE / bpp;
    static const char *fr[DP_MAP_COUNT] = {
        "inconnu", "code naturel", "code forcé", "code statique",
        "donnée copiée", "donnée lue", "donnée statique", "table", "padding",
        "donnée inférée", "code inféré", "padding inféré" };
    int cols = banks < PER_ROW ? banks : PER_ROW;
    int rows = (banks + PER_ROW - 1) / PER_ROW;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    // legend band sizing: one title line + one line per class present
    int order[DP_MAP_COUNT], ncls = 0;
    for (int c = 0; c < DP_MAP_COUNT; c++)
        if (s->cls[c]) order[ncls++] = c;
    for (int i = 1; i < ncls; i++) { // insertion sort, largest share first
        int cur = order[i], j = i - 1;
        while (j >= 0 && s->cls[order[j]] < s->cls[cur]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = cur;
    }
    enum { FSCALE = 2 };
    int w = GAP + cols * (BANK_PX + GAP);
    // two justified columns only when each half can hold an entry; otherwise
    // one left-aligned column
    int two_cols = w / 2 - 12 >= 110;
    int half = two_cols ? (ncls + 1) / 2 : ncls;
    // the common legend scale (a mixed-size legend reads badly): the largest
    // scale at which the widest entry of each column still fits its column.
    // The line height follows the scale: a 1x legend must not keep the 2x
    // interline.
    int lavail = two_cols ? w / 2 - 14 : w - 12, ravail = w - GAP - w / 2;
    int ls = FSCALE, rs = FSCALE;
    double pct0 = s->total ? 100.0 / (double)s->total : 0.0;
    for (int r = 0; r < half; r++) {
        if (r < ncls) {
            char line[64];
            double p = s->cls[order[r]] * pct0;
            if (p > 0.0 && p < 0.05) snprintf(line, sizeof line, "%s <0.1%%", fr[order[r]]);
            else snprintf(line, sizeof line, "%s %.1f%%", fr[order[r]], p);
            int need = (7 + 3) * ls + legend_text_width(line, ls);
            while (ls > 1 && need > lavail) { ls--; need = (7 + 3) * ls + legend_text_width(line, ls); }
        }
        if (two_cols && r + half < ncls) {
            char line[64];
            double p = s->cls[order[r + half]] * pct0;
            if (p > 0.0 && p < 0.05) snprintf(line, sizeof line, "%s <0.1%%", fr[order[r + half]]);
            else snprintf(line, sizeof line, "%s %.1f%%", fr[order[r + half]], p);
            int need = (7 + 3) * rs + legend_text_width(line, rs);
            while (rs > 1 && need > ravail) { rs--; need = (7 + 3) * rs + legend_text_width(line, rs); }
        }
    }
    int s_common = ls < rs ? ls : rs; // both columns at the same scale
    ls = rs = s_common;
    int LH = 7 * s_common + 4;
    int map_h = GAP + rows * (BANK_H + GAP);
    int heat_oy = map_h - GAP; // heat grid right below the ROM grid
    int grid_h = heat ? heat_oy + map_h : map_h;
    int n_meth = methods ? 1 : 0; // one legend line for the method shares
    // identity block: title row, then system / file / sizes / converter
    char size_txt[24];
    if (rom_size >= 1024 * 1024)
        snprintf(size_txt, sizeof size_txt, "%.1f Mio", (double)rom_size / (1024.0 * 1024.0));
    else
        snprintf(size_txt, sizeof size_txt, "%u Kio", rom_size / 1024);
    char info_a[192];
    snprintf(info_a, sizeof info_a, "%s - %s - %s", sysname ? sysname : "?",
             filename && filename[0] ? filename : "?", size_txt);
    int head_LH = 7 * FSCALE + 4;
    int foot_LH = 11; // the 1-px note (+ heat scale) + deepeel signature, bottom right
    int h = grid_h + GAP + head_LH + GAP + (7 + 4) + GAP + n_meth * (7 + 4 + GAP) + half * LH + GAP + foot_LH;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) { free(m); return 0; }
    for (size_t i = 0; i < (size_t)w * h; i++) { rgb[i * 3] = 0x18; rgb[i * 3 + 1] = 0x18; rgb[i * 3 + 2] = 0x1c; }
    // heat scale: log2(hits + 1) against the hottest byte of the ROM
    double hlog = 0.0;
    if (heat) {
        uint32_t hmax = 0;
        for (size_t k = 0; k < (size_t)banks * 0x4000; k++)
            if (heat[k] > hmax) hmax = heat[k];
        hlog = hmax ? log2((double)hmax + 1.0) : 0.0;
    }
    for (int b = 0; b < banks; b++) {
        int ox = GAP + (b % PER_ROW) * (BANK_PX + GAP), oy = GAP + (b / PER_ROW) * (BANK_H + GAP);
        for (int g = 0; g < 0x4000 / bpp; g++) {
            size_t k = (size_t)b * 0x4000 + (size_t)g * (size_t)bpp;
            int x = g % TILE, y = g / TILE;
            uint8_t *px = rgb + ((size_t)(oy + y) * w + ox + x) * 3;
            int cls = m[k];
            uint32_t hv = heat ? heat[k] : 0;
            if (bpp > 1) { // dominant class of the group (a known class wins a tie); hottest byte
                unsigned cnt[DP_MAP_COUNT] = {0};
                for (int q = 0; q < bpp; q++) {
                    cnt[m[k + (size_t)q]]++;
                    if (heat && heat[k + (size_t)q] > hv) hv = heat[k + (size_t)q];
                }
                cls = 0;
                for (int c2 = 1; c2 < DP_MAP_COUNT; c2++)
                    if (cnt[c2] >= cnt[cls] && cnt[c2]) cls = c2;
                if (cnt[0] > cnt[cls]) cls = 0;
            }
            const uint8_t *c = map_rgb[cls];
            px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
            if (heat) {
                uint8_t *hp = rgb + ((size_t)(heat_oy + oy + y) * w + ox + x) * 3;
                if (hv && hlog > 0.0) heat_rgb(log2((double)hv + 1.0) / hlog, hp);
                else { hp[0] = 0x22; hp[1] = 0x22; hp[2] = 0x28; }
            }
        }
    }
    // the legend itself: rom name, identity line, the classes in two columns,
    // then the bottom-right note — a real grid-sized square (one cell = 64
    // ROM bytes), "64 o", a slash and the deepeel signature, one small line
    int y = grid_h + GAP;
    double pct = s->total ? 100.0 / (double)s->total : 0.0;
    char head[160];
    // truncated, never rounded up: 100.0% only when nothing is unknown
    snprintf(head, sizeof head, "%s - %.1f%%", title[0] ? title : "?",
             s->unknown ? floor(s->known * pct * 10.0) / 10.0 : 100.0);
    int head_scale = fit_scale(head, w, GAP, FSCALE);
    legend_text(rgb, w, h, GAP, y, head, (const uint8_t *)"\xe0\xe0\xe0", head_scale);
    y += head_LH + GAP;
    legend_text(rgb, w, h, GAP, y, info_a, (const uint8_t *)"\x90\x90\x98", 1);
    y += (7 + 4) + GAP;
    if (methods) {
        legend_text(rgb, w, h, GAP, y, methods, (const uint8_t *)"\xc0\xc0\xc8", 1);
        y += (7 + 4) + GAP;
    }
    for (int r = 0; r < half; r++) {
        int ry = y + r * LH;
        if (r < ncls) {
            int c = order[r];
            char line[64];
            double p = s->cls[c] * pct;
            if (p > 0.0 && p < 0.05) snprintf(line, sizeof line, "%s <0.1%%", fr[c]);
            else snprintf(line, sizeof line, "%s %.1f%%", fr[c], p);
            legend_entry(rgb, w, h, ry, line, map_rgb[c], ls, lavail, -1);
        }
        if (two_cols && r + half < ncls) {
            int c = order[r + half];
            char line[64];
            double p = s->cls[c] * pct;
            if (p > 0.0 && p < 0.05) snprintf(line, sizeof line, "%s <0.1%%", fr[c]);
            else snprintf(line, sizeof line, "%s %.1f%%", fr[c], p);
            legend_entry(rgb, w, h, ry, line, map_rgb[c], rs, ravail, w - GAP);
        }
    }
    // the bottom line: heat scale (left, when drawn), then on the right
    // "1 px = 1 o", "/", the deepeel signature
    {
        char sig[40];
        snprintf(sig, sizeof sig, "deepeel %s", DEEPEEL_VERSION);
        const uint8_t *col = (const uint8_t *)"\x70\x70\x78";
        int fy = y + half * LH + GAP;
        if (heat) {
            const char *lt = "chaleur (jeu) : froid", *rt = "chaud";
            int x = GAP;
            legend_text(rgb, w, h, x, fy, lt, col, 1);
            x += legend_text_width(lt, 1) + 4;
            for (int xx = 0; xx < 64; xx++)
                for (int yy = 0; yy < 7; yy++) {
                    int px = x + xx, py = fy + yy;
                    if (px >= w || py >= h) continue;
                    heat_rgb(xx / 63.0, rgb + ((size_t)py * w + px) * 3);
                }
            x += 64 + 4;
            legend_text(rgb, w, h, x, fy, rt, col, 1);
        }
        char note_buf[24];
        snprintf(note_buf, sizeof note_buf, "1 px = %d o", bpp);
        const char *px_note = note_buf;
        int total = legend_text_width(px_note, 1) + 4 + legend_text_width("/", 1) + 4 +
                    legend_text_width(sig, 1);
        int x = w - GAP - total;
        legend_text(rgb, w, h, x, fy, px_note, col, 1);
        x += legend_text_width(px_note, 1) + 4;
        legend_text(rgb, w, h, x, fy, "/", col, 1);
        x += legend_text_width("/", 1) + 4;
        legend_text(rgb, w, h, x, fy, sig, col, 1);
    }
    DpBuf cv;
    dp_buf_init(&cv);
    dp_map_cv_encode(m, (size_t)banks * 0x4000, &cv);
    DpPngText items[3 + 8];
    int ni = 0;
    items[ni++] = (DpPngText){ "dpCv", cv.p, cv.len, 0 };
    items[ni++] = (DpPngText){ "deepeel_map", map_text, map_len, 0 };
    items[ni++] = (DpPngText){ "deepeel_meta", meta, strlen(meta), 0 };
    for (int i = 0; i < n_extra && ni < (int)(sizeof items / sizeof items[0]); i++)
        items[ni++] = extra[i];
    int ok = dp_png_save_chunks(path, w, h, rgb, items, ni);
    dp_buf_free(&cv);
    free(rgb);
    return ok;
}
