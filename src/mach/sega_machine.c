// sega_machine.c: the SG-1000/SMS/GG/MSX m0boy runtime as a DpMachine
// (spec §7.2).  The explorer still drives DpSegaBackend directly; this
// wrapper is the kernel-facing view, built on the same singleton instance so
// both see the same state.  There is no single-step entry in the backend, so
// run() executes one instruction through the same service/decode/execute path
// the explorer's play_step uses (one z80_decoded_t cache per wrapper).
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "../cpu/z80/z80.h"
#include "../platform/sega/backend.h"

typedef struct {
    const DpSegaBackend *b;
    z80_decoded_t d;
    const DpObservers *obs;
    void *user;
} SegaMach;

static uint32_t sega_state_bytes(DpMachine *m) { return (uint32_t)((SegaMach *)m->impl)->b->state_size; }
static void sega_save(DpMachine *m, uint8_t *buf) { ((SegaMach *)m->impl)->b->save(buf); }
static void sega_load(DpMachine *m, const uint8_t *buf) { ((SegaMach *)m->impl)->b->load(buf); }
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
        if (!z80_decode(c, &s->d)) return 0;
        uint8_t bytes[8] = { 0 };
        for (unsigned j = 0; j < s->d.length && j < sizeof bytes; j++) bytes[j] = s->b->read((uint16_t)(c->pc + j));
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

static void sega_observe(DpMachine *m, const DpObservers *o, void *user) {
    SegaMach *s = m->impl;
    s->obs = o; s->user = user;
}

static void sega_close(DpMachine *m) {
    free(m->impl);
    free(m);
}

DpMachine *dp_machine_sega(const void *backend) {
    const DpSegaBackend *b = backend;
    SegaMach *s = calloc(1, sizeof *s);
    DpMachine *m = calloc(1, sizeof *m);
    if (!s || !m) { free(s); free(m); return NULL; }
    s->b = b;
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
    m->observe = sega_observe;
    m->close = sega_close;
    return m;
}
