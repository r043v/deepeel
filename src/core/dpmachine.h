// dpmachine.h: the generic machine and ISA interfaces of the exploration
// kernel (spec §7.2, §7.3).  A platform supplies a DpMachine (execution,
// state, memory geometry) and a DpIsa (static decoding); the kernel modules
// consume only these.  Every member is optional: the kernel degrades and
// reports what is missing.
#ifndef DP_DPMACHINE_H
#define DP_DPMACHINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
    uint8_t pair;      // DP_FLOW_INDIRECT / register-indirect CALL: the DpRegState register holding
                       // the target (per-ISA indices below), DP_REG_PAIR_NONE when memory-based
} DpFlowInfo;

#define DP_REG_PAIR_NONE 255

// -- DpRegState: the constant-transfer contract (C3, the generic port of the
// Sega closure's register tracking).  One slot per register the ISA names
// (indices below); for every slot `known` has a bit per value byte: 1 = the
// byte is a proven constant of `v`.  A byte never proven stays unknown and is
// never guessed (the Sega rule); the kernel treats the whole state as opaque
// (hash + equality), only the ISA and the machine interpret the indices.
#define DP_REG_N 24
#define DP_REG_MAX_BYTES 4
typedef struct DpRegState {
    uint32_t v[DP_REG_N];
    uint8_t known[DP_REG_N];   // per byte: 1 = proven constant
} DpRegState;

static inline void dp_regstate_clear(DpRegState *s) { memset(s, 0, sizeof *s); }

// register indices per ISA: dp_isa_*.reg_effect and the platform's get_regs
// agree on them.  Byte registers occupy byte 0 of their slot; pairs occupy
// bytes 0..1 (low byte first) of their slot; the 68000 uses bytes 0..3 (its
// pointers are 24-bit: the kernel masks the value with ptr_width bytes).
enum { DP_ARMR_R0 = 0, DP_ARMR_SP = 13, DP_ARMR_LR = 14, DP_ARMR_PC = 15, DP_ARMR_NREGS = 16 };
#define DP_ARM_THUMB 0x40000000u   // code addresses of the ARM ISA: this bit of `pc` marks the Thumb state
enum { DP_SM83R_BC, DP_SM83R_DE, DP_SM83R_HL, DP_SM83R_SP, DP_SM83R_AF, DP_SM83R_A, DP_SM83R_NREGS = DP_SM83R_A + 1 };
enum {
    DP_Z80R_BC, DP_Z80R_DE, DP_Z80R_HL, DP_Z80R_IX, DP_Z80R_IY, DP_Z80R_SP,
    DP_Z80R_A, DP_Z80R_F, DP_Z80R_B, DP_Z80R_C, DP_Z80R_D, DP_Z80R_E, DP_Z80R_H, DP_Z80R_L,
    DP_Z80R_BC2, DP_Z80R_DE2, DP_Z80R_HL2, DP_Z80R_AF2, DP_Z80R_NREGS = DP_Z80R_AF2 + 1
};
enum { DP_M6502R_A, DP_M6502R_X, DP_M6502R_Y, DP_M6502R_S, DP_M6502R_NREGS = DP_M6502R_S + 1 };
enum { DP_HUCR_A, DP_HUCR_X, DP_HUCR_Y, DP_HUCR_S, DP_HUCR_MPR0,
       DP_HUCR_MPR7 = DP_HUCR_MPR0 + 7, DP_HUCR_NREGS = DP_HUCR_MPR7 + 1 };
enum { DP_M68KR_D0, DP_M68KR_D7 = DP_M68KR_D0 + 7, DP_M68KR_A0 = 8, DP_M68KR_A7 = 15,
       DP_M68KR_NREGS = DP_M68KR_A7 + 1 };

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

typedef struct DpMachine DpMachine;   // the machine hooks take and return it (declared in full below)

