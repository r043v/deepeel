// dpmachine.h: the generic machine and ISA interfaces of the exploration
// kernel (spec §7.2, §7.3).  A platform supplies a DpMachine (execution,
// state, memory geometry) and a DpIsa (static decoding); the kernel modules
// consume only these.  Every member is optional: the kernel degrades and
// reports what is missing.
#ifndef DP_DPMACHINE_H
#define DP_DPMACHINE_H

#include <stddef.h>
#include <stdint.h>

#include "../platform/platform.h"

// control-flow class of one instruction
typedef enum {
    DP_FLOW_SEQ = 0,   // falls through
    DP_FLOW_JUMP,      // unconditional direct jump
    DP_FLOW_COND,      // conditional (direct target or return) + fall-through
    DP_FLOW_CALL,      // call (direct target; may be conditional: see `cond`)
    DP_FLOW_RET,       // return (unconditional, no fall-through)
    DP_FLOW_INDIRECT,  // computed target (JP HL, ...)
    DP_FLOW_ILLEGAL    // undefined opcode
} DpFlow;

typedef struct DpFlowInfo {
    uint8_t kind;      // DpFlow
    uint8_t length;    // bytes of the instruction
    uint8_t fall;      // 1 when execution can continue at pc + length
    uint8_t call;      // 1 when the transfer pushes a return address
    int32_t target;    // absolute CPU address of a direct transfer, -1 if none
    int8_t cond;       // condition index (CPU specific), -1 if unconditional
    uint8_t rst;       // 1 for a restart vector (call to a fixed address)
    uint8_t halt;      // 1 when the run ends here (HALT / STOP)
} DpFlowInfo;

// register provenance lattice (spec §7.3 regs_effects): the pointer pair
// (HL on the SM83/Z80) and the accumulator, each either a proven value or
// unknown.  pmask: bit0 = low byte known, bit1 = high byte known.
typedef struct DpProvRegs {
    uint16_t p;
    uint8_t pmask, a, aknown;
} DpProvRegs;

// ROM access given to the effect function: read(ctx, addr, &v) returns 1 and
// the byte when `addr` is a provable ROM address (the caller records the
// proof), 0 otherwise
typedef struct DpProvRom {
    void *ctx;
    int (*read)(void *ctx, uint32_t addr, uint8_t *v);
} DpProvRom;

// data-memory operand of an instruction (spec §7.3 mem_operand), the input of
// the access typing of §5: what is touched and how the address is formed
enum { DP_MA_NONE = 0, DP_MA_READ = 1, DP_MA_WRITE = 2, DP_MA_RW = 3 };
enum { DP_AM_NONE = 0, DP_AM_ABS, DP_AM_HIGH_IMM, DP_AM_HIGH_REG, DP_AM_PAIR, DP_AM_STACK };
typedef struct DpMemOp {
    uint8_t access;   // DP_MA_*
    uint8_t mode;     // DP_AM_*
    uint8_t pair;     // DP_AM_PAIR: 0 BC, 1 DE, 2 HL, 3 SP
    int8_t post;      // pair post-increment (+1/-1), 0 otherwise
    uint8_t width;    // bytes touched
    uint32_t addr;    // DP_AM_ABS: the address; DP_AM_HIGH_IMM: $FF00 + n
} DpMemOp;

typedef struct DpIsa {
    const char *name;                      // "sm83", "z80", ...
    uint8_t ptr_width;                     // bytes of a code pointer
    uint8_t big_endian;
    // length of the instruction starting with `op`
    int (*length)(uint8_t op);
    // classify the instruction at `bytes` (at least `length` readable bytes)
    // located at CPU address `pc`
    int (*flow)(const uint8_t *bytes, uint32_t pc, DpFlowInfo *out);
    // condition index of a conditional opcode, -1 otherwise
    int (*cond_of)(uint8_t op);
    // state mask of the flag a condition tests (polarity in the low bit of cond)
    uint8_t (*cond_flag)(int cond);
    // micro-effects of the instruction on the provenance lattice
    void (*regs_effect)(const uint8_t *bytes, DpProvRegs *r, const DpProvRom *rom);
    // 1 when `op` is an undefined opcode
    int (*illegal)(uint8_t op);
    // textual form; returns the length or 0 when undefined
    int (*disasm)(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz);
    // data operand of the instruction at `bytes`; 1 when it touches memory
    int (*mem_operand)(const uint8_t *bytes, DpMemOp *out);
    // C1: capabilities the kernel used to read off the ISA name
    uint32_t code_min;                     // lowest address legal as a jump-table target (sm83: past the cartridge header)
    uint8_t hl_jp_table;                   // `jp (hl)` dispatches through a table the closure can follow
    uint8_t abs_ind_table;                 // `jmp (abs)` / `jmp (abs,x)` dispatch through a word table
    int (*imm_ptr)(const uint8_t *bytes, uint32_t *imm);   // B9: 1 when the instruction loads a 16-bit immediate pointer into a register pair
    int (*imm_byte)(const uint8_t *bytes, uint8_t *v);     // B9: 1 when the instruction loads an 8-bit immediate (two nearby ones form a split pointer)
} DpIsa;

