// ws_machine.c: the WonderSwan m0boy runtime as a DpMachine (spec §7.2).
// The kernel-facing wrapper over the bridge (src/platform/ws/wsmach.c): a
// flat 20-bit linear bus whose ROM image maps through the bank registers at
// $20000 and up; the machine owns one heap runtime per instance, so clones
// are plain new machines.
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "../core/dpmachine.h"
#include "../explore/core.h"
#include "../cpu/v30mz/v30mz.h"
#include "../platform/ws/wsmach.h"
#include "../platform/ws/ws_platform.h"
#include "ws.h"

typedef struct {
    const DpObservers *obs;
    void *user;
    const uint8_t *rom; size_t rom_size;   // for clone()
    DpWs *d;
} WsWrap;

static DpWs *ws_of(DpMachine *m) { return ((WsWrap *)m->impl)->d; }

// C13: the dense RAM index: the 64 KiB RAM page ($00000-$0FFFF)
static int64_t ws_ram_index(DpMachine *m, uint32_t a) {
    (void)m;
    a &= 0xFFFFFu;
    return a < 0x10000u ? (int64_t)a : -1;
}
static uint32_t ws_ram_addr(DpMachine *m, uint32_t i) { (void)m; return i; }

static const uint8_t *ws_ram_view(DpMachine *m) {
    DpWs *d = ws_of(m);
    return ws_of(m) ? d->ws.ram : NULL;
}

static uint32_t ws_wrap_state_bytes(DpMachine *m) { (void)m; return (uint32_t)sizeof(Ws); }

static void ws_save(DpMachine *m, uint8_t *buf) { dp_ws_save(ws_of(m), buf); }
static void ws_load(DpMachine *m, const uint8_t *buf) { dp_ws_load(ws_of(m), buf); }
static int ws_invert(DpMachine *m, int cond) { return dp_ws_invert_condition(ws_of(m), cond); }
static int ws_get_regs_w(DpMachine *m, DpRegState *out) { return dp_ws_get_regs(ws_of(m), out); }

static uint8_t ws_fetch(DpMachine *m, uint32_t a) { return dp_ws_peek(ws_of(m), a); }

static void ws_fetch_n(DpMachine *m, uint32_t a, uint8_t *o, unsigned n) {
    for (unsigned i = 0; i < n; i++) o[i] = dp_ws_peek(ws_of(m), a + i);
}

// the wide vector table: the IRQ vectors sit at (vector*4) in segment 0,
// offset word first, segment word second — the handlers are linear targets
static unsigned ws_vector_seeds(DpMachine *m, uint32_t *out, unsigned max) {
    DpWs *d = ws_of(m);
    unsigned n = 0;
    uint32_t base = (uint32_t)(dp_ws_peek(d, 0xb0) & 0xF8) << 3;   // the base register's vector number
    for (unsigned bit = 0; bit < 8 && n < max; bit++) {
        uint32_t v = (base + bit) * 4;
        uint32_t ip = (uint32_t)dp_ws_peek(d, v) | (uint32_t)dp_ws_peek(d, v + 1) << 8;
        uint32_t cs = (uint32_t)dp_ws_peek(d, v + 2) | (uint32_t)dp_ws_peek(d, v + 3) << 8;
        uint32_t pc = (cs << 4) | ip;
        if (!pc || dp_ws_rom_offset(d, pc) == 0xFFFFFFFFu) continue;
        unsigned dup = 0;
        for (unsigned j = 0; j < n; j++) dup |= out[j] == pc;
        if (!dup) out[n++] = pc;
    }
    return n;
}

static int64_t ws_rom_index(DpMachine *m, uint32_t a) {
    uint32_t off = dp_ws_rom_offset(ws_of(m), a);
    return off == 0xFFFFFFFFu ? -1 : (int64_t)off;
}

static int ws_run(DpMachine *m) { return dp_ws_step(ws_of(m)); }

static uint32_t ws_pc(DpMachine *m) { return dp_ws_pc(ws_of(m)); }
static void ws_set_pc(DpMachine *m, uint32_t pc) { dp_ws_set_pc(ws_of(m), pc); }

// nonsense cuts: only the RAM page, the SRAM window and the ROM ever hold
// code; the stack lives in the RAM page
static int ws_cut_pc(DpMachine *m, uint32_t pc) {
    pc &= 0xFFFFFu;
    if (pc < 0x20000u) return 0;
    return dp_ws_rom_offset(ws_of(m), pc) == 0xFFFFFFFFu;
}
static int ws_cut_sp(DpMachine *m) {
    uint32_t sp = dp_ws_sp(ws_of(m));
    return sp >= 0x10000u;
}

