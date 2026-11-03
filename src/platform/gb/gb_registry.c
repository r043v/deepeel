// gb_registry.c: the Game Boy register registry, IRQ table, memory regions
// and reset entries.  The machine semantics themselves live in the m0boy
// runtime (src/mach/m0boy.c); this is the classification data the report,
// the section liveness and the seeding walk use.
#include "../../core/machine.h"
#include "../../platform/platform.h"

static const DpIoReg gb_io_regs[] = {
    // cartridge mapper windows (MBC registers)
    { 0x0000, 0x1FFF, "RAMG", "mapper", 2, DP_IMPL_RAMG | DP_IMPL_MODELLED },
    { 0x2000, 0x2FFF, "ROMB0", "mapper", 2, DP_IMPL_BANK_LO | DP_IMPL_MODELLED },
    { 0x3000, 0x3FFF, "ROMB1", "mapper", 2, DP_IMPL_BANK_HI | DP_IMPL_MODELLED },
    { 0x4000, 0x5FFF, "RAMB", "mapper", 2, DP_IMPL_RAM_BANK | DP_IMPL_BANK_HI | DP_IMPL_MODELLED },
    { 0x6000, 0x7FFF, "MODE", "mapper", 2, DP_IMPL_MODE | DP_IMPL_MODELLED },
    // system
    { 0xFF00, 0xFF00, "P1", "input", 3, DP_IMPL_EVENT },
    { 0xFF01, 0xFF01, "SB", "serial", 3, 0 },
    { 0xFF02, 0xFF02, "SC", "serial", 3, DP_IMPL_EVENT },
    { 0xFF04, 0xFF04, "DIV", "timer", 3, DP_IMPL_EVENT },
    { 0xFF05, 0xFF05, "TIMA", "timer", 3, DP_IMPL_EVENT },
    { 0xFF06, 0xFF06, "TMA", "timer", 2, 0 },
    { 0xFF07, 0xFF07, "TAC", "timer", 3, DP_IMPL_EVENT },
    { 0xFF0F, 0xFF0F, "IF", "interrupt", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
    // sound
    { 0xFF10, 0xFF10, "NR10", "sound", 2, 0 },
    { 0xFF11, 0xFF11, "NR11", "sound", 2, 0 },
    { 0xFF12, 0xFF12, "NR12", "sound", 2, 0 },
    { 0xFF13, 0xFF13, "NR13", "sound", 2, 0 },
    { 0xFF14, 0xFF14, "NR14", "sound", 2, 0 },
    { 0xFF16, 0xFF16, "NR21", "sound", 2, 0 },
    { 0xFF17, 0xFF17, "NR22", "sound", 2, 0 },
    { 0xFF18, 0xFF18, "NR23", "sound", 2, 0 },
    { 0xFF19, 0xFF19, "NR24", "sound", 2, 0 },
    { 0xFF1A, 0xFF1A, "NR30", "sound", 2, 0 },
    { 0xFF1B, 0xFF1B, "NR31", "sound", 2, 0 },
    { 0xFF1C, 0xFF1C, "NR32", "sound", 2, 0 },
    { 0xFF1D, 0xFF1D, "NR33", "sound", 2, 0 },
    { 0xFF1E, 0xFF1E, "NR34", "sound", 2, 0 },
    { 0xFF20, 0xFF20, "NR41", "sound", 2, 0 },
    { 0xFF21, 0xFF21, "NR42", "sound", 2, 0 },
    { 0xFF22, 0xFF22, "NR43", "sound", 2, 0 },
    { 0xFF23, 0xFF23, "NR44", "sound", 2, 0 },
    { 0xFF24, 0xFF24, "NR50", "sound", 2, 0 },
    { 0xFF25, 0xFF25, "NR51", "sound", 2, 0 },
    { 0xFF26, 0xFF26, "NR52", "sound", 3, 0 },
    { 0xFF30, 0xFF3F, "WAVE", "sound", 3, 0 },
    // video
    { 0xFF40, 0xFF40, "LCDC", "video", 3, 0 },
    { 0xFF41, 0xFF41, "STAT", "video", 3, DP_IMPL_EVENT },
    { 0xFF42, 0xFF42, "SCY", "video", 2, 0 },
    { 0xFF43, 0xFF43, "SCX", "video", 2, 0 },
    { 0xFF44, 0xFF44, "LY", "video", 1, DP_IMPL_EVENT },
    { 0xFF45, 0xFF45, "LYC", "video", 2, 0 },
    { 0xFF46, 0xFF46, "DMA", "video", 2, DP_IMPL_DMA_SRC | DP_IMPL_MODELLED },
    { 0xFF47, 0xFF47, "BGP", "video", 2, 0 },
    { 0xFF48, 0xFF48, "OBP0", "video", 2, 0 },
    { 0xFF49, 0xFF49, "OBP1", "video", 2, 0 },
    { 0xFF4A, 0xFF4A, "WY", "video", 2, 0 },
    { 0xFF4B, 0xFF4B, "WX", "video", 2, 0 },
    // CGB
    { 0xFF4D, 0xFF4D, "KEY1", "cgb", 3, DP_IMPL_WINDOW },
    // CGB registers: the m0boy GB runtime is DMG (one VRAM bank, no HDMA),
    // so VBK/SVBK/HDMA are listed for the record, not modelled
    { 0xFF4F, 0xFF4F, "VBK", "cgb", 2, DP_IMPL_WINDOW },
    { 0xFF50, 0xFF50, "BOOTROM", "sys", 2, 0 },
    { 0xFF51, 0xFF51, "HDMA1", "cgb", 2, DP_IMPL_HDMA },
    { 0xFF52, 0xFF52, "HDMA2", "cgb", 2, DP_IMPL_HDMA },
    { 0xFF53, 0xFF53, "HDMA3", "cgb", 2, DP_IMPL_HDMA },
    { 0xFF54, 0xFF54, "HDMA4", "cgb", 2, DP_IMPL_HDMA },
    { 0xFF55, 0xFF55, "HDMA5", "cgb", 3, DP_IMPL_HDMA },
    { 0xFF56, 0xFF56, "RP", "cgb", 3, 0 },
    { 0xFF68, 0xFF68, "BCPS", "cgb", 2, 0 },
    { 0xFF69, 0xFF69, "BCPD", "cgb", 3, 0 },
    { 0xFF6A, 0xFF6A, "OCPS", "cgb", 2, 0 },
    { 0xFF6B, 0xFF6B, "OCPD", "cgb", 3, 0 },
    { 0xFF6C, 0xFF6C, "OPRI", "cgb", 2, 0 },
    { 0xFF70, 0xFF70, "SVBK", "cgb", 2, DP_IMPL_WINDOW },
    { 0xFF72, 0xFF72, "FF72", "cgb", 3, 0 },
    { 0xFF73, 0xFF73, "FF73", "cgb", 3, 0 },
    { 0xFF74, 0xFF74, "FF74", "cgb", 3, 0 },
    { 0xFF75, 0xFF75, "FF75", "cgb", 3, 0 },
    { 0xFF76, 0xFF76, "PCM12", "cgb", 1, DP_IMPL_EVENT },
    { 0xFF77, 0xFF77, "PCM34", "cgb", 1, DP_IMPL_EVENT },
    { 0xFFFF, 0xFFFF, "IE", "interrupt", 3, DP_IMPL_IRQ | DP_IMPL_MODELLED },
};

static const DpIrq gb_irqs[] = {
    { 0, 0x40, "VBlank" },
    { 1, 0x48, "LCDSTAT" },
    { 2, 0x50, "Timer" },
    { 3, 0x58, "Serial" },
    { 4, 0x60, "Joypad" },
};

static const DpRegion gb_regions[] = {
    { 0x0000, 0x3FFF, "ROM fixed (bank 0)", DP_SPACE_ROM },
    { 0x4000, 0x7FFF, "ROM banked", DP_SPACE_ROM },
    { 0x8000, 0x9FFF, "VRAM", DP_SPACE_VRAM },
    { 0xA000, 0xBFFF, "cartridge RAM", DP_SPACE_SRAM },
    { 0xC000, 0xDFFF, "WRAM", DP_SPACE_WRAM },
    { 0xE000, 0xFDFF, "echo RAM", DP_SPACE_ECHO },
    { 0xFE00, 0xFE9F, "OAM", DP_SPACE_OAM },
    { 0xFEA0, 0xFEFF, "unusable", DP_SPACE_UNUSABLE },
    { 0xFF00, 0xFF7F, "IO", DP_SPACE_IO },
    { 0xFF80, 0xFFFE, "HRAM", DP_SPACE_HRAM },
    { 0xFFFF, 0xFFFF, "IE", DP_SPACE_IE },
};

static const DpEntry gb_entries[] = {
    { 0x0100, DP_SEED_RESET, "reset" },
    { 0x0000, DP_SEED_RST, "rst00" },
    { 0x0008, DP_SEED_RST, "rst08" },
    { 0x0010, DP_SEED_RST, "rst10" },
    { 0x0018, DP_SEED_RST, "rst18" },
    { 0x0020, DP_SEED_RST, "rst20" },
    { 0x0028, DP_SEED_RST, "rst28" },
    { 0x0030, DP_SEED_RST, "rst30" },
    { 0x0038, DP_SEED_RST, "rst38" },
    { 0x0040, DP_SEED_IRQ, "irq_vblank" },
    { 0x0048, DP_SEED_IRQ, "irq_stat" },
    { 0x0050, DP_SEED_IRQ, "irq_timer" },
    { 0x0058, DP_SEED_IRQ, "irq_serial" },
    { 0x0060, DP_SEED_IRQ, "irq_joypad" },
};

static const DpPlatform gb_platform = {
    "Game Boy",
    gb_io_regs, (int)(sizeof gb_io_regs / sizeof gb_io_regs[0]),
    gb_irqs, (int)(sizeof gb_irqs / sizeof gb_irqs[0]),
    gb_regions, (int)(sizeof gb_regions / sizeof gb_regions[0]),
    gb_entries, (int)(sizeof gb_entries / sizeof gb_entries[0]),
    0x0100, 0xFFFE, 0x01, 0xB0, 0x00, 0x13, 0x00, 0xD8, 0x01, 0x4D,
};

const DpPlatform *dp_platform_gb(void) { return &gb_platform; }
