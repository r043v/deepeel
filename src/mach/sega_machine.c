// sega_machine.c: the SG-1000/SMS/GG/MSX m0boy runtime as a DpMachine
// (spec §7.2).  The explorer still drives DpSegaBackend directly; this
// wrapper is the kernel-facing view, built on the same singleton instance so
// both see the same state.  There is no single-step entry in the backend, so
// run() executes one instruction through the same service/decode/execute path
// the explorer's play_step uses (one z80_decoded_t cache per wrapper).
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "../explore/core.h"
#include "../cpu/z80/z80.h"
#include "../platform/sega/backend.h"

typedef struct {
    const DpSegaBackend *b;
    z80_decoded_t d;
    const DpObservers *obs;
    void *user;
    // the callbacks observe() wrapped, called through after reporting
    uint8_t (*prev_read)(void *, uint16_t);
    void (*prev_write)(void *, uint16_t, uint8_t);
    void *prev_user;
    const uint8_t *rom; size_t rom_size; const void *(*pick)(unsigned);
    // this instance's own bus callbacks: a state saved by another instance
    // (a worker clone) carries that instance's pointers, which must not survive a load
    uint8_t (*base_read)(void *, uint16_t);
    void (*base_write)(void *, uint16_t, uint8_t);
    uint8_t (*base_in)(void *, uint16_t);
    void (*base_out)(void *, uint16_t, uint8_t);
    void *base_user;
    int wrapped, fetching;   // fetching: opcode reads are not data accesses
    int init_arg;            // backend init's last argument: SMS flag, or the MSX mapper
} SegaMach;

static uint32_t sega_state_bytes(DpMachine *m) { return (uint32_t)((SegaMach *)m->impl)->b->state_size; }
static void sega_save(DpMachine *m, uint8_t *buf) { ((SegaMach *)m->impl)->b->save(buf); }
static void sega_reinstall(SegaMach *s);
static void sega_load(DpMachine *m, const uint8_t *buf) {
    SegaMach *s = m->impl;
    s->b->load(buf);
    sega_reinstall(s);   // a restored state carries the original bus callbacks
}
static uint8_t sega_fetch(DpMachine *m, uint32_t a) { return ((SegaMach *)m->impl)->b->read((uint16_t)a); }
static int64_t sega_rom_index(DpMachine *m, uint32_t a) { return ((SegaMach *)m->impl)->b->rom_index((uint16_t)a); }

// one instruction: service (IRQ/NMI), then decode from the live bus and
// execute — the explorer's play_step path, without its instrumentation
static int sega_run(DpMachine *m) {
    SegaMach *s = m->impl;
    z80_t *c = s->b->cpu();
    if (c->fault) return 0;
    unsigned cycles = z80_service(c);
    if (!cycles) {
        s->fetching = 1;
        if (!z80_decode(c, &s->d)) { s->fetching = 0; return 0; }
        uint8_t bytes[8] = { 0 };
        for (unsigned j = 0; j < s->d.length && j < sizeof bytes; j++) bytes[j] = s->b->read((uint16_t)(c->pc + j));
        s->fetching = 0;
        DpZ80Instruction in;
        if (!dp_z80_decode(bytes, s->d.length, c->pc, &in)) return 0;
        if (s->obs && s->obs->on_step) s->obs->on_step(s->user, c->pc, bytes[0], 0);
        cycles = z80_execute_decoded(c, &s->d);
        z80_finish(c, cycles);
    }
    s->b->advance(cycles);
    return 1;
}

static uint32_t sega_pc(DpMachine *m) { return ((SegaMach *)m->impl)->b->cpu()->pc; }
static void sega_set_pc(DpMachine *m, uint32_t pc) { ((SegaMach *)m->impl)->b->cpu()->pc = (uint16_t)pc; }

// the base write callback bypasses the observation wrapper: a poke is not a CPU access
static void sega_poke(DpMachine *m, uint32_t a, uint8_t v) {
    SegaMach *s = m->impl;
    if (s->base_write) s->base_write(s->base_user, (uint16_t)a, v);
}

static uint8_t sega_obs_read(void *u, uint16_t a) {
    SegaMach *s = u;
    uint8_t v = s->prev_read(s->prev_user, a);
    if (!s->fetching && s->obs && s->obs->on_read) s->obs->on_read(s->user, a, v);
    return v;
}
static void sega_obs_write(void *u, uint16_t a, uint8_t v) {
    SegaMach *s = u;
    if (s->obs && s->obs->on_write) s->obs->on_write(s->user, a, v);
    s->prev_write(s->prev_user, a, v);
}

static void sega_reinstall(SegaMach *s) {
    z80_t *c = s->b->cpu();
    c->read = s->base_read; c->write = s->base_write; c->in = s->base_in; c->out = s->base_out;
    c->user = s->base_user;
    if (!s->wrapped) return;
    s->prev_read = c->read; s->prev_write = c->write; s->prev_user = c->user;
    c->read = sega_obs_read; c->write = sega_obs_write; c->user = s;
}

static void sega_observe(DpMachine *m, const DpObservers *o, void *user) {
    SegaMach *s = m->impl;
    s->obs = o; s->user = user;
    // data accesses: interpose on the CPU bus callbacks once (instruction
    // fetches go through z80_decode's own path and are not reported)
    z80_t *c = s->b->cpu();
    if (o && c->read != sega_obs_read && c->read && c->write) {
        s->prev_read = c->read; s->prev_write = c->write; s->prev_user = c->user;
        c->read = sega_obs_read; c->write = sega_obs_write; c->user = s;
        s->wrapped = 1;
    }
}

