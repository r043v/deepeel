// pce.h: the PC Engine bridge.  deepeel embeds m0boy's PC Engine machine
// (targets/pce/runtime: the HuC6280 interpreter, the VDC, the VCE, the PSG,
// the timer and the pad) and drives it one instruction at a time.  Every
// cartridge access goes through this bridge (the machine's direct ROM
// windows are disabled), so the explorer sees each ROM data read, each
// VDC port write and each MPR change.
//
// ROM keys: the HuCard is a linear image of 8 KiB pages; the house index
// bank * $4000 + (addr & $3FFF) over 16 KiB banks equals the linear offset
// page * $2000 + (addr & $1FFF), which is what dp_pce_rom_index returns.
#ifndef DP_PCE_H
#define DP_PCE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// --- analysis decoder (side-effect free) ------------------------------------
typedef enum {
    DP_HUC_NEXT,      // straight-line
    DP_HUC_JUMP,      // jmp abs / bra: target known
    DP_HUC_INDIRECT,  // jmp (abs) / jmp (abs,x): target from memory
    DP_HUC_CALL,      // jsr abs / bsr rel
    DP_HUC_RETURN,    // rts / rti
    DP_HUC_BRANCH,    // conditional (flag branch or BBR/BBS)
    DP_HUC_BRK,       // software interrupt through $FFF6
    DP_HUC_ILLEGAL    // undefined opcode (a two-cycle NOP on the machine)
} DpHucFlow;

typedef struct {
    uint8_t op, length;
    uint8_t flow;        // DpHucFlow
    uint8_t mode;        // addressing mode (DP_HUC_M_*)
    uint8_t mapchange;   // TAM: the windows change after this instruction
    int8_t cond;         // flag branches: 0..7 (see dp_huc_taken); BBR/BBS: 8+bit, 16+bit
    uint16_t target;     // JUMP/CALL/BRANCH target
    uint16_t operand;    // absolute/zero-page operand (memory modes)
} DpHucIns;

enum {
    DP_HUC_M_IMP, DP_HUC_M_IMM, DP_HUC_M_ZP, DP_HUC_M_ZPX, DP_HUC_M_ZPY,
    DP_HUC_M_ABS, DP_HUC_M_ABSX, DP_HUC_M_ABSY, DP_HUC_M_IZX, DP_HUC_M_IZY,
    DP_HUC_M_IZP, DP_HUC_M_IND, DP_HUC_M_INDX, DP_HUC_M_REL, DP_HUC_M_ZPREL,
    DP_HUC_M_BLOCK, DP_HUC_M_TST_ZP, DP_HUC_M_TST_ABS
};

// decode at pc from up to 7 bytes; returns 0 when size cannot cover it
int dp_huc_decode(const uint8_t *b, size_t size, uint16_t pc, DpHucIns *out);
void dp_huc_format(const DpHucIns *in, const uint8_t *b, char *out, size_t size);

// --- machine ----------------------------------------------------------------
enum { DP_PCE_LINE_CYCLES_CSH = 455 };

typedef struct {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t mpr[8];
    uint8_t irq_low, irq_mask;
    int speed;
} DpPceCpu;

// bus observer: the explorer installs it, the bridge calls it for every
// cartridge or hardware access the CPU makes (never for WRAM)
typedef struct {
    void *user;
    void (*rom_read)(void *user, uint32_t cell, uint16_t addr);
    void (*io_write)(void *user, uint16_t reg, uint8_t value); // reg: $0000-$1FFF of the hardware page
    void (*io_read)(void *user, uint16_t reg, uint8_t value);
} DpPceObserver;

