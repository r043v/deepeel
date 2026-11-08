// registry.c: the SMS/GG/SG port tables as native DpIoReg entries (the
// GB/NES/PCE/MD model), converted from the DpSegaPort metadata at first
// use.  Category and impl flags are decided by register name — the tables
// above stay the single source of truth for addresses, decode masks and
// direction.  The explorer's own port counters (io_read/io_write per byte)
// are the liveness evidence; this registry names what each count belongs to.
#include "sega.h"
#include "../platform.h"
#include "../msx/explore.h" // dp_msx_system: the MSX port table
#include <stdlib.h>
#include <string.h>

static void port_impl(const char *name, uint8_t rw, const char **cat, uint16_t *impl) {
    *impl = 0;
    if (!strncmp(name, "VDP_STATUS", 10)) { *cat = "video"; *impl = DP_IMPL_IRQ | DP_IMPL_EVENT; return; }
    if (!strcmp(name, "V_COUNTER") || !strcmp(name, "H_COUNTER")) { *cat = "video"; *impl = DP_IMPL_EVENT; return; }
    if (!strncmp(name, "VDP", 3)) { *cat = "video"; *impl = DP_IMPL_DMA_SRC; return; }
    if (!strncmp(name, "SN76489", 7) || !strncmp(name, "YM2413", 6)) { *cat = "sound"; return; }
    if (!strncmp(name, "GG_STEREO", 9)) { *cat = "sound"; return; }
    if (!strncmp(name, "JOYPAD", 6) || !strcmp(name, "GG_START")) { *cat = "input"; *impl = DP_IMPL_EVENT; return; }
    if (!strncmp(name, "GG_SERIAL", 9)) { *cat = "serial"; *impl = DP_IMPL_IRQ; return; }
    if (!strncmp(name, "GG_PARALLEL", 11)) { *cat = "serial"; return; }
    if (!strncmp(name, "MEMORY_CONTROL", 14)) { *cat = "mapper"; return; }
    if (!strncmp(name, "IO_CONTROL", 10) || !strncmp(name, "AUDIO_CONTROL", 13)) { *cat = "sys"; return; }
    *cat = "sys"; // anything new lands observable, never silent
    (void)rw;
}

const DpPlatform *dp_platform_sega(const DpSegaSystem *s) {
    if (!s) return NULL;
    static DpPlatform plat[4]; // sg, sms, gg, msx
    static DpIoReg regs[4][32];
    int si = !strcmp(s->id, "sms") ? 1 : !strcmp(s->id, "gg") ? 2
           : !strcmp(s->id, "msx") ? 3 : 0;
    DpPlatform *p = &plat[si];
    if (p->name) return p;
    int n = 0;
    for (unsigned i = 0; i < s->port_count && n < 32; i++) {
        const DpSegaPort *sp = &s->ports[i];
        const char *cat;
        uint16_t impl;
        port_impl(sp->name, sp->rw, &cat, &impl);
        regs[si][n] = (DpIoReg){ sp->value, sp->value, sp->name, cat, sp->rw, impl };
        n++;
    }
    static const DpIrq sega_irqs[2] = { { 0, 0x0038, "INT (frame/line)" }, { 0, 0x0066, "NMI (pause)" } };
    static const DpRegion sega_regions[3] = {
        { 0x0000, 0x7FFF, "cartridge slots 0/1", DP_SPACE_ROM },
        { 0x8000, 0xBFFF, "cartridge slot 2", DP_SPACE_ROM },
        { 0xC000, 0xFFFF, "work RAM (mirrored)", DP_SPACE_WRAM },
    };
    p->name = s->name;
    p->io_regs = regs[si];
    p->io_reg_count = n;
    p->irqs = sega_irqs;
    p->irq_count = 2;
    p->regions = sega_regions;
    p->region_count = 3;
    p->entries = NULL;
    p->entry_count = 0;
    return p;
}

const DpPlatform *dp_platform_msx(void) {
    // the MSX1 port table as native DpIoReg entries: same conversion, the
    // msx DpSegaPort table stays the single source of truth
    return dp_platform_sega(dp_msx_system());
}
