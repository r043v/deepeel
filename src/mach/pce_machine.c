// pce_machine.c: the PC Engine m0boy runtime as a DpMachine (spec §7.2).
// The explorer still drives the runtime singleton directly; this wrapper is
// the kernel-facing view over the same instance (exploration is sequential).
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "../platform/pce/pce.h"
#include "../explore/core.h"

typedef struct {
    const DpObservers *obs;
    void *user;
} PceWrap;

static uint32_t pce_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_pce_state_size(); }
static void pce_save(DpMachine *m, uint8_t *buf) { (void)m; dp_pce_save(buf); }
static void pce_load(DpMachine *m, const uint8_t *buf) { (void)m; dp_pce_load(buf); }
static uint8_t pce_fetch(DpMachine *m, uint32_t a) { (void)m; return dp_pce_peek((uint16_t)a); }
static int64_t pce_rom_index(DpMachine *m, uint32_t a) { (void)m; return dp_pce_rom_index((uint16_t)a); }

// one step (an instruction or an interrupt dispatch)
static int pce_run(DpMachine *m) {
    (void)m;
    int frame_done = 0;
    return dp_pce_step(&frame_done) > 0;
}

static void pce_trace_fn(void *user, uint16_t pc, uint8_t op) {
    PceWrap *w = user;
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, pc, op, 0);
}
static void pce_observe(DpMachine *m, const DpObservers *o, void *user) {
    PceWrap *w = m->impl;
    w->obs = o; w->user = user;
    dp_pce_trace(o ? pce_trace_fn : NULL, w);
}

static void pce_close(DpMachine *m) {
    free(m->impl);
    free(m);
}

// wraps the live runtime singleton (dp_pce_init already done, or call it first)
static uint32_t pce_pc(DpMachine *m) { (void)m; return dp_pce_cpu().pc; }
static void pce_set_pc(DpMachine *m, uint32_t pc) { (void)m; dp_pce_set_pc((uint16_t)pc); }

// technique 2 (interrupts): where the CPU unmasks (CLI, PLP, RTI, or a write to the interrupt
// mask $1402) one alternative per source (1 = VDC, 2 = timer) is forked with that source raised,
// as the old PCE engine's fork_irq
static int pce_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    if (k >= 2) return 0;
    int unmask = bytes[0] == 0x58 || bytes[0] == 0x28 || bytes[0] == 0x40;
    if (!unmask && (bytes[0] == 0x8D || bytes[0] == 0x9C) && (((bytes[2] << 8) | bytes[1]) & 0x1FFF) == 0x1402) unmask = 1;
    if (!unmask) return 0;
    uint8_t *tmp = malloc(m->state_bytes(m));
    if (!tmp) return 0;
    m->save(m, tmp);
    dp_pce_raise((int)k + 1);
    m->save(m, save);
    m->load(m, tmp);
    free(tmp);
    return 1;
}
const DpPlatformHooks *dp_machine_pce_hooks(void) {
    static const DpPlatformHooks h = { .event_probe = pce_event_probe };
    return &h;
}

DpMachine *dp_machine_pce(void) {
    PceWrap *w = calloc(1, sizeof *w);
    DpMachine *m = calloc(1, sizeof *m);
    if (!w || !m) { free(w); free(m); return NULL; }
    m->name = "pce";
    m->impl = w;             // the huc6280 DpIsa wrapper is not extracted yet
    m->state_bytes = pce_state_bytes;
    m->save = pce_save;
    m->load = pce_load;
    m->fetch = pce_fetch;
    m->rom_index = pce_rom_index;
    m->run = pce_run;
    m->observe = pce_observe;
    m->close = pce_close;
    m->isa = dp_isa_huc6280();
    m->pc = pce_pc;
    m->set_pc = pce_set_pc;
    return m;
}
