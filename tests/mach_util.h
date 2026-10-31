// mach_util.h: machine backend helpers shared by the test binaries.
#ifndef DP_MACH_UTIL_H
#define DP_MACH_UTIL_H
#include "../src/core/cart.h"
#include "../src/mach/m0boy.h"

// open machine instance 0 for a cartridge (tests run one at a time)
static DpMach *mach_for(const DpCart *c) {
    int vi = dp_mach_pick_variant(c->rom);
    if (vi < 0) return NULL;
    DpMach *m = dp_mach_open(vi, 0);
    if (!m) return NULL;
    dp_mach_set_rom(m, c->rom, c->rom_size);
    return m;
}
#endif
