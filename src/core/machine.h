// machine.h: the shared records of an exploration (copy records, bank
// events) and the canonical ROM key.  The machine itself is the m0boy
// runtime embedded as a dependency — see src/mach/m0boy.h.
#ifndef DP_MACHINE_H
#define DP_MACHINE_H

#include "cart.h"
#include "../common.h"

// one observed copy: a source range that landed in a destination range
typedef struct DpCopyRec {
    uint8_t channel;      // 0 = CPU loop, 1 = OAM DMA
    uint8_t verbatim;     // destination bytes equal the source bytes exactly
    uint8_t src_space, dst_space;
    uint32_t src_bank, dst_bank;
    uint16_t src_lo, src_hi; // inclusive
    uint16_t dst_lo, dst_hi;
    uint32_t bytes;
    uint16_t pc;          // site that produced it
} DpCopyRec;

// bank switch observed on a mapper write
typedef struct DpBankEvent {
    uint32_t seq;
    uint16_t pc;
    uint8_t window; // 0 = rom0, 1 = romN, 2 = ram
    uint32_t old_bank, new_bank;
} DpBankEvent;

#define DP_CRAM_PAGES 512 // 128 KiB cart RAM max

// canonical ROM index: bank * 0x4000 + (addr within the 16 KiB bank).
// Both windows fold into one bank: 0x0000-0x3FFF and 0x4000-0x7FFF are
// offsets 0-0x3FFF of the bank they currently map.
static inline uint32_t dp_rom_index(uint32_t bank, uint16_t addr) {
    return bank * 0x4000u + (addr & 0x3FFFu);
}
// index of the pc under the current bank window
static inline uint32_t dp_rom_key_bank(uint32_t bank, uint16_t pc) {
    return dp_rom_index(bank, pc);
}

#endif