const DpIsa *dp_isa_sm83(void);
const DpIsa *dp_isa_z80(void);
const DpIsa *dp_isa_m6502(void);
const DpIsa *dp_isa_huc6280(void);
const DpIsa *dp_isa_68000(void);

// normalised register view (pc/sp and the flag byte; regs are CPU specific)
typedef struct DpRegs {
    uint32_t pc, sp;
    uint16_t flags;
    uint16_t reg[8];
} DpRegs;

// observation hooks (spec §7.2 observe): every executed instruction and every
// CPU data access, as the engine sees them
typedef struct DpObservers {
    void (*on_step)(void *user, uint32_t pc, uint8_t op, int entry);
    void (*on_read)(void *user, uint32_t addr, uint8_t v);
    void (*on_write)(void *user, uint32_t addr, uint8_t v);
    // B11: a copy made by hardware, not by the CPU (VDP/OAM/HDMA DMA, block transfer): `len` source bytes from `src` are
    // moved to a destination the machine owns; the CPU never read them
    void (*on_copy)(void *user, uint32_t src, uint32_t len, unsigned channel);
} DpObservers;
enum { DP_COPY_DMA = 1 };

// a component of the incoming state a section's behaviour may depend on
// (spec §7.2 ctx_components): the bit index is the platform's DP_CTX_* value
enum { DP_CC_REG = 0, DP_CC_PAGES };
typedef struct DpCtxComp {
    const char *name;
    uint8_t kind;      // DP_CC_REG (one bit of ctx_mask) or DP_CC_PAGES (256-byte pages)
    int bit;           // ctx_mask bit for DP_CC_REG, -1 otherwise
    uint32_t bytes;    // size of the component
} DpCtxComp;

// an input the explorer can inject to open the other side of a data-dependent
// branch (spec §7.2 probes)
struct DpMachine;
typedef struct DpProbe {
    const char *name;
    const char *desc;
    uint32_t values;                         // number of distinct values
    void (*apply)(struct DpMachine *, uint32_t value);
} DpProbe;

typedef struct DpMachine DpMachine;

// register seeds for one entry point (music rips: the init call carries the
// song number in A and starts on the rip's stack)
typedef struct DpSeedRegs { uint8_t a; uint16_t sp; } DpSeedRegs;

// C12: the kernel pad mask (DpMachine.set_input).  Per port 16 bits, port 1 in bits 0..15, port 2 in 16..31 (the
// session / TAS markers of the explorer live above bit 28, so a pad mask must stay below 1 << 29).
enum {
    DP_IN_UP = 1 << 0, DP_IN_DOWN = 1 << 1, DP_IN_LEFT = 1 << 2, DP_IN_RIGHT = 1 << 3,
    DP_IN_B1 = 1 << 4,       // NES A, PCE I, GB A, Mega Drive A, Sega button 1
    DP_IN_B2 = 1 << 5,       // NES B, PCE II, GB B, Mega Drive B, Sega button 2
    DP_IN_START = 1 << 6,    // Start / Run
    DP_IN_SELECT = 1 << 7,   // Select (GB, NES, PCE); the Sega reset key
    DP_IN_C = 1 << 8,        // Mega Drive C
    DP_IN_X = 1 << 9, DP_IN_Y = 1 << 10, DP_IN_Z = 1 << 11, DP_IN_MODE = 1 << 12,   // Mega Drive six-button
    DP_IN_PORT2 = 16         // shift of the second port
};