// A code table: `stride` bytes per entry, `rel` = signed offsets from the base (else absolute pointers).  base_reg >= 0:
// the base is the value of that register slot plus `disp`, read from the concrete CPU (or a walk's proven constants) -- the
// dispatch routine shared by several callers, each passing its own table in a register (the table is the caller's `lea`).
typedef struct DpCodeTable { uint32_t base; uint8_t stride, rel; int8_t base_reg; int32_t disp; uint8_t slots; uint32_t hi; int8_t tdelta; uint16_t count; } DpCodeTable;   // hi: bits OR-ed into a 16-bit absolute entry (the dispatcher seeded the register's high word: moveq #1,Dn ; swap Dn)
//   // slots: the entries are instructions (bra.w/bra.s/jmp) of `stride` bytes, each slot itself a root

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
    // C3: the constant-transfer function of the instruction at `bytes`: a
    // register written with a proven value (immediate, copy of a known byte,
    // deterministic arithmetic, proven ROM operand) keeps or gains its known
    // bytes; a written register with no proven value loses them; unwritten
    // registers pass through.  `rom` proves constant reads from the ROM (the
    // caller records the evidence).  In and out may not alias.  NULL: this ISA
    // has no constant tracking (the kernel leaves the state untouched).
    void (*reg_effect)(const uint8_t *bytes, uint32_t pc, const DpRegState *in, DpRegState *out,
                       const DpProvRom *rom);
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
    int (*acc_imm)(const uint8_t *bytes, uint8_t *v);      // C3: 1 when the instruction loads the accumulator with an immediate
    int (*st_acc_abs)(const uint8_t *bytes, uint32_t *addr);   // C3: 1 when the instruction stores the accumulator at an absolute address
    // C3: the stack-pointer register slot and the bytes a push/pop/call/ret
    // moves it by.  When the walk carries a fully known SP, a `ret` follows the
    // address read from the machine's stack (the Sega closure's concrete-stack
    // returns).  `sp_addr_mask` is the part of the register that indexes memory
    // (6502 S is 8 bits) and `sp_base` the stack page ORed on top ($0100);
    // both default to the z80 case (16-bit flat).  sp_width 0: this ISA has no
    // modeled stack (the feature is off)
    unsigned sp_slot;
    unsigned sp_width;
    uint32_t sp_addr_mask;
    uint32_t sp_base;
    // "zero data marked code" point 3: 1 when the byte window decodes like
    // data (repeated single-byte calls, accumulator-store soup, undocumented
    // no-op prefixes — never the head of real code).  The kernel refuses an
    // untrusted walk entry there.  NULL: this ISA has no data signature.
    int (*entry_data_sig)(const uint8_t *w, unsigned n);
    // C3: concrete-stack returns, per return opcode.  The instruction pulls
    // `width` bytes starting `off` bytes above the stack pointer (6502 pushes
    // pre-decrement: the return word sits at S+1..S+2; z80/68000 read at SP),
    // adds `bias` to the word (6502 `rts` pops the address minus one), and the
    // stack pointer bumps by `width`.  The stack is read in the ISA's own
    // endianness.  0 returned: this opcode is not modeled (no concrete-stack
    // continuation).  NULL: the walk falls back to `sp_width` for every
    // return (z80, sm83).
    int (*sp_ret)(const uint8_t *bytes, unsigned *width, unsigned *bias, unsigned *off);
    // C3: 1 when the instruction is an OUT; `port_known` = the port expression is an
    // immediate (`out (n),a`), otherwise it rides a register pair (`out (c),a`).  NULL:
    // this ISA has no ports.
    int (*out_port)(const uint8_t *bytes, int *port_known, uint16_t *port);
    // M1 (P8): direct RAM load into the accumulator/index register (techniques 3, 4, 7).
    // Returns the instruction length, fills the target address, whether the addressing
    // mode is indexed (abs,X: the kernel skips the immediate-compare signature then),
    // the width in bytes and the destination data register (wide ISAs), else 0.
    // NULL: this ISA has no direct RAM load.
    int (*ram_load)(const uint8_t *bytes, uint32_t *addr, int *idx_reg, int *width, int *dreg);
    // M1: opcode pairing the byte loaded by `ram_load` with an immediate compare
    // (6502/huc6280 CMP #, z80/sm83 CP n): one signature site (techniques 3+4).
    // 0: none, or the ISA pairs its loads through `ram_signature_wide` instead (68000).
    uint8_t cmp_imm;
    // M1: wide-ISAs extension of the signature pairing: a wide load followed by an
    // immediate compare of the loaded register, or a standalone compare of an absolute
    // address with an immediate.  `b` points at the load instruction bytes (`dl_len`
    // bytes), `f` at the 10 bytes following it.  Fills address, value and width;
    // returns 1 on a signature.  NULL: none.
    int (*ram_signature_wide)(struct DpMachine *m, uint32_t pc, const uint8_t *b, const uint8_t *f,
                              int dl_len, int dl_width, int dl_dreg, uint32_t dl_addr,
                              uint32_t *addr, uint32_t *val, int *width);
    // M1: proven length (in entries) of the word/long table whose base load follows
    // the RAM load within 16 bytes (technique 7).  NULL: none.
    unsigned (*table_proven)(const struct DpIsa *isa, struct DpMachine *m, uint32_t pc, int len);
    // M1: 1 when a ROM table access indexed by the loaded value follows within 16
    // bytes (technique 7).  NULL: none.
    int (*rom_table_follows)(const struct DpIsa *isa, struct DpMachine *m, uint32_t pc, int len);
    // B4: bytes of one table entry (2: a word table everywhere but the 68000, 4: its long tables);
    // the index replay scales the proven length by it.  0: 2 assumed.
    uint8_t table_entry_width;
    // Classify an indirect dispatch: 1 = register table (parameter selects
    // the register view), 2 = absolute pointer table (parameter is its base).
    int (*table_dispatch)(const uint8_t *bytes, uint32_t *parameter);
    // PC-relative code tables (68000): at an indirect jmp/jsr, the table it indexes.  rel = entries are
    // signed 16-bit offsets from the table base; else absolute 32-bit pointers.  *load_pc: address of the
    // instruction that loaded the table address (0: the jump itself), checked against the executed trail.
    int (*code_table)(struct DpMachine *m, uint32_t pc, const uint8_t *bytes, DpCodeTable *out, uint32_t *load_pc);
    // B10: 1 when the instruction stores an immediate code address in memory (RAM function-pointer slot, object
    // routine field: `move.l #routine,x(a0)`); the closure walks the address as an untrusted entry
    int (*code_ptr_store)(const uint8_t *bytes, uint32_t *imm);
    // ISAs with several instruction sets: the bits of `pc` that carry the decoder state (ARM: the Thumb tag).  They travel
    // with every code address (DpMachine.fetch / rom_index ignore them) and the kernel records them per instruction head
    // (DpCoreResult.isa_mode), so the static passes decode each byte the way it ran.  0: one instruction set.
    uint32_t mode_mask;
    // little-endian ISAs whose normal instruction stream looks like a table of 0x4000-0x7FFF words: 1 turns that
    // "banked pointer table" data guard off
    uint8_t no_ptr16_guard;
    // the code address of a register-indirect transfer: `v` is the proven register value (ARM interworking: bit 0
    // selects Thumb).  NULL: the value is the address.
    uint32_t (*reg_target)(const uint8_t *bytes, uint32_t pc, uint32_t v);
    // static analysis of the whole image: candidate routine heads (prologues, pointer words) as code addresses, each
    // walked as an untrusted entry (provisional, never "executed").  `base` is the CPU address of rom[0].  Returns the
    // number written to `out` (at most `max`).
    size_t (*static_roots)(const uint8_t *rom, size_t n, uint32_t base, uint32_t *out, size_t max);
} DpIsa;

