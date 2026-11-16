// md_machine.c: the Mega Drive m0boy runtime as a DpMachine (spec §7.2).
// The explorer still drives DpMdMach directly; this wrapper is the
// kernel-facing view, built on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "mdmach.h"
#include "../explore/core.h"
#include "../platform/md/md_registry.h"

typedef struct {
    DpMdMach *m;
    const DpObservers *obs;
    void *user;
    const uint8_t *rom; size_t rom_size;   // for clone()
    uint64_t frames;   // completed frames (relative clock for sessions)
    int pad_armed, pad_seen;   // pad-port read detection for the event probe
} MdMachWrap;

// C13: dense RAM index: 68000 work RAM $FF0000-$FFFFFF -> 0..$FFFF, Z80 bus RAM $A00000-$A0FFFF -> $10000..$1FFFF
static int64_t md_ram_index(DpMachine *m, uint32_t a) {
    (void)m; a &= 0xFFFFFFu;
    if (a >= 0xFF0000u) return (int64_t)(a & 0xFFFFu);
    if (a >= 0xA00000u && a < 0xA10000u) return (int64_t)(0x10000u + (a & 0xFFFFu));
    return -1;
}
static uint32_t md_ram_addr(DpMachine *m, uint32_t i) { (void)m; return i < 0x10000u ? 0xFF0000u + i : 0xA00000u + (i - 0x10000u); }

