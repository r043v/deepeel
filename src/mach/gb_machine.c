// gb_machine.c: the m0boy runtime as a DpMachine (spec §7.2).  The explorer
// still drives DpMach directly; this wrapper is the kernel-facing view, built
// on the same instance so both see the same state.
#include <stdlib.h>

#include "../core/dpmachine.h"
#include "m0boy.h"

static uint32_t gb_state_bytes(DpMachine *m) { return dp_mach_state_bytes(m->impl); }
static void gb_save(DpMachine *m, uint8_t *buf) { dp_mach_save(m->impl, buf); }
static void gb_load(DpMachine *m, const uint8_t *buf) { dp_mach_load(m->impl, buf); }
static uint8_t gb_fetch(DpMachine *m, uint32_t a) { return dp_mach_op(m->impl, (uint16_t)a); }
static int gb_run(DpMachine *m) { return dp_mach_run(m->impl); }
// the GB window geometry is bank dependent: only the fixed bank 0 is known
// without the mapper state, so the view answers for $0000-$3FFF
static int64_t gb_rom_index(DpMachine *m, uint32_t a) {
    (void)m;
    return a < 0x4000 ? (int64_t)a : -1;
}
static void gb_close(DpMachine *m) {
    if (!m) return;
    dp_mach_close(m->impl);
    free(m);
}

// wraps an open DpMach (ownership moves to the machine); NULL on failure
DpMachine *dp_machine_gb(DpMach *impl) {
    if (!impl) return NULL;
    DpMachine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->name = "gb";
    m->plat = dp_platform_gb();
    m->isa = dp_isa_sm83();
    m->impl = impl;
    m->state_bytes = gb_state_bytes;
    m->save = gb_save;
    m->load = gb_load;
    m->rom_index = gb_rom_index;
    m->fetch = gb_fetch;
    m->run = gb_run;
    m->close = gb_close;
    return m;
}
