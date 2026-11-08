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
    int (*read)(void *ctx, uint16_t addr, uint8_t *v);
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
    uint16_t addr;    // DP_AM_ABS: the address; DP_AM_HIGH_IMM: $FF00 + n
} DpMemOp;

typedef struct DpIsa {
    const char *name;                      // "sm83", "z80", ...
    uint8_t ptr_width;                     // bytes of a code pointer
    uint8_t big_endian;
    // length of the instruction starting with `op`
    int (*length)(uint8_t op);
    // classify the instruction at `bytes` (at least `length` readable bytes)
    // located at CPU address `pc`
    int (*flow)(const uint8_t *bytes, uint16_t pc, DpFlowInfo *out);
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
} DpIsa;

const DpIsa *dp_isa_sm83(void);
const DpIsa *dp_isa_z80(void);

// normalised register view (pc/sp and the flag byte; regs are CPU specific)
typedef struct DpRegs {
    uint32_t pc, sp;
    uint16_t flags;
    uint16_t reg[8];
} DpRegs;

// observation hooks (spec §7.2 observe): every executed instruction and every
// CPU data access, as the engine sees them
typedef struct DpObservers {
    void (*on_step)(void *user, uint16_t pc, uint8_t op, int entry);
    void (*on_read)(void *user, uint16_t addr, uint8_t v);
    void (*on_write)(void *user, uint16_t addr, uint8_t v);
} DpObservers;

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
struct DpMachine {
    const char *name;                      // "gb", "sms", "nes", ...
    const DpPlatform *plat;
    const DpIsa *isa;
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
    void (*observe)(struct DpMachine *, const DpObservers *, void *user);
    int (*ctx_components)(struct DpMachine *, const DpCtxComp **out);   // count
    int (*probes)(struct DpMachine *, const DpProbe **out);             // count
    void (*close)(struct DpMachine *);
};

struct DpMach;
// the GB backend (src/mach/gb_machine.c); takes ownership of the DpMach
DpMachine *dp_machine_gb(struct DpMach *impl);

// the SG/SMS/GG/MSX backend (src/mach/sega_machine.c); borrows the singleton
// backend instance (exploration is sequential).  const void *: DpSegaBackend
// is a typedef of an anonymous struct, not forward-declarable here.
DpMachine *dp_machine_sega(const void *backend);

struct DpMdMach;
// the NES backend (src/mach/nes_machine.c): opens the runtime from a
// cartridge image (mapper <0: auto) and owns it
DpMachine *dp_machine_nes(const uint8_t *rom, size_t size, int mapper, char *err, size_t errsz);
// the Mega Drive backend (src/mach/md_machine.c); takes ownership of the machine
DpMachine *dp_machine_md(struct DpMdMach *impl);
// the PC Engine backend (src/mach/pce_machine.c): wraps the live runtime
// singleton (dp_pce_init first)
DpMachine *dp_machine_pce(void);
// the libretro backend (src/mach/lr_machine.c), level A (§8.1): frames only,
// the kernel degrades; takes ownership of the DpLibretro instance
struct DpLibretro;
DpMachine *dp_machine_libretro(struct DpLibretro *lr);

#endif
