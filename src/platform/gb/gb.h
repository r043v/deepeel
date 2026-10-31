// gb.h: Game Boy platform extras for the play sessions — the hardware clock
// tick and the pad latch.  The forking explorer uses none of this.
#ifndef MS_GB_H
#define MS_GB_H

#include "../../core/machine.h"

#include <stdint.h>

// advance the event-driven hardware by t T-cycles: DIV, TIMA (overflow ->
// TMA reload + IF bit 2), the PPU line clock (LY 0..153, VBlank IF bit 0 at
// LY 144, LYC match, approximate STAT mode bits).  Line and mode timings are
// approximations good enough for LY polls and HALT waits, not a PPU model.
void gb_tick(MsMachine *m, uint32_t t);

// latch the pad (a=1 b=2 select=4 start=8 right=16 left=32 up=64 down=128);
// a newly pressed key raises IF bit 4, as on hardware
void ms_gb_set_buttons(MsMachine *m, uint8_t mask);

// T-cycles still to run before the current video frame ends (70224 total)
static inline uint32_t gb_frame_left(const MsMachine *m) {
    uint32_t ly = m->io[0x44 - 0xFF00];
    uint32_t t = ly * 456u + m->clk_line;
    uint32_t in_frame = t % 70224u;
    return 70224u - in_frame;
}

// T-cycles until the next event that could wake a HALT and raise an
// interrupt enabled in IE, or 0 when none exists (halt forever).
uint32_t gb_halt_advance(const MsMachine *m);

// screenshot renderer: the current VRAM/OAM/registers as a 160x144 DMG
// grayscale frame (shades 0-3, row-major).  A snapshot renderer, not a
// cycle PPU — mid-frame register changes are not reproduced.
void gb_render_frame(const MsMachine *m, uint8_t *px);

#endif
