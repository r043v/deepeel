// lr_machine.c: a libretro core (level A, unmodified) as a DpMachine
// (spec §7.2 + §8.1).  Frames only: no step, no fetch, no rom_index, no bus
// observers — the kernel degrades and dp_lr_degradations() says so.  What
// remains is real: state serialization, frame advance, RAM view and the
// input ports.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "libretro.h"

static uint32_t lr_state_bytes(DpMachine *m) { return (uint32_t)dp_lr_state_size(m->impl); }
static void lr_save(DpMachine *m, uint8_t *buf) { dp_lr_save(m->impl, buf, dp_lr_state_size(m->impl)); }
static void lr_load(DpMachine *m, const uint8_t *buf) { dp_lr_load(m->impl, buf, dp_lr_state_size(m->impl)); }

// one frame; the return says a frame ran (the core may render none when the
// display is off, that is still progress at this level)
static int lr_run(DpMachine *m) {
    dp_lr_advance(m->impl, 1);
    return 1;
}

static void lr_close(DpMachine *m) {
    dp_lr_close(m->impl);
    free(m);
}

// takes ownership of the DpLibretro instance (rom already loaded)
DpMachine *dp_machine_libretro(DpLibretro *lr) {
    DpMachine *m = calloc(1, sizeof *m);
    if (!m) { dp_lr_close(lr); return NULL; }
    m->name = "libretro";
    m->impl = lr;
    m->state_bytes = lr_state_bytes;
    m->save = lr_save;
    m->load = lr_load;
    m->run = lr_run;
    m->close = lr_close;
    // no isa (no step), no fetch, no rom_index, no observe: the kernel
    // degrades and dp_lr_degradations() names the missing surfaces
    return m;
}
