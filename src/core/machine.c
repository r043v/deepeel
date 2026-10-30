#include "machine.h"

static uint64_t page_hash(const uint8_t *p, int n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static void rehash_vram(MsMachine *m, uint16_t addr) {
    int pg = ((addr - 0x8000) >> 8) & 31;
    m->ph_vram[m->vram_bank & 1][pg] = page_hash(m->vram[m->vram_bank & 1] + (pg << 8), 256);
}
static void rehash_wram(MsMachine *m, uint16_t addr) {
    int pg = ((addr - 0xC000) >> 8) & 31;
    m->ph_wram[pg] = page_hash(m->wram + (pg << 8), 256);
}
static void rehash_hram(MsMachine *m) { m->ph_hram = page_hash(m->hram, 0x7F); }
static void rehash_oam(MsMachine *m) { m->ph_oam = page_hash(m->oam, 0xA0); }
static void rehash_cram(MsMachine *m, uint32_t off) {
    if (off < m->cart->ram_size) {
        uint32_t pg = off >> 8;
        if (pg < MS_CRAM_PAGES)
            m->ph_cram[pg] = page_hash(m->cart->ram + (pg << 8),
                                       (int)((m->cart->ram_size - (pg << 8)) < 256 ? m->cart->ram_size - (pg << 8) : 256));
    }
}

// --- mapper (called by the platform hooks in gb_plat.c) ---------------------

void ms_mapper_update_windows(MsMachine *m) {
    const MsCart *c = m->cart;
    uint32_t lo = m->rom_lo, hi = m->rom_hi;
    uint32_t romN = 1, rom0 = 0;
    switch (c->mbc) {
    case MS_MBC1:
        if (lo == 0) lo = 1;
        romN = (hi << 5) | lo;
        rom0 = (m->mbc_mode & 1) ? (hi & 3) : 0;
        break;
    case MS_MBC2:
        if (lo == 0) lo = 1; // rom_lo already masked to 4 bits by the hook
        romN = lo & 0x0F;
        break;
    case MS_MBC3:
        if (lo == 0) lo = 1;
        romN = lo & 0x7F;
        break;
    case MS_MBC5:
        romN = ((hi & 1) << 8) | lo;
        break;
    default:
        romN = 1;
        break;
    }
    if (romN >= (uint32_t)c->rom_banks) romN %= (uint32_t)c->rom_banks;
    if (rom0 >= (uint32_t)c->rom_banks) rom0 = 0;
    m->win_rom0 = rom0;
    m->win_romN = romN;
    uint32_t ram = m->ram_bank;
    switch (c->mbc) {
    case MS_MBC1: ram &= 3; break;
    case MS_MBC3: ram &= 3; break; // RTC banks 08-0C logged only
    case MS_MBC5: ram &= 0x0F; break;
    default: ram = 0; break;
    }
    m->win_ram = ram;
}

uint32_t ms_map_sram(const MsMachine *m, uint16_t addr) {
    // physical cartridge-RAM offset for an A000-BFFF access, or UINT32_MAX
    if (!m->ramg || m->cart->ram_size == 0) return UINT32_MAX;
    uint32_t off = (uint32_t)(addr - 0xA000) + m->win_ram * 0x2000;
    if (off >= m->cart->ram_size) off %= m->cart->ram_size;
    return off;
}

// --- machine lifecycle -------------------------------------------------------

void ms_machine_init(MsMachine *m, const MsCart *cart, const MsPlatform *plat) {
    memset(m, 0, sizeof *m);
    m->cart = cart;
    m->plat = plat;
    m->sp = plat->init_sp;
    m->pc = plat->init_pc;
    m->a = plat->init_a;
    m->f = plat->init_f;
    m->b = plat->init_b;
    m->c = plat->init_c;
    m->d = plat->init_d;
    m->e = plat->init_e;
    m->h = plat->init_h;
    m->l = plat->init_l;
    m->rom_lo = 1;
    ms_mapper_update_windows(m);
    // initial page hashes
    for (int b = 0; b < 2; b++)
        for (int pg = 0; pg < 32; pg++) m->ph_vram[b][pg] = 0;
    rehash_hram(m);
    rehash_oam(m);
    if (cart->ram_size)
        for (uint32_t pg = 0; pg < MS_CRAM_PAGES && (pg << 8) < cart->ram_size; pg++) rehash_cram(m, pg << 8);
}

uint64_t ms_state_hash(const MsMachine *m) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = ms_fnv1a_u32(h, m->pc);
    h = ms_fnv1a_u32(h, m->sp);
    uint8_t regs[8] = { m->a, m->f, m->b, m->c, m->d, m->e, m->h, m->l };
    h = ms_fnv1a(h, regs, 8);
    h = ms_fnv1a(h, &(uint8_t){ m->ime }, 1);
    h = ms_fnv1a(h, &(uint8_t){ m->ime_delay }, 1);
    h = ms_fnv1a(h, &(uint8_t){ m->halted }, 1);
    h = ms_fnv1a_u32(h, m->win_rom0);
    h = ms_fnv1a_u32(h, m->win_romN);
    h = ms_fnv1a_u32(h, m->win_ram);
    h = ms_fnv1a(h, &(uint8_t){ m->ramg }, 1);
    h = ms_fnv1a(h, &(uint8_t){ m->mbc_mode }, 1);
    h = ms_fnv1a(h, &(uint8_t){ m->vram_bank }, 1);
    h = ms_fnv1a(h, &(uint8_t){ m->ie }, 1);
    h = ms_fnv1a(h, m->io, 0x80);
    for (int i = 0; i < 32; i++) h = ms_fnv1a_u64(h, m->ph_wram[i]);
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 32; i++) h = ms_fnv1a_u64(h, m->ph_vram[b][i]);
    h = ms_fnv1a_u64(h, m->ph_hram);
    h = ms_fnv1a_u64(h, m->ph_oam);
    uint32_t cram_pages = m->cart->ram_size ? (uint32_t)((m->cart->ram_size + 255) >> 8) : 0;
    if (cram_pages > MS_CRAM_PAGES) cram_pages = MS_CRAM_PAGES;
    for (uint32_t i = 0; i < cram_pages; i++) h = ms_fnv1a_u64(h, m->ph_cram[i]);
    return h;
}

