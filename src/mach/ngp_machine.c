// ngp_machine.c: the Neo Geo Pocket m0boy runtime as a DpMachine (spec §7.2).
// The kernel-facing wrapper over the bridge (src/platform/ngp/ngpmach.c):
// the same thread-local machine, the generic geometry of a 24-bit flat bus
// whose ROM image maps at $200000/$800000.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "../explore/core.h"
#include "../cpu/tlcs900/tlcs900.h"
#include "../platform/ngp/ngpmach.h"
#include "../platform/ngp/ngp_platform.h"
#include "ngp.h"     // the m0boy runtime's machine global (thread-local here)

typedef struct {
    const DpObservers *obs;
    void *user;
    const uint8_t *rom; size_t rom_size;   // for clone()
} NgpWrap;

// C13: the dense RAM index: the 16 KiB of $4000-$7FFF (work RAM + the sound
// Z80's shared 4 KiB) -> 0..$3FFF
static int64_t ngp_ram_index(DpMachine *m, uint32_t a) {
    (void)m;
    a &= 0xFFFFFFu;
    return (a >= 0x4000u && a < 0x8000u) ? (int64_t)(a - 0x4000) : -1;
}
static uint32_t ngp_ram_addr(DpMachine *m, uint32_t i) { (void)m; return 0x4000u + i; }

static const uint8_t *ngp_ram_view(DpMachine *m) { (void)m; return machine.ram; }

static uint32_t ngp_wrap_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_ngp_state_size(); }
static void ngp_save(DpMachine *m, uint8_t *buf) { (void)m; dp_ngp_save(buf); }
static void ngp_load(DpMachine *m, const uint8_t *buf) { (void)m; dp_ngp_load(buf); }

static uint8_t ngp_fetch(DpMachine *m, uint32_t a) { (void)m; return dp_ngp_peek(a); }

static void ngp_fetch_n(DpMachine *m, uint32_t a, uint8_t *o, unsigned n) {
    (void)m;
    for (unsigned i = 0; i < n; i++) o[i] = dp_ngp_peek(a + i);
}

// the wide vector table: the 18 RAM slots at $6FB8 the BIOS fills with its
// RETI stubs — a game-written ROM handler is a forced start (G1); at seed
// time the slots hold BIOS addresses, so the seeds come up empty and the IRQ
// paths are explored through the hooks' forks instead
static unsigned ngp_vector_seeds(DpMachine *m, uint32_t *out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < 18 && n < max; i++) {
        uint32_t pc = (uint32_t)dp_ngp_peek(0x6FB8u + i * 4) |
                      ((uint32_t)dp_ngp_peek(0x6FB9u + i * 4) << 8) |
                      ((uint32_t)dp_ngp_peek(0x6FBAu + i * 4) << 16);
        if (!pc || (pc & 0xFF0000u) != 0x200000u && (pc & 0xFF0000u) != 0x800000u) continue;
        unsigned dup = 0;
        for (unsigned j = 0; j < n; j++) dup |= out[j] == pc;
        if (!dup) out[n++] = pc;
    }
    return n;
}

static int64_t ngp_rom_index(DpMachine *m, uint32_t a) {
    (void)m;
    uint32_t off = dp_ngp_rom_offset(a);
    return off == 0xFFFFFFFFu ? -1 : (int64_t)off;
}

static int ngp_run(DpMachine *m) { (void)m; return dp_ngp_step(); }

static uint32_t ngp_pc(DpMachine *m) { (void)m; return dp_ngp_pc(); }
static void ngp_set_pc(DpMachine *m, uint32_t pc) { (void)m; dp_ngp_set_pc(pc); }

