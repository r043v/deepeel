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
} DpFlowInfo;

typedef struct DpIsa {
    const char *name;                      // "sm83", "z80", ...
    uint8_t ptr_width;                     // bytes of a code pointer
    uint8_t big_endian;
    // length of the instruction starting with `op`
    int (*length)(uint8_t op);
    // classify the instruction at `bytes` (at least `length` readable bytes)
    // located at CPU address `pc`
    int (*flow)(const uint8_t *bytes, uint16_t pc, DpFlowInfo *out);
    // textual form; returns the length or 0 when undefined
    int (*disasm)(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz);
} DpIsa;

const DpIsa *dp_isa_sm83(void);

// normalised register view (pc/sp and the flag byte; regs are CPU specific)
typedef struct DpRegs {
    uint32_t pc, sp;
    uint16_t flags;
    uint16_t reg[8];
} DpRegs;

typedef struct DpMachine {
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
    void (*close)(struct DpMachine *);
} DpMachine;

struct DpMach;
// the GB backend (src/mach/gb_machine.c); takes ownership of the DpMach
DpMachine *dp_machine_gb(struct DpMach *impl);

#endif