const DpIsa *dp_isa_sm83(void);
const DpIsa *dp_isa_arm920t(void);
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
    // CPU port I/O (Z80 IN/OUT): `port` is the 8-bit port, `v` the value read or written
    void (*on_io)(void *user, uint16_t port, uint8_t v, int write);
    // optional: a token naming the observer's current mode (e.g. forced vs natural); a machine may skip re-observing
    // an identical block transfer only while the token is unchanged
    uint32_t (*mode)(void *user);
} DpObservers;
enum { DP_COPY_DMA = 1, DP_COPY_GB_VRAM = 2, DP_COPY_GB_OAM = 3 };

// a component of the incoming state a section's behaviour may depend on
// (spec §7.2 ctx_components): the bit index is the platform's DP_CTX_* value
enum { DP_CC_REG = 0, DP_CC_PAGES };
typedef struct DpCtxComp {
    const char *name;
    uint8_t kind;      // DP_CC_REG (one bit of ctx_mask) or DP_CC_PAGES (256-byte pages)
    int bit;           // ctx_mask bit for DP_CC_REG, -1 otherwise
    uint32_t bytes;    // size of the component
} DpCtxComp;


// --pad-full: the k-th subset of the buttons in `bits` (k deposited into the set bit positions); the count is 1 << popcount
static inline uint32_t dp_pad_subset(uint32_t bits, uint32_t k) {
    uint32_t r = 0;
    for (uint32_t b = bits; b && k; b &= b - 1, k >>= 1) if (k & 1) r |= b & -b;
    return r;
}
static inline uint32_t dp_pad_count(uint32_t bits) { return 1u << __builtin_popcount(bits); }

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
    uint16_t bank_window;                  // P10: CPU base address of the switchable 16 KiB window (GB/Sega/PCE $4000, NES $8000); 0: unknown (the cov offsets of banks > 0 stay image-relative)
    uint8_t fixed_low_banked;              // 1: pc < one bank is bank 0 and the next window holds the code's own bank (GB): static proof follows absolute targets
    uint8_t fixed_high;                    // 1 = the high window is the fixed (last) bank and the low one switches (NES); 0 = the reverse (GB)
    uint32_t save_ram_en_addr; uint8_t save_ram_en_val, save_ram_en;   // B10: write that enables the cart RAM before seeding
    uint32_t save_ram_base, save_ram_len;  // B10: battery/cart RAM window the game may validate at boot (0 len: none)
    uint32_t work_ram_base, work_ram_len;  // A25: the CPU-visible work RAM range (0 len: unknown, defaults to $C000/$2000)
    // Optional DSY1 frame writer for machines with comparable state beyond the CPU WRAM window.
    void (*dump_dsy)(struct DpMachine *, FILE *, uint32_t frame);
    // C13: dense index of the CPU-addressable RAM the core keys its per-address tables on (ram_index -1: not RAM).
    // Absent: the low 16 address bits (every 16-bit CPU).  ram_addr is the inverse, ram_size the index count.
    int64_t (*ram_index)(struct DpMachine *, uint32_t addr);
    uint32_t (*ram_addr)(struct DpMachine *, uint32_t idx);
    // Optional path-local verbatim ROM provenance, saved with machine state.
    // Values are compact ROM indices plus one; zero means unknown.
    uint32_t (*ram_source)(struct DpMachine *, uint32_t idx);
    void (*ram_source_set)(struct DpMachine *, uint32_t idx, uint32_t source);
    // Optional fast path: pointer to the first ram_view_len RAM indices as contiguous bytes (same values as fetch(ram_addr(i))), so the
    // exploration core hashes RAM without one indirect call per byte.
    const uint8_t *(*ram_view)(struct DpMachine *);
    uint32_t ram_view_len;
    uint32_t ram_size;
    // pad probes: the buttons the pad has (DP_IN_* of port 1) and the core's --pad-full level (0: the platform catalogue,
    // 1: every subset of input_bits, 2: also the extended buttons in input_bits_ext); set by the core before each probe
    uint32_t input_bits, input_bits_ext; unsigned pad_full;
    void *impl;                            // backend instance
    // state images
    uint32_t (*state_bytes)(struct DpMachine *);
    void (*save)(struct DpMachine *, uint8_t *buf);
    void (*load)(struct DpMachine *, const uint8_t *buf);
    // optional: portable snapshots, a PNG of the screen carrying the whole machine.  When present, snapshot files are
    // written in that form and --from-state accepts them.  Both return 1 on success; a failed load changes nothing.
    int (*shot_save)(struct DpMachine *, const char *path);
    int (*shot_load)(struct DpMachine *, const char *path);
    // memory geometry
    int64_t (*rom_index)(struct DpMachine *, uint32_t addr);  // <0 when not ROM
    uint8_t (*fetch)(struct DpMachine *, uint32_t addr);      // opcode view
    unsigned (*vector_seeds)(struct DpMachine *, uint32_t *pcs, unsigned max);   // optional: handler addresses of a wide-address vector table (68000 exception vectors), seeded as forced starts
    void (*fetch_n)(struct DpMachine *, uint32_t addr, uint8_t *out, unsigned n);   // optional: n consecutive fetch() bytes in one call (the per-instruction window)
    // execution (NULL when the backend only advances by frames)
    int (*run)(struct DpMachine *);
    // program counter access for the generic explorer (core.c): the forced
    // side of a conditional is taken by moving pc on a restored state
    uint32_t (*pc)(struct DpMachine *);
    void (*set_pc)(struct DpMachine *, uint32_t pc);
    // Invert the flag tested by a conditional instruction before executing
    // it. Returns 0 when that condition cannot be inverted by this backend.
    int (*invert_condition)(struct DpMachine *, int condition);
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
    uint8_t rom_top_win;  // how many 16 KiB windows, counted from $0000, the mapper can fill with ROM (jp-table target bound; 0: unknown, 2 assumed)
    // Optional data-destination knowledge (the shared DP_DEST_* model in
    // coverage.h): where a write to `addr` lands in the hardware (VRAM, OAM,
    // palette, sound registers...).  0 = untyped.  The kernel records the
    // destination of the data each ROM byte fed; NULL: no typing here.
    uint8_t (*data_dest)(struct DpMachine *, uint32_t addr);
    // writes one byte through the machine's memory map, unobserved (NULL: unsupported);
    // the entry point of the RAM-seeding techniques (docs/PLAN-MUTUALISATION.fr.md, step 3)
    void (*poke)(struct DpMachine *, uint32_t addr, uint8_t value);
    // C3: 1 when a write to `addr` programs the cartridge mapper although the address is not in the ROM window (Sega $FFFC-$FFFF);
    // NULL: only writes into the ROM window count
    int (*mapper_reg)(struct DpMachine *, uint32_t addr);
    // C3: 1 when an OUT to this port is a memory-control write (a mapper/slot decoder can
    // hide the cartridge ROM and the stack RAM): the static walk then discards its concrete
    // SP and its ghost-return pile (the Sega closure's port_effect).  `port_known` = the
    // port expression is a known constant.  NULL: no port knowledge (never memory-control).
    int (*port_control)(const struct DpMachine *, uint16_t port, int port_known);
    // holds a pad mask (bits 0..5 = up/down/left/right/button1/button2) until changed;
    // NULL: the machine has no input model.  Entry point of the kernel's held-pad sessions.
    void (*set_input)(struct DpMachine *, uint32_t mask);
    // the masks the explorer's play and random sessions press, when the console's keys differ from the default
    // catalogue (B1, Start, B2, the four directions, Select, C); NULL: the default.  Idle (0) comes first.
    const uint32_t *play_pads; unsigned n_play_pads;
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
    /* optional: takes the stereo 44.1 kHz int16 frames produced since the last call (at most max_frames); returns the frame count (watch sound) */
    unsigned (*audio_read)(struct DpMachine *, int16_t *stereo, unsigned max_frames);
    void (*observe)(struct DpMachine *, const DpObservers *, void *user);
    int (*ctx_components)(struct DpMachine *, const DpCtxComp **out);   // count
    // C11: hash of the DP_CC_REG components (CPU registers, mapper state; no clocks, no RAM).  With the RAM pages a section reads
    // (DpCoreResult.live) it is the context projection the kernel evicts on.  NULL: no projection on this machine (cell key only).
    uint64_t (*ctx_reg_hash)(struct DpMachine *);
    // C3: the concrete CPU registers of the machine *right now* (the static
    // closure's bootstrapping, generic: the executed step seeds a walk whose
    // state is the live register state, per the ISA's DpRegState indices,
    // every byte known).  Returns 0 when the CPU is in no meaningful state.
    // NULL: no concrete seeding on this machine (the walk stays static-only).
    int (*get_regs)(struct DpMachine *, DpRegState *out);
   // P15/M5: the DpProbe catalogue is removed (nothing consumed it since 0.368): the pad
   // probes (--pad-full over DpMachine.input_bits) and the event-probe hooks supersede it
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
void dp_machine_sega_mapper(DpMachine *m, unsigned mapper);   /* explicit SMS/GG mapper (DP_SEGA_MAPPER_*), applied after every init */
void dp_machine_sega_set_msx(DpMachine *m, int msx);   /* marks an MSX machine (keyboard probes, no Pause NMI) */
void dp_machine_sega_gg(DpMachine *m, int gg);   /* Game Gear: the memory-control port is $3E */
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
// the Neo Geo Pocket backend (src/mach/ngp_machine.c): dp_ngp_init(data,size)
// first (src/platform/ngp/ngpmach.h), the thread-local runtime singleton
const DpIsa *dp_isa_tlcs900(void);
const DpIsa *dp_isa_v30mz(void);
/* the WonderSwan backend (src/mach/ws_machine.c): dp_ws_init(data,size) first
 * (src/platform/ws/wsmach.h), one heap Ws per machine — clones are trivial */