// shared DP_DEST_* knowledge: the sprite table window (the OAM copy), the
// colour palette page and the tile data page
static uint8_t ws_data_dest(DpMachine *m, uint32_t a) {
    DpWs *d = ws_of(m);
    a &= 0xFFFFFu;
    if (a >= 0xFE00u && a < 0xFF00u) return DP_DEST_CRAM;      // the colour palette page
    if (a < 0x4000u) return DP_DEST_VRAM;                      // the character/tile page
    unsigned sprite_base = (d->ws.display[4] & (d->ws.video_mode ? 63 : 31)) << 9;
    if (a >= sprite_base && a < sprite_base + 512u) return DP_DEST_OAM;
    return DP_DEST_NONE;
}

static uint64_t ws_state_hash(DpMachine *m) { return dp_ws_state_hash(ws_of(m)); }
static uint64_t ws_ctx_reg_hash(DpMachine *m) { return dp_ws_ctx_hash(ws_of(m)); }
static uint64_t ws_wrap_frame(DpMachine *m) { return dp_ws_frame(ws_of(m)); }

static void ws_set_input(DpMachine *m, uint32_t mask) { dp_ws_set_input(ws_of(m), mask); }

static void ws_poke(DpMachine *m, uint32_t a, uint8_t v) { dp_ws_poke(ws_of(m), a, v); }

static void ws_observe(DpMachine *m, const DpObservers *o, void *user) {
    WsWrap *w = m->impl;
    w->obs = o; w->user = user;
    dp_ws_observe(ws_of(m), o, user);
}

// the context components: the CPU registers and the RAM pages a section
// may read (the whole 64 KiB page holds the video RAMs)
static const DpCtxComp ws_ctx[] = {
    { "ax", DP_CC_REG, 0, 2 }, { "cx", DP_CC_REG, 1, 2 }, { "dx", DP_CC_REG, 2, 2 },
    { "bx", DP_CC_REG, 3, 2 }, { "sp", DP_CC_REG, 4, 2 }, { "bp", DP_CC_REG, 5, 2 },
    { "si", DP_CC_REG, 6, 2 }, { "di", DP_CC_REG, 7, 2 },
    { "fl", DP_CC_REG, 8, 2 }, { "cs", DP_CC_REG, 9, 2 }, { "ip", DP_CC_REG, 10, 2 },
    { "ds", DP_CC_REG, 11, 2 }, { "es", DP_CC_REG, 12, 2 }, { "ss", DP_CC_REG, 13, 2 },
    { "wram", DP_CC_PAGES, -1, 0x10000 },
};
static int ws_ctx_components(DpMachine *m, const DpCtxComp **out) {
    (void)m;
    *out = ws_ctx;
    return (int)(sizeof ws_ctx / sizeof *ws_ctx);
}

static unsigned ws_machine_audio(DpMachine *m, int16_t *st, unsigned max) {
    DpWs *d = ws_of(m);
    // the runtime produces WS_AUDIO_RATE (24 kHz); the watch expects 44.1 kHz —
    // hand over the raw samples, the count scaled to the caller's rate
    unsigned avail = (d->audio_tail + d->audio_cap - d->audio_head) % d->audio_cap;
    unsigned want = avail * 441u / 240u;
    if (want > max) want = max;
    for (unsigned i = 0; i < want; i++) {
        unsigned src = (unsigned)((uint64_t)i * 240u / 441u);
        unsigned idx = (d->audio_head + src) % d->audio_cap;
        st[i * 2] = d->audio[idx * 2];
        st[i * 2 + 1] = d->audio[idx * 2 + 1];
    }
    d->audio_head = (d->audio_head + (unsigned)((uint64_t)want * 240u / 441u)) % d->audio_cap;
    return want;
}

static int ws_render(DpMachine *m, uint32_t *argb, unsigned cap, int *w, int *h) {
    return dp_ws_render(ws_of(m), argb, cap, w, h);
}

// technique 2 on the WonderSwan: an instruction that reads the pad ports
// ($B5/$B6) forks one state per held pad; the held pad stays in the saved
// state (the game's next reads see it)
static int ws_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc; (void)bytes;
    static const uint32_t pads[] = { 0, DP_IN_B1, DP_IN_B2, DP_IN_B1 | DP_IN_B2,
                                     DP_IN_UP, DP_IN_DOWN, DP_IN_LEFT, DP_IN_RIGHT, DP_IN_START,
                                     DP_IN_SELECT, DP_IN_Y, DP_IN_X, DP_IN_Z };
    unsigned nfull = m->pad_full ? dp_pad_count(m->input_bits) : 0;
    if (k >= (nfull ? nfull : sizeof pads / sizeof *pads)) return 0;
    DpWs *d = ws_of(m);
    uint32_t size = ws_wrap_state_bytes(m);
    uint8_t *pre = malloc(size);
    if (!pre) return 0;
    m->save(m, pre);
    dp_ws_pad_arm(d, 1);
    int progress = ws_run(m);
    int hit = dp_ws_pad_seen(d);
    dp_ws_pad_arm(d, 0);
    m->load(m, pre);
    free(pre);
    if (!progress || !hit) return 0;
    pre = malloc(size);
    if (!pre) return 0;
    m->save(m, pre);
    if (nfull) dp_ws_set_input(d, dp_pad_subset(m->input_bits, k));
    else dp_ws_set_input(d, pads[k]);
    ws_run(m);   // the probe instruction runs again, this time with the pad held
    m->save(m, save);
    m->load(m, pre);
    free(pre);
    return 1;
}

