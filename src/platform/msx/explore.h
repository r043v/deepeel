#ifndef DP_MSX_EXPLORE_H
#define DP_MSX_EXPLORE_H
#include "../sega/explore.h"
#include "../sega/backend.h"
// Shared result format uses physical cartridge offsets and physical RAM cells.
const DpSegaBackend *dp_msx_backend(void);
const DpSegaSystem *dp_msx_system(void);
DpSegaOptions dp_msx_options(void);
#endif
