// report.h: text / JSON / .cov output of the results.
#ifndef MS_REPORT_H
#define MS_REPORT_H

#include "../explore/explorer.h"
#include "../common.h"

void ms_report_text(const MsResults *res, const MsConfig *cfg, MsBuf *out);
void ms_report_json(const MsResults *res, const MsConfig *cfg, MsBuf *out);
void ms_report_cov(const MsResults *res, MsBuf *out);
// ROM data-read heat per 256-byte page, m0boy's romdata-*.prof format
void ms_report_romdata(const MsResults *res, MsBuf *out);

// classification of a 64-byte block of the ROM for the map views
char ms_block_class(const MsResults *res, int bank, int block);

// ---- .map: classification of 100% of the ROM (see map.c)
enum { MS_MAP_UNKNOWN, MS_MAP_CODE_NAT, MS_MAP_CODE_FORCED, MS_MAP_CODE_STATIC,
       MS_MAP_DATA_COPIED, MS_MAP_DATA_READ, MS_MAP_TABLE, MS_MAP_PAD,
       MS_MAP_INF_DATA, MS_MAP_INF_CODE, MS_MAP_INF_PAD, MS_MAP_COUNT };
typedef struct MsMapSummary {
    uint64_t total, known, unknown, code, data, pad, inferred;
    uint64_t cls[MS_MAP_COUNT];
} MsMapSummary;
// malloc'd per-byte class array (banks*0x4000), caller frees
uint8_t *ms_map_build(const MsResults *res, MsMapSummary *sum);
const char *ms_map_class_name(int c);
int ms_map_is_code(int c);
int ms_map_is_data(int c);
void ms_map_summary_text(const MsMapSummary *s, MsBuf *out);
void ms_report_map(const MsResults *res, MsBuf *out);
int ms_report_map_png(const MsResults *res, const char *path);
void ms_report_html(const MsResults *res, MsBuf *out);

#endif
