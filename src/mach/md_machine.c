// md_machine.c: the Mega Drive m0boy runtime as a DpMachine (spec §7.2).
// The explorer still drives DpMdMach directly; this wrapper is the
// kernel-facing view, built on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "mdmach.h"
#include "../explore/core.h"

typedef struct {
    DpMdMach *m;
    const DpObservers *obs;
    void *user;
    const uint8_t *rom; size_t rom_size;   // for clone()
    uint64_t frames;   // completed frames (relative clock for sessions)
    int pad_armed, pad_seen;   // pad-port read detection for the event probe
} MdMachWrap;

static uint32_t md_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_mdmach_state_size() + 8; }
static void md_save(DpMachine *m, uint8_t *buf) { dp_mdmach_save(((MdMachWrap *)m->impl)->m, buf); memcpy(buf + dp_mdmach_state_size(), &((MdMachWrap *)m->impl)->frames, 8); }
static void md_load(DpMachine *m, const uint8_t *buf) { dp_mdmach_load(((MdMachWrap *)m->impl)->m, buf); memcpy(&((MdMachWrap *)m->impl)->frames, buf + dp_mdmach_state_size(), 8); }
// the byte at the address: 68000 is big-endian, peek32 returns the aligned word
static uint8_t md_fetch(DpMachine *m, uint32_t a) {
    uint32_t w = dp_mdmach_peek32(((MdMachWrap *)m->impl)->m, a & ~3u);
    return (uint8_t)(w >> (24 - 8 * (a & 3)));
}
static void md_poke(DpMachine *m, uint32_t a, uint8_t v) { dp_mdmach_poke(((MdMachWrap *)m->impl)->m, a, v); }
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
    if (w->pad_armed && addr >= 0xA10002u && addr <= 0xA10005u) w->pad_seen = 1;
    if (w->obs && w->obs->on_read)
        for (unsigned k = 0; k < size; k++) w->obs->on_read(w->user, addr + k, 0);
}
static void md_on_write(void *user, uint32_t addr, unsigned size, uint32_t value) {
    MdMachWrap *w = user;
    if (w->obs && w->obs->on_write)
        for (unsigned k = 0; k < size; k++) w->obs->on_write(w->user, addr + k, (uint8_t)(value >> (24 - 8 * k)));
}
static void md_on_frame(void *user) { ((MdMachWrap *)user)->frames++; }
static uint64_t md_frame(DpMachine *m) { return ((MdMachWrap *)m->impl)->frames; }
static void md_observe(DpMachine *m, const DpObservers *o, void *user) {
    MdMachWrap *w = m->impl;
    w->obs = o; w->user = user;
    DpMdHooks h = { .user = w, .on_read = md_on_read, .on_write = md_on_write, .on_frame = md_on_frame };
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

// kernel mask (up down left right button1 button2 start) -> MD pad bits
// (0-3 directions, 16 A, 32 B, 64 C, 128 Start): button1 holds A and C, button2 B
static void md_set_input(DpMachine *m, uint32_t mask) {
    uint8_t pad = (uint8_t)(mask & 0x0F);
    if (mask & 0x10) pad |= 16 | 64;
    if (mask & 0x20) pad |= 32;
    if (mask & 0x40) pad |= 128;
    dp_mdmach_set_pad(((MdMachWrap *)m->impl)->m, pad);
}

// technique 2 on MD: an instruction that reads the pad data port ($A10003/$A10005) forks one
// state per pad held (Start, A+C, B, directions); the held pad stays in the saved state, so the
// game's next reads see it.  The instruction is dry-run once to see whether it touches the port.
static int md_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc; (void)bytes;
    static const uint32_t pads[] = {0x40, 0x10, 0x20, 0x08, 0x04, 0x01, 0x02};
    if (k >= sizeof pads / sizeof *pads) return 0;
    MdMachWrap *w = m->impl;
    uint8_t *pre = malloc(md_state_bytes(m));
    if (!pre) return 0;
    m->save(m, pre);
    w->pad_armed = 1; w->pad_seen = 0;
    md_run(m);
    w->pad_armed = 0;
    int hit = w->pad_seen;
    m->load(m, pre);
    if (hit) { md_set_input(m, pads[k]); m->save(m, save); m->load(m, pre); }
    free(pre);
    return hit;
}
// IRQ fork (technique 2): where the vertical interrupt is enabled and unmasked, one state with
// the interrupt raised (the old engine's irq_forks); delay 1 only
static int md_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    MdMachWrap *w = m->impl;
    if (delay != 1) return 0;
    if (((dp_mdmach_sr(w->m) >> 8) & 7u) >= 6u || !(dp_mdmach_vdp_reg(w->m, 1) & 0x20)) return 0;
    m->save(m, save);
    dp_mdmach_state_set_irq(save, 1, 0);
    return 1;
}
static const DpPlatformHooks md_hooks = { .event_probe = md_event_probe, .irq_alt = md_irq_alt };
const DpPlatformHooks *dp_machine_md_hooks(void) { return &md_hooks; }

// the runtime (68000, VDP, RAM) is thread-local: a clone is a shell until its worker attaches
static int md_attach(DpMachine *m) {
    MdMachWrap *w = m->impl;
    if (w->m) return 1;
    w->m = dp_mdmach_open(w->rom, w->rom_size);
    if (!w->m) return 0;
    dp_mdmach_reset(w->m);
    return 1;
}
static DpMachine *md_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    MdMachWrap *o = m->impl;
    if (!o->rom) return NULL;
    DpMachine *c = dp_machine_md(NULL);
    if (!c) return NULL;
    dp_machine_md_source(c, o->rom, o->rom_size);
    c->attach = md_attach;
    return c;
}
void dp_machine_md_source(DpMachine *m, const uint8_t *rom, size_t size) {
    MdMachWrap *w = m->impl;
    w->rom = rom; w->rom_size = size;
    m->clone = md_clone;
}

static void md_close(DpMachine *m) {
    if (((MdMachWrap *)m->impl)->m) dp_mdmach_close(((MdMachWrap *)m->impl)->m);
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
    m->poke = md_poke;
    m->rom_index = md_rom_index;
    m->run = md_run;
    m->observe = md_observe;
    m->close = md_close;
    m->isa = dp_isa_68000();
    m->pc = md_pc;
    m->set_pc = md_set_pc;
    m->set_input = md_set_input;
    m->frame = md_frame;
    return m;
}