struct DpMachine {
    const char *name;                      // "gb", "sms", "nes", ...
    const DpPlatform *plat;
    const DpIsa *isa;
    uint32_t code_base;                    // C3/B8: address of the first 16 KiB ROM window (GB 0, NES 0x8000)
    uint8_t fixed_high;                    // 1 = the high window is the fixed (last) bank and the low one switches (NES); 0 = the reverse (GB)
    uint32_t save_ram_en_addr; uint8_t save_ram_en_val, save_ram_en;   // B10: write that enables the cart RAM before seeding
    uint32_t save_ram_base, save_ram_len;  // B10: battery/cart RAM window the game may validate at boot (0 len: none)
    uint32_t work_ram_base, work_ram_len;  // A25: the CPU-visible work RAM range (0 len: unknown, defaults to $C000/$2000)
    // C13: dense index of the CPU-addressable RAM the core keys its per-address tables on (ram_index -1: not RAM).
    // Absent: the low 16 address bits (every 16-bit CPU).  ram_addr is the inverse, ram_size the index count.
    int64_t (*ram_index)(struct DpMachine *, uint32_t addr);
    uint32_t (*ram_addr)(struct DpMachine *, uint32_t idx);
    uint32_t ram_size;
    void *impl;                            // backend instance
    // state images
    uint32_t (*state_bytes)(struct DpMachine *);
    void (*save)(struct DpMachine *, uint8_t *buf);
    void (*load)(struct DpMachine *, const uint8_t *buf);
    // memory geometry
    int64_t (*rom_index)(struct DpMachine *, uint32_t addr);  // <0 when not ROM
    uint8_t (*fetch)(struct DpMachine *, uint32_t addr);      // opcode view
    // execution (NULL when the backend only advances by frames)
    int (*run)(struct DpMachine *);
    // program counter access for the generic explorer (core.c): the forced
    // side of a conditional is taken by moving pc on a restored state
    uint32_t (*pc)(struct DpMachine *);
    void (*set_pc)(struct DpMachine *, uint32_t pc);
    // Optional: prepare the CPU registers of a seeded entry point (music rips:
    // the init call carries the song number in A and starts on the rip's
    // stack).  a == 0xFF / sp == 0xFFFF leave the register untouched.  NULL:
    // the kernel counts the seed in DpCoreResult.seed_regs_absent (never
    // silent).
    int (*seed_regs)(struct DpMachine *, uint8_t a, uint16_t sp);
    // Optional: map the 16 KiB ROM block `bank` into the platform's seed
    // window ($4000 on GB/Sega/PCE, $8000 on NES) through raw mapper writes —
    // the per-bank seeding (the banks parallel to the kernel's seeds).  0 when
    // this board cannot show that block: the kernel counts it in
    // DpCoreResult.seed_banks_absent (never silent).
    int (*bank_view)(struct DpMachine *, unsigned bank);
    // Optional nonsense cuts (the GB legacy eviction, mutualized — audit E2):
    // 1 when executing at `pc` is senseless (echo/OAM RAM, I/O page — the bytes
    // never become code evidence), resp. 1 when the current stack pointer is
    // outside the platform's RAM space (checked when a RET is about to pop).
    // NULL: no cut from this machine; every cut is counted, never silent.
    int (*cut_pc)(struct DpMachine *, uint32_t pc);
    int (*cut_sp)(struct DpMachine *);
    // Optional static-closure geometry (audit E5): log2 of the ROM window the
    // ISA's pointer tables fold addresses into (sm83/z80: 14 — a 16 KiB window,
    // low 16 KiB fixed to bank 0).  0 = the house bank * 0x4000 convention;
    // the kernel's banked static walk (closure_walk_img / the stride-3 tables)
    // consumes it instead of assuming the geometry.
    uint8_t linear;   // 1 when the ROM is one linear space starting at address 0 (the image is the ROM, one window; MD)
    uint8_t bank_shift;
    uint8_t page_shift;   // B8: log2 of the ROM page size when the executing address of a page depends on the mapper (PCE: 13); the kernel journals the window each page ran at
    // Optional data-destination knowledge (the shared DP_DEST_* model in
    // coverage.h): where a write to `addr` lands in the hardware (VRAM, OAM,
    // palette, sound registers...).  0 = untyped.  The kernel records the
    // destination of the data each ROM byte fed; NULL: no typing here.
    uint8_t (*data_dest)(struct DpMachine *, uint32_t addr);
    // writes one byte through the machine's memory map, unobserved (NULL: unsupported);
    // the entry point of the RAM-seeding techniques (docs/PLAN-MUTUALISATION.fr.md, step 3)
    void (*poke)(struct DpMachine *, uint32_t addr, uint8_t value);
    // holds a pad mask (bits 0..5 = up/down/left/right/button1/button2) until changed;
    // NULL: the machine has no input model.  Entry point of the kernel's held-pad sessions.
    void (*set_input)(struct DpMachine *, uint32_t mask);
    // C14: the next CPU read of `addr` returns `value`, once; the arming lives in the saved state.  NULL: the
    // machine's probes patch the underlying state (save / patch / restore) instead.  Returns 0 when unsupported.
    int (*arm_read)(struct DpMachine *, uint32_t addr, uint8_t value);
    // B5: hash of the whole machine state with the monotonic clocks excluded (cycle / frame counters): two equal
    // hashes at the same pc mean the machine is in the same state.  NULL: unsupported (the kernel counts it).
    uint64_t (*state_hash)(struct DpMachine *);
    // completed frames since power-on (NULL: unknown); lets the kernel replay a movie
    uint64_t (*frame)(struct DpMachine *);
    // Optional game-screen access (the generic live watch, platform-neutral): watch(on) makes the machine capture its picture
    // (rendering costs time: off by default); render() writes the last completed frame as ARGB (at most `cap` pixels) and
    // returns 1 with its size, 0 when there is no picture.  NULL: this machine has no screen view (the watch says so).
    void (*watch)(struct DpMachine *, int on);
    int (*render)(struct DpMachine *, uint32_t *argb, unsigned cap, int *w, int *h);
    void (*observe)(struct DpMachine *, const DpObservers *, void *user);
    int (*ctx_components)(struct DpMachine *, const DpCtxComp **out);   // count
    int (*probes)(struct DpMachine *, const DpProbe **out);             // count
    // An independent machine of the same program for parallel workers (`worker` >= 1),
    // already initialized and at its power-on state; NULL when unsupported.
    struct DpMachine *(*clone)(struct DpMachine *, unsigned worker);
    // Optional: bind the machine to the calling thread.  The kernel calls it first in each worker
    // thread (never on worker 0, which already runs); a clone() may then return a cheap shell and
    // build the runtime here, so thread-local platform state belongs to the thread that uses it.
    // Returns 0 when the machine cannot be built: that worker retires, the others go on.
    int (*attach)(struct DpMachine *);
    void (*close)(struct DpMachine *);
};

