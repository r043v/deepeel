// nes_machine.c: the NES m0boy runtime as a DpMachine (spec §7.2).  The
// explorer still drives DpNesMach directly; this wrapper is the kernel-facing
// view, built on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "nesmach.h"
#include "../explore/core.h"

typedef struct {
    DpNesMach *m;
    const DpObservers *obs;
    void *user;
    // clones keep the image and open their runtime lazily, in the worker thread that
    // will use it: the NES machine state is thread-local
    uint8_t *img; size_t img_size; int mapper;
    const uint8_t *src; size_t src_size;   // the original image (owned by the caller)
    uint64_t frames;   // completed frames since this machine started (a relative clock for sessions)
} NesMachWrap;
static DpNesMach *nes_impl(DpMachine *m) {
    NesMachWrap *w = m->impl;
    if (!w->m && w->img) { char err[64]; w->m = dp_nesmach_open(w->img, w->img_size, w->mapper, err, sizeof err); }
    return w->m;
}

static void nes_poke(DpMachine *m, uint32_t a, uint8_t v) { (void)nes_impl(m); dp_nesmach_poke(a, v); }
static uint32_t nes_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_nesmach_state_size() + 8; }
static void nes_save(DpMachine *m, uint8_t *buf) { dp_nesmach_save(nes_impl(m), buf); memcpy(buf + dp_nesmach_state_size(), &((NesMachWrap *)m->impl)->frames, 8); }
static void nes_load(DpMachine *m, const uint8_t *buf) { dp_nesmach_load(nes_impl(m), buf); memcpy(&((NesMachWrap *)m->impl)->frames, buf + dp_nesmach_state_size(), 8); }
static uint8_t nes_fetch(DpMachine *m, uint32_t a) { return dp_nesmach_read(nes_impl(m), a); }
static int64_t nes_rom_index(DpMachine *m, uint32_t a) { return dp_nesmach_rom_index(nes_impl(m), a); }

// one step; a completed frame boundary inside it still counts as progress
static int nes_run(DpMachine *m) {
    NesMachWrap *w = m->impl;
    DpNesMach *n = nes_impl(m);
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, dp_nesmach_pc(n), 0, 0);
    return dp_nesmach_run(n, 1) > 0;
}

static void nes_on_step(void *user, unsigned pc, unsigned op) {
    NesMachWrap *w = user;
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, pc, (uint8_t)op, 0);
}
static void nes_on_read(void *user, unsigned addr, unsigned value) {
    NesMachWrap *w = user;
    if (w->obs && w->obs->on_read) w->obs->on_read(w->user, addr, (uint8_t)value);
}
static void nes_on_write(void *user, unsigned addr, unsigned value) {
    NesMachWrap *w = user;
    if (w->obs && w->obs->on_write) w->obs->on_write(w->user, addr, (uint8_t)value);
}
static void nes_on_frame(void *user) { ((NesMachWrap *)user)->frames++; }
static uint64_t nes_frame(DpMachine *m) { return ((NesMachWrap *)m->impl)->frames; }
static void nes_observe(DpMachine *m, const DpObservers *o, void *user) {
    NesMachWrap *w = m->impl;
    w->obs = o; w->user = user;
    DpNesHooks h = { .user = w, .on_step = nes_on_step, .on_read = nes_on_read, .on_write = nes_on_write, .on_frame = nes_on_frame };
    dp_nesmach_set_hooks(nes_impl(m), &h);
}

static uint32_t nes_pc(DpMachine *m) { return dp_nesmach_pc(nes_impl(m)); }
static void nes_set_pc(DpMachine *m, uint32_t pc) { dp_nesmach_set_pc(nes_impl(m), pc); }
// kernel pad mask (bit0 up, 1 down, 2 left, 3 right, 4 button 1, 5 button 2, 6 start)
// -> NES pad 0 (A, B, Select, Start, Up, Down, Left, Right = bits 0..7)
static void nes_set_input(DpMachine *m, uint32_t k) {
    uint8_t p = 0;
    if (k & 0x10) p |= 0x01;   // A
    if (k & 0x20) p |= 0x02;   // B
    if (k & 0x40) p |= 0x08;   // Start
    if (k & 0x01) p |= 0x10;
    if (k & 0x02) p |= 0x20;
    if (k & 0x04) p |= 0x40;
    if (k & 0x08) p |= 0x80;
    dp_nesmach_set_input(nes_impl(m), p, 0);
}
size_t dp_machine_nes_prg_size(DpMachine *m) { return dp_nesmach_prg_size(nes_impl(m)); }