// IRQ forks: where the game has unmasked a source, one state with the
// V-blank pending (delay 1) or a timer tick pending (delay 2) — the
// runtime's own controller delivers it on the next tick
static int ws_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    if (delay > 2) return 0;
    DpWs *d = ws_of(m);
    unsigned bit = delay == 1 ? 6 : 7;              // VBLANK / TIMER_1
    if (!(d->ws.irq_enable & (1u << bit))) return 0;
    if (!(d->ws.flags & 0x200)) return 0;           // IF: the CPU must take it
    m->save(m, save);
    dp_ws_set_int(save, bit);
    return 1;
}

static const DpPlatformHooks ws_hooks = { .event_probe = ws_event_probe, .irq_alt = ws_irq_alt };
const DpPlatformHooks *dp_machine_ws_hooks(void) { return &ws_hooks; }

// clones are plain new machines over the same image
static DpMachine *ws_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    WsWrap *o = m->impl;
    if (!o->rom) return NULL;
    DpMachine *c = dp_machine_ws();
    if (!c) return NULL;
    dp_machine_ws_source(c, o->rom, o->rom_size);
    return c;
}
static int ws_attach(DpMachine *m) {
    WsWrap *w = m->impl;
    if (!w->rom) { fprintf(stderr, "deepeel: ws: attach: no image\n"); return 0; }
    if (w->d) return 1;
    w->d = dp_ws_open(w->rom, w->rom_size);
    return w->d != NULL;
}
void dp_machine_ws_source(DpMachine *m, const uint8_t *rom, size_t size) {
    WsWrap *w = m->impl;
    w->rom = rom; w->rom_size = size;
    m->clone = ws_clone;
    m->attach = ws_attach;
}

static void ws_close(DpMachine *m) {
    WsWrap *w = m->impl;
    dp_ws_close(w->d);
    free(w);
    free(m);
}

// The whole keypad for play and random sessions: both direction pads (X1..X4 = left, Select, right, Y masks;
// Y1..Y4 = up, X, down, Z), A, B and Start.
static const uint32_t ws_pads[] = {0, DP_IN_B1, DP_IN_START, DP_IN_B2, DP_IN_LEFT, DP_IN_SELECT, DP_IN_RIGHT, DP_IN_Y,
                                   DP_IN_UP, DP_IN_DOWN, DP_IN_X, DP_IN_Z};
DpMachine *dp_machine_ws(void) {
    WsWrap *w = calloc(1, sizeof *w);
    DpMachine *m = calloc(1, sizeof *m);
    if (!w || !m) { free(w); free(m); return NULL; }
    m->name = "ws";
    m->plat = dp_ws_platform();
    m->isa = dp_isa_v30mz();
    m->work_ram_base = 0x0000u; m->work_ram_len = 0x10000u;
    m->save_ram_base = 0x10000u; m->save_ram_len = 0x10000u;   // the battery SRAM window
    m->ram_index = ws_ram_index; m->ram_addr = ws_ram_addr; m->ram_size = 0x10000u;
    m->ram_view = ws_ram_view; m->ram_view_len = 0x10000u;
    m->input_bits = DP_IN_UP | DP_IN_DOWN | DP_IN_LEFT | DP_IN_RIGHT | DP_IN_B1 | DP_IN_B2 | DP_IN_START;
    m->impl = w;
    m->state_bytes = ws_wrap_state_bytes;
    m->save = ws_save;
    m->load = ws_load;
    m->fetch = ws_fetch;
    m->fetch_n = ws_fetch_n;
    m->vector_seeds = ws_vector_seeds;
    m->poke = ws_poke;
    m->rom_index = ws_rom_index;
    m->linear = 1;                // the cov offsets are the image offsets
    m->code_base = 0x0000u;
    m->bank_window = 0;           // the 64 KiB segments have no 16 KiB window
    m->run = ws_run;
    m->pc = ws_pc;
    m->set_pc = ws_set_pc;
    m->invert_condition = ws_invert;
    m->cut_pc = ws_cut_pc;
    m->cut_sp = ws_cut_sp;
    m->data_dest = ws_data_dest;
    m->state_hash = ws_state_hash;
    m->ctx_reg_hash = ws_ctx_reg_hash;
    m->frame = ws_wrap_frame;
    m->set_input = ws_set_input;
    m->play_pads = ws_pads; m->n_play_pads = sizeof ws_pads / sizeof *ws_pads;
    m->observe = ws_observe;
    m->ctx_components = ws_ctx_components;
    m->get_regs = ws_get_regs_w;
    m->audio_read = ws_machine_audio;
    m->render = ws_render;
    m->close = ws_close;
    return m;
}