// the fork: a TLCS-900H condition is a combination of the S/Z/V/C flags — the
// inversion rewrites SR so the negated condition holds.  F/T test no flag
// (their truth is constant) and DJNZ's counter is not addressable here: both
// return 0, the fork is skipped (never silent: the site just stays uncapped).
static int ngp_invert_condition(DpMachine *m, int condition) {
    (void)m;
    const uint8_t F_S = 0x80, F_Z = 0x40, F_V = 0x04, F_C = 0x01;
    uint8_t f = (uint8_t)(machine.cpu.sr & 0xFF);
    switch (condition) {
        case 1: case 9: f ^= F_V; break;                          // LT <-> GE: V flipped
        case 2: case 10:                                          // LE -> GT: Z cleared, S^V forced 0
            f = (uint8_t)((f & ~(F_Z | F_V)) | (f & F_S)); break;
        case 3: case 11: f = (uint8_t)(f & ~(F_Z | F_C)); break;  // ULE -> UGT
        case 4: case 12: f ^= F_V; break;                         // OV <-> NOV
        case 5: case 13: f ^= F_S; break;                         // MI <-> PL
        case 6: case 14: f ^= F_Z; break;                         // EQ <-> NE
        case 7: case 15: f ^= F_C; break;                         // ULT <-> UGE
        default: return 0;                                        // F, T, DJNZ
    }
    machine.cpu.sr = (uint16_t)((machine.cpu.sr & 0xFF00) | f);
    return 1;
}

// register seeds (music rips): the convention is the song number in A and the
// rip's stack in XSP (a == 0xFF / sp == 0xFFFF leave the register untouched)
static int ngp_seed_regs(DpMachine *m, uint8_t a, uint16_t sp) {
    (void)m;
    if (a != 0xFF) machine.cpu.bank[(machine.cpu.sr >> 8) & 3][0] =
        (machine.cpu.bank[(machine.cpu.sr >> 8) & 3][0] & 0xFFFFFF00u) | a;
    if (sp != 0xFFFF) machine.cpu.gpr[3] = sp;
    return 1;
}

// nonsense cuts (audit E2): the I/O page and the unmapped gaps never hold code;
// the stack lives in work RAM — anything else pops garbage
static int ngp_cut_pc(DpMachine *m, uint32_t pc) {
    (void)m;
    pc &= 0xFFFFFFu;
    return !((pc >= 0x4000u && pc < 0x8000u) || (pc >= 0x200000u && pc < 0x400000u) ||
             (pc >= 0x800000u && pc < 0xA00000u) || (pc & 0xFF0000u) == 0xFF0000u);
}
static int ngp_cut_sp(DpMachine *m) {
    uint32_t sp = dp_ngp_sp();
    return sp < 0x4000u || sp >= 0x8000u;
}

// shared DP_DEST_* knowledge: the K2GE's RAMs and the PSG
static uint8_t ngp_data_dest(DpMachine *m, uint32_t a) {
    (void)m;
    a &= 0xFFFFFFu;
    if (a >= 0x00A0u && a <= 0x00A3u) return DP_DEST_MUSIC;
    if (a >= 0x008200u && a <= 0x0083FFu) return DP_DEST_CRAM;
    if (a >= 0x008800u && a <= 0x0088FFu) return DP_DEST_OAM;
    if (a >= 0x008C00u && a <= 0x008C3Fu) return DP_DEST_CRAM;
    if (a >= 0x009000u && a <= 0x009FFFu) return DP_DEST_VRAM;
    if (a >= 0x00A000u && a <= 0x00BFFFu) return DP_DEST_VRAM;
    return DP_DEST_NONE;
}

static uint64_t ngp_state_hash(DpMachine *m) { (void)m; return dp_ngp_state_hash(); }
static uint64_t ngp_ctx_reg_hash(DpMachine *m) { (void)m; return dp_ngp_ctx_hash(); }
static uint64_t ngp_wrap_frame(DpMachine *m) { (void)m; return dp_ngp_frame(); }

static void ngp_set_input(DpMachine *m, uint32_t mask) { (void)m; dp_ngp_set_input(mask); }

static void ngp_poke(DpMachine *m, uint32_t a, uint8_t v) { (void)m; dp_ngp_poke(a, v); }

static void ngp_observe(DpMachine *m, const DpObservers *o, void *user) {
    NgpWrap *w = m->impl;
    w->obs = o; w->user = user;
    dp_ngp_observe(o, user);
}

