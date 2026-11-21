// registry.c: the Neo Geo Pocket / Neo Geo Pocket Color hardware registry
// (spec §3): every register surface the machine models, the interrupt sources
// and the memory regions.  The 24-bit bus needs find(); there is no 64 KiB
// slot table.  First match wins: the narrower registers come first.
#include <string.h>
#include "../platform.h"

// the TLCS-900H internal I/O page (0x000000-0x0000FF) and the K2GE/K1GE
// (0x008000-0x00BFFF), the sound Z80's control, the cartridge windows
static const DpIoReg ngp_io[] = {
    // the CPU's internal I/O (the TMP95C061 family page)
    { 0x000020, 0x000020, "TRUN", "timer", 3, DP_IMPL_MODELLED },
    { 0x000022, 0x000023, "TREG01", "timer", 3, DP_IMPL_MODELLED | DP_IMPL_EVENT },
    { 0x000024, 0x000024, "T01MOD", "timer", 3, DP_IMPL_MODELLED },
    { 0x000025, 0x000025, "TFFCR", "timer", 3, DP_IMPL_MODELLED },
    { 0x000026, 0x000027, "TREG23", "timer", 3, DP_IMPL_MODELLED | DP_IMPL_EVENT },
    { 0x000028, 0x000028, "T23MOD", "timer", 3, DP_IMPL_MODELLED },
    { 0x000029, 0x000029, "TRDC", "timer", 3, DP_IMPL_MODELLED },
    { 0x000050, 0x000050, "SERBUF", "serial", 3, DP_IMPL_MODELLED },
    { 0x00006F, 0x00006F, "WDT", "sys", 2, DP_IMPL_MODELLED },
    { 0x000070, 0x00007A, "INTCTRL", "interrupt", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0x00007C, 0x00007F, "DMAVEC", "interrupt", 2, DP_IMPL_DMA_SRC | DP_IMPL_MODELLED },
    { 0x000091, 0x000097, "RTC", "sys", 1, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0x0000A0, 0x0000A1, "PSG", "sound", 3, DP_IMPL_MODELLED },
    { 0x0000A2, 0x0000A3, "DAC", "sound", 2, DP_IMPL_MODELLED },
    { 0x0000B2, 0x0000B2, "COMSTAT", "serial", 3, DP_IMPL_MODELLED },
    { 0x0000B8, 0x0000B8, "CHIP_EN", "sound", 2, DP_IMPL_MODELLED },
    { 0x0000B9, 0x0000B9, "Z80_EN", "sound", 2, DP_IMPL_MODELLED },
    { 0x0000BA, 0x0000BA, "Z80_NMI", "sound", 2, DP_IMPL_MODELLED },
    { 0x0000BC, 0x0000BC, "COMM", "serial", 3, DP_IMPL_MODELLED },
    // the K2GE (colour) / K1GE-compat (mono) registers
    { 0x008000, 0x008000, "INTCNT", "video", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    { 0x008002, 0x008005, "WBA/WSI", "video", 3, DP_IMPL_MODELLED },
    { 0x008006, 0x008006, "SCRPER", "video", 3, DP_IMPL_MODELLED },
    { 0x008008, 0x008009, "RASCNT", "video", 1, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0x008010, 0x008010, "STAT", "video", 1, DP_IMPL_EVENT | DP_IMPL_MODELLED },
    { 0x008012, 0x008012, "OUTCOL", "video", 3, DP_IMPL_MODELLED },
    { 0x008020, 0x008021, "SPOFS", "video", 3, DP_IMPL_MODELLED },
    { 0x008030, 0x008030, "PLNSWP", "video", 3, DP_IMPL_MODELLED },
    { 0x008032, 0x008035, "SCROFS", "video", 3, DP_IMPL_MODELLED },
    { 0x008101, 0x008107, "SPPALT", "video", 2, DP_IMPL_MODELLED },
    { 0x008109, 0x00810F, "SCP1PLT", "video", 2, DP_IMPL_MODELLED },
    { 0x008111, 0x008117, "SCP2PLT", "video", 2, DP_IMPL_MODELLED },
    { 0x008118, 0x008118, "BGCOLOR", "video", 3, DP_IMPL_MODELLED },
    { 0x0087E0, 0x0087E0, "GE_RESET", "video", 2, DP_IMPL_MODELLED },
    { 0x0087E2, 0x0087E2, "GEMODE", "video", 3, DP_IMPL_MODE | DP_IMPL_MODELLED },
    { 0x008200, 0x0083FF, "PALETTE", "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    { 0x008800, 0x0088FF, "SPRITES", "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    { 0x008C00, 0x008C3F, "SPRPAL", "video", 2, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    { 0x009000, 0x009FFF, "SCROLL", "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    { 0x00A000, 0x00BFFF, "CHARACTER", "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    // the cartridge flash command/status window (the save chip)
    { 0x00220000, 0x00230000, "FLASHSTAT", "mapper", 1, DP_IMPL_MODELLED },
    { 0x00200000, 0x003FFFFF, "CART1", "memory", 3, 0 },
    { 0x00800000, 0x009FFFFF, "CART2", "memory", 3, 0 },
};
#define NGP_IO_N (int)(sizeof ngp_io / sizeof ngp_io[0])

// the interrupt sources the machine raises (the RAM vector table's slots);
// `vector` is the RAM table slot address (0x6FB8 + 4*slot)
static const DpIrq ngp_irqs[] = {
    { 5, 0x6FCC, "VBLANK" },
    { 6, 0x6FD0, "Z80" },
    { 7, 0x6FD4, "TIMER0" },
    { 8, 0x6FD8, "TIMER1" },
    { 9, 0x6FDC, "TIMER2" },
    { 10, 0x6FE0, "TIMER3" },
    { 12, 0x6FE4, "RX0" },
    { 11, 0x6FE8, "TX0" },
};

// the reset entry: the BIOS reads the cartridge header's 24-bit entry at
// 0x20001C and the machine boots the cartridge directly, so the reset flow is
// the natural path (no 16-bit entry); the IRQ handlers live in the RAM vector
// table the BIOS fills (0x6FB8, non-ROM at seed time) — the machine's
// vector_seeds reads them the wide way, the MD convention.
// No DpRegion table: like the Mega Drive, the memory map is documented in
// docs/COUVERTURE.fr.md §3 — the 16-bit DpRegion struct cannot hold a
// 24-bit bus, and the kernel's register accounting uses find().

static int ngp_find(uint32_t a) {
    a &= 0xFFFFFFu;
    for (int i = 0; i < NGP_IO_N; i++)
        if (a >= ngp_io[i].lo && a <= ngp_io[i].hi) return i;
    return -1;
}

const DpPlatform *dp_ngp_platform(void) {
    static DpPlatform p;
    if (p.name) return &p;
    p.name = "ngp";
    p.io_regs = ngp_io;
    p.io_reg_count = NGP_IO_N;
    p.irqs = ngp_irqs;
    p.irq_count = (int)(sizeof ngp_irqs / sizeof ngp_irqs[0]);
    p.regions = NULL;   // the 24-bit bus does not fit DpRegion (see above)
    p.region_count = 0;
    p.entries = NULL;
    p.entry_count = 0;
    p.find = ngp_find;
    return &p;
}
