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
    int skip;          // operand-byte reads still to ignore for the running instruction
    uint8_t dmc_addr, dmc_len;   // $4012 / $4013 as last written (B11: the DMC sample DMA is a hardware copy)
} NesMachWrap;
static DpNesMach *nes_impl(DpMachine *m) {
    NesMachWrap *w = m->impl;
    if (!w->m && w->img) { char err[64]; w->m = dp_nesmach_open(w->img, w->img_size, w->mapper, err, sizeof err); }
    return w->m;
}

static void nes_watch(DpMachine *m, int on) { dp_nesmach_watch_enable(nes_impl(m), on); }
static int nes_render(DpMachine *m, uint32_t *argb, unsigned cap, int *w, int *h) {
    if (cap < 160u * 128u) return 0;
    dp_nesmach_render(nes_impl(m), argb); *w = 160; *h = 128; return 1;
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

// instruction length of the 6502 opcode space (illegal opcodes included): the core's read hook
// reports the operand bytes too, and they are not data reads
static unsigned nes_oplen(unsigned op) {
    unsigned l = op & 0x1F;
    if (op == 0x20) return 3;
    if (op == 0x00 || op == 0x40 || op == 0x60 || l == 0x08 || l == 0x0A || l == 0x18 || l == 0x1A) return 1;
    if (l == 0x0C || l == 0x0D || l == 0x0E || l == 0x0F || l == 0x19 || l == 0x1B || l == 0x1C || l == 0x1D || l == 0x1E || l == 0x1F) return 3;
    return 2;
}
static void nes_on_step(void *user, unsigned pc, unsigned op) {
    NesMachWrap *w = user;
    w->skip = (int)nes_oplen(op) - 1;
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, pc, (uint8_t)op, 0);
}
static void nes_on_read(void *user, unsigned addr, unsigned value) {
    NesMachWrap *w = user;
    if (w->skip > 0) { w->skip--; return; }   // an operand byte of the running instruction
    if (w->obs && w->obs->on_read) w->obs->on_read(w->user, addr, (uint8_t)value);
}
static void nes_on_write(void *user, unsigned addr, unsigned value) {
    NesMachWrap *w = user;
    if (w->obs && w->obs->on_write) w->obs->on_write(w->user, addr, (uint8_t)value);
    if (addr == 0x4012) w->dmc_addr = (uint8_t)value;
    else if (addr == 0x4013) w->dmc_len = (uint8_t)value;
    else if (addr == 0x4015 && (value & 0x10) && w->obs && w->obs->on_copy) {
        uint32_t len = (uint32_t)w->dmc_len * 16u + 1u, base = 0xC000u + (uint32_t)w->dmc_addr * 64u;
        if (base + len > 0x10000u) {   // the sample address wraps from $FFFF to $8000
            uint32_t head = 0x10000u - base;
            w->obs->on_copy(w->user, base, head, DP_COPY_DMA);
            w->obs->on_copy(w->user, 0x8000u, len - head, DP_COPY_DMA);
        } else w->obs->on_copy(w->user, base, len, DP_COPY_DMA);
    }
}
static void nes_on_frame(void *user) { ((NesMachWrap *)user)->frames++; }
static uint64_t nes_frame(DpMachine *m) { return ((NesMachWrap *)m->impl)->frames; }
static void nes_observe(DpMachine *m, const DpObservers *o, void *user) {
    NesMachWrap *w = m->impl;
    w->obs = o; w->user = user;
    DpNesHooks h = { .user = w, .on_step = nes_on_step, .on_read = nes_on_read, .on_write = nes_on_write, .on_frame = nes_on_frame };
    dp_nesmach_set_hooks(nes_impl(m), &h);
}

