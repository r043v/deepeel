// report.h: text / JSON / .cov output of the results.
#ifndef DP_REPORT_H
#define DP_REPORT_H

#include "../explore/explorer.h"
#include "../common.h"

void dp_report_text(const DpResults *res, const DpConfig *cfg, DpBuf *out);
void dp_report_json(const DpResults *res, const DpConfig *cfg, DpBuf *out);
void dp_report_cov(const DpResults *res, DpBuf *out);
// ROM data-read heat per 256-byte page, m0boy's romdata-*.prof format
void dp_report_romdata(const DpResults *res, DpBuf *out);

// classification of a 64-byte block of the ROM for the map views
char dp_block_class(const DpResults *res, int bank, int block);

// ---- .map: classification of 100% of the ROM (see map.c)
enum { DP_MAP_UNKNOWN, DP_MAP_CODE_NAT, DP_MAP_CODE_FORCED, DP_MAP_CODE_STATIC,
       DP_MAP_DATA_COPIED, DP_MAP_DATA_READ, DP_MAP_TABLE, DP_MAP_PAD,
       DP_MAP_INF_DATA, DP_MAP_INF_CODE, DP_MAP_INF_PAD, DP_MAP_COUNT };
typedef struct DpMapSummary {
    uint64_t total, known, unknown, code, data, pad, inferred;
    uint64_t cls[DP_MAP_COUNT];
} DpMapSummary;
// malloc'd per-byte class array (banks*0x4000), caller frees
uint8_t *dp_map_build(const DpResults *res, DpMapSummary *sum);
const char *dp_map_class_name(int c);
int dp_map_is_code(int c);
int dp_map_is_data(int c);
void dp_map_summary_text(const DpMapSummary *s, DpBuf *out);
// the unclassified ranges (largest first), from a dp_map_build array
void dp_map_residual_text(const DpResults *res, const uint8_t *m, DpBuf *out);
void dp_report_map(const DpResults *res, DpBuf *out);
int dp_report_map_png(const DpResults *res, const char *path);
void dp_report_html(const DpResults *res, DpBuf *out);

#endif
