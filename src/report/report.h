// report.h: text / JSON / .cov output of the results.
#ifndef DP_REPORT_H
#define DP_REPORT_H

#include "../explore/explorer.h"
#include "../common.h"

void dp_report_text(const DpResults *res, const DpConfig *cfg, DpBuf *out);
void dp_report_json(const DpResults *res, const DpConfig *cfg, DpBuf *out);
void dp_report_cov(const DpResults *res, DpBuf *out);
// same lines, hits from the played sessions only (the map PNG's deepeel_cov)
void dp_report_cov_play(const DpResults *res, DpBuf *out);
// ROM data-read heat per 256-byte page, m0boy's romdata-*.prof format
void dp_report_romdata(const DpResults *res, DpBuf *out);
// same, played reads only (the map PNG's deepeel_rom, m0boy's --rom-profile)
void dp_report_romdata_play(const DpResults *res, DpBuf *out);

// ---- heat: played usage per memory-map zone (heat.c)
typedef struct DpZone {
    uint16_t bank, lo, hi; // lo/hi inclusive, 0x0000..0x3FFF within the bank
    uint8_t cls;           // DP_MAP_* class of the run
    uint32_t bytes;
    uint64_t exec, reads;  // played instructions executed / ROM bytes read
    uint32_t rank;         // 1 = hottest
    int hot;               // in the hot set (90% of usage)
} DpZone;
typedef struct DpHeat {
    DpZone *z; // ROM order
    int n, used_n, hot_n;
    uint32_t hot_bytes;
    uint64_t exec_total, read_total;
} DpHeat;
// zones of a dp_map_build array, ranked by played usage
int dp_heat_build(const DpResults *res, const uint8_t *map, DpHeat *h);
void dp_heat_free(DpHeat *h);
void dp_heat_text(const DpHeat *h, DpBuf *out);              // deepeel_heat chunk
void dp_heat_top_text(const DpHeat *h, int top, DpBuf *out); // text report
// per-byte played heat for the map image (malloc'd, banks*0x4000)
uint32_t *dp_heat_bytes(const DpResults *res);

// classification of a 64-byte block of the ROM for the map views
char dp_block_class(const DpResults *res, int bank, int block);

// ---- .map: classification of 100% of the ROM (see map.c)
enum { DP_MAP_UNKNOWN, DP_MAP_CODE_NAT, DP_MAP_CODE_FORCED, DP_MAP_CODE_STATIC,
       DP_MAP_DATA_COPIED, DP_MAP_DATA_READ, DP_MAP_DATA_STATIC, DP_MAP_TABLE, DP_MAP_PAD,
       DP_MAP_INF_DATA, DP_MAP_INF_CODE, DP_MAP_INF_PAD, DP_MAP_PROB_CODE, DP_MAP_COUNT };
typedef struct DpMapSummary {
    uint64_t total, known, unknown, code, data, pad, inferred, probable;
    uint64_t cls[DP_MAP_COUNT];
} DpMapSummary;
// malloc'd per-byte class array (banks*0x4000), caller frees
uint8_t *dp_map_build(const DpResults *res, DpMapSummary *sum);
// sticky inferred classes persisted beside coverage.bin (a resumed run never loses an inferred byte)
int dp_map_sticky_save(const DpResults *res, const char *path);
long dp_map_sticky_load(DpResults *res, const char *path);
// what each exploration method added, in % of the whole ROM: tas, random, fork, attract, static+inferred
void dp_map_methods_pct(const DpResults *res, const DpMapSummary *s, double out[5]);
const char *dp_map_class_name(int c);
const uint8_t *dp_map_rgb(int cls); // the .map.png colour of a class (3 bytes)
int dp_map_is_code(int c);
int dp_map_is_data(int c);
void dp_map_summary_text(const DpMapSummary *s, DpBuf *out);
// the unclassified ranges (largest first), from a dp_map_build array
void dp_map_residual_text(const DpResults *res, const uint8_t *m, DpBuf *out);
// one unclassified range (rom coordinates: lo/hi inside the bank)
typedef struct DpResidualRange {
    uint16_t bank, lo, hi; // lo/hi inclusive, 0x0000..0x3FFF within the bank
    uint32_t len;
} DpResidualRange;
// every unclassified range of a dp_map_build array, up to cap of them;
// returns the total number of ranges (even beyond cap)
int dp_map_residual_list(const DpResults *res, const uint8_t *m, DpResidualRange *out, int cap);
// what blocks one residual range (edge classes): code-code/code-data/
// data-data/padding-fringe/edge-code/edge-data/bank — explains, never classifies
const char *dp_residual_mechanism(const uint8_t *m, const DpResidualRange *r);
// per-mechanism rollup of the residual unknown (ranges, bytes, biggest)
void dp_residual_anatomy_text(const DpResults *res, const uint8_t *m, DpBuf *out);
// compact machine-readable profile of one run (rom identity, config echo,
// map summary, full residual list, every counter, tree and play facts):
// the aggregation input of a multi-ROM campaign
struct DpTree;
void dp_report_summary(const DpResults *res, const DpConfig *cfg, const struct DpTree *tree,
                       const char *stop_reason, unsigned long long frames, int tas_n, DpBuf *out);
void dp_report_map(const DpResults *res, DpBuf *out);
// rom_path feeds the legend's name fallback (the internal title comes first);
// cfg rides into the "deepeel_meta" tEXt chunk (version + settings + result)
int dp_report_map_png(const DpResults *res, const DpConfig *cfg, const char *rom_path,
                      const char *path);
// re-render the squares image from a saved .map text (no re-exploration):
// the text is embedded verbatim as "deepeel_map", meta rides in "deepeel_meta"
int dp_map_png_from_maptext(const char *text, size_t len, int banks, const char *title,
                            const char *meta, const char *path, const char *sysname,
                            const char *filename, unsigned rom_size);
// dpCv: lossless class vector (RLE) embedded in the map PNG; decode returns 1
// when the runs fill exactly n bytes
int dp_map_ch_encode(const DpResults *res, DpBuf *out);   // dpCh: flags and hit counters
void dp_map_cv_encode(const uint8_t *m, size_t n, DpBuf *out);
int dp_map_cv_decode(const uint8_t *p, size_t len, uint8_t *m, size_t n);
int dp_map_cv_to_text(const uint8_t *cv, size_t len, int banks, DpBuf *out);
extern unsigned dp_map_png_budget; // pixel budget of the squares image (default 16 Mpx)
void dp_report_html(const DpResults *res, DpBuf *out);

#endif
