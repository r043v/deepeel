// Z80 analysis uses the same decoder and executor as the m0boy runtime.
#ifndef DP_Z80_H
#define DP_Z80_H
#include <z80.h>
#include <stddef.h>

typedef enum { DP_Z80_NEXT, DP_Z80_JUMP, DP_Z80_CALL, DP_Z80_RETURN,
               DP_Z80_INDIRECT, DP_Z80_HALT, DP_Z80_REPEAT } DpZ80Flow;
typedef struct {
    z80_decoded_t raw;
    DpZ80Flow flow;
    uint16_t target;
    int condition; // -1 unconditional; 0..7 NZ/Z/NC/C/PO/PE/P/M; 8 DJNZ
} DpZ80Instruction;

// A bounded, side-effect-free byte view: no bus accesses, including IO, occur.
// Returns zero on truncation or a full address-space prefix chain.
int dp_z80_decode(const uint8_t *bytes, size_t size, uint16_t pc, DpZ80Instruction *out);
int dp_z80_condition(int condition, const z80_t *cpu);
// Change only the tested flag (or B for DJNZ) to choose the requested side.
int dp_z80_force(int condition, z80_t *cpu, int taken);
// ED repetition tests the post-decrement counter, and CPIR/CPDR also test
// the comparison with (HL). Source is a side-effect-free memory peek.
int dp_z80_repeat_taken(const DpZ80Instruction *in,const z80_t *cpu,uint8_t source);
int dp_z80_force_repeat(const DpZ80Instruction *in,z80_t *cpu,uint8_t source,int taken);
// Input port read by one instruction, including block IO, before execution.
int dp_z80_input_port(const z80_decoded_t *in,const z80_t *cpu,uint16_t *port);
void dp_z80_format(const DpZ80Instruction *in, char *out, size_t size);
#endif
