// registry.c: the NES hardware registry — CPU-space registers, memory
// regions, interrupt entries.  Like every platform registry this is the
// classification data the report and the section liveness use; the machine
// semantics live in nes.c.
#include "../platform.h"

static const DpIoReg nes_io_regs[] = {
    // PPU (mirrored every 8 bytes through $3FFF)
    { 0x2000, 0x2000, "PUCTRL", "video", 2, DP_IMPL_IRQ | DP_IMPL_EVENT },
    { 0x2001, 0x2001, "PUMASK", "video", 2, 0 },
    { 0x2002, 0x2002, "PUSTAT", "video", 1, DP_IMPL_IRQ | DP_IMPL_EVENT },
    { 0x2003, 0x2003, "OAMADDR", "video", 2, 0 },
    { 0x2004, 0x2004, "OAMDATA", "video", 3, 0 },
    { 0x2005, 0x2005, "PUSCROLL", "video", 2, 0 },
    { 0x2006, 0x2006, "PUADDR", "video", 2, 0 },
    { 0x2007, 0x2007, "PPUDATA", "video", 3, 0 },
    // APU and IO
    { 0x4000, 0x4013, "APU", "sound", 2, 0 },
    { 0x4014, 0x4014, "OAMDMA", "video", 2, DP_IMPL_DMA_SRC | DP_IMPL_MODELLED },
    { 0x4015, 0x4015, "APUSTATUS", "sound", 3, DP_IMPL_IRQ | DP_IMPL_EVENT },
    { 0x4016, 0x4016, "CTRL1", "input", 3, DP_IMPL_EVENT },
    { 0x4017, 0x4017, "CTRL2/FRAME", "input", 3, DP_IMPL_IRQ | DP_IMPL_EVENT },
    // cartridge
    { 0x4020, 0x5FFF, "OPENBUS", "sys", 3, 0 },
    { 0x6000, 0x7FFF, "PRGRAM", "mapper", 3, DP_IMPL_MODELLED },
    { 0x8000, 0xFFFF, "MAPPER", "mapper", 2, DP_IMPL_BANK_LO | DP_IMPL_MODELLED },
};

static const DpIrq nes_irqs[] = {
    { 0, 0xFFFA, "NMI" },   // PPU vblank, edge, masked by PUCTRL bit 7
    { 1, 0xFFFE, "IRQ" },   // APU frame or MMC3 timer, level, masked by I
};

static const DpRegion nes_regions[] = {
    { 0x0000, 0x07FF, "internal RAM", DP_SPACE_WRAM },
    { 0x0800, 0x1FFF, "RAM mirror", DP_SPACE_WRAM },
    { 0x2000, 0x3FFF, "PPU registers", DP_SPACE_IO },
    { 0x4000, 0x401F, "APU and IO", DP_SPACE_IO },
    { 0x4020, 0x5FFF, "open bus", DP_SPACE_UNUSABLE },
    { 0x6000, 0x7FFF, "cartridge RAM", DP_SPACE_SRAM },
    { 0x8000, 0xBFFF, "PRG switchable", DP_SPACE_ROM },
    { 0xC000, 0xFFFF, "PRG fixed", DP_SPACE_ROM },
};

static const DpEntry nes_entries[] = {
    { 0xFFFC, DP_SEED_RESET, "reset" }, // the vector cell; the explorer reads it
    { 0xFFFA, DP_SEED_IRQ, "nmi" },
    { 0xFFFE, DP_SEED_IRQ, "irq" },
};

static const DpPlatform nes_platform = {
    "Nintendo Entertainment System (2A03/6502 + PPU)",
    nes_io_regs, (int)(sizeof nes_io_regs / sizeof nes_io_regs[0]),
    nes_irqs, (int)(sizeof nes_irqs / sizeof nes_irqs[0]),
    nes_regions, (int)(sizeof nes_regions / sizeof nes_regions[0]),
    nes_entries, (int)(sizeof nes_entries / sizeof nes_entries[0]),
};

const DpPlatform *dp_nes_platform(void) { return &nes_platform; }

void dp_nes_regs(FILE *out) {
    fputs("NES (2A03: 6502 without decimal mode, 1.79 MHz NTSC) hardware registry\n"
        "CPU map:\n"
        "  $0000-$07FF rw internal RAM, mirrored every $800 up to $1FFF\n"
        "  $2000-$3FFF rw PPU registers, mirrored every 8\n"
        "  $4000-$401F w  APU and IO; $4015 r status; $4016/$4017 r controllers\n"
        "  $4020-$5FFF --  open bus\n"
        "  $6000-$7FFF rw  cartridge PRG RAM (8 KiB in this profile)\n"
        "  $8000-$FFFF r   PRG ROM through the cartridge mapper\n"
        "PPU registers:\n"
        "  $2000 w PUCTRL  bit 7 enables the vblank NMI (edge, latched by the CPU)\n"
        "  $2002 r PUSTAT  bit 7 vblank; the read clears it and the scroll latch\n"
        "  $2003-$2007     OAM address/data, scroll, VRAM address/data (buffered read)\n"
        "  $4014 w OAMDMA  256-byte page copy into OAM, 513/514 CPU cycles: the\n"
        "                  only hardware copy channel; a page at or above $80 reads ROM\n"
        "APU:\n"
        "  $4000-$4013 pulse/triangle/noise/DMC registers\n"
        "  $4015 r APUSTATUS channel status; bit 6 DMC; read clears the frame IRQ\n"
        "  $4017 w frame counter; bit 7 inhibits the frame IRQ, bit 6 masks it to IRQ line\n"
        "Mapper boards modelled:\n"
        "  0 NROM: fixed 16/32 KiB; 1 MMC1: 5-bit serial at $8000-$FFFF, PRG mode in reg 0;\n"
        "  2 UxROM: 16 KiB switchable at $8000, last bank fixed at $C000;\n"
        "  3 CNROM: CHR bank at $8000-$FFFF, PRG never switches;\n"
        "  4 MMC3: $8000/$C000 pairs swap by reg 6/7 and PRG mode bit 6, IRQ timer\n"
        "    $C000/$C001/$E000/$E001 (counter reloaded by PPU A12 in hardware; this\n"
        "    profile drives the counter from scanline events)\n"
        "Interrupts:\n"
        "  NMI  $FFFA vblank, edge, enabled by PUCTRL bit 7, unaffected by I\n"
        "  IRQ  $FFFE level, MMC3 timer or APU frame, masked by the I flag\n"
        "Reset $FFFC.  Physical PRG keys: bank * $4000 + (addr & $3FFF) over 16 KiB\n"
        "banks; $8000-$BFFF and $C000-$FFFF take the bank of their current window.\n"
        "Limits: no audio synthesis, no pixel PPU, no A12 filtering on the MMC3\n"
        "counter, DMC DMA cycle stealing and open-bus decay not modelled.\n", out);
}
