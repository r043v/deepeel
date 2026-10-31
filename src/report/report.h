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

#endif