struct DpMach;
// the GB backend (src/mach/gb_machine.c); takes ownership of the DpMach
DpMachine *dp_machine_gb(struct DpMach *impl);
const struct DpPlatformHooks *dp_machine_gb_hooks(void);
/* tell a GB machine where its ROM is so clone() can start parallel workers on other compiled instances */
void dp_machine_gb_source(DpMachine *m, const uint8_t *rom, size_t size);
struct DpTas;
/* the movie whose recorder sync profile (timing quirks, power-on RAM) every GB instance takes */
void dp_machine_gb_set_tas(DpMachine *m, const struct DpTas *tas);
const struct DpPlatformHooks *dp_machine_md_hooks(void);
/* tell an MD machine where its ROM is so clone() can start parallel workers */
void dp_machine_md_source(DpMachine *m, const uint8_t *rom, size_t size);

// the SG/SMS/GG/MSX backend (src/mach/sega_machine.c); borrows the singleton
// backend instance (exploration is sequential).  const void *: DpSegaBackend
// is a typedef of an anonymous struct, not forward-declarable here.
DpMachine *dp_machine_sega(const void *backend);
// Tell a Sega machine where its program comes from so clone() can start a
// worker instance: `pick` maps a worker number to its backend instance.
void dp_machine_sega_tas(DpMachine *m, int sys_key, int profile);   /* movie bit 6 meaning + SMSHawk timing profile (0 = off) */
void dp_machine_sega_init_arg(DpMachine *m, int arg);   /* backend init's last argument for clones (MSX mapper) */
void dp_machine_sega_source(DpMachine *m, const uint8_t *rom, size_t size, const void *(*pick)(unsigned worker));

struct DpMdMach;
// the NES backend (src/mach/nes_machine.c): opens the runtime from a
// cartridge image (mapper <0: auto) and owns it
size_t dp_machine_nes_prg_size(DpMachine *m);
const uint8_t *dp_machine_nes_prg(DpMachine *m);   // the PRG image (bank*0x4000 + offset layout)
struct DpPlatformHooks;
const struct DpPlatformHooks *dp_machine_nes_hooks(void);
const struct DpPlatformHooks *dp_machine_nes_hooks_full(int pad_full);   // pad_full: every port-1 mask at each strobe site (--pad-full)
void dp_machine_sega_set_msx(DpMachine *m, int msx);   /* marks an MSX machine (keyboard probes, no Pause NMI) */
const struct DpPlatformHooks *dp_machine_sega_hooks(void);   /* Z80 platform hooks (VDP status / counters probes) */   /* NES platform hooks for the kernel (event probes) */
DpMachine *dp_machine_nes(const uint8_t *rom, size_t size, int mapper, char *err, size_t errsz);
// the Mega Drive backend (src/mach/md_machine.c); takes ownership of the machine
DpMachine *dp_machine_md(struct DpMdMach *impl);
// the PC Engine backend (src/mach/pce_machine.c): wraps the live runtime
// singleton (dp_pce_init first)
DpMachine *dp_machine_pce(void);
/* give the PCE machine its ROM image (kept by the caller) so clone() can start workers */
void dp_machine_pce_source(DpMachine *m, const uint8_t *data, size_t size);
const struct DpPlatformHooks *dp_machine_pce_hooks(void);   /* IRQ-source probes */
// the libretro backend (src/mach/lr_machine.c), level A (§8.1): frames only,
// the kernel degrades; takes ownership of the DpLibretro instance
struct DpLibretro;
DpMachine *dp_machine_libretro(struct DpLibretro *lr);

#endif
