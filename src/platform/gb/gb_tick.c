// gb_tick.c: the play-session hardware clock.  Advances DIV, the timer and
// the PPU line counter by T-cycles; raises IF bits the way a DMG would.
// Timings are the standard ones for DIV (64 T/step), the timer rates and the
// 456 T/70224 T line/frame; STAT mode boundaries inside a line are
// approximate (OAM 0-80, VRAM 80-252, HBlank 252-456).  See docs/GB.fr.md.
#include "gb.h"

#include "../../core/machine.h"

#define IO(r) (m->io[(r) - 0xFF00])

static void set_if(MsMachine *m, uint8_t bit) { IO(0xFF0F) |= bit; }

void gb_tick(MsMachine *m, uint32_t t) {
    // DIV: increments every 64 T-cycles
    m->clk_div += t;
    while (m->clk_div >= 64) {
        m->clk_div -= 64;
        IO(0xFF04) = (uint8_t)(IO(0xFF04) + 1);
    }
    // TIMA: enabled by TAC bit 2, rate by TAC bits 1-0
    uint8_t tac = IO(0xFF07);
    if (tac & 0x04) {
        static const uint32_t period[4] = { 1024, 16, 64, 256 };
        uint32_t p = period[tac & 3];
        m->clk_tima += t;
        while (m->clk_tima >= p) {
            m->clk_tima -= p;
            if (++IO(0xFF05) == 0) { // overflow: reload TMA, raise the timer IRQ
                IO(0xFF05) = IO(0xFF06);
                set_if(m, 0x04);
            }
        }
    }
    // PPU line clock: 456 T-cycles per line, 154 lines per frame
    m->clk_line += t;
    while (m->clk_line >= 456) {
        m->clk_line -= 456;
        uint8_t ly = (uint8_t)(IO(0xFF44) + 1);
        if (ly > 153) ly = 0;
        IO(0xFF44) = ly;
        uint8_t stat = IO(0xFF41);
        if (ly == 144) {
            set_if(m, 0x01); // VBlank
            if (stat & 0x10) set_if(m, 0x02);
        }
        // LYC match and mode-2 (OAM) STAT sources, on visible lines
        if (ly < 144 && (stat & 0x20)) set_if(m, 0x02);
        if (stat & 0x40 && IO(0xFF45) == ly) set_if(m, 0x02);
    }
    // approximate STAT mode bits inside the current line
    {
        uint32_t pos = m->clk_line;
        uint8_t mode;
        if (IO(0xFF44) >= 144) mode = 1;
        else mode = pos < 80 ? 2 : (pos < 252 ? 3 : 0);
        IO(0xFF41) = (uint8_t)((IO(0xFF41) & 0x7C) | 0x80 | mode);
    }
}

void ms_gb_set_buttons(MsMachine *m, uint8_t mask) {
    uint8_t pressed = (uint8_t)(mask & ~m->buttons);
    m->buttons = mask;
    if (pressed) IO(0xFF0F) |= 0x10; // joypad interrupt on a new press
}

uint32_t gb_halt_advance(const MsMachine *m) {
    // smallest advance that can actually raise an IF bit enabled in IE: a
    // timer overflow, a STAT source enabled in STAT, VBlank, or the next
    // frame boundary (a new key press).  Returns 0 when nothing can ever
    // wake this halt.
    uint8_t ie = (uint8_t)(m->ie & 0x1F);
    if (!ie) return 0;
    uint32_t best = UINT32_MAX;
    uint32_t frame_left = gb_frame_left(m);
    if ((IO(0xFF07) & 0x04) && (ie & 0x04)) {
        static const uint32_t period[4] = { 1024, 16, 64, 256 };
        uint32_t p = period[IO(0xFF07) & 3];
        // cycles left to the next overflow of TIMA
        uint32_t ticks_left = (uint32_t)(256 - IO(0xFF05)) * p - m->clk_tima;
        if (ticks_left < best) best = ticks_left;
    }
    uint8_t ly = IO(0xFF44);
    uint32_t to_line = 456 - m->clk_line;
    if (ie & 0x01) { // VBlank always raises IF bit 0 at LY 144, next frame included
        uint32_t lines = ly < 144 ? (uint32_t)(144 - ly) : (uint32_t)(154 - ly + 144);
        uint32_t t = lines * 456u - m->clk_line;
        if (t < best) best = t;
    }
    if (ie & 0x02) { // STAT sources, only when enabled in STAT
        uint8_t stat = IO(0xFF41);
        if (ly < 144 && (stat & 0x20) && to_line < best) best = to_line; // mode 2, every visible line
        if (stat & 0x40) { // LYC match at a line start
            uint8_t lyc = IO(0xFF45);
            uint32_t lines = lyc >= ly ? (uint32_t)(lyc - ly) : (uint32_t)(154 - ly + lyc);
            if (!lines) lines = 1;
            uint32_t t = lines * 456u - m->clk_line;
            if (t < best) best = t;
        }
    }
    if ((ie & 0x10) && frame_left < best) best = frame_left; // a key may be pressed next frame
    if (best == UINT32_MAX) return 0;
    if (best == 0) best = 1;
    return best;
}
