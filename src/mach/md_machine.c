// md_machine.c: the Mega Drive m0boy runtime as a DpMachine (spec §7.2).
// The explorer still drives DpMdMach directly; this wrapper is the
// kernel-facing view, built on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "mdmach.h"

typedef struct {
    DpMdMach *m;
    const DpObservers *obs;
    void *user;
} MdMachWrap;

static uint32_t md_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_mdmach_state_size(); }
static void md_save(DpMachine *m, uint8_t *buf) { dp_mdmach_save(((MdMachWrap *)m->impl)->m, buf); }
static void md_load(DpMachine *m, const uint8_t *buf) { dp_mdmach_load(((MdMachWrap *)m->impl)->m, buf); }
// the byte at the address: 68000 is big-endian, peek32 returns the aligned word
static uint8_t md_fetch(DpMachine *m, uint32_t a) {
    uint32_t w = dp_mdmach_peek32(((MdMachWrap *)m->impl)->m, a & ~3u);
    return (uint8_t)(w >> (24 - 8 * (a & 3)));
}
// the ROM sits at $000000: a visible CPU address is its physical offset
static int64_t md_rom_index(DpMachine *m, uint32_t a) {
    return dp_mdmach_rom_visible(((MdMachWrap *)m->impl)->m, a) ? (int64_t)a : -1;
}

// one instruction: max_steps 1; a cut or fault reports no progress
static int md_run(DpMachine *m) {
    MdMachWrap *w = m->impl;
    int cut = 0, fault = 0;
    return dp_mdmach_run(w->m, 1, &cut, &fault) > 0;
}

static void md_on_read(void *user, uint32_t addr, unsigned size) {
    MdMachWrap *w = user;
    if (w->obs && w->obs->on_read)
        for (unsigned k = 0; k < size; k++) w->obs->on_read(w->user, addr + k, 0);
}
static void md_on_write(void *user, uint32_t addr, unsigned size, uint32_t value) {
    MdMachWrap *w = user;
    if (w->obs && w->obs->on_write)
        for (unsigned k = 0; k < size; k++) w->obs->on_write(w->user, addr + k, (uint8_t)(value >> (24 - 8 * k)));
}
static void md_observe(DpMachine *m, const DpObservers *o, void *user) {
    MdMachWrap *w = m->impl;
    w->obs = o; w->user = user;
    DpMdHooks h = { .user = w, .on_read = md_on_read, .on_write = md_on_write };
    dp_mdmach_set_hooks(w->m, &h);
}

static uint32_t md_pc(DpMachine *m) { return dp_mdmach_pc(((MdMachWrap *)m->impl)->m); }
// the pc lives in the saved state: round-trip it through the buffer
static void md_set_pc(DpMachine *m, uint32_t pc) {
    MdMachWrap *w = m->impl;
    uint8_t *buf = malloc(dp_mdmach_state_size());
    if (!buf) return;
    dp_mdmach_save(w->m, buf);
    dp_mdmach_state_set_pc(buf, pc);
    dp_mdmach_load(w->m, buf);
    free(buf);
}

static void md_close(DpMachine *m) {
    dp_mdmach_close(((MdMachWrap *)m->impl)->m);
    free(m->impl);
    free(m);
}

// takes ownership of the DpMdMach
DpMachine *dp_machine_md(DpMdMach *impl) {
    MdMachWrap *w = calloc(1, sizeof *w);
    DpMachine *m = calloc(1, sizeof *m);
    if (!w || !m) { free(w); free(m); return NULL; }
    w->m = impl;
    m->name = "md";
    m->impl = w;             // the 68000 DpIsa wrapper is not extracted yet
    m->state_bytes = md_state_bytes;
    m->save = md_save;
    m->load = md_load;
    m->fetch = md_fetch;
    m->rom_index = md_rom_index;
    m->run = md_run;
    m->observe = md_observe;
    m->close = md_close;
    m->isa = dp_isa_68000();
    m->pc = md_pc;
    m->set_pc = md_set_pc;
    return m;
}
