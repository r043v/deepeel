// gb_plat.c: the Game Boy platform — complete register registry, MBC mapper
// hooks, OAM DMA and HDMA copy channels, reset entries.
//
// Register semantics that cannot be modelled without real hardware timing
// (timer/PPU/serial progress) are held static; their IRQ entries are seeded
// instead, so handler code is still explored.  See docs/GB.fr.md.
#include "../../core/machine.h"
#include "gb.h"

// ---------------------------------------------------------------------------
// register registry: every register/range a GB ROM can meaningfully touch
// ---------------------------------------------------------------------------
static const MsIoReg gb_io_regs[] = {
    // cartridge mapper windows (MBC registers)
    { 0x0000, 0x1FFF, "RAMG", "mapper", 2, MS_IMPL_RAMG | MS_IMPL_MODELLED },
    { 0x2000, 0x2FFF, "ROMB0", "mapper", 2, MS_IMPL_BANK_LO | MS_IMPL_MODELLED },
    { 0x3000, 0x3FFF, "ROMB1", "mapper", 2, MS_IMPL_BANK_HI | MS_IMPL_MODELLED },
    { 0x4000, 0x5FFF, "RAMB", "mapper", 2, MS_IMPL_RAM_BANK | MS_IMPL_BANK_HI | MS_IMPL_MODELLED },
    { 0x6000, 0x7FFF, "MODE", "mapper", 2, MS_IMPL_MODE | MS_IMPL_MODELLED },
    // system
    { 0xFF00, 0xFF00, "P1", "input", 3, MS_IMPL_EVENT },
    { 0xFF01, 0xFF01, "SB", "serial", 3, 0 },
    { 0xFF02, 0xFF02, "SC", "serial", 3, MS_IMPL_EVENT },
    { 0xFF04, 0xFF04, "DIV", "timer", 3, MS_IMPL_EVENT },
    { 0xFF05, 0xFF05, "TIMA", "timer", 3, MS_IMPL_EVENT },
    { 0xFF06, 0xFF06, "TMA", "timer", 2, 0 },
    { 0xFF07, 0xFF07, "TAC", "timer", 3, MS_IMPL_EVENT },
    { 0xFF0F, 0xFF0F, "IF", "interrupt", 3, MS_IMPL_IRQ | MS_IMPL_MODELLED },
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
    { 0xFF41, 0xFF41, "STAT", "video", 3, MS_IMPL_EVENT },
    { 0xFF42, 0xFF42, "SCY", "video", 2, 0 },
    { 0xFF43, 0xFF43, "SCX", "video", 2, 0 },
    { 0xFF44, 0xFF44, "LY", "video", 1, MS_IMPL_EVENT },
    { 0xFF45, 0xFF45, "LYC", "video", 2, 0 },
    { 0xFF46, 0xFF46, "DMA", "video", 2, MS_IMPL_DMA_SRC | MS_IMPL_MODELLED },
    { 0xFF47, 0xFF47, "BGP", "video", 2, 0 },
    { 0xFF48, 0xFF48, "OBP0", "video", 2, 0 },
    { 0xFF49, 0xFF49, "OBP1", "video", 2, 0 },
    { 0xFF4A, 0xFF4A, "WY", "video", 2, 0 },
    { 0xFF4B, 0xFF4B, "WX", "video", 2, 0 },
    // CGB
    { 0xFF4D, 0xFF4D, "KEY1", "cgb", 3, MS_IMPL_WINDOW },
    { 0xFF4F, 0xFF4F, "VBK", "cgb", 2, MS_IMPL_WINDOW | MS_IMPL_MODELLED },
    { 0xFF50, 0xFF50, "BOOTROM", "sys", 2, 0 },
    { 0xFF51, 0xFF51, "HDMA1", "cgb", 2, MS_IMPL_HDMA | MS_IMPL_MODELLED },
    { 0xFF52, 0xFF52, "HDMA2", "cgb", 2, MS_IMPL_HDMA | MS_IMPL_MODELLED },
    { 0xFF53, 0xFF53, "HDMA3", "cgb", 2, MS_IMPL_HDMA | MS_IMPL_MODELLED },
    { 0xFF54, 0xFF54, "HDMA4", "cgb", 2, MS_IMPL_HDMA | MS_IMPL_MODELLED },
    { 0xFF55, 0xFF55, "HDMA5", "cgb", 3, MS_IMPL_HDMA | MS_IMPL_MODELLED },
    { 0xFF56, 0xFF56, "RP", "cgb", 3, 0 },
    { 0xFF68, 0xFF68, "BCPS", "cgb", 2, 0 },
    { 0xFF69, 0xFF69, "BCPD", "cgb", 3, 0 },
    { 0xFF6A, 0xFF6A, "OCPS", "cgb", 2, 0 },
    { 0xFF6B, 0xFF6B, "OCPD", "cgb", 3, 0 },
    { 0xFF6C, 0xFF6C, "OPRI", "cgb", 2, 0 },
    { 0xFF70, 0xFF70, "SVBK", "cgb", 2, MS_IMPL_WINDOW },
    { 0xFF72, 0xFF72, "FF72", "cgb", 3, 0 },
    { 0xFF73, 0xFF73, "FF73", "cgb", 3, 0 },
    { 0xFF74, 0xFF74, "FF74", "cgb", 3, 0 },
    { 0xFF75, 0xFF75, "FF75", "cgb", 3, 0 },
    { 0xFF76, 0xFF76, "PCM12", "cgb", 1, MS_IMPL_EVENT },
    { 0xFF77, 0xFF77, "PCM34", "cgb", 1, MS_IMPL_EVENT },
    { 0xFFFF, 0xFFFF, "IE", "interrupt", 3, MS_IMPL_IRQ | MS_IMPL_MODELLED },
};

