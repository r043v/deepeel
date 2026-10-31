// machine.h: the concrete machine a path runs on — CPU registers, mapped
// windows, RAM arrays and IO state.  Deterministic: RAM starts zeroed, the
// reset register set comes from the platform, so equal states are equal and
// dedup is sound.
#ifndef MS_MACHINE_H
#define MS_MACHINE_H

#include "cart.h"
#include "../platform/platform.h"

// access kinds reported by the trace hooks
typedef enum { MS_ACC_FETCH = 0, MS_ACC_DATA = 1, MS_ACC_IO = 2, MS_ACC_STACK = 3 } MsAccKind;

// control flow of the last executed instruction
typedef enum {
    MS_CTRL_NONE = 0,
    MS_CTRL_JP,
    MS_CTRL_JR,
    MS_CTRL_JP_CC,
    MS_CTRL_JR_CC,
    MS_CTRL_CALL,
    MS_CTRL_CALL_CC,
    MS_CTRL_RET,
    MS_CTRL_RET_CC,
    MS_CTRL_RETI,
    MS_CTRL_RST,
    MS_CTRL_JP_HL, // indirect
    MS_CTRL_HALT,
    MS_CTRL_STOP,
    MS_CTRL_BAD, // undefined opcode
} MsCtrl;

typedef struct MsStepInfo {
    uint16_t pc;       // pc before the instruction
    uint8_t len;
    uint8_t opcode;
    MsCtrl ctrl;
    uint16_t target;   // direct target / vector
    uint8_t cond_taken; // for *_CC: natural outcome with current flags
} MsStepInfo;

// one observed copy: a source range that landed in a destination range
typedef struct MsCopyRec {
    uint8_t channel;      // 0 = CPU loop, 1 = OAM DMA, 2 = HDMA
    uint8_t verbatim;     // destination bytes equal the source bytes exactly
    uint8_t src_space, dst_space;
    uint32_t src_bank, dst_bank;
    uint16_t src_lo, src_hi; // inclusive
    uint16_t dst_lo, dst_hi;
    uint32_t bytes;
    uint16_t pc;          // site that produced it
} MsCopyRec;

// bank switch observed on a mapper write
typedef struct MsBankEvent {
    uint32_t seq;
    uint16_t pc;
    uint8_t window; // 0 = rom0, 1 = romN, 2 = ram
    uint32_t old_bank, new_bank;
} MsBankEvent;

// trace hooks: the explorer registers these to record everything that moves.
typedef struct MsTraceVt {
    void (*mem_read)(void *ctx, MsSpace sp, uint16_t addr, uint8_t v, MsAccKind kind, uint32_t bank);
    void (*mem_write)(void *ctx, MsSpace sp, uint16_t addr, uint8_t v, MsAccKind kind, uint32_t bank);
    void (*mapper_write)(void *ctx, uint16_t addr, uint8_t v, uint8_t window, uint32_t old, uint32_t new);
    void (*io_access)(void *ctx, uint16_t addr, uint8_t v, int is_write);
    void (*copy)(void *ctx, const MsCopyRec *rec);
    void (*step)(void *ctx, const MsStepInfo *info);
    void (*event)(void *ctx, int kind, uint32_t a, uint32_t b);
} MsTraceVt;

// trace event kinds
enum { MS_EV_HALT = 1, MS_EV_STOP, MS_EV_BADOP, MS_EV_BADRET, MS_EV_IRQ };

#define MS_CRAM_PAGES 512 // 128 KiB cart RAM max

typedef struct MsMachine {
    // CPU
    uint16_t pc, sp;
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t ime, ime_delay, halted;
    // session pad latch (play sessions only): a=1 b=2 select=4 start=8,
    // right=16 left=32 up=64 down=128; 1 = held.  The forking explorer
    // never touches it.
    uint8_t buttons;
    // session clock (T-cycles), advanced by the platform tick; the forking
    // explorer never ticks, so these stay 0 there
    uint32_t clk_div, clk_tima, clk_line;
    // memory
    const MsCart *cart;
    const MsPlatform *plat;
    uint8_t vram[2][0x2000];
    uint8_t wram[0x2000];
    uint8_t hram[0x7F];
    uint8_t oam[0xA0];
    uint8_t ie;
    uint8_t io[0x80]; // 0xFF00-0xFF7F plain storage backing
    // cartridge RAM backing: points at cart->ram by default; play sessions
    // replace it with a private copy so machines stay snapshot-able and
    // thread-safe (fork paths share, exactly as before)
    uint8_t *sram;
    uint32_t sram_size;
    // mapper state
    uint8_t rom_lo, rom_hi, ram_bank, ramg, mbc_mode, vram_bank;
    // derived windows (updated by the mapper hooks)
    uint32_t win_rom0, win_romN, win_ram;
    // 256-byte page hashes, maintained on write, so a state hash costs
    // registers + windows + io + ~70 words instead of 20 KiB
    uint64_t ph_vram[2][32];
    uint64_t ph_wram[32];
    uint64_t ph_hram, ph_oam;
    uint64_t ph_cram[MS_CRAM_PAGES];
    // tracing
    const MsTraceVt *trace;
    void *trace_ctx;
} MsMachine;

void ms_machine_init(MsMachine *m, const MsCart *cart, const MsPlatform *plat);
void ms_mapper_update_windows(MsMachine *m); // recompute windows from mapper state
uint32_t ms_map_sram(const MsMachine *m, uint16_t addr); // cart RAM offset or UINT32_MAX
uint64_t ms_state_hash(const MsMachine *m);

uint8_t ms_peek(const MsMachine *m, uint16_t addr);        // no trace, no side effect
uint8_t ms_read(MsMachine *m, uint16_t addr, MsAccKind kind); // traced, no side effects on mapper
uint16_t ms_read16(MsMachine *m, uint16_t addr, MsAccKind kind);
void ms_write(MsMachine *m, uint16_t addr, uint8_t v, MsAccKind kind);
void ms_write16(MsMachine *m, uint16_t addr, uint16_t v, MsAccKind kind);
uint8_t ms_fetch(MsMachine *m, uint16_t addr); // traced fetch
uint16_t ms_fetch16(MsMachine *m, uint16_t addr);

// canonical ROM index: bank * 0x4000 + (addr within the 16 KiB bank).
// Both windows fold into one bank: 0x0000-0x3FFF and 0x4000-0x7FFF are
// offsets 0-0x3FFF of the bank they currently map.
static inline uint32_t ms_rom_index(uint32_t bank, uint16_t addr) {
    return bank * 0x4000u + (addr & 0x3FFFu);
}
// index of the pc under the current windows
static inline uint32_t ms_rom_key(const MsMachine *m, uint16_t pc) {
    uint32_t bank = (pc < 0x4000) ? m->win_rom0 : m->win_romN;
    return ms_rom_index(bank, pc);
}

#endif
