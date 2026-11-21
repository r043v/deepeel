// registry.c: the WonderSwan/WonderSwan Color hardware registry (spec §3):
// every I/O port surface the machine models, the interrupt sources and the
// memory map.  The bus is one flat 20-bit linear space (segment << 4 |
// offset); the I/O is CPU port space (IN/OUT), so port_io is set and find()
// is not needed — every port is 8-bit and the kernel indexes io_regs by
// port number.
#include <string.h>
#include "../platform.h"

// the port map, from m0boy's runtime dispatch (ws.c ws_in/ws_out).  First
// match wins: the port windows (DMA, audio, EEPROM) come before the
// general-purpose video range 0x00-0x3F.
static const DpIoReg ws_io[] = {
    { 0x00, 0x00, "DISP_CTRL",  "video", 3, DP_IMPL_MODELLED },
    { 0x01, 0x01, "BACK_COLOR", "video", 3, DP_IMPL_MODELLED },
    { 0x02, 0x02, "LINE",       "video", 1, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0x03, 0x03, "LINE_CMP",   "video", 3, DP_IMPL_IRQ | DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0x04, 0x04, "SPR_BASE",   "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    { 0x05, 0x05, "SPR_FIRST",  "video", 3, DP_IMPL_MODELLED },
    { 0x06, 0x06, "SPR_COUNT",  "video", 3, DP_IMPL_MODELLED },
    { 0x07, 0x07, "SCR_MAP",    "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    { 0x08, 0x0e, "SCR_WINDOW", "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    { 0x10, 0x10, "IRQ_BASE",   "interrupt", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0x15, 0x15, "KEY_PAD",    "input", 1, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0x18, 0x18, "LINE_OVERRIDE", "video", 2, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0x1a, 0x1b, "ICON",       "sys", 2, DP_IMPL_MODELLED },
    { 0x20, 0x25, "PROM_BANKS", "mapper", 2, DP_IMPL_BANK_LO | DP_IMPL_MODELLED },
    { 0x40, 0x48, "DMA",        "sys", 2, DP_IMPL_DMA_SRC | DP_IMPL_HDMA | DP_IMPL_MODELLED },
    { 0x4a, 0x52, "AUD_DMA",    "sound", 2, DP_IMPL_DMA_SRC | DP_IMPL_MODELLED },
    { 0x60, 0x60, "SYS_MODE",   "sys", 3, DP_IMPL_MODE | DP_IMPL_MODELLED },
    { 0x64, 0x6b, "AUD_DMA2",   "sound", 2, DP_IMPL_DMA_SRC | DP_IMPL_MODELLED },
    { 0x80, 0x9e, "PSG",        "sound", 3, DP_IMPL_MODELLED },
    { 0xa0, 0xa0, "SYS_CTRL",   "sys", 3, DP_IMPL_MODELLED },
    { 0xa2, 0xa3, "TIMER_CTRL", "timer", 3, DP_IMPL_MODELLED },
    { 0xa4, 0xa7, "TIMER_PERIOD", "timer", 2, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0xa8, 0xab, "TIMER_COUNT", "timer", 1, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0xb0, 0xb0, "IRQ_BASE_REG", "interrupt", 2, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0xb1, 0xb3, "SERIAL",     "serial", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0xb2, 0xb2, "IRQ_ENABLE", "interrupt", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0xb4, 0xb4, "IRQ_PENDING", "interrupt", 1, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0xb5, 0xb6, "KEYS",       "input", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0xb7, 0xb7, "NMI",        "sys", 2, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0xba, 0xbe, "EEPROM_INT", "sys", 3, DP_IMPL_MODELLED },
    { 0xc0, 0xc3, "BANKS",      "mapper", 3, DP_IMPL_BANK_LO | DP_IMPL_BANK_HI | DP_IMPL_MODELLED },
    { 0xc4, 0xc8, "EEPROM_CART", "sys", 3, DP_IMPL_MODELLED },
    { 0xca, 0xcb, "RTC",        "sys", 3, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0xcc, 0xd5, "MAPPER_2003", "mapper", 3, DP_IMPL_BANK_HI | DP_IMPL_BANK_LO | DP_IMPL_MODELLED },
};
#define WS_IO_N (int)(sizeof ws_io / sizeof ws_io[0])

// the interrupt sources: `bit` is the pending/enable bit, `vector` the
// vector-table slot address (segment 0, the runtime reads the handler from
// (vector*4) as offset and (vector*4+2) as segment).  The base register
// (0xb0) shifts every vector; the registry shows the power-on values.
static const DpIrq ws_irqs[] = {
    { 0, 0x00, "SERIAL_SEND" },
    { 1, 0x04, "SERIAL_RECV" },
    { 2, 0x08, "CART_LINE" },
    { 3, 0x0c, "CART" },
    { 4, 0x10, "LINE_HIT" },
    { 5, 0x14, "TIMER_2 (vertical)" },
    { 6, 0x18, "VBLANK (vertical blank)" },
    { 7, 0x1c, "TIMER_1 (horizontal)" },
};

// the memory map, documented (the linear 20-bit space does not fit DpRegion's
// 16-bit lo/hi either — same convention as the Mega Drive):
//   $00000-$0FFFF  RAM (64 KiB colour / 16 KiB mirrored mono)
//   $10000-$1FFFF  SRAM (when banked in)
//   $20000+        ROM, one 64 KiB segment per bank register pair
// VRAM/OAM/sprite/palette live inside the RAM page (data_dest types them).

const DpPlatform *dp_ws_platform(void) {
    static DpPlatform p;
    if (p.name) return &p;
    p.name = "ws";
    p.io_regs = ws_io;
    p.io_reg_count = WS_IO_N;
    p.irqs = ws_irqs;
    p.irq_count = (int)(sizeof ws_irqs / sizeof ws_irqs[0]);
    p.regions = NULL;
    p.region_count = 0;
    p.entries = NULL;
    p.entry_count = 0;
    p.port_io = 1;
    p.find = NULL;
    return &p;
}