static const MsIrq gb_irqs[] = {
    { 0, 0x40, "VBlank" },
    { 1, 0x48, "LCDSTAT" },
    { 2, 0x50, "Timer" },
    { 3, 0x58, "Serial" },
    { 4, 0x60, "Joypad" },
};

static const MsRegion gb_regions[] = {
    { 0x0000, 0x3FFF, "ROM fixed (bank 0)", MS_SPACE_ROM },
    { 0x4000, 0x7FFF, "ROM banked", MS_SPACE_ROM },
    { 0x8000, 0x9FFF, "VRAM", MS_SPACE_VRAM },
    { 0xA000, 0xBFFF, "cartridge RAM", MS_SPACE_SRAM },
    { 0xC000, 0xDFFF, "WRAM", MS_SPACE_WRAM },
    { 0xE000, 0xFDFF, "echo RAM", MS_SPACE_ECHO },
    { 0xFE00, 0xFE9F, "OAM", MS_SPACE_OAM },
    { 0xFEA0, 0xFEFF, "unusable", MS_SPACE_UNUSABLE },
    { 0xFF00, 0xFF7F, "IO", MS_SPACE_IO },
    { 0xFF80, 0xFFFE, "HRAM", MS_SPACE_HRAM },
    { 0xFFFF, 0xFFFF, "IE", MS_SPACE_IE },
};

static const MsEntry gb_entries[] = {
    { 0x0100, MS_SEED_RESET, "reset" },
    { 0x0000, MS_SEED_RST, "rst00" },
    { 0x0008, MS_SEED_RST, "rst08" },
    { 0x0010, MS_SEED_RST, "rst10" },
    { 0x0018, MS_SEED_RST, "rst18" },
    { 0x0020, MS_SEED_RST, "rst20" },
    { 0x0028, MS_SEED_RST, "rst28" },
    { 0x0030, MS_SEED_RST, "rst30" },
    { 0x0038, MS_SEED_RST, "rst38" },
    { 0x0040, MS_SEED_IRQ, "irq_vblank" },
    { 0x0048, MS_SEED_IRQ, "irq_stat" },
    { 0x0050, MS_SEED_IRQ, "irq_timer" },
    { 0x0058, MS_SEED_IRQ, "irq_serial" },
    { 0x0060, MS_SEED_IRQ, "irq_joypad" },
};

// ---------------------------------------------------------------------------
// mapper hooks
// ---------------------------------------------------------------------------
void ms_mapper_update_windows(MsMachine *m); // machine.c