static uint32_t md_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_mdmach_state_size() + 8; }
static void md_save(DpMachine *m, uint8_t *buf) { dp_mdmach_save(((MdMachWrap *)m->impl)->m, buf); memcpy(buf + dp_mdmach_state_size(), &((MdMachWrap *)m->impl)->frames, 8); }
static void md_load(DpMachine *m, const uint8_t *buf) { dp_mdmach_load(((MdMachWrap *)m->impl)->m, buf); memcpy(&((MdMachWrap *)m->impl)->frames, buf + dp_mdmach_state_size(), 8); }
// the byte at the address: 68000 is big-endian, peek32 returns the aligned word
static uint8_t md_fetch(DpMachine *m, uint32_t a) {
    if ((a & 0xFFFFFFu) >= 0xFF0000u) return dp_mdmach_ram8(((MdMachWrap *)m->impl)->m, a);   // work RAM (code run from RAM, learned bytes)
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
        for (unsigned k = 0; k < size; k++) {   // A1: the byte actually read (peek, no side effect), not 0
            uint32_t a = addr + k;
            uint8_t v = (a & 0xFFFFFFu) >= 0xFF0000u ? dp_mdmach_ram8(w->m, a)
                      : (uint8_t)(dp_mdmach_peek32(w->m, a & ~3u) >> (24 - 8 * (a & 3u)));
            w->obs->on_read(w->user, a, v);
        }
}
static void md_on_write(void *user, uint32_t addr, unsigned size, uint32_t value) {
    MdMachWrap *w = user;
    if (w->obs && w->obs->on_write)
        for (unsigned k = 0; k < size; k++) w->obs->on_write(w->user, addr + k, (uint8_t)(value >> (8 * (size - 1 - k))));   // big-endian slice of a size-byte value (was a fixed 24-bit shift: wrong for byte/word writes)
}
static void md_on_dma_read(void *user, uint32_t addr, unsigned size) {
    MdMachWrap *w = user;
    if (w->obs && w->obs->on_copy) w->obs->on_copy(w->user, addr, size, DP_COPY_DMA);
}
static void md_on_frame(void *user) { ((MdMachWrap *)user)->frames++; }
static int md_render(DpMachine *m, uint32_t *argb, unsigned cap, int *w, int *h) {
    if (cap < 320u * 224u) return 0;
    dp_mdmach_render(((MdMachWrap *)m->impl)->m, argb); *w = 320; *h = 224; return 1;
}
static uint64_t md_frame(DpMachine *m) { return ((MdMachWrap *)m->impl)->frames; }
static void md_observe(DpMachine *m, const DpObservers *o, void *user) {
    MdMachWrap *w = m->impl;
    w->obs = o; w->user = user;
    DpMdHooks h = { .user = w, .on_read = md_on_read, .on_write = md_on_write, .on_frame = md_on_frame, .on_dma_read = md_on_dma_read };
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

// register seeds (music rips): the convention is the song number in D0 and the
// rip's stack in A7; the 68000 registers live in the saved state, so the same
// round-trip carries them (a == 0xFF / sp == 0xFFFF leave the register untouched)
static int md_seed_regs(DpMachine *m, uint8_t a, uint16_t sp) {
    MdMachWrap *w = m->impl;
    uint8_t *buf = malloc(dp_mdmach_state_size());
    if (!buf) return 0;
    dp_mdmach_save(w->m, buf);
    if (a != 0xFF) dp_mdmach_state_set_dn_word(buf, 0, a);
    if (sp != 0xFFFF) dp_mdmach_state_set_an(buf, 7, sp);
    dp_mdmach_load(w->m, buf);
    free(buf);
    return 1;
}
// nonsense cut (audit E2): the 68000 stack lives in work RAM $FF0000-$FFFFFF —
// an SP below that pops ROM or bus garbage (no echo/I-O pc cut: the MD maps
// nothing a path could reasonably execute into and call code)
static int md_cut_sp(DpMachine *m) { return dp_mdmach_sp(((MdMachWrap *)m->impl)->m) < 0xFF0000u; }
// shared DP_DEST_* knowledge: the PSG ($C00011) is fed music data, the VDP
// data port ($C00000) writes graphics memory (the CRAM/VSRAM distinction
// needs the VDP address register state, not exposed by the runtime: VRAM)
static uint8_t md_data_dest(DpMachine *m, uint32_t a) {
    (void)m;
    if ((a & 0xFFFFFFu) == 0xC00011u) return DP_DEST_MUSIC;
    if ((a & 0xFFFFFFu) >= 0xC00000u && (a & 0xFFFFFFu) <= 0xC00003u) return DP_DEST_VRAM;
    if ((a & 0xFFFFFFu) >= 0x200000u && (a & 0xFFFFFFu) <= 0x20FFFFu) return DP_DEST_SRAM;
    return DP_DEST_NONE;
}

// kernel mask (DpInput) -> MD pad bits (0-3 directions, 16 A, 32 B, 64 C, 128 Start); C12: A and C are distinct
// buttons and X/Y/Z/Mode are carried to the machine (bits 8..11 of the pad word) when it models the six-button pad
static uint64_t md_state_hash(DpMachine *m) { return dp_mdmach_state_hash(((MdMachWrap *)m->impl)->m); }   // B5
static void md_set_input(DpMachine *m, uint32_t mask) {
    uint8_t pad = (uint8_t)(mask & 0x0F);
    if (mask & DP_IN_B1) pad |= 16;
    if (mask & DP_IN_B2) pad |= 32;
    if (mask & DP_IN_C) pad |= 64;
    if (mask & DP_IN_START) pad |= 128;
    dp_mdmach_set_pad(((MdMachWrap *)m->impl)->m, pad);
    uint8_t ext = 0;   // X/Y/Z/Mode plug the six-button pad in (bit 7); 0 keeps the three-button pad
    if (mask & DP_IN_Z) ext |= 1;
    if (mask & DP_IN_Y) ext |= 2;
    if (mask & DP_IN_X) ext |= 4;
    if (mask & DP_IN_MODE) ext |= 8;
    dp_mdmach_set_pad_ext(((MdMachWrap *)m->impl)->m, ext ? (uint8_t)(0x80 | ext) : 0);
}

// technique 2 on MD: an instruction that reads the pad data port ($A10003/$A10005) forks one
// state per pad held (Start, A+C, B, directions); the held pad stays in the saved state, so the
// game's next reads see it.  The instruction is dry-run once to see whether it touches the port.
static int md_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    static const uint32_t pads[] = {0x40, 0x110, 0x20, 0x08, 0x04, 0x01, 0x02, 0x10, 0x100};   // C12: A+C, then A and C alone
    static const uint16_t vdp[] = {0x3400, 0x3408, 0x3404, 0x3402, 0x340C};   // VDP status: idle, VBlank, HBlank, DMA busy, VBlank+HBlank
    MdMachWrap *w = m->impl;
    // `move.w $C00004,Dn` or `move.w (An),Dn` with An at the VDP control port: one state per status value,
    // the destination data register overwritten in the state saved after the read
    unsigned op = (unsigned)bytes[0] << 8 | bytes[1];
    int dn = -1;
    if ((op & 0xF1FF) == 0x3039 && bytes[2] == 0x00 && bytes[3] == 0xC0 && bytes[4] == 0x00 && (bytes[5] == 0x04 || bytes[5] == 0x06)) dn = (int)((op >> 9) & 7);
    else if ((op & 0xF1F8) == 0x3010) { uint32_t a = dp_mdmach_an(w->m, op & 7); if (a == 0xC00004u || a == 0xC00006u) dn = (int)((op >> 9) & 7); }
    if (dn >= 0) {
        if (k >= sizeof vdp / sizeof *vdp) return 0;
        uint8_t *pre = malloc(md_state_bytes(m));
        if (!pre) return 0;
        m->save(m, pre);
        md_run(m);
        m->save(m, save);
        dp_mdmach_state_set_dn_word(save, (unsigned)dn, vdp[k]);
        m->load(m, pre);
        free(pre);
        return 1;
    }
    uint32_t nfull = m->pad_full ? dp_pad_count(m->input_bits | (m->pad_full > 1 ? m->input_bits_ext : 0)) : 0;
    if (k >= (nfull ? nfull : sizeof pads / sizeof *pads)) return 0;
    uint8_t *pre = malloc(md_state_bytes(m));
    if (!pre) return 0;
    m->save(m, pre);
    w->pad_armed = 1; w->pad_seen = 0;
    md_run(m);
    w->pad_armed = 0;
    int hit = w->pad_seen;
    m->load(m, pre);
    if (hit) { md_set_input(m, nfull ? dp_pad_subset(m->input_bits | (m->pad_full > 1 ? m->input_bits_ext : 0), k) : pads[k]); m->save(m, save); m->load(m, pre); }
    free(pre);
    return hit;
}
// IRQ fork (technique 2): where the vertical interrupt is enabled and unmasked, one state with
// the interrupt raised (the old engine's irq_forks); delay 1 only
// IRQ forks: delay 1 synthesizes the V-int at this site (the game has frame
// interrupts enabled), delay 2 the H-int (the line counter of VDP reg 10); the
// runtime raises both naturally on its own lines, the fork adds the variant
// where the interrupt lands right here (technique 9's site-local form)
static int md_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    MdMachWrap *w = m->impl;
    if (delay > 2) return 0;
    unsigned reg1 = dp_mdmach_vdp_reg(w->m, 1);
    if (((dp_mdmach_sr(w->m) >> 8) & 7u) >= 6u) return 0;   // IPL masks everything
    if (delay == 1 && !(reg1 & 0x20)) return 0;             // V-int enable
    if (delay == 2 && !(reg1 & 0x10)) return 0;             // H-int enable
    m->save(m, save);
    dp_mdmach_state_set_irq(save, delay == 1, delay == 2);
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
// ctx_components: the 68000 state a section can depend on (data/address registers, SR) and the RAM pages
static const DpCtxComp md_ctx[] = {
    { "d0", DP_CC_REG, 0, 4 }, { "d1", DP_CC_REG, 1, 4 }, { "d2", DP_CC_REG, 2, 4 }, { "d3", DP_CC_REG, 3, 4 }, { "d4", DP_CC_REG, 4, 4 }, { "d5", DP_CC_REG, 5, 4 }, { "d6", DP_CC_REG, 6, 4 }, { "d7", DP_CC_REG, 7, 4 }, { "a0", DP_CC_REG, 8, 4 }, { "a1", DP_CC_REG, 9, 4 }, { "a2", DP_CC_REG, 10, 4 }, { "a3", DP_CC_REG, 11, 4 }, { "a4", DP_CC_REG, 12, 4 }, { "a5", DP_CC_REG, 13, 4 }, { "a6", DP_CC_REG, 14, 4 }, { "sp", DP_CC_REG, 15, 4 }, { "sr", DP_CC_REG, 16, 2 }, { "mapper bank", DP_CC_REG, 17, 2 },
    { "work ram", DP_CC_PAGES, -1, 0x10000 }, { "vram", DP_CC_PAGES, -1, 0x10000 }, { "z80 ram", DP_CC_PAGES, -1, 0x2000 },
};
static int md_ctx_components(DpMachine *m, const DpCtxComp **out) {
    (void)m;
    *out = md_ctx;
    return (int)(sizeof md_ctx / sizeof *md_ctx);
}

