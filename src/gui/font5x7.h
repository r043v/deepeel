// font5x7.h: embedded 5x7 bitmap font (ASCII 0x20-0x7E), classic column-major
// glyphs (bit 0 = top row), public domain shape set.
#ifndef DP_FONT5X7_H
#define DP_FONT5X7_H

#include <stdint.h>

// 5 bytes per glyph, one byte per column, bit 0 = top pixel
extern const uint8_t dp_font5x7[95][5];

#endif
