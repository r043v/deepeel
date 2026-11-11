// gb_machine.c: the m0boy runtime as a DpMachine (spec §7.2).  The explorer
// still drives DpMach directly; this wrapper is the kernel-facing view, built
// on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "m0boy.h"
#include "../explore/core.h"

// The kernel-facing view of one m0boy instance.  Each wrapper owns its observer and its step
// counter, so several instances (parallel workers, one per compiled slot) run side by side.
typedef struct GbWrap {
    DpMach *d;
    DpObservers o;
    void *user;
    unsigned seen;            // instructions started in the current gb_run
    int joy_armed, joy_seen;  // joypad-port read detection for the event probe
    const uint8_t *rom; size_t rom_size;   // for clone()
} GbWrap;
static GbWrap *gbw(DpMachine *m) { return m->impl; }

// the frame counter rides at the end of the saved state so a restored fork resumes at its own frame
static uint32_t gb_state_bytes(DpMachine *m) { return dp_mach_state_bytes(gbw(m)->d) + 8; }
static void gb_save(DpMachine *m, uint8_t *buf) {
    DpMach *d = gbw(m)->d;
    dp_mach_save(d, buf);
    uint64_t f = *d->api->frame;
    memcpy(buf + dp_mach_state_bytes(d), &f, 8);
}
static void gb_load(DpMachine *m, const uint8_t *buf) {
    DpMach *d = gbw(m)->d;
    dp_mach_load(d, buf);
    uint64_t f; memcpy(&f, buf + dp_mach_state_bytes(d), 8);
    *d->api->frame = (typeof(*d->api->frame))f;
}
static uint8_t gb_fetch(DpMachine *m, uint32_t a) { return dp_mach_op(gbw(m)->d, (uint16_t)a); }
// compact ROM index bank * 0x4000 + (addr & 0x3FFF); the window bank comes from the mapper state
static int64_t gb_rom_index(DpMachine *m, uint32_t a) {
    DpMach *d = gbw(m)->d;
    if (a >= 0x8000) return -1;
    uint32_t bank = dp_gbl_bank_of(d->api, (uint16_t)a);
    int64_t idx = (int64_t)bank * 0x4000 + (a & 0x3FFF);
    return gbw(m)->rom_size && (size_t)idx >= gbw(m)->rom_size ? -1 : idx;
}
// --- observe: DpObservers over the engine hooks ---------------------------------
// one instruction per gb_run: the hook of the second instruction cuts before it executes
static void gb_on_step(DpMach *m, uint16_t pc, uint8_t op, int entry) {
    GbWrap *g = m->owner;
    if (g->seen++ >= 1) dp_mach_cut(m, DP_CUT_STEPS);
    if (g->o.on_step) g->o.on_step(g->user, pc, op, entry);
}
static void gb_on_read(DpMach *m, uint16_t a, uint8_t v) {
    GbWrap *g = m->owner;
    if (g->joy_armed && a == 0xFF00) g->joy_seen = 1;
    if (g->o.on_read) g->o.on_read(g->user, a, v);
}
static void gb_on_write(DpMach *m, uint16_t a, uint8_t v) {
    GbWrap *g = m->owner;
    if (g->o.on_write) g->o.on_write(g->user, a, v);
}
static void gb_attach_hooks(DpMachine *m) {
    GbWrap *g = gbw(m);
    dp_mach_attach(g->d, g, gb_on_step, gb_on_read, gb_on_write, NULL);
}
static void gb_observe(DpMachine *m, const DpObservers *o, void *user) {
    GbWrap *g = gbw(m);
    g->o = *o;
    g->user = user;
    gb_attach_hooks(m);
}
// one instruction; a halted machine with nothing to wake it reports no progress
static int gb_run(DpMachine *m) {
    GbWrap *g = gbw(m);
    g->seen = 0;
    dp_mach_run(g->d);
    return g->seen > 0;
}
static uint32_t gb_pc(DpMachine *m) { return gbw(m)->d->api->gbr->pc; }
static void gb_set_pc(DpMachine *m, uint32_t pc) { gbw(m)->d->api->gbr->pc = (uint16_t)pc; }
static void gb_poke(DpMachine *m, uint32_t a, uint8_t v) { gbw(m)->d->api->wr((uint16_t)a, v); }
static uint64_t gb_frame(DpMachine *m) { return *gbw(m)->d->api->frame; }
// kernel mask (up down left right button1 button2 start) -> GB pad (right left up down a b select start)
static void gb_set_input(DpMachine *m, uint32_t mask) {
    uint8_t pad = 0;
    if (mask & 0x01) pad |= 0x04;   // up
    if (mask & 0x02) pad |= 0x08;   // down
    if (mask & 0x04) pad |= 0x02;   // left
    if (mask & 0x08) pad |= 0x01;   // right
    if (mask & 0x10) pad |= 0x10;   // a
    if (mask & 0x20) pad |= 0x20;   // b
    if (mask & 0x40) pad |= 0x80;   // start
    gbw(m)->d->api->set_buttons(pad);
}

