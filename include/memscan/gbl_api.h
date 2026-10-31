/* Bridge to the m0boy GB machine (targets/meta/runtime/gbl_*), compiled as
 * prefixed instances: one per (variant, worker slot).  A variant fixes the
 * compile-time cartridge shape of the runtime (GBL_MBC, GBL_ERAM_SIZE); the
 * ROM itself is shared through ms_gbl_rom_p.  The engine drives an instance
 * through its MsGblApi and receives observations through the ms_gbl_hook_*
 * calls the generated forwarders make.  This file and the generated
 * instances file are the only contract between memscan and the m0boy
 * runtime; see cmake/ms_gbl_build.cmake. */
#ifndef MS_GBL_API_H
#define MS_GBL_API_H

#include <stdint.h>

#include "gbl.h" /* m0boy runtime public API (gbl_cpu_t, sizes) */
#include "ms_gbl_counts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything memscan may touch on one compiled instance. */
typedef struct MsGblApi {
    int variant; /* index in the variant table */
    void (*reset)(void);
    void (*run)(void); /* never returns: hooks longjmp out */
    void (*set_buttons)(uint8_t pressed);
    uint32_t (*state_bytes)(void);
    void (*state_io)(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
    gbl_cpu_t *gbr;
    uint8_t *io, *ie, *wram, *vram, *oam, *hram, *joyp, *eram;
    const uint8_t *const *rmap; /* gbl_rmap: rmap[a >> 12] + a, NULL = slow */
    uint32_t *frame, *interp_instrs;
    uint8_t *interp_only, *skip_render;
    uint8_t *rom;        /* the instance's ROM copy, filled by ms_mach_set_rom */
    uint32_t rom_max;
    /* the instance's real bus write (mapper registers, seeding): bypasses
     * the interposed read/write hooks on purpose */
    void (*wr)(uint16_t a, uint8_t v);
} MsGblApi;

extern const MsGblApi *const ms_gbl_inst[MS_GBL_VARIANTS][MS_GBL_MACHINES];
extern const uint8_t ms_gbl_variant_mbc[MS_GBL_VARIANTS];
extern const uint32_t ms_gbl_variant_eram[MS_GBL_VARIANTS];
const char *ms_gbl_variant_name(int v);

/* Hook implementations live in src/mach/m0boy.c; the generated forwarders
 * call them with the (variant, instance) coordinates.  The trace and vblank
 * hooks may longjmp (path cut). */
void ms_gbl_hook_coverage(int v, int i, uint16_t bank, uint16_t addr, int entry);
void ms_gbl_hook_trace(int v, int i, uint16_t pc);
void ms_gbl_hook_read(int v, int i, uint16_t a, uint8_t val);
void ms_gbl_hook_write(int v, int i, uint16_t a, uint8_t val);
uint16_t *ms_gbl_hook_line_begin(int v, int i, uint8_t ly);
void ms_gbl_hook_line_end(int v, int i, uint8_t ly);
void ms_gbl_hook_vblank(int v, int i);
void ms_gbl_hook_safe(int v, int i);

#ifdef __cplusplus
}
#endif

#endif