static void gb_rom_write(MsMachine *m, uint16_t addr, uint8_t v) {
    const MsCart *c = m->cart;
    switch (c->mbc) {
    case MS_MBC_NONE:
        break; // no registers at all
    case MS_MBC1:
        if (addr < 0x2000)
            m->ramg = ((v & 0x0F) == 0x0A);
        else if (addr < 0x4000)
            m->rom_lo = v & 0x1F;
        else if (addr < 0x6000)
            m->rom_hi = v & 3, m->ram_bank = v & 3; // one 2-bit reg, meaning per mode
        else
            m->mbc_mode = v & 1;
        break;
    case MS_MBC2:
        if (addr >= 0x4000) break;
        if (!(addr & 0x0100))
            m->ramg = ((v & 0x0F) == 0x0A);
        else
            m->rom_lo = v & 0x0F;
        break;
    case MS_MBC3:
        if (addr < 0x2000)
            m->ramg = ((v & 0x0F) == 0x0A);
        else if (addr < 0x4000)
            m->rom_lo = v & 0x7F;
        else if (addr < 0x6000) {
            if ((v & 0xF8) == 0x08) break; // RTC register select: logged only
            m->ram_bank = v & 3;
        }
        break;
    case MS_MBC5:
        if (addr < 0x2000)
            m->ramg = ((v & 0x0F) == 0x0A);
        else if (addr < 0x3000)
            m->rom_lo = v; // bank 0 is legal on MBC5
        else if (addr < 0x4000)
            m->rom_hi = v & 1;
        else if (addr < 0x6000)
            m->ram_bank = v & 0x0F;
        break;
    default: // unknown mapper: generic heuristic
        if (addr < 0x2000)
            m->ramg = ((v & 0x0F) == 0x0A);
        else if (addr < 0x4000) {
            m->rom_lo = v & 0x3F;
            if (m->rom_lo == 0) m->rom_lo = 1;
        } else if (addr < 0x6000)
            m->ram_bank = v & 3;
        break;
    }
    ms_mapper_update_windows(m);
}

// ---------------------------------------------------------------------------
// copy channels: OAM DMA ($FF46) and HDMA ($FF51-$FF55)
// ---------------------------------------------------------------------------

static void record_copy(MsMachine *m, const MsCopyRec *rec) {
    if (m->trace && m->trace->copy) m->trace->copy(m->trace_ctx, rec);
}

static void oam_dma(MsMachine *m, uint8_t page) {
    uint16_t src = (uint16_t)(page << 8);
    MsSpace sp;
    uint32_t bank;
    if (src < 0x8000) {
        sp = MS_SPACE_ROM;
        bank = src < 0x4000 ? m->win_rom0 : m->win_romN;
    } else if (src < 0xA000) {
        sp = MS_SPACE_VRAM;
        bank = m->vram_bank & 1;
    } else if (src < 0xE000) {
        sp = src < 0xC000 ? MS_SPACE_SRAM : MS_SPACE_WRAM;
        bank = m->win_ram;
    } else {
        sp = MS_SPACE_HRAM;
        bank = 0;
    }
    for (int i = 0; i < 0xA0; i++)
        m->oam[i] = ms_peek(m, (uint16_t)(src + i));
    // rehash oam pages
    m->ph_oam = 0;
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 0xA0; i++) { h ^= m->oam[i]; h *= 0x100000001b3ull; }
        m->ph_oam = h;
    }
    MsCopyRec rec = { 1, 1, (uint8_t)sp, MS_SPACE_OAM, bank, 0, src, (uint16_t)(src + 0x9F), 0xFE00, 0xFE9F, 0xA0, m->pc };
    record_copy(m, &rec);
}

