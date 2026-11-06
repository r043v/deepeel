/* Bridge to the m0boy GB machine (targets/meta/runtime/gbl_*), compiled as
 * prefixed instances: one per (variant, worker slot).  A variant fixes the
 * compile-time cartridge shape of the runtime (GBL_MBC, GBL_ERAM_SIZE); the
 * ROM itself is shared through dp_gbl_rom_p.  The engine drives an instance
 * through its DpGblApi and receives observations through the dp_gbl_hook_*
 * calls the generated forwarders make.  This file and the generated
 * instances file are the only contract between deepeel and the m0boy
 * runtime; see cmake/dp_gbl_build.cmake. */
#ifndef DP_GBL_API_H
#define DP_GBL_API_H

#include <stdint.h>

#include "gbl.h" /* m0boy runtime public API (gbl_cpu_t, sizes) */
#include "dp_gbl_counts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything deepeel may touch on one compiled instance. */
typedef struct DpGblApi {
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
    uint8_t *rom;        /* the instance's ROM copy, filled by dp_mach_set_rom */
    uint32_t rom_max;
    /* the instance's real bus write (mapper registers, seeding): bypasses
     * the interposed read/write hooks on purpose */
    void (*wr)(uint16_t a, uint8_t v);
    /* movie sync compatibility knobs (gbl.h): 0 = hardware behaviour */
    uint8_t *sync_div_skew, *sync_vba_off_clock;
    /* GBHawk profile (gbl.h): pad latch delay in cycles, extra DIV skew */
    uint32_t *sync_pad_delay;
    uint16_t *sync_div_extra;
    /* GBHawk profile (gbl.h): PPU line-153 phase at the $0100 reset */
    uint16_t *sync_ppu_phase;
    /* applies sync_ppu_phase to the loaded PPU state (call right after the
     * movie machine's power-on state is in, cyc still 0) */
    void (*sync_ppu_apply)(void);
    /* bank shown in the $0000-$3FFF window: 0 except on a large MBC1 in mode 1 */
    const uint16_t *rom0_bank;
    /* ROM size in 16 KiB banks (power of two): the mapper's bank mask */
    uint32_t *rom_banks;
    /* MBC1M multicart wiring flag */
    uint8_t *mbc1m;
    /* VBA echoRAMFix=0 movies: $E000-$FDFF is separate memory; set *sync_echo_old, then call sync_echo_apply */
    uint8_t *sync_echo_old;
    void (*sync_echo_apply)(void);
    /* VBA 1.7.2 cores (gbl.h): LY reads see the state this many cycles earlier */
    int16_t *sync_ly_lag;
    /* VBA 1.7.2 cores (gbl.h): cycles taken off the interrupt dispatch */
    int16_t *sync_irq_trim;
} DpGblApi;

/* the ROM bank behind a pc/address of the fixed-or-switchable windows */
static inline uint16_t dp_gbl_bank_of(const DpGblApi *a, uint16_t addr) {
    return addr < 0x4000 ? *a->rom0_bank : a->gbr->rom_bank;
}

extern const DpGblApi *const dp_gbl_inst[DP_GBL_VARIANTS][DP_GBL_MACHINES];
extern const uint8_t dp_gbl_variant_mbc[DP_GBL_VARIANTS];
extern const uint32_t dp_gbl_variant_eram[DP_GBL_VARIANTS];
const char *dp_gbl_variant_name(int v);

/* Hook implementations live in src/mach/m0boy.c; the generated forwarders
 * call them with the (variant, instance) coordinates.  The trace and vblank
 * hooks may longjmp (path cut). */
void dp_gbl_hook_coverage(int v, int i, uint16_t bank, uint16_t addr, int entry);
void dp_gbl_hook_trace(int v, int i, uint16_t pc);
void dp_gbl_hook_read(int v, int i, uint16_t a, uint8_t val);
void dp_gbl_hook_write(int v, int i, uint16_t a, uint8_t val);
uint16_t *dp_gbl_hook_line_begin(int v, int i, uint8_t ly);
void dp_gbl_hook_line_end(int v, int i, uint8_t ly);
void dp_gbl_hook_vblank(int v, int i);
void dp_gbl_hook_safe(int v, int i);

#ifdef __cplusplus
}
#endif

#endif
