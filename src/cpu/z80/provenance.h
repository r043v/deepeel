#ifndef DP_Z80_PROVENANCE_H
#define DP_Z80_PROVENANCE_H
#include "z80.h"
// Zero means unknown; otherwise physical ROM byte index + 1. Arithmetic and
// bit operations invalidate origin even when their numeric result is unchanged.
typedef uint32_t DpZ80Origin;
enum {
    DP_Z80_TAG_B, DP_Z80_TAG_C, DP_Z80_TAG_D, DP_Z80_TAG_E,
    DP_Z80_TAG_H, DP_Z80_TAG_L, DP_Z80_TAG_UNUSED, DP_Z80_TAG_A,
    DP_Z80_TAG_F, DP_Z80_TAG_IXH, DP_Z80_TAG_IXL, DP_Z80_TAG_IYH,
    DP_Z80_TAG_IYL, DP_Z80_TAG_SPH, DP_Z80_TAG_SPL, DP_Z80_TAG_I,
    DP_Z80_TAG_B2, DP_Z80_TAG_C2, DP_Z80_TAG_D2, DP_Z80_TAG_E2,
    DP_Z80_TAG_H2, DP_Z80_TAG_L2, DP_Z80_TAG_A2, DP_Z80_TAG_F2,
    DP_Z80_TAG_COUNT
};
typedef struct {
    DpZ80Origin reg[DP_Z80_TAG_COUNT];
    struct { uint16_t address; DpZ80Origin origin; } writes[2];
    unsigned write_count,write_at;
    DpZ80Origin output;
} DpZ80Provenance;
typedef DpZ80Origin (*DpZ80OriginRead)(void *,uint16_t);
// Clear the transient write plan before interrupt service or an instruction.
void dp_z80_provenance_clear_plan(DpZ80Provenance *p);
// Compute pure transfers from the pre-instruction registers and memory tags.
// The executor remains authoritative for values, timing and bus effects.
void dp_z80_provenance_begin(DpZ80Provenance *p,const z80_t *cpu,
                             const z80_decoded_t *d,DpZ80OriginRead read,void *user);
DpZ80Origin dp_z80_provenance_write(DpZ80Provenance *p,uint16_t address);
void dp_z80_provenance_force(DpZ80Provenance *p,const DpZ80Instruction *in,
                             const z80_t *before,const z80_t *after);
void dp_z80_provenance_stack_changed(DpZ80Provenance *p);
#endif
