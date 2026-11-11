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
    // clones keep the image and initialise the runtime lazily in the worker thread
    // that uses it: the PCE machine state is thread-local
    uint8_t *img; size_t img_size; int inited;
    uint64_t frames;   // completed frames (relative clock for sessions)
    const uint8_t *src; size_t src_size;
} PceWrap;
static void pce_ensure(DpMachine *m) {
    PceWrap *w = m->impl;
    if (w->img && !w->inited) { dp_pce_init(w->img, w->img_size); w->inited = 1; }
}

static uint32_t pce_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_pce_state_size() + 8; }
static void pce_save(DpMachine *m, uint8_t *buf) { pce_ensure(m); dp_pce_save(buf); memcpy(buf + dp_pce_state_size(), &((PceWrap *)m->impl)->frames, 8); }
static void pce_load(DpMachine *m, const uint8_t *buf) { pce_ensure(m); dp_pce_load(buf); memcpy(&((PceWrap *)m->impl)->frames, buf + dp_pce_state_size(), 8); }
static uint8_t pce_fetch(DpMachine *m, uint32_t a) { pce_ensure(m); return dp_pce_peek((uint16_t)a); }
static int64_t pce_rom_index(DpMachine *m, uint32_t a) { pce_ensure(m); return dp_pce_rom_index((uint16_t)a); }

// one step (an instruction or an interrupt dispatch)
static int pce_run(DpMachine *m) {
    pce_ensure(m);
    int frame_done = 0;
    int ok = dp_pce_step(&frame_done) > 0;
    if (frame_done) ((PceWrap *)m->impl)->frames++;
    return ok;
}

static uint64_t pce_frame(DpMachine *m) { return ((PceWrap *)m->impl)->frames; }

static void pce_trace_fn(void *user, uint16_t pc, uint8_t op) {
    PceWrap *w = user;
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, pc, op, 0);
}
static void pce_observe(DpMachine *m, const DpObservers *o, void *user) {
    PceWrap *w = m->impl;
    pce_ensure(m);
    w->obs = o; w->user = user;
    dp_pce_trace(o ? pce_trace_fn : NULL, w);
}

static void pce_close(DpMachine *m) {
    PceWrap *w = m->impl;
    if (w->img) { if (w->inited) dp_pce_close(); free(w->img); }
    free(m->impl);
    free(m);
}

// wraps the live runtime singleton (dp_pce_init already done, or call it first)
static uint32_t pce_pc(DpMachine *m) { pce_ensure(m); return dp_pce_cpu().pc; }
static void pce_set_pc(DpMachine *m, uint32_t pc) { pce_ensure(m); dp_pce_set_pc((uint16_t)pc); }

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

static int pce_attach(DpMachine *m) { pce_ensure(m); return 1; }
static DpMachine *pce_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    PceWrap *o = m->impl, *w = calloc(1, sizeof *w);
    DpMachine *c = calloc(1, sizeof *c);
    if (!w || !c || !o->src) { free(w); free(c); return NULL; }
    *c = *m;
    w->img = malloc(o->src_size);
    if (!w->img) { free(w); free(c); return NULL; }
    memcpy(w->img, o->src, o->src_size);
    w->img_size = o->src_size;
    c->impl = w;
    c->attach = pce_attach;
    return c;
}
void dp_machine_pce_source(DpMachine *m, const uint8_t *data, size_t size) {
    PceWrap *w = m->impl;
    w->src = data; w->src_size = size;
    m->clone = pce_clone;
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
    m->frame = pce_frame;
    return m;
}