// register seeds (music rips): the 6502 A register and stack pointer are
// directly writable (a == 0xFF / sp == 0xFFFF leave the register untouched)
static int nes_seed_regs(DpMachine *m, uint8_t a, uint16_t sp) {
    if (a != 0xFF) dp_nesmach_set_a(a);
    if (sp != 0xFFFF) dp_nesmach_set_sp(sp);
    return 1;
}
// per-bank seeding: the $8000 window shows 16 KiB block `bank` (bank 0
// included — the reset view is not assumed; a NROM has nothing to select)
static int nes_bank_view(DpMachine *m, unsigned bank) {
    return dp_nesmach_set_bank(nes_impl(m), bank) || dp_nesmach_mapper(nes_impl(m)) == 0;
}
// nonsense cut (audit E2): the PPU/APU/pad register page $2000-$401F never
// holds code; the RAM mirrors $0800-$1FFF are ordinary RAM (code lives there
// after a copy), and the 6502 stack page is always valid RAM (no stack cut)
static int nes_cut_pc(DpMachine *m, uint32_t pc) { (void)m; return pc >= 0x2000 && pc < 0x4020; }
// shared DP_DEST_* knowledge: $2007 goes to VRAM or the palette by the current
// PPU address, $4014 is the OAM DMA page, $4011 the DMC direct load
static uint8_t nes_data_dest(DpMachine *m, uint32_t a) {
    if (a == 0x2007) return dp_nesmach_ppu_v(nes_impl(m)) >= 0x3F00u ? DP_DEST_CRAM : DP_DEST_VRAM;
    if (a == 0x4014) return DP_DEST_OAM;
    if (a == 0x4011) return DP_DEST_SAMPLE;
    return DP_DEST_NONE;
}
static uint32_t nes_pc(DpMachine *m) { return dp_nesmach_pc(nes_impl(m)); }
static void nes_set_pc(DpMachine *m, uint32_t pc) { dp_nesmach_set_pc(nes_impl(m), pc); }
// kernel pad mask (bit0 up, 1 down, 2 left, 3 right, 4 button 1, 5 button 2, 6 start)
// -> NES pad 0 (A, B, Select, Start, Up, Down, Left, Right = bits 0..7)
static uint64_t nes_state_hash(DpMachine *m) {   // B5: the saved image minus cycle/frame clocks
    uint32_t sb = m->state_bytes(m);
    uint8_t *b = malloc(sb);
    if (!b) return 0;
    m->save(m, b);
    uint64_t h = dp_nesmach_state_hash(b);
    free(b);
    return h;
}
static int nes_arm_read(DpMachine *m, uint32_t addr, uint8_t value) { dp_nesmach_arm_read(nes_impl(m), (uint16_t)addr, value); return 1; }
static void nes_set_input(DpMachine *m, uint32_t k) {
    uint8_t p = 0;
    if (k & 0x10) p |= 0x01;   // A
    if (k & 0x20) p |= 0x02;   // B
    if (k & 0x40) p |= 0x08;   // Start
    if (k & 0x01) p |= 0x10;
    if (k & 0x02) p |= 0x20;
    if (k & 0x04) p |= 0x40;
    if (k & 0x08) p |= 0x80;
    if (k & DP_IN_SELECT) p |= 0x04;   // C12: Select
    uint8_t q = 0;                      // C12: port 2
    uint32_t k2 = k >> DP_IN_PORT2;
    if (k2 & 0x10) q |= 0x01;
    if (k2 & 0x20) q |= 0x02;
    if (k2 & 0x40) q |= 0x08;
    if (k2 & 0x80) q |= 0x04;
    if (k2 & 0x01) q |= 0x10;
    if (k2 & 0x02) q |= 0x20;
    if (k2 & 0x04) q |= 0x40;
    if (k2 & 0x08) q |= 0x80;
    dp_nesmach_set_input(nes_impl(m), p, q);
}
const uint8_t *dp_machine_nes_prg(DpMachine *m) { return dp_nesmach_prg_rom(nes_impl(m)); }
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
    m->work_ram_base = 0; m->work_ram_len = 0x800u;
    m->bank_shift = 14; m->code_base = 0x8000u; m->fixed_high = 1;   // C3/B8: 16 KiB PRG windows, the last bank fixed high
    m->save_ram_base = 0x6000u; m->save_ram_len = 0x2000u;
    m->isa = dp_isa_m6502();
    m->pc = nes_pc;
    m->set_pc = nes_set_pc;
    m->set_input = nes_set_input;
    m->arm_read = nes_arm_read;
    m->state_hash = nes_state_hash;
    m->frame = nes_frame;
    m->watch = nes_watch; m->render = nes_render;
    m->impl = w;             // the m6502 DpIsa wrapper is not extracted yet
    m->state_bytes = nes_state_bytes;
    m->save = nes_save;
    m->load = nes_load;
    m->fetch = nes_fetch;
    m->poke = nes_poke;
    m->rom_index = nes_rom_index;
    m->seed_regs = nes_seed_regs;
    m->bank_view = nes_bank_view;
    m->cut_pc = nes_cut_pc;
    m->data_dest = nes_data_dest;
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
    (void)pc;
    static const uint8_t st[] = { 0x80, 0x40, 0x20, 0xC0, 0xA0, 0xE0, 0x00 };
    DpNesMach *n = nes_impl(m);
    if ((bytes[0] == 0xAD || bytes[0] == 0xBD || bytes[0] == 0x8D || bytes[0] == 0x8E || bytes[0] == 0x8C) && bytes[1] == 0x16 && bytes[2] == 0x40) {   // read or strobe write of the controller port
        // controller read (LDA $4016 / LDA $4016,X): one button held from here on per alternative (as the Sega pad probe)
        static const uint32_t pads[] = { 0x10, 0x20, 0x40, 0x01, 0x02, 0x04, 0x08, 0x80 };   // C12: + Select
        int full = user != NULL;   // --pad-full (D10): every one of the 256 port-1 masks at each strobe site
        if (k >= (full ? 256u : sizeof pads / sizeof *pads)) return 0;
        uint8_t *tmp = malloc(m->state_bytes(m));
        if (!tmp) return 0;
        m->save(m, tmp);
        m->set_input(m, full ? (uint32_t)k : pads[k]);
        m->save(m, save);
        m->load(m, tmp);
        free(tmp);
        return 1;
    }
    if (k >= sizeof st) return 0;
    if (!((bytes[0] == 0xAD || bytes[0] == 0x2C) && bytes[1] == 0x02 && bytes[2] == 0x20)) return 0;
    if (dp_nesmach_nmi_pending(n)) return 0;
    // C14: arm the next $2002 read to return st[k] (the armed state is part of the save), then restore
    uint32_t sb = m->state_bytes(m);
    uint8_t *tmp = malloc(sb);
    if (!tmp) return 0;
    m->save(m, tmp);
    nes_arm_read(m, 0x2002, st[k]);
    m->save(m, save);
    m->load(m, tmp);
    free(tmp);
    return 1;
}
// techniques 9 / IRQ fork: with an NMI edge or the mapper IRQ line (MMC3 scanline
// counter) pending, the state where the service is held off `delay` instructions
// (the edge/line is cleared for those instructions, then raised again)
static int nes_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    DpNesMach *n = nes_impl(m);
    int nmi = dp_nesmach_nmi_pending(n);
    if (!nmi && !dp_nesmach_irq_pending(n)) return 0;
    uint32_t sb = m->state_bytes(m);
    uint8_t *tmp = malloc(sb), *off = malloc(sb);
    if (!tmp || !off) { free(tmp); free(off); return 0; }
    m->save(m, tmp);
    memcpy(off, tmp, sb);
    *dp_nesmach_state_nmi_edge(off) = 0;
    *dp_nesmach_state_irq_line(off) = 0;
    m->load(m, off);
    int ok = 1;
    for (unsigned i = 0; i < delay && ok; i++) ok = m->run(m);
    if (ok) {
        m->save(m, save);
        *dp_nesmach_state_nmi_edge(save) = (uint8_t)nmi;
        *dp_nesmach_state_irq_line(save) = (uint8_t)!nmi;   // the NMI edge outranks: only one line re-raised
    }
    m->load(m, tmp);
    free(tmp); free(off);
    return ok;
}
const DpPlatformHooks *dp_machine_nes_hooks_full(int pad_full) {
    static const DpPlatformHooks h = { .event_probe = nes_event_probe, .irq_alt = nes_irq_alt };
    static const DpPlatformHooks hf = { .user = (void *)1, .event_probe = nes_event_probe, .irq_alt = nes_irq_alt };
    return pad_full ? &hf : &h;
}
const DpPlatformHooks *dp_machine_nes_hooks(void) { return dp_machine_nes_hooks_full(0); }
