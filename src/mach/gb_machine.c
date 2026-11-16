// gb_machine.c: the m0boy runtime as a DpMachine (spec §7.2).  The explorer
// still drives DpMach directly; this wrapper is the kernel-facing view, built
// on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "m0boy.h"
#include "../explore/core.h"
#include "../explore/tas.h"
#include "../explore/gambatte_init.h"
#include <stddef.h>

static void gb_tas_tune(DpMach *mach, const DpTas *tas);


// The kernel-facing view of one m0boy instance.  Each wrapper owns its observer and its step
// counter, so several instances (parallel workers, one per compiled slot) run side by side.
typedef struct GbWrap {
    DpMach *d;
    DpObservers o;
    void *user;
    unsigned seen;            // instructions started in the current gb_run
    int joy_armed, joy_seen;  // joypad-port read detection for the event probe
    const uint8_t *rom; size_t rom_size;   // for clone()
    const DpTas *tas;         // movie whose recorder sync profile every instance takes
    int last_ei;              // the last executed instruction was EI: the IRQ check skips the next one
    uint8_t first_op;         // opcode of the instruction executed by the current gb_run
} GbWrap;
static GbWrap *gbw(DpMachine *m) { return m->impl; }
// the generic game-screen watch: the PPU renders lines only while asked (DpMach.render); shades 0..3 become grays
static void gb_watch(DpMachine *m, int on) { DpMach *d = gbw(m)->d; d->render = on; d->api->skip_render[0] = !on; }
static int gb_render(DpMachine *m, uint32_t *argb, unsigned cap, int *w, int *h) {
    DpMach *d = gbw(m)->d;
    if (cap < 160u * 144u || !d->render) return 0;
    for (int y = 0; y < 144; y++) for (int x = 0; x < 160; x++) { uint32_t v = 255u - d->screen[y][x] * 85u; argb[y * 160 + x] = 0xff000000u | v << 16 | v << 8 | v; }
    *w = 160; *h = 144; return 1;
}

