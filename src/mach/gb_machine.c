// gb_machine.c: the m0boy runtime as a DpMachine (spec §7.2).  The explorer
// still drives DpMach directly; this wrapper is the kernel-facing view, built
// on the same instance so both see the same state.
#include <stdlib.h>

#include "../core/dpmachine.h"
#include "m0boy.h"

static uint32_t gb_state_bytes(DpMachine *m) { return dp_mach_state_bytes(m->impl); }
static void gb_save(DpMachine *m, uint8_t *buf) { dp_mach_save(m->impl, buf); }
static void gb_load(DpMachine *m, const uint8_t *buf) { dp_mach_load(m->impl, buf); }
static uint8_t gb_fetch(DpMachine *m, uint32_t a) { return dp_mach_op(m->impl, (uint16_t)a); }
static int gb_run(DpMachine *m) { return dp_mach_run(m->impl); }
// the GB window geometry is bank dependent: only the fixed bank 0 is known
// without the mapper state, so the view answers for $0000-$3FFF
static int64_t gb_rom_index(DpMachine *m, uint32_t a) {
    (void)m;
    return a < 0x4000 ? (int64_t)a : -1;
}
// --- observe: DpObservers over the engine hooks ---------------------------------
typedef struct GbObs {
    DpObservers o;
    void *user;
} GbObs;
static void gb_on_step(DpMach *m, uint16_t pc, uint8_t op, int entry) {
    GbObs *g = m->owner;
    if (g->o.on_step) g->o.on_step(g->user, pc, op, entry);
}
static void gb_on_read(DpMach *m, uint16_t a, uint8_t v) {
    GbObs *g = m->owner;
    if (g->o.on_read) g->o.on_read(g->user, a, v);
}
static void gb_on_write(DpMach *m, uint16_t a, uint8_t v) {
    GbObs *g = m->owner;
    if (g->o.on_write) g->o.on_write(g->user, a, v);
}
static GbObs gb_obs; // one observer per process: the instance owns the hooks
static void gb_observe(DpMachine *m, const DpObservers *o, void *user) {
    gb_obs.o = *o;
    gb_obs.user = user;
    dp_mach_attach(m->impl, &gb_obs, gb_on_step, gb_on_read, gb_on_write, NULL);
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
    DpMach *d = m->impl;
    d->api->set_buttons((uint8_t)v);
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

static void gb_close(DpMachine *m) {
    if (!m) return;
    dp_mach_close(m->impl);
    free(m);
}

// wraps an open DpMach (ownership moves to the machine); NULL on failure
DpMachine *dp_machine_gb(DpMach *impl) {
    if (!impl) return NULL;
    DpMachine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->name = "gb";
    m->plat = dp_platform_gb();
    m->isa = dp_isa_sm83();
    m->impl = impl;
    m->state_bytes = gb_state_bytes;
    m->save = gb_save;
    m->load = gb_load;
    m->rom_index = gb_rom_index;
    m->fetch = gb_fetch;
    m->run = gb_run;
    m->observe = gb_observe;
    m->ctx_components = gb_ctx_components;
    m->probes = gb_probes;
    m->close = gb_close;
    return m;
}
