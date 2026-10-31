// mach_util.h: machine backend helpers shared by the test binaries.
#ifndef MS_MACH_UTIL_H
#define MS_MACH_UTIL_H
#include "../src/core/cart.h"
#include "../src/mach/m0boy.h"

// open machine instance 0 for a cartridge (tests run one at a time)
static MsMach *mach_for(const MsCart *c) {
    int vi = ms_mach_pick_variant(c->rom);
    if (vi < 0) return NULL;
    MsMach *m = ms_mach_open(vi, 0);
    if (!m) return NULL;
    ms_mach_set_rom(m, c->rom, c->rom_size);
    return m;
}
#endif
