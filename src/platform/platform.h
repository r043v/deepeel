// platform.h: the per-platform hardware contract.
//
// A platform is a complete registry of the hardware surfaces a ROM can touch:
// every register (IO range or mapper window), every IRQ, the memory regions,
// the reset entry points, and the hooks that give registers their semantics
// (bank switching, DMA copy channels...).  The explorer is platform-agnostic;
// adding a console means writing one of these, not touching the core.
#ifndef MS_PLATFORM_H
#define MS_PLATFORM_H

#include "../common.h"
#include "../core/cart.h"

struct MsMachine;

// --- implications: what a register write means for the memory map -----------
enum {
    MS_IMPL_NONE = 0,
    MS_IMPL_BANK_LO = 1 << 0,   // selects low bits of a ROM bank
    MS_IMPL_BANK_HI = 1 << 1,   // selects high bits of a ROM bank
    MS_IMPL_RAM_BANK = 1 << 2,  // selects a RAM bank
    MS_IMPL_RAMG = 1 << 3,      // gates cartridge RAM
    MS_IMPL_MODE = 1 << 4,      // banking mode select
    MS_IMPL_DMA_SRC = 1 << 5,   // triggers a copy channel
    MS_IMPL_HDMA = 1 << 6,      // H-blank/instant DMA copy
    MS_IMPL_IRQ = 1 << 7,       // interrupt flags / mask
    MS_IMPL_EVENT = 1 << 8,     // event-driven value (timer, PPU line...): the
                                // explorer holds it static and seeds the IRQ
                                // entries instead
    MS_IMPL_WINDOW = 1 << 9,    // selects a memory window (VRAM bank...)
    MS_IMPL_MODELLED = 1 << 10, // semantics modelled by this explorer build
};

// one register (or register window for mapper ranges)
typedef struct MsIoReg {
    uint16_t lo, hi;
    const char *name;
    const char *category; // "video" "sound" "timer" "input" "serial" "interrupt" "mapper" "cgb" "sys"
    uint8_t rw;           // 1 = read, 2 = write, 3 = both
    uint16_t impl;
} MsIoReg;

typedef struct MsIrq {
    uint8_t bit;      // bit in IF/IE
    uint16_t vector;  // handler address
    const char *name;
} MsIrq;

typedef enum {
    MS_SPACE_ROM = 0,
    MS_SPACE_VRAM,
    MS_SPACE_SRAM,
    MS_SPACE_WRAM,
    MS_SPACE_ECHO,
    MS_SPACE_OAM,
    MS_SPACE_UNUSABLE,
    MS_SPACE_IO,
    MS_SPACE_HRAM,
    MS_SPACE_IE,
} MsSpace;

typedef struct MsRegion {
    uint16_t lo, hi;
    const char *name;
    MsSpace space;
} MsRegion;

// seed kinds for the entry list
enum { MS_SEED_RESET = 0, MS_SEED_RST, MS_SEED_IRQ, MS_SEED_POINTER };

typedef struct MsEntry {
    uint16_t addr;
    uint8_t kind; // MS_SEED_*
    const char *name;
} MsEntry;

typedef struct MsPlatform {
    const char *name;
    const MsIoReg *io_regs;
    int io_reg_count;
    const MsIrq *irqs;
    int irq_count;
    const MsRegion *regions;
    int region_count;
    const MsEntry *entries;
    int entry_count;
    // reset-time CPU state
    uint16_t init_pc, init_sp;
    uint8_t init_a, init_f, init_b, init_c, init_d, init_e, init_h, init_l;
    // hooks (the machine routes through them)
    // mapper window 0x0000-0x7FFF: bank registers
    void (*rom_write)(struct MsMachine *m, uint16_t addr, uint8_t v);
    uint8_t (*io_read)(struct MsMachine *m, uint16_t addr);
    void (*io_write)(struct MsMachine *m, uint16_t addr, uint8_t v);
    // advance the event-driven hardware (timer, PPU line clock...) by t
    // T-cycles.  Play sessions tick every instruction; the forking explorer
    // leaves the clock still and seeds the IRQ entries instead.  May be NULL.
    void (*tick)(struct MsMachine *m, uint32_t t);
} MsPlatform;

// only platform implemented so far; more CPUs/consoles plug in here
const MsPlatform *ms_platform_gb(void);

#endif
