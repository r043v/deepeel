// nes_machine.c: the NES m0boy runtime as a DpMachine (spec §7.2).  The
// explorer still drives DpNesMach directly; this wrapper is the kernel-facing
// view, built on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "nesmach.h"

typedef struct {
    DpNesMach *m;
    const DpObservers *obs;
    void *user;
} NesMachWrap;

static uint32_t nes_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_nesmach_state_size(); }
static void nes_save(DpMachine *m, uint8_t *buf) { dp_nesmach_save(((NesMachWrap *)m->impl)->m, buf); }
static void nes_load(DpMachine *m, const uint8_t *buf) { dp_nesmach_load(((NesMachWrap *)m->impl)->m, buf); }
static uint8_t nes_fetch(DpMachine *m, uint32_t a) { return dp_nesmach_read(((NesMachWrap *)m->impl)->m, a); }
static int64_t nes_rom_index(DpMachine *m, uint32_t a) { return dp_nesmach_rom_index(((NesMachWrap *)m->impl)->m, a); }

// one step; a completed frame boundary inside it still counts as progress
static int nes_run(DpMachine *m) {
    NesMachWrap *w = m->impl;
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, dp_nesmach_pc(w->m), 0, 0);
    return dp_nesmach_run(w->m, 1) > 0;
}

static void nes_on_step(void *user, unsigned pc, unsigned op) {
    NesMachWrap *w = user;
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, pc, (uint8_t)op, 0);
}
static void nes_on_read(void *user, unsigned addr) {
    NesMachWrap *w = user;
    if (w->obs && w->obs->on_read) w->obs->on_read(w->user, addr, 0);
}
static void nes_on_write(void *user, unsigned addr, unsigned value) {
    NesMachWrap *w = user;
    if (w->obs && w->obs->on_write) w->obs->on_write(w->user, addr, (uint8_t)value);
}
static void nes_observe(DpMachine *m, const DpObservers *o, void *user) {
    NesMachWrap *w = m->impl;
    w->obs = o; w->user = user;
    DpNesHooks h = { .user = w, .on_step = nes_on_step, .on_read = nes_on_read, .on_write = nes_on_write };
    dp_nesmach_set_hooks(w->m, &h);
}

static void nes_close(DpMachine *m) {
    free(m->impl);
    free(m);
}

// opens the runtime from a cartridge image and wraps it; the DpMachine owns
// the DpNesMach (NULL with err filled on a refused image)
DpMachine *dp_machine_nes(const uint8_t *rom, size_t size, int mapper, char *err, size_t errsz) {
    DpNesMach *impl = dp_nesmach_open(rom, size, mapper, err, errsz);
    if (!impl) return NULL;
    NesMachWrap *w = calloc(1, sizeof *w);
    DpMachine *m = calloc(1, sizeof *m);
    if (!w || !m) { dp_nesmach_close(impl); free(w); free(m); return NULL; }
    w->m = impl;
    m->name = "nes";
    m->impl = w;             // the m6502 DpIsa wrapper is not extracted yet
    m->state_bytes = nes_state_bytes;
    m->save = nes_save;
    m->load = nes_load;
    m->fetch = nes_fetch;
    m->rom_index = nes_rom_index;
    m->run = nes_run;
    m->observe = nes_observe;
    m->close = nes_close;
    return m;
}
