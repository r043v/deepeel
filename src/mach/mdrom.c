/* Storage for the globals MdFull expects (md_full.h declares them const;
 * defined here, in a unit that does not include that header). */
#include <stdint.h>
#include <string.h>
#include "mdmach.h"

uint8_t md_rom[DP_MD_ROM_MAX];
uint32_t md_rom_size;

void dp_mdrom_set(const uint8_t *rom, size_t size) {
    memset(md_rom, 0, sizeof md_rom);
    memcpy(md_rom, rom, size);
    md_rom_size = (uint32_t)size;
}

uint32_t dp_mdrom_peek32(uint32_t a) {
    a &= 0xFFFFFFu;
    if (a + 4u > md_rom_size) return 0;
    return (uint32_t)md_rom[a] << 24 | (uint32_t)md_rom[a + 1] << 16 |
           (uint32_t)md_rom[a + 2] << 8 | md_rom[a + 3];
}

/* The native (generated-code) stepper is not built here: deepeel always runs
 * the interpreter so every instruction passes the trace hooks. */
struct MdFull;
void md_full_step_native(struct MdFull *m);
void md_full_step_interp(struct MdFull *m);
void md_full_step_native(struct MdFull *m) { md_full_step_interp(m); }