void dp_pce_reference(int pcehawk);   // frame cut of the core that recorded the movie (0: TurboNyma, 1: PCEHawk)
int dp_pce_init(const uint8_t *data, size_t size); // strips a 512-byte copier header
void dp_pce_close(void);
size_t dp_pce_rom_size(void);
const uint8_t *dp_pce_rom(void);
void dp_pce_observe(const DpPceObserver *o);       // NULL: no observer
// the bridge's view of the CPU (registers, MPR, interrupt latches)
DpPceCpu dp_pce_cpu(void);
void dp_pce_set_flags(uint8_t p);
int dp_pce_enter(uint32_t rom_page, uint16_t pc); // map the page under pc's window, set pc; 0: unmappable
void dp_pce_set_a(uint8_t a);       // accumulator (event probes overwrite a status read)
void dp_pce_set_pc(uint16_t pc);    // explicit entry point (the live mapping applies)
// the physical cell of a CPU address under the live MPR (-1: not ROM)
int64_t dp_pce_rom_index(uint16_t addr);
int64_t dp_pce_rom_index_mpr(const uint8_t *mpr, uint16_t addr);
// side-effect-free byte view (no I/O side effects; hardware reads $FF)
uint8_t dp_pce_peek(uint16_t addr);
void dp_pce_poke(uint16_t addr, uint8_t v);   /* work RAM through the mapping, no CPU access */
// true when the next step dispatches an interrupt instead of an instruction
int dp_pce_irq_due(void);
// one step: an interrupt dispatch or one instruction; the line clock
// (VDC line, timer, IRQ latches) advances by the cycles spent.  Returns the
// cycles spent; *frame_done set when a frame (262 lines) completed.
unsigned dp_pce_step(int *frame_done);
// raise an interrupt source now (exploration's IRQ forks): 1 IRQ1/VDC
// vblank, 2 timer
void dp_pce_raise(int source);
void dp_pce_input(uint8_t buttons);  // PCE_BTN_* layout of m0boy
uint8_t dp_pce_input_get(void);
int dp_pce_multitap_enabled(void);
void dp_pce_multitap(int on);                    // five-pad adapter model (opt-in, scan outside the snapshot)
void dp_pce_pad_n(unsigned n, uint8_t buttons);  // pads 2..5 (n = 1..4); pad 1 is dp_pce_input
unsigned dp_pce_tap_index(void);
uint8_t dp_pce_pad_port_read(void);
void dp_pce_pad_port_write(uint8_t v);
uint32_t dp_pce_frames(void);
void dp_pce_watch_enable(int on);               // capture the VDC rows (costs time: off by default)
void dp_pce_set_frame_hook(void (*fn)(void));   // called when a frame completes in dp_pce_step
int dp_pce_render(uint32_t *argb, unsigned cap, int *w, int *h);
// whole-machine snapshot (CPU, WRAM, VDC with VRAM, VCE, PSG, timer, pad,
// line clock); dp_pce_state_hash covers the control state only (CPU, WRAM,
// VDC registers, timer, pad, line phase) — the dedup key
size_t dp_pce_state_size(void);
void dp_pce_save(void *dst);
void dp_pce_load(const void *src);
uint64_t dp_pce_state_hash(void);
// the 9-bit VRAM-side view of the current frame for screenshots: the
// machine renders rows only when asked
uint64_t dp_pce_vram_hash(void);
uint8_t *dp_pce_wram(void);     // the 8 KiB work RAM (page $F8)
uint8_t dp_pce_mpr(unsigned i);
void dp_pce_reset(void);
// fast frames: the native interpreter runs to the frame's end; the trace
// callback sees every instruction (pc, opcode) and every interrupt entry
typedef void (*DpPceTraceFn)(void *user, uint16_t pc, uint8_t op);
void dp_pce_trace(DpPceTraceFn fn, void *user);
void dp_pce_run_frame(void);
// a coarse fingerprint of the background map (exploration cells)
uint64_t dp_pce_screen_key(void);
// opcode length (1 for the undefined opcodes) and definedness
uint8_t dp_huc_length(uint8_t op);
int dp_huc_defined(uint8_t op);         // the reset line (power-on state kept)

// --- registry -----------------------------------------------------------------
#include "../platform.h"
const DpPlatform *dp_pce_platform(void);
void dp_pce_regs(FILE *out);
#endif