static void nes_close(DpMachine *m) {
    NesMachWrap *w = m->impl;
    if (w->img) { if (w->m) dp_nesmach_close(w->m); free(w->img); }
    free(m->impl);
    free(m);
}

static int nes_attach(DpMachine *m) { return nes_impl(m) != NULL; }
static DpMachine *nes_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    NesMachWrap *o = m->impl, *w = calloc(1, sizeof *w);
    DpMachine *c = calloc(1, sizeof *c);
    if (!w || !c || !o->src) { free(w); free(c); return NULL; }
    *c = *m;
    w->img = malloc(o->src_size);
    if (!w->img) { free(w); free(c); return NULL; }
    memcpy(w->img, o->src, o->src_size);
    w->img_size = o->src_size; w->mapper = o->mapper;
    c->impl = w;
    c->attach = nes_attach;
    return c;
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
    m->isa = dp_isa_m6502();
    m->pc = nes_pc;
    m->set_pc = nes_set_pc;
    m->set_input = nes_set_input;
    m->frame = nes_frame;
    m->impl = w;             // the m6502 DpIsa wrapper is not extracted yet
    m->state_bytes = nes_state_bytes;
    m->save = nes_save;
    m->load = nes_load;
    m->fetch = nes_fetch;
    m->poke = nes_poke;
    m->rom_index = nes_rom_index;
    m->run = nes_run;
    m->observe = nes_observe;
    m->close = nes_close;
    m->clone = nes_clone;
    w->src = rom; w->src_size = size; w->mapper = mapper;
    return m;
}

// technique 2: the first LDA/BIT abs $2002 at a ROM site forks one state per status byte
// the PPU can present (vblank, sprite 0, overflow and their combinations); no probe while
// an NMI edge is pending, a planted vblank must not mask the interrupt the hardware raises
static int nes_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    static const uint8_t st[] = { 0x80, 0x40, 0x20, 0xC0, 0xA0, 0xE0, 0x00 };
    DpNesMach *n = nes_impl(m);
    if ((bytes[0] == 0xAD || bytes[0] == 0xBD || bytes[0] == 0x8D || bytes[0] == 0x8E || bytes[0] == 0x8C) && bytes[1] == 0x16 && bytes[2] == 0x40) {   // read or strobe write of the controller port
        // controller read (LDA $4016 / LDA $4016,X): one button held from here on per alternative (as the Sega pad probe)
        static const uint32_t pads[] = { 0x10, 0x20, 0x40, 0x01, 0x02, 0x04, 0x08 };
        if (k >= sizeof pads / sizeof *pads) return 0;
        uint8_t *tmp = malloc(m->state_bytes(m));
        if (!tmp) return 0;
        m->save(m, tmp);
        m->set_input(m, pads[k]);
        m->save(m, save);
        m->load(m, tmp);
        free(tmp);
        return 1;
    }
    if (k >= sizeof st) return 0;
    if (!((bytes[0] == 0xAD || bytes[0] == 0x2C) && bytes[1] == 0x02 && bytes[2] == 0x20)) return 0;
    if (dp_nesmach_nmi_pending(n)) return 0;
    unsigned keep = dp_nesmach_ppu_status(n);
    dp_nesmach_set_ppu_status(st[k]);
    dp_nesmach_save(n, save); memcpy(save + dp_nesmach_state_size(), &((NesMachWrap *)m->impl)->frames, 8);
    dp_nesmach_set_ppu_status(keep);
    return 1;
}
// techniques 9 / IRQ fork: with an NMI edge pending, the state where the NMI is serviced `delay`
// instructions later (the edge is held off for those instructions, then raised again)
static int nes_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    if (!dp_nesmach_nmi_pending(nes_impl(m))) return 0;
    uint32_t sb = m->state_bytes(m);
    uint8_t *tmp = malloc(sb), *off = malloc(sb);
    if (!tmp || !off) { free(tmp); free(off); return 0; }
    m->save(m, tmp);
    memcpy(off, tmp, sb);
    *dp_nesmach_state_nmi_edge(off) = 0;
    m->load(m, off);
    int ok = 1;
    for (unsigned i = 0; i < delay && ok; i++) ok = m->run(m);
    if (ok) { m->save(m, save); *dp_nesmach_state_nmi_edge(save) = 1; }
    m->load(m, tmp);
    free(tmp); free(off);
    return ok;
}
const DpPlatformHooks *dp_machine_nes_hooks(void) {
    static const DpPlatformHooks h = { .event_probe = nes_event_probe, .irq_alt = nes_irq_alt };
    return &h;
}