// C3: the concrete 32-bit registers (the current bank's view and the globals,
// plus the four banks in slots 8-19; every byte known)
static int ngp_get_regs(DpMachine *m, DpRegState *out) {
    (void)m;
    dp_regstate_clear(out);
    for (unsigned i = 0; i < 8; i++) {
        out->v[DP_TLCSR_WA + i] = dp_ngp_reg(i);
        out->known[DP_TLCSR_WA + i] = 0x0F;
    }
    for (unsigned b = 0; b < 4; b++)
        for (unsigned r = 0; r < 4; r++) {
            unsigned slot = DP_TLCSR_BANK0 + b * 4 + r;
            if (slot >= DP_REG_N) continue;
            out->v[slot] = machine.cpu.bank[b][r];
            out->known[slot] = 0x0F;
        }
    return 1;
}

// the context components: the register state a section can depend on and the
// RAM pages it may read
static const DpCtxComp ngp_ctx[] = {
    { "wa", DP_CC_REG, 0, 4 }, { "bc", DP_CC_REG, 1, 4 }, { "de", DP_CC_REG, 2, 4 }, { "hl", DP_CC_REG, 3, 4 },
    { "ix", DP_CC_REG, 4, 4 }, { "iy", DP_CC_REG, 5, 4 }, { "iz", DP_CC_REG, 6, 4 }, { "sp", DP_CC_REG, 7, 4 },
    { "sr", DP_CC_REG, 8, 2 },
    { "wram", DP_CC_PAGES, -1, 0x3000 }, { "z80 ram", DP_CC_PAGES, -1, 0x1000 },
    { "scroll", DP_CC_PAGES, -1, 0x1000 }, { "character", DP_CC_PAGES, -1, 0x2000 },
    { "sprites", DP_CC_PAGES, -1, 0x100 }, { "palette", DP_CC_PAGES, -1, 0x200 },
};
static int ngp_ctx_components(DpMachine *m, const DpCtxComp **out) {
    (void)m;
    *out = ngp_ctx;
    return (int)(sizeof ngp_ctx / sizeof *ngp_ctx);
}

static unsigned ngp_audio_read(DpMachine *m, int16_t *st, unsigned max) { (void)m; return dp_ngp_audio_read(st, max); }
static int ngp_render(DpMachine *m, uint32_t *argb, unsigned cap, int *w, int *h) { (void)m; return (int)dp_ngp_render(argb, cap, w, h); }

// technique 2 on NGP: an instruction whose BIOS pad call lands the buttons in
// the workspace byte ($6F82) forks one state per pad held; the held pad stays
// in the saved state (the game's next reads see it)
static int ngp_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc; (void)bytes;
    static const uint32_t pads[] = { 0, NGP_BTN_A, NGP_BTN_B, NGP_BTN_A | NGP_BTN_B,
                                     NGP_BTN_UP, NGP_BTN_DOWN, NGP_BTN_LEFT, NGP_BTN_RIGHT,
                                     NGP_BTN_OPTION };
    unsigned nfull = m->pad_full ? dp_pad_count(m->input_bits) : 0;
    if (k >= (nfull ? nfull : sizeof pads / sizeof *pads)) return 0;
    uint8_t *pre = malloc(ngp_wrap_state_bytes(m));
    if (!pre) return 0;
    m->save(m, pre);
    dp_ngp_pad_arm(1);
    int progress = ngp_run(m);
    int hit = dp_ngp_pad_seen();
    dp_ngp_pad_arm(0);
    m->load(m, pre);
    free(pre);
    if (!progress || !hit) return 0;
    pre = malloc(ngp_wrap_state_bytes(m));
    if (!pre) return 0;
    m->save(m, pre);
    if (nfull) dp_ngp_set_input(dp_pad_subset(m->input_bits, k));
    else dp_ngp_set_input(pads[k]);
    ngp_run(m);   // the probe instruction runs again, this time with the pad held
    m->save(m, save);
    m->load(m, pre);
    free(pre);
    return 1;
}

