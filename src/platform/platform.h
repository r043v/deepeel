// platform.h: the per-platform hardware contract.
//
// A platform is a complete registry of the hardware surfaces a ROM can touch:
// every register (IO range or mapper window), every IRQ, the memory regions,
// the reset entry points, and the hooks that give registers their semantics
// (bank switching, DMA copy channels...).  The explorer is platform-agnostic;
// adding a console means writing one of these, not touching the core.
#ifndef DP_PLATFORM_H
#define DP_PLATFORM_H

#include "../common.h"
#include "../core/cart.h"

// --- implications: what a register write means for the memory map -----------
enum {
    DP_IMPL_NONE = 0,
    DP_IMPL_BANK_LO = 1 << 0,   // selects low bits of a ROM bank
    DP_IMPL_BANK_HI = 1 << 1,   // selects high bits of a ROM bank
    DP_IMPL_RAM_BANK = 1 << 2,  // selects a RAM bank
    DP_IMPL_RAMG = 1 << 3,      // gates cartridge RAM
    DP_IMPL_MODE = 1 << 4,      // banking mode select
    DP_IMPL_DMA_SRC = 1 << 5,   // triggers a copy channel
    DP_IMPL_HDMA = 1 << 6,      // H-blank/instant DMA copy
    DP_IMPL_IRQ = 1 << 7,       // interrupt flags / mask
    DP_IMPL_EVENT = 1 << 8,     // event-driven value (timer, PPU line...): the
                                // explorer holds it static and seeds the IRQ
                                // entries instead
    DP_IMPL_WINDOW = 1 << 9,    // selects a memory window (VRAM bank...)
    DP_IMPL_MODELLED = 1 << 10, // semantics modelled by this explorer build
};

// one register (or register window for mapper ranges)
typedef struct DpIoReg {
    uint16_t lo, hi;
    const char *name;
    const char *category; // "video" "sound" "timer" "input" "serial" "interrupt" "mapper" "cgb" "sys"
    uint8_t rw;           // 1 = read, 2 = write, 3 = both
    uint16_t impl;
} DpIoReg;

typedef struct DpIrq {
    uint8_t bit;      // bit in IF/IE
    uint16_t vector;  // handler address
    const char *name;
} DpIrq;

typedef enum {
    DP_SPACE_ROM = 0,
    DP_SPACE_VRAM,
    DP_SPACE_SRAM,
    DP_SPACE_WRAM,
    DP_SPACE_ECHO,
    DP_SPACE_OAM,
    DP_SPACE_UNUSABLE,
    DP_SPACE_IO,
    DP_SPACE_HRAM,
    DP_SPACE_IE,
} DpSpace;

typedef struct DpRegion {
    uint16_t lo, hi;
    const char *name;
    DpSpace space;
} DpRegion;

// seed kinds for the entry list
enum { DP_SEED_RESET = 0, DP_SEED_RST, DP_SEED_IRQ, DP_SEED_POINTER };

typedef struct DpEntry {
    uint16_t addr;
    uint8_t kind; // DP_SEED_*
    const char *name;
} DpEntry;

typedef struct DpPlatform {
    const char *name;
    const DpIoReg *io_regs;
    int io_reg_count;
    const DpIrq *irqs;
    int irq_count;
    const DpRegion *regions;
    int region_count;
    const DpEntry *entries;
    int entry_count;
} DpPlatform;

// only platform implemented so far; more CPUs/consoles plug in here
const DpPlatform *dp_platform_gb(void);

#endif