// the frame counter rides at the end of the saved state so a restored fork resumes at its own frame
static uint32_t gb_state_bytes(DpMachine *m) { return dp_mach_state_bytes(gbw(m)->d) + 8; }
// B5: exact state hash without the cycle counters (gbl_cpu_t.cyc and everything after it in the header only encode where time stood)
static uint64_t gb_state_hash(DpMachine *m) {
    uint32_t mb = dp_mach_state_bytes(gbw(m)->d);
    uint8_t *img = malloc(mb ? mb : 1);
    if (!img) return 0;
    dp_mach_save(gbw(m)->d, img);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < mb; i++) {
        if (i >= offsetof(gbl_cpu_t, cyc) && i < sizeof(gbl_cpu_t)) continue;
        h = (h ^ img[i]) * 0x100000001b3ull;
    }
    free(img);
    return h;
}
static void gb_save(DpMachine *m, uint8_t *buf) {
    DpMach *d = gbw(m)->d;
    dp_mach_save(d, buf);
    uint64_t f = *d->api->frame | ((uint64_t)gbw(m)->last_ei << 63);   // EI shadow rides in the top bit
    memcpy(buf + dp_mach_state_bytes(d), &f, 8);
}
static void gb_load(DpMachine *m, const uint8_t *buf) {
    DpMach *d = gbw(m)->d;
    dp_mach_load(d, buf);
    uint64_t f; memcpy(&f, buf + dp_mach_state_bytes(d), 8);
    gbw(m)->last_ei = (int)(f >> 63);
    f &= ~(1ull << 63);
    *d->api->frame = (typeof(*d->api->frame))f;
}
// side-effect-free view: work RAM bank 0/1 area and HRAM are read from the instance, the rest through the op view
static uint8_t gb_fetch(DpMachine *m, uint32_t a) {
    DpMach *d = gbw(m)->d;
    if (a >= 0xC000 && a < 0xE000 && d->api->wram) return d->api->wram[a - 0xC000];
    if (a >= 0xFF80 && a < 0xFFFF && d->api->hram) return d->api->hram[a - 0xFF80];
    return dp_mach_op(d, (uint16_t)a);
}
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
    if (g->seen == 0) g->first_op = op;
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
    // the interpreter keeps the EI delay in a local that a cut loses: after an EI, hide the pending
    // event for this run so the instruction after EI still executes before the interrupt
    volatile uint32_t *ne = &g->d->api->gbr->next_evt;
    uint32_t saved = *ne, far = g->d->api->gbr->cyc + 0x40000000u;
    if (g->last_ei) *ne = far;
    dp_mach_run(g->d);
    if (g->last_ei && *ne == far) *ne = saved;
    if (g->seen > 0) g->last_ei = (g->first_op == 0xFB);
    return g->seen > 0;
}
static uint32_t gb_pc(DpMachine *m) { return gbw(m)->d->api->gbr->pc; }
static void gb_set_pc(DpMachine *m, uint32_t pc) { gbw(m)->d->api->gbr->pc = (uint16_t)pc; }
// register seeds (music rips): the SM83 registers are directly writable through the engine
static int gb_seed_regs(DpMachine *m, uint8_t a, uint16_t sp) {
    if (a != 0xFF) gbw(m)->d->api->gbr->a = a;
    if (sp != 0xFFFF) gbw(m)->d->api->gbr->sp = sp;
    return 1;
}
static void gb_poke(DpMachine *m, uint32_t a, uint8_t v) { gbw(m)->d->api->wr((uint16_t)a, v); }
// per-bank seeding: the $4000 window shows 16 KiB block `bank` (the kernel's seed_banks);
// bank 0 included — the reset view is not assumed, the boot may have selected another bank
static int gb_bank_view(DpMachine *m, unsigned bank) {
    gb_poke(m, 0x2000, (uint8_t)bank);
    if (bank > 0xff) gb_poke(m, 0x3000, (uint8_t)(bank >> 8));
    return 1;
}
// nonsense cuts (audit E2): echo RAM $E000-$FDFF and OAM $FE00-$FEFF never hold
// real code (HRAM $FF00+ does — the boot code runs there); a stack outside
// WRAM/HRAM pops garbage
static int gb_cut_pc(DpMachine *m, uint32_t pc) { (void)m; return pc >= 0xE000 && pc < 0xFF00; }
// shared DP_DEST_* knowledge: where a write lands in the hardware (the GB's
// finer DP_DF_* types project onto these same destinations)
static uint8_t gb_data_dest(DpMachine *m, uint32_t a) {
    (void)m;
    if (a >= 0x8000 && a < 0xA000) return DP_DEST_VRAM;
    if (a >= 0xFE00 && a < 0xFEA0) return DP_DEST_OAM;
    if (a >= 0xFF10 && a <= 0xFF25) return DP_DEST_MUSIC;
    if (a >= 0xFF30 && a <= 0xFF3F) return DP_DEST_SAMPLE;
    if (a == 0xFF47 || a == 0xFF48 || a == 0xFF49 || a == 0xFF69 || a == 0xFF6B) return DP_DEST_CRAM;
    if (a >= 0xA000 && a < 0xC000) return DP_DEST_SRAM;
    return DP_DEST_NONE;
}
static int gb_cut_sp(DpMachine *m) { uint16_t sp = gbw(m)->d->api->gbr->sp; return sp < 0xC000 || sp > 0xFFFE; }
static uint64_t gb_frame(DpMachine *m) { return *gbw(m)->d->api->frame; }
// kernel mask (up down left right button1 button2 start select) -> the engine's set_buttons layout
// (A=1 B=2 Select=4 Start=8 Right=16 Left=32 Up=64 Down=128, what the movies and the old engine feed)
static void gb_set_input(DpMachine *m, uint32_t mask) {
    uint8_t pad = 0;
    if (mask & 0x01) pad |= 0x40;   // up
    if (mask & 0x02) pad |= 0x80;   // down
    if (mask & 0x04) pad |= 0x20;   // left
    if (mask & 0x08) pad |= 0x10;   // right
    if (mask & 0x10) pad |= 0x01;   // a
    if (mask & 0x20) pad |= 0x02;   // b
    if (mask & 0x40) pad |= 0x08;   // start
    if (mask & 0x80) pad |= 0x04;   // select
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
static uint64_t gb_ctx_reg_hash(DpMachine *m) {   // the register block before the clocks (includes pc, sp, flags, ime, rom bank)
    const uint8_t *b = (const uint8_t *)gbw(m)->d->api->gbr;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < offsetof(gbl_cpu_t, cyc); i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
// C3: the concrete SM83 registers (every byte known) for the static closure's bootstrapping
static int gb_get_regs(DpMachine *m, DpRegState *out) {
    const gbl_cpu_t *c = gbw(m)->d->api->gbr;
    dp_regstate_clear(out);
    out->v[DP_SM83R_BC] = c->bc; out->v[DP_SM83R_DE] = c->de; out->v[DP_SM83R_HL] = c->hl; out->v[DP_SM83R_SP] = c->sp;
    out->v[DP_SM83R_A] = c->a;
    out->known[DP_SM83R_BC] = out->known[DP_SM83R_DE] = out->known[DP_SM83R_HL] = out->known[DP_SM83R_SP] = 3;
    out->known[DP_SM83R_A] = 1;
    return 1;
}
static int gb_ctx_components(DpMachine *m, const DpCtxComp **out) {
    (void)m;
    *out = gb_ctx;
    return (int)(sizeof gb_ctx / sizeof *gb_ctx);
}


// technique 2 on GB: an instruction that reads the joypad port ($FF00) forks one state per button
// held; the held pad stays in the saved state.  The instruction is dry-run once to see the read.
static int gb_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    static const uint8_t pads[] = {0x80, 0x10, 0x20, 0x40, 0x04, 0x08, 0x02, 0x01};
    static const uint8_t ly[] = {0x00, 0x90, 0x99, 0x8F};     // line 0, first VBlank line, last line, last visible line
    static const uint8_t stat[] = {0x00, 0x01, 0x02, 0x03};   // PPU mode 0..3
    GbWrap *g = gbw(m);
    // a read of LY ($FF44) or STAT ($FF41) forks one state per plausible value: the instruction runs
    // on the live machine, A is overwritten, the state is saved, then the live state is restored
    int rly  = (bytes[0] == 0xF0 && bytes[1] == 0x44) || (bytes[0] == 0xFA && bytes[1] == 0x44 && bytes[2] == 0xFF);
    int rsta = (bytes[0] == 0xF0 && bytes[1] == 0x41) || (bytes[0] == 0xFA && bytes[1] == 0x41 && bytes[2] == 0xFF);
    if (rly || rsta) {
        const uint8_t *vals = rly ? ly : stat;
        unsigned n = rly ? sizeof ly : sizeof stat;
        if (k >= n) return 0;
        uint8_t *pre = malloc(m->state_bytes(m));
        if (!pre) return 0;
        m->save(m, pre);
        gb_run(m);
        g->d->api->gbr->a = vals[k];
        m->save(m, save);
        m->load(m, pre);
        free(pre);
        return 1;
    }
    if (k >= sizeof pads) return 0;
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
        if (o->tas) gb_tas_tune(d, o->tas);
        DpMachine *c = gb_wrap(d);
        if (!c) { dp_mach_close(d); return NULL; }
        gbw(c)->rom = o->rom; gbw(c)->rom_size = o->rom_size; gbw(c)->tas = o->tas;
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
    m->work_ram_base = 0xC000u; m->work_ram_len = 0x2000u;
    m->save_ram_en = 1; m->save_ram_en_addr = 0x0000u; m->save_ram_en_val = 0x0A;
    m->save_ram_base = 0xA000u; m->save_ram_len = 0x2000u;
    m->plat = dp_platform_gb();
    m->bank_shift = 14;   // the SM83's 16 KiB pointer-table window (the static closure's geometry, E5)
    m->rom_top_win = 2; m->bank_window = 0x4000u;   // P10: the switchable window maps its bank at $4000   // $0000 fixed, $4000 switchable: a code target never sits at $8000 and beyond
    m->isa = dp_isa_sm83();
    m->impl = g;
    m->state_bytes = gb_state_bytes;
    m->save = gb_save;
    m->state_hash = gb_state_hash;
    m->load = gb_load;
    m->rom_index = gb_rom_index;
    m->fetch = gb_fetch;
    m->run = gb_run;
    m->pc = gb_pc;
    m->set_pc = gb_set_pc;
    m->seed_regs = gb_seed_regs;
    m->bank_view = gb_bank_view;
    m->cut_pc = gb_cut_pc;
    m->cut_sp = gb_cut_sp;
    m->data_dest = gb_data_dest;
    m->poke = gb_poke;
    m->set_input = gb_set_input;
    m->frame = gb_frame;
    m->watch = gb_watch; m->render = gb_render;
    m->observe = gb_observe;
    m->ctx_components = gb_ctx_components;
    m->ctx_reg_hash = gb_ctx_reg_hash;
    m->get_regs = gb_get_regs;
    m->close = gb_close;
    return m;
}

// applies the movie's recorder sync profile to this instance and to every clone
void dp_machine_gb_set_tas(DpMachine *m, const struct DpTas *tas) {
    gbw(m)->tas = tas;
    gb_tas_tune(gbw(m)->d, tas);
}

// per-recorder machine tuning for the movie loaded in w (sync profiles, frame 0 input)
static void gb_tas_tune(DpMach *mach, const DpTas *tas) {
    // the recording emulator's timing quirks (gbl.h movie sync
    // knobs), for this worker's machine only and while the movie
    // plays: random play after it runs on hardware timing again
    // BizHawk (Gambatte) powers a cartridge up with blank battery RAM
    // filled with $FF: games that read their save slot (Mario Land 2)
    // branch on it, so a zeroed RAM replays another game
    if ((tas->recorder == DP_TAS_REC_BIZHAWK || (tas->recorder == DP_TAS_REC_VBA && tas->recorder_rev == 0 && !getenv("DEEPEEL_TAS_NOINIT"))) && mach->api->eram)
        memset(mach->api->eram, 0xFF, dp_gbl_variant_eram[mach->api->variant]);
    // VBA-rr's V8 core (VBM minor version 1) powers WRAM up with a fixed
    // pattern ($FF x8 / $0F x8, blocks of $800 alternating), HRAM with $FF
    // (observed on the VBA-rr headless harness); the V7 core (minor 0)
    // powers up zeroed, like m0boy: uninitialised reads depend on it
    if (tas->recorder == DP_TAS_REC_VBA && tas->recorder_rev >= 1 && !getenv("DEEPEEL_TAS_NOINIT")) {
        for (int i = 0; i < 0x2000; i++)
            mach->api->wram[i] = (((i >> 3) ^ (i >> 11)) & 1) ? 0x0F : 0xFF;
        memset(mach->api->hram, 0xFF, 0x7F);
    }
    // BizHawk's Gambatte starts from a fixed power-on RAM dump, not zeros
    if (tas->recorder == DP_TAS_REC_BIZHAWK && tas->gambatte && !getenv("DEEPEEL_TAS_NOINIT")) {
        memcpy(mach->api->wram, dp_gambatte_wram, 0x2000);
        memcpy(mach->api->hram, dp_gambatte_hram, 0x7F);
    }
    // BizHawk's GBHawk (gbl.h): the pad is latched at the vblank
    // edge inside its fixed frame (65,664 cycles to LY 144); the
    // worker applies the next mask just after the m0boy vblank, so
    // the pad needs a 14,481-cycle delay (measured window
    // 14,470..14,550; the PPU phase below moves the m0boy vblank
    // edge but the pad window keeps 14,481).  The divider phase at
    // $0100 is 0xFE4D, and GBHawk is 405 cycles into line 153
    // there (LY zeroed by its cycle-6 rule; gbl.h, sync_ppu_phase:
    // applied to the loaded state, it makes every LY read match).
    // GBHawk keeps the WRAM zero (no power-on dump) and its frames
    // are always 70224 cycles, like the vblank clock: no mode-3
    // clock change applies.
    if (tas->recorder == DP_TAS_REC_BIZHAWK && tas->gbhawk) {
        *mach->api->sync_pad_delay = getenv("DEEPEEL_TAS_PADDELAY")
            ? (uint32_t)strtoul(getenv("DEEPEEL_TAS_PADDELAY"), NULL, 0) : 14481u;
        *mach->api->sync_div_extra = getenv("DEEPEEL_TAS_DIVEXTRA")
            ? (uint16_t)strtoul(getenv("DEEPEEL_TAS_DIVEXTRA"), NULL, 0) : 0xFE4Du;
        *mach->api->sync_ppu_phase = getenv("DEEPEEL_TAS_PPUPHASE")
            ? (uint16_t)strtoul(getenv("DEEPEEL_TAS_PPUPHASE"), NULL, 0) : 405u;
        mach->api->sync_ppu_apply();
        fprintf(stderr, "deepeel: TAS %s: GBHawk sync profile (pad latch +14481 cycles, DIV phase $FE4D, PPU phase 405)\n",
                tas->source);
    }
    // BizHawk's frame clock (gbl.h, sync_vba_off_clock 3): a frame ends at
    // the video frame or when its sample budget runs out
    if (tas->recorder == DP_TAS_REC_BIZHAWK && tas->gambatte && !tas->equal_frames && !getenv("DEEPEEL_TAS_NOMODE3"))
        *mach->api->sync_vba_off_clock = 3;
    // Gambatte's DIV phase against the m0boy clock, measured on SML2 (DIV
    // reads that seed its RNG): any skew in 25..64 matches, 40 is mid-range.
    if (tas->recorder == DP_TAS_REC_BIZHAWK && tas->gambatte)
        *mach->api->sync_div_skew = getenv("DEEPEEL_TAS_DIVSKEW") ? (uint8_t)atoi(getenv("DEEPEEL_TAS_DIVSKEW")) : 40;
    if (tas->echo_old && !getenv("DEEPEEL_TAS_NOECHO")) {
        *mach->api->sync_echo_old = 1;
        mach->api->sync_echo_apply();
        fprintf(stderr, "deepeel: TAS %s: VBA echoRAMFix off ($E000-$FDFF is separate memory)\n", tas->source);
    }
    int vba_old = tas->recorder == DP_TAS_REC_VBA && tas->recorder_rev == 0;
    if (vba_old) {
        // VBA 1.7.2 based recorders (VBM minor version 0): input
        // read at each vblank, no frame while the LCD is off, a
        // ten-line line 0 after an enable.  Partial: their CPU
        // timing quirks (cycle table, HALT wake-up) stay hardware
        *mach->api->sync_vba_off_clock = getenv("DEEPEEL_TAS_OFFCLOCK")
            ? (uint8_t)atoi(getenv("DEEPEEL_TAS_OFFCLOCK")) : 2;
        // V7 reads LY at the start of the reading instruction (gbl.h, sync_ly_lag)
        *mach->api->sync_ly_lag = getenv("DEEPEEL_TAS_LYLAG")
            ? (int16_t)atoi(getenv("DEEPEEL_TAS_LYLAG")) : 1;
        // V7 counts no tick for the interrupt dispatch (20 cycles on the hardware)
        *mach->api->sync_irq_trim = getenv("DEEPEEL_TAS_IRQTRIM")
            ? (int16_t)atoi(getenv("DEEPEEL_TAS_IRQTRIM")) : 20;
        // V7 wakes a halted CPU on its 96-cycle sound grid (gbl.h, sync_halt_snd);
        // DEEPEEL_TAS_HALTSND = grid phase in cycles, -1 = hardware wake-up
        {
            const char *hs = getenv("DEEPEEL_TAS_HALTSND");
            *mach->api->sync_halt_snd = (int16_t)((hs ? atoi(hs) : 0) + 1);
        }
        fprintf(stderr, "deepeel: TAS %s: VBA 1.7.2 sync profile (input at vblank, no LCD-off frame, "
                        "ten-line enable)\n", tas->source);
    } else if (tas->recorder == DP_TAS_REC_VBA) {
        *mach->api->sync_div_skew = 4;
        *mach->api->sync_vba_off_clock = 1;
        fprintf(stderr, "deepeel: TAS %s: VBA sync profile (DIV read +4 cycles, VBA LCD-off frame clock)\n",
                tas->source);
    }
}