// --- ctx_components: what a section's behaviour can depend on ---------------------
static const DpCtxComp gb_ctx[] = {
    { "a", DP_CC_REG, 0, 1 },    { "f", DP_CC_REG, 1, 1 },    { "b", DP_CC_REG, 2, 1 },
    { "c", DP_CC_REG, 3, 1 },    { "d", DP_CC_REG, 4, 1 },    { "e", DP_CC_REG, 5, 1 },
    { "h", DP_CC_REG, 6, 1 },    { "l", DP_CC_REG, 7, 1 },    { "sp", DP_CC_REG, 8, 2 },
    { "ime", DP_CC_REG, 9, 1 },  { "ie", DP_CC_REG, 10, 1 },  { "rom bank", DP_CC_REG, 11, 2 },
    { "ram gate", DP_CC_REG, 12, 1 }, { "vram bank", DP_CC_REG, 13, 1 },
    { "wram", DP_CC_PAGES, -1, 0x2000 }, { "vram", DP_CC_PAGES, -1, 0x2000 },
    { "sram", DP_CC_PAGES, -1, 0x4000 }, { "hram", DP_CC_PAGES, -1, 0x7F },
    { "oam", DP_CC_PAGES, -1, 0xA0 },    { "io", DP_CC_PAGES, -1, 0x80 },
};
static int gb_ctx_components(DpMachine *m, const DpCtxComp **out) {
    (void)m;
    *out = gb_ctx;
    return (int)(sizeof gb_ctx / sizeof *gb_ctx);
}

// --- probes: inputs that open the other side of a data-dependent branch ------------
static void gb_probe_buttons(DpMachine *m, uint32_t v) {
    gbw(m)->d->api->set_buttons((uint8_t)v);
}
static const DpProbe gb_probe_list[] = {
    { "buttons", "joypad state (bit set = pressed: right left up down a b select start)", 256,
      gb_probe_buttons },
};
static int gb_probes(DpMachine *m, const DpProbe **out) {
    (void)m;
    *out = gb_probe_list;
    return (int)(sizeof gb_probe_list / sizeof *gb_probe_list);
}

// technique 2 on GB: an instruction that reads the joypad port ($FF00) forks one state per button
// held; the held pad stays in the saved state.  The instruction is dry-run once to see the read.
static int gb_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc; (void)bytes;
    static const uint8_t pads[] = {0x80, 0x10, 0x20, 0x40, 0x04, 0x08, 0x02, 0x01};
    if (k >= sizeof pads) return 0;
    GbWrap *g = gbw(m);
    uint8_t *pre = malloc(m->state_bytes(m));
    if (!pre) return 0;
    m->save(m, pre);
    g->joy_armed = 1; g->joy_seen = 0;
    gb_run(m);
    g->joy_armed = 0;
    int hit = g->joy_seen;
    m->load(m, pre);
    if (hit) { g->d->api->set_buttons(pads[k]); m->save(m, save); m->load(m, pre); }
    free(pre);
    return hit;
}
// IRQ fork: where the vertical blank interrupt is enabled and the master enable is on, one state
// with the VBlank request raised (delay 1 only)
static int gb_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    DpMach *d = gbw(m)->d;
    if (delay != 1 || !d->api->gbr->ime || !d->api->ie || !(*d->api->ie & 1) || (d->api->io[0x0F] & 1)) return 0;
    d->api->io[0x0F] |= 1;     // raise the request, snapshot, then lower it: the live machine is untouched
    m->save(m, save);
    d->api->io[0x0F] &= (uint8_t)~1u;
    return 1;
}
static const DpPlatformHooks gb_hooks = { .event_probe = gb_event_probe, .irq_alt = gb_irq_alt };
const DpPlatformHooks *dp_machine_gb_hooks(void) { return &gb_hooks; }

static void gb_close(DpMachine *m) {
    if (!m) return;
    dp_mach_close(gbw(m)->d);
    free(m->impl);
    free(m);
}

static DpMachine *gb_wrap(DpMach *d);
// a clone is another compiled instance of the same variant, reset to power-on
static DpMachine *gb_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    GbWrap *o = gbw(m);
    if (!o->rom) return NULL;
    for (int ii = 0; ii < DP_GBL_MACHINES; ii++) {
        DpMach *d = dp_mach_open_exclusive(o->d->vi, ii);
        if (!d) continue;
        dp_mach_set_rom(d, o->rom, (uint32_t)o->rom_size);
        dp_mach_reset(d);
        DpMachine *c = gb_wrap(d);
        if (!c) { dp_mach_close(d); return NULL; }
        gbw(c)->rom = o->rom; gbw(c)->rom_size = o->rom_size;
        c->clone = gb_clone;
        return c;
    }
    return NULL;   // every compiled instance is taken
}
void dp_machine_gb_source(DpMachine *m, const uint8_t *rom, size_t size) {
    gbw(m)->rom = rom; gbw(m)->rom_size = size;
    m->clone = gb_clone;
}

// wraps an open DpMach (ownership moves to the machine); NULL on failure
DpMachine *dp_machine_gb(DpMach *impl) { return gb_wrap(impl); }
static DpMachine *gb_wrap(DpMach *impl) {
    if (!impl) return NULL;
    DpMachine *m = calloc(1, sizeof *m);
    GbWrap *g = calloc(1, sizeof *g);
    if (!m || !g) { free(m); free(g); return NULL; }
    g->d = impl;
    m->name = "gb";
    m->plat = dp_platform_gb();
    m->isa = dp_isa_sm83();
    m->impl = g;
    m->state_bytes = gb_state_bytes;
    m->save = gb_save;
    m->load = gb_load;
    m->rom_index = gb_rom_index;
    m->fetch = gb_fetch;
    m->run = gb_run;
    m->pc = gb_pc;
    m->set_pc = gb_set_pc;
    m->poke = gb_poke;
    m->set_input = gb_set_input;
    m->frame = gb_frame;
    m->observe = gb_observe;
    m->ctx_components = gb_ctx_components;
    m->probes = gb_probes;
    m->close = gb_close;
    return m;
}
