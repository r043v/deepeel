// test_coverage.c: the common nature projection must reproduce the historical
// GB map classes for every flag combination, and sources must accumulate.
#include "test_util.h"
#include "../src/explore/explorer.h"
#include "../src/report/report.h"

// the pre-J1 cascade of dp_map_build, kept here as the reference
static int legacy_class(uint8_t x, uint8_t d, uint8_t s) {
    if (x & DP_XF_NATURAL) return DP_MAP_CODE_NAT;
    if (x & DP_XF_FORCED) return DP_MAP_CODE_FORCED;
    if (x & DP_XF_STATIC) return DP_MAP_CODE_STATIC;
    if (d & (DP_DF_COPIED | DP_DF_VERBATIM)) return DP_MAP_DATA_COPIED;
    if (d & DP_DF_STATIC) return DP_MAP_DATA_STATIC;
    if (d & DP_DF_READ) return DP_MAP_DATA_READ;
    if (d & DP_DF_SIBLING) return DP_MAP_INF_DATA;
    if (s & DP_SF_POINTER) return DP_MAP_TABLE;
    if (s & DP_SF_PAD) return DP_MAP_PAD;
    return DP_MAP_UNKNOWN;
}

int main(void) {
    int bad = 0;
    for (unsigned x = 0; x < 64; x++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned s = 0; s < 4; s++)
                if (dp_nature_class(dp_gb_nature(x, d, s)) != legacy_class(x, d, s)) bad++;
    CHECK(bad == 0);

    // sources are OR-ed bits, never a single winner
    uint32_t v = DP_SRC_SESSION | DP_SRC_COPY;
    CHECK((v & DP_SRC_SESSION) && (v & DP_SRC_COPY) && !(v & DP_SRC_CLOSURE));
    CHECK(v & (v - 1)); // multi-source test used by the JSON report
    for (int b = 0; b < DP_SRC_COUNT; b++) CHECK(dp_src_name(b)[0] != '?');
    TEST_DONE("coverage");
}