// --- memory routing ----------------------------------------------------------

static MsSpace space_of(uint16_t addr) {
    if (addr < 0x8000) return MS_SPACE_ROM;
    if (addr < 0xA000) return MS_SPACE_VRAM;
    if (addr < 0xC000) return MS_SPACE_SRAM;
    if (addr < 0xE000) return MS_SPACE_WRAM;
    if (addr < 0xFE00) return MS_SPACE_ECHO;
    if (addr < 0xFEA0) return MS_SPACE_OAM;
    if (addr < 0xFF00) return MS_SPACE_UNUSABLE;
    if (addr < 0xFF80) return MS_SPACE_IO;
    if (addr < 0xFFFF) return MS_SPACE_HRAM;
    return MS_SPACE_IE;
}

uint8_t ms_peek(const MsMachine *m, uint16_t addr) {
    const MsCart *c = m->cart;
    if (addr < 0x4000) {
        uint32_t off = (uint32_t)addr + m->win_rom0 * 0x4000ull;
        return off < c->rom_size ? c->rom[off] : 0xFF;
    }
    if (addr < 0x8000) {
        uint32_t off = (uint32_t)(addr - 0x4000) + m->win_romN * 0x4000ull;
        return off < c->rom_size ? c->rom[off] : 0xFF;
    }
    if (addr < 0xA000) return m->vram[m->vram_bank & 1][addr - 0x8000];
    if (addr < 0xC000) {
        uint32_t off = ms_map_sram(m, addr);
        return off == UINT32_MAX ? 0xFF : c->ram[off];
    }
    if (addr < 0xE000) return m->wram[addr - 0xC000];
    if (addr < 0xFE00) return m->wram[addr - 0xE000];
    if (addr < 0xFEA0) return m->oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0x00;
    if (addr < 0xFF80) return 0x00; // peek never triggers io hooks
    if (addr < 0xFFFF) return m->hram[addr - 0xFF80];
    return m->ie;
}

uint8_t ms_read(MsMachine *m, uint16_t addr, MsAccKind kind) {
    MsSpace sp = space_of(addr);
    uint8_t v = 0;
    uint32_t bank = 0;
    const MsCart *c = m->cart;
    switch (sp) {
    case MS_SPACE_ROM: {
        uint32_t wbank = addr < 0x4000 ? m->win_rom0 : m->win_romN;
        uint32_t off = (uint32_t)(addr & 0x3FFF) + wbank * 0x4000ull;
        v = off < c->rom_size ? c->rom[off] : 0xFF;
        bank = wbank;
        break;
    }
    case MS_SPACE_VRAM:
        v = m->vram[m->vram_bank & 1][addr - 0x8000];
        bank = m->vram_bank & 1;
        break;
    case MS_SPACE_SRAM: {
        uint32_t off = ms_map_sram(m, addr);
        v = off == UINT32_MAX ? 0xFF : c->ram[off];
        bank = m->win_ram;
        break;
    }
    case MS_SPACE_WRAM: v = m->wram[addr - 0xC000]; break;
    case MS_SPACE_ECHO: v = m->wram[addr - 0xE000]; break;
    case MS_SPACE_OAM: v = m->oam[addr - 0xFE00]; break;
    case MS_SPACE_UNUSABLE: v = 0x00; break;
    case MS_SPACE_IO:
        v = m->plat && m->plat->io_read ? m->plat->io_read(m, addr) : m->io[addr - 0xFF00];
        break;
    case MS_SPACE_HRAM: v = m->hram[addr - 0xFF80]; break;
    case MS_SPACE_IE: v = m->ie; break;
    }
    if (m->trace && m->trace->mem_read) m->trace->mem_read(m->trace_ctx, sp, addr, v, kind, bank);
    return v;
}