static void sega_close(DpMachine *m) {
    free(m->impl);
    free(m);
}

DpMachine *dp_machine_sega(const void *backend);
static DpMachine *sega_clone(DpMachine *m, unsigned worker) {
    SegaMach *s = m->impl;
    if (!s->pick || !s->rom || !worker) return NULL;
    const DpSegaBackend *b = s->pick(worker);
    if (!b || b->init(s->rom, s->rom_size, NULL, s->init_arg) != 1) return NULL;
    DpMachine *c = dp_machine_sega(b);
    if (c) { dp_machine_sega_source(c, s->rom, s->rom_size, s->pick); ((SegaMach *)c->impl)->init_arg = s->init_arg; }
    return c;
}
void dp_machine_sega_source(DpMachine *m, const uint8_t *rom, size_t size, const void *(*pick)(unsigned worker)) {
    SegaMach *s = m->impl;
    s->rom = rom; s->rom_size = size; s->pick = pick;
    m->clone = sega_clone;
}

void dp_machine_sega_init_arg(DpMachine *m, int arg) { ((SegaMach *)m->impl)->init_arg = arg; }

DpMachine *dp_machine_sega(const void *backend) {
    const DpSegaBackend *b = backend;
    SegaMach *s = calloc(1, sizeof *s);
    DpMachine *m = calloc(1, sizeof *m);
    if (!s || !m) { free(s); free(m); return NULL; }
    s->b = b;
    s->init_arg = 1;
    { z80_t *c0 = b->cpu(); s->base_read = c0->read; s->base_write = c0->write; s->base_in = c0->in; s->base_out = c0->out; s->base_user = c0->user; }
    m->name = "sega";
    m->isa = dp_isa_z80();
    m->impl = s;
    m->state_bytes = sega_state_bytes;
    m->save = sega_save;
    m->load = sega_load;
    m->fetch = sega_fetch;
    m->rom_index = sega_rom_index;
    m->run = sega_run;
    m->pc = sega_pc;
    m->set_pc = sega_set_pc;
    m->poke = sega_poke;
    m->observe = sega_observe;
    m->close = sega_close;
    return m;
}

// technique 2 on SMS/GG/SG: `in a,(n)` of the VDP status ($BF mirrors), the V counter
// ($7E) or the H counter ($7F) forks one state per plausible value.  The IN runs on the
// live machine, A is overwritten with the alternative, the state is saved, then the live
// state is restored: the saved state sits just after the IN.  No probe while an
// interrupt line is pending (a planted status must not hide it).  `in a,($DC)` forks
// one pad-1 line low per alternative (input probes, one button at a time).
static int sega_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    static const uint8_t st[] = {0x00, 0x20, 0x40, 0x80, 0xC0, 0xE0, 0xFF};   // collision|overflow|vblank|line, combos
    static const uint8_t vc[] = {0x00, 0x7F, 0x80, 0xC0, 0xFF};
    static const uint8_t hc[] = {0x00, 0x20, 0x40, 0x80, 0xE0, 0xFF};
    static const uint8_t pad[] = {0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF};   // pad 1: one line low (up, down, left, right, 1, 2)
    if (bytes[0] != 0xDB) return 0;                       // in a,(n)
    SegaMach *s = m->impl;
    z80_t *c = s->b->cpu();
    if (c->irq || c->nmi) return 0;
    const uint8_t *vals; unsigned n;
    switch (bytes[1] & 0xC1) {
    case 0x81: vals = st; n = sizeof st; break;
    case 0x40: vals = vc; n = sizeof vc; break;
    case 0x41: vals = hc; n = sizeof hc; break;
    case 0xC0: vals = pad; n = sizeof pad; break;   // $DC/$C0 joypad port A
    default: return 0;
    }
    if (k >= n) return 0;
    uint8_t *tmp = malloc(m->state_bytes(m));
    if (!tmp) return 0;
    m->save(m, tmp);
    int ok = m->run(m);
    if (ok) { c->a = vals[k]; m->save(m, save); }
    m->load(m, tmp);
    free(tmp);
    return ok;
}
// IRQ fork: the alternative to servicing a pending interrupt is to run the next
// instruction with the line masked, then re-arm it (the old engine's skip_irq).
static int sega_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    SegaMach *s = m->impl;
    z80_t *c = s->b->cpu();
    if (!(c->nmi || (c->irq && c->iff1 && !c->ei_delay))) return 0;
    uint8_t irq = c->irq, nmi = c->nmi;
    uint8_t *tmp = malloc(m->state_bytes(m));
    if (!tmp) return 0;
    m->save(m, tmp);
    c->irq = c->nmi = 0;
    int ok = 1;
    for (unsigned i = 0; i < delay && ok; i++) ok = m->run(m);
    if (ok) { c->irq = irq; c->nmi = nmi; m->save(m, save); }
    m->load(m, tmp);
    free(tmp);
    return ok;
}
const DpPlatformHooks *dp_machine_sega_hooks(void) {
    static const DpPlatformHooks h = { .event_probe = sega_event_probe, .irq_alt = sega_irq_alt };
    return &h;
}
