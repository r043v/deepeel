#ifndef DP_GAMBATTE_INIT_H
#define DP_GAMBATTE_INIT_H
#include <stdint.h>
// BizHawk/Gambatte DMG post-boot-ROM memory image ($C000-$DFFF, $FF80-$FFFE)
extern const uint8_t dp_gambatte_wram[0x2000];
extern const uint8_t dp_gambatte_hram[0x7F];
#endif