DpMachine *dp_machine_md(DpMdMach *impl) {
    MdMachWrap *w = calloc(1, sizeof *w);
    DpMachine *m = calloc(1, sizeof *m);
    if (!w || !m) { free(w); free(m); return NULL; }
    w->m = impl;
    m->name = "md";
    m->work_ram_base = 0xFF0000u; m->work_ram_len = 0x10000u;
    m->ram_index = md_ram_index; m->ram_addr = md_ram_addr; m->ram_size = 0x20000u;
    m->input_bits = 0x17F; m->input_bits_ext = DP_IN_X | DP_IN_Y | DP_IN_Z | DP_IN_MODE;   // d-pad, A B C, Start; the six-button keys
    m->impl = w;             // the 68000 DpIsa wrapper is not extracted yet
    m->state_bytes = md_state_bytes;
    m->save = md_save;
    m->load = md_load;
    m->fetch = md_fetch;
    m->poke = md_poke;
    m->rom_index = md_rom_index;
    m->linear = 1;
    m->run = md_run;
    m->observe = md_observe;
    m->close = md_close;
    m->isa = dp_isa_68000();
    m->ctx_components = md_ctx_components;
    m->pc = md_pc;
    m->set_pc = md_set_pc;
    m->set_input = md_set_input;
    m->state_hash = md_state_hash;
    m->seed_regs = md_seed_regs;   // no 68000 mapper: bank_view stays NULL (sans objet)
    m->cut_sp = md_cut_sp;
    m->data_dest = md_data_dest;
    m->frame = md_frame;
    m->render = md_render;   // the VDP renders from its state on demand: no watch switch
    m->plat = dp_md_platform();
    return m;
}