uint16_t ms_read16(MsMachine *m, uint16_t addr, MsAccKind kind) {
    uint16_t lo = ms_read(m, addr, kind);
    uint16_t hi = ms_read(m, (uint16_t)(addr + 1), kind);
    return lo | (hi << 8);
}

void ms_write(MsMachine *m, uint16_t addr, uint8_t v, MsAccKind kind) {
    MsSpace sp = space_of(addr);
    uint32_t bank = 0;
    if (addr < 0x8000) {
        // mapper window: bank registers, never ROM data
        const MsPlatform *plat = m->plat;
        uint32_t old0 = m->win_rom0, oldN = m->win_romN, oldR = m->win_ram;
        if (plat && plat->rom_write) plat->rom_write(m, addr, v);
        if (m->trace) {
            if (m->win_rom0 != old0)
                m->trace->mapper_write(m->trace_ctx, addr, v, 0, old0, m->win_rom0);
            else if (m->win_romN != oldN)
                m->trace->mapper_write(m->trace_ctx, addr, v, 1, oldN, m->win_romN);
            else if (m->win_ram != oldR)
                m->trace->mapper_write(m->trace_ctx, addr, v, 2, oldR, m->win_ram);
            else if (m->trace->io_access)
                m->trace->io_access(m->trace_ctx, addr, v, 1);
        }
        return;
    }
    switch (sp) {
    case MS_SPACE_VRAM:
        m->vram_bank &= 1;
        m->vram[m->vram_bank][addr - 0x8000] = v;
        rehash_vram(m, addr);
        bank = m->vram_bank;
        break;
    case MS_SPACE_SRAM: {
        uint32_t off = ms_map_sram(m, addr);
        if (off != UINT32_MAX) {
            m->cart->ram[off] = v;
            rehash_cram(m, off);
            bank = m->win_ram;
        }
        break;
    }
    case MS_SPACE_WRAM:
        m->wram[addr - 0xC000] = v;
        rehash_wram(m, addr);
        break;
    case MS_SPACE_ECHO:
        m->wram[addr - 0xE000] = v;
        rehash_wram(m, addr);
        break;
    case MS_SPACE_OAM:
        m->oam[addr - 0xFE00] = v;
        rehash_oam(m);
        break;
    case MS_SPACE_UNUSABLE: break;
    case MS_SPACE_IO:
        if (m->plat && m->plat->io_write) m->plat->io_write(m, addr, v);
        else m->io[addr - 0xFF00] = v;
        break;
    case MS_SPACE_HRAM:
        m->hram[addr - 0xFF80] = v;
        rehash_hram(m);
        break;
    case MS_SPACE_IE: m->ie = v; break;
    default: break;
    }
    if (m->trace && m->trace->mem_write) m->trace->mem_write(m->trace_ctx, sp, addr, v, kind, bank);
}

void ms_write16(MsMachine *m, uint16_t addr, uint16_t v, MsAccKind kind) {
    ms_write(m, addr, (uint8_t)v, kind);
    ms_write(m, (uint16_t)(addr + 1), (uint8_t)(v >> 8), kind);
}

uint8_t ms_fetch(MsMachine *m, uint16_t addr) {
    uint8_t v = ms_peek(m, addr);
    MsSpace sp = space_of(addr);
    if (m->trace && m->trace->mem_read) {
        uint32_t bank = addr < 0x4000 ? m->win_rom0 : (addr < 0x8000 ? m->win_romN : 0);
        m->trace->mem_read(m->trace_ctx, sp, addr, v, MS_ACC_FETCH, bank);
    }
    return v;
}

uint16_t ms_fetch16(MsMachine *m, uint16_t addr) {
    uint16_t lo = ms_fetch(m, addr);
    uint16_t hi = ms_fetch(m, (uint16_t)(addr + 1));
    return lo | (hi << 8);
}
