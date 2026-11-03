// test_sega_coverage.c: the common nature projection must reproduce the
// historical Sega map cascade for every flag combination.
#include "test_util.h"
#include "../src/platform/sega/explore.h"

// the pre-J1 cascade of dp_sega_map_build, kept here as the reference
static int legacy_class(uint8_t f, uint8_t st) {
    if (f & DP_SEGA_NATURAL) return DP_SMAP_CODE_NAT;
    if (f & DP_SEGA_FORCED) return DP_SMAP_CODE_FORCED;
    if (f & DP_SEGA_STATIC) return DP_SMAP_CODE_STATIC;
    if (f & DP_SEGA_COPIED) return DP_SMAP_DATA_COPIED;
    if (f & DP_SEGA_STATIC_DATA) return DP_SMAP_DATA_STATIC;
    if (f & DP_SEGA_DATA) return DP_SMAP_DATA_READ;
    if (st & DP_SEGA_TABLE) return DP_SMAP_TABLE;
    if (st & DP_SEGA_PAD) return DP_SMAP_PAD;
    return DP_SMAP_UNKNOWN;
}

int main(void) {
    int bad = 0;
    for (unsigned f = 0; f < 256; f++)
        for (unsigned st = 0; st < 256; st++)
            if (dp_nature_class(dp_sega_nature((uint8_t)f, (uint8_t)st)) != legacy_class((uint8_t)f, (uint8_t)st)) bad++;
    CHECK(bad == 0);
    TEST_DONE("sega_coverage");
}