DpMachine *dp_machine_ws(void);
void dp_machine_ws_source(DpMachine *m, const uint8_t *data, size_t size);
const struct DpPlatformHooks *dp_machine_ws_hooks(void);   /* pad + IRQ-source probes */
DpMachine *dp_machine_ngp(void);
void dp_machine_ngp_source(DpMachine *m, const uint8_t *data, size_t size);
const struct DpPlatformHooks *dp_machine_ngp_hooks(void);   /* pad + IRQ-source probes */
// the libretro backend (src/mach/lr_machine.c), level A (§8.1): frames only,
// the kernel degrades; takes ownership of the DpLibretro instance
struct DpLibretro;
DpMachine *dp_machine_libretro(struct DpLibretro *lr);

// -- GP32 (S3C2400 / ARM920T, the m0boy runtime) -------------------------------
enum { DP_GP32_IMAGE_AUTO = 0, DP_GP32_IMAGE_PAYLOAD, DP_GP32_IMAGE_SNAPSHOT };
typedef struct DpGp32Options {
    const char *exec_path;        // .fxe / .gxb / .zpk application
    const char *bios_path;        // NULL: HLE firmware
    const char *card_path;        // SmartMedia image or directory; NULL: what the package carries
    const char *state_dir;        // holds the page store; NULL: memory
    int card_readonly;
    int image;                    // DP_GP32_IMAGE_*
    unsigned probe_frames;        // 0: default
    unsigned irq_cap;             // interrupt forks per source (0: no cap)
    unsigned cpi;                 // FCLK cycles per instruction: sets how many instructions a frame holds (0: 16)
} DpGp32Options;
DpMachine *dp_machine_gp32(const DpGp32Options *o);                 // NULL: the reason was printed
const uint8_t *dp_gp32_image(DpMachine *m, size_t *size);           // what rom_index indexes (owned by the machine)
void dp_gp32_report(DpMachine *m, FILE *out);
// registers the image refers to, against the ones the exploration touched
void dp_gp32_report_refs(DpMachine *m, FILE *out, const uint64_t *reads, const uint64_t *writes);
int64_t dp_gp32_bios_offset(DpMachine *m);                           // offset of the BIOS in the image, -1: HLE
const struct DpPlatformHooks *dp_machine_gp32_hooks(void);
const struct DpPlatform *dp_machine_gp32_platform(void);

#endif