static void hdma(MsMachine *m, uint8_t v) {
    if (!(m->io[0x55 - 0xFF00] & 0x80) && !(v & 0x80) && v == 0xFF)
        return; // terminating a general-purpose DMA: nothing to copy
    uint16_t src = (uint16_t)((m->io[0x51 - 0xFF00] << 8) | (m->io[0x52 - 0xFF00] & 0xF0));
    int src_wram = (m->io[0x52 - 0xFF00] & 0x80) != 0;
    uint16_t dst = 0x8000 | (((m->io[0x53 - 0xFF00] << 8) | (m->io[0x54 - 0xFF00] & 0xF0)) & 0x1FF0);
    int blocks = (v & 0x7F) + 1;
    int len = (v & 0x80) ? 0x10 : blocks * 0x10;
    if (len > 0x800) len = 0x800;
    MsSpace sp = src_wram ? MS_SPACE_WRAM : MS_SPACE_ROM;
    uint32_t sbank = 0;
    if (!src_wram)
        sbank = src < 0x4000 ? m->win_rom0 : m->win_romN;
    for (int i = 0; i < len; i++) {
        uint16_t s = (uint16_t)(src + i);
        uint8_t b = ms_peek(m, s);
        uint16_t d = (uint16_t)((dst + i) & 0x1FFF);
        m->vram[m->vram_bank & 1][d] = b;
    }
    // rehash touched vram pages
    for (int pg = (dst - 0x8000) >> 8; pg <= ((dst + len - 1) - 0x8000) >> 8; pg++) {
        if (pg < 0 || pg > 31) continue;
        uint64_t h = 0xcbf29ce484222325ull;
        const uint8_t *p = m->vram[m->vram_bank & 1] + (pg << 8);
        for (int i = 0; i < 256; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
        m->ph_vram[m->vram_bank & 1][pg] = h;
    }
    MsCopyRec rec = { 2, 1, (uint8_t)sp, MS_SPACE_VRAM, sbank, (uint32_t)(m->vram_bank & 1),
                      src, (uint16_t)(src + len - 1), dst, (uint16_t)(dst + len - 1),
                      (uint32_t)len, m->pc };
    record_copy(m, &rec);
}

// ---------------------------------------------------------------------------
// io hooks
// ---------------------------------------------------------------------------

static uint8_t gb_io_read(MsMachine *m, uint16_t addr) {
    uint8_t r = addr - 0xFF00;
    switch (addr) {
    case 0xFF00: {
        // P1 matrix: bits 4-5 select, active-low; 0 = key pressed
        uint8_t sel = (uint8_t)(m->io[r] & 0x30);
        uint8_t v = 0x0F;
        if (!(sel & 0x10)) v &= (uint8_t)(~m->buttons & 0x0F);        // a b select start
        if (!(sel & 0x20)) v &= (uint8_t)(~(m->buttons >> 4) & 0x0F); // right left up down
        return (uint8_t)(0xC0 | sel | v);
    }
    case 0xFF0F: return (uint8_t)(0xE0 | (m->io[r] & 0x1F));
    case 0xFF41: return (uint8_t)((m->io[r] & 0x78) | ((m->io[0x45 - 0xFF00] == m->io[0x44 - 0xFF00]) ? 0x04 : 0));
    case 0xFF55: return 0xFF; // no transfer in flight (instant model)
    default: return m->io[r];
    }
}

static void gb_io_write(MsMachine *m, uint16_t addr, uint8_t v) {
    uint8_t r = addr - 0xFF00;
    m->io[r] = v; // plain storage backing
    switch (addr) {
    case 0xFF04: m->io[r] = 0; break; // DIV write resets the counter
    case 0xFF0F: m->io[r] = v & 0x1F; break;
    case 0xFF46: oam_dma(m, v); break;
    case 0xFF4F: m->vram_bank = v & 1; break;
    case 0xFF55: hdma(m, v); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// platform definition
// ---------------------------------------------------------------------------

static const MsPlatform gb_platform = {
    .name = "gb",
    .io_regs = gb_io_regs,
    .io_reg_count = (int)(sizeof gb_io_regs / sizeof gb_io_regs[0]),
    .irqs = gb_irqs,
    .irq_count = (int)(sizeof gb_irqs / sizeof gb_irqs[0]),
    .regions = gb_regions,
    .region_count = (int)(sizeof gb_regions / sizeof gb_regions[0]),
    .entries = gb_entries,
    .entry_count = (int)(sizeof gb_entries / sizeof gb_entries[0]),
    // DMG reset state (boot ROM exit)
    .init_pc = 0x0100,
    .init_sp = 0xFFFE,
    .init_a = 0x01,
    .init_f = 0xB0,
    .init_b = 0x00,
    .init_c = 0x13,
    .init_d = 0x00,
    .init_e = 0xD8,
    .init_h = 0x01,
    .init_l = 0x4D,
    .rom_write = gb_rom_write,
    .io_read = gb_io_read,
    .io_write = gb_io_write,
    .tick = gb_tick,
};

const MsPlatform *ms_platform_gb(void) { return &gb_platform; }