// IRQ forks: where the game has unmasked the sources, one state with the
// V-int pending (delay 1) or a timer tick pending (delay 2) — the runtime's
// own controller delivers it on the next tick
static int ngp_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    if (delay > 2) return 0;
    uint8_t mask = (uint8_t)((machine.cpu.sr >> 12) & 7);
    if (mask == 7) return 0;                        // everything masked at power-on
    if (delay == 1 && !(machine.gfx.control_int & 0x80)) return 0;   // the V-int enable
    m->save(m, save);
    dp_ngp_state_set_int(save, delay == 1 ? NGP_INT_VBLANK : NGP_INT_TIMER0);
    return 1;
}

static const DpPlatformHooks ngp_hooks = { .event_probe = ngp_event_probe, .irq_alt = ngp_irq_alt };
const DpPlatformHooks *dp_machine_ngp_hooks(void) { return &ngp_hooks; }

// the runtime is thread-local: a clone is a shell until its worker attaches
static int ngp_attach(DpMachine *m) {
    NgpWrap *w = m->impl;
    if (!w->rom) return 0;
    return dp_ngp_init(w->rom, w->rom_size);
}
static DpMachine *ngp_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    NgpWrap *o = m->impl;
    if (!o->rom) return NULL;
    DpMachine *c = dp_machine_ngp();
    if (!c) return NULL;
    dp_machine_ngp_source(c, o->rom, o->rom_size);
    return c;
}
void dp_machine_ngp_source(DpMachine *m, const uint8_t *rom, size_t size) {
    NgpWrap *w = m->impl;
    w->rom = rom; w->rom_size = size;
    m->attach = ngp_attach;   // every worker (the root first) boots its thread-local machine here
    m->clone = ngp_clone;
}

static void ngp_close(DpMachine *m) {
    free(m->impl);
    free(m);
}

DpMachine *dp_machine_ngp(void) {
    NgpWrap *w = calloc(1, sizeof *w);
    DpMachine *m = calloc(1, sizeof *m);
    if (!w || !m) { free(w); free(m); return NULL; }
    m->name = "ngp";
    m->plat = dp_ngp_platform();
    m->isa = dp_isa_tlcs900();
    m->work_ram_base = 0x4000u; m->work_ram_len = 0x3000u;
    m->save_ram_base = 0x3F0000u; m->save_ram_len = 0x10000u;   // the 2 MiB carts' save window
    m->ram_index = ngp_ram_index; m->ram_addr = ngp_ram_addr; m->ram_size = 0x4000u;
    m->ram_view = ngp_ram_view; m->ram_view_len = 0x4000u;
    m->input_bits = DP_IN_UP | DP_IN_DOWN | DP_IN_LEFT | DP_IN_RIGHT | DP_IN_B1 | DP_IN_B2 | DP_IN_START;
    m->impl = w;
    m->state_bytes = ngp_wrap_state_bytes;
    m->save = ngp_save;
    m->load = ngp_load;
    m->fetch = ngp_fetch;
    m->fetch_n = ngp_fetch_n;
    m->vector_seeds = ngp_vector_seeds;
    m->poke = ngp_poke;
    m->rom_index = ngp_rom_index;
    m->linear = 1;                // the cov offsets are the image offsets
    m->code_base = 0x200000u;     // the cartridge floor
    m->run = ngp_run;
    m->pc = ngp_pc;
    m->set_pc = ngp_set_pc;
    m->invert_condition = ngp_invert_condition;
    m->seed_regs = ngp_seed_regs;
    m->cut_pc = ngp_cut_pc;
    m->cut_sp = ngp_cut_sp;
    m->data_dest = ngp_data_dest;
    m->state_hash = ngp_state_hash;
    m->ctx_reg_hash = ngp_ctx_reg_hash;
    m->frame = ngp_wrap_frame;
    m->set_input = ngp_set_input;
    m->observe = ngp_observe;
    m->ctx_components = ngp_ctx_components;
    m->get_regs = ngp_get_regs;
    m->audio_read = ngp_audio_read;
    m->render = ngp_render;
    m->close = ngp_close;
    return m;
}
