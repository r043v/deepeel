// m6502.h: a self-contained 6502 core (the NES 2A03 CPU), in the same shape
// as the z80 core: a machine-facing executor driven by read/write callbacks,
// plus a bounded, side-effect-free analysis API the explorer drives (decode,
// branch condition tests and forcing, formatting).  Decimal mode is absent
// (the 2A03 has it disabled); undefined opcodes are jams, never guesses.
#ifndef DP_M6502_H
#define DP_M6502_H
#include <stddef.h>
#include <stdint.h>

// machine state: caller wires read/write bus callbacks (RAM, PPU, mappers)
typedef struct {
    uint8_t a, x, y, s, p; // p: NV-BDIZC, bit 5 always set
    uint16_t pc;
    unsigned cyc;          // cycle counter, machine-owned accumulation
    int jammed;            // an undefined opcode was executed
    uint8_t nmi, irq;      // level inputs, sampled by m6502_service
    uint8_t nmi_pending;   // edge-detected NMI latch
    uint8_t nmi_level;     // previous NMI level, for the edge detector
    int irq_disable_cycles;
    void *user;
    uint8_t (*read)(void *user, uint16_t addr);
    void (*write)(void *user, uint16_t addr, uint8_t value);
} m6502_t;

// flow classes the explorer reasons about
typedef enum {
    DP_M6502_NEXT,     // straight-line
    DP_M6502_JUMP,     // absolute absolute/indirect jump, target known
    DP_M6502_INDIRECT, // jmp ($xxxx) with a page-boundary alias risk, or jmp ()
    DP_M6502_CALL,     // jsr, target known
    DP_M6502_RETURN,   // rts / rti
    DP_M6502_BRANCH,   // conditional branch; condition below
    DP_M6502_JAM       // undefined opcode
} DpM6502Flow;

// branch conditions: tested flag and polarity, in a compact encoding
// 0..3: N V C Z clear; 4..7: N V C Z set
typedef enum {
    DP_M6502_CC_N=0, DP_M6502_CC_V, DP_M6502_CC_C, DP_M6502_CC_Z,
    DP_M6502_CS_N=4, DP_M6502_CS_V, DP_M6502_CS_C, DP_M6502_CS_Z
} DpM6502Cond;

typedef struct {
    uint8_t op;            // opcode byte
    uint8_t length;        // total bytes 1..3
    uint8_t addr_mode;     // see enum below
    uint8_t access;        // 0 read, 1 write, 2 read-modify-write, 3 none, 4 jump/call/return
    unsigned base_cycles;
    DpM6502Flow flow;
    uint16_t target;       // JUMP/CALL/BRANCH target
    int8_t condition;      // BRANCH: DP_M6502_CC_*/CS_*; -1 otherwise
} DpM6502Instruction;

// addressing modes (analysis only; the executor has its own switch)
enum {
    DP_M6502_IMP=0, DP_M6502_IMM, DP_M6502_ZP, DP_M6502_ZPX, DP_M6502_ZPY,
    DP_M6502_ABS, DP_M6502_ABSX, DP_M6502_ABSY, DP_M6502_INDX, DP_M6502_INDY,
    DP_M6502_IND, DP_M6502_REL
};

// bounded decode of one instruction from a byte view: never touches the bus
// (including zero-page fetches), so indirect and indexed modes yield a
// target of 0 with flow NEXT-side semantics kept by the caller.  Returns 0
// when `size` cannot cover the full instruction.
int dp_m6502_decode(const uint8_t *bytes, size_t size, uint16_t pc, DpM6502Instruction *out);
// true when the branch would be taken with this flag state
int dp_m6502_condition(int condition, const m6502_t *cpu);
// flip only the tested flag so the branch takes the requested side; returns
// the previous flag value so callers can restore exact state
int dp_m6502_force(int condition, m6502_t *cpu, int taken);
// mnemonic + operand formatting for reports and the GUI detail view
void dp_m6502_format(const DpM6502Instruction *in, uint8_t lo, uint8_t hi, char *out, size_t size);

// --- machine-facing API ----------------------------------------------------
void m6502_reset(m6502_t *cpu);                 // read the reset vector
// service pending interrupt lines and dispatch: returns cycles consumed
// (7 on dispatch, 2 on poll).  NMI is edge-latched, IRQ is level, both
// masked by I during polling.
unsigned m6502_service(m6502_t *cpu);
// execute one instruction; returns cycles consumed, 0 on jam (jammed=1)
unsigned m6502_step(m6502_t *cpu);
#endif
