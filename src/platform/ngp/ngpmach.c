// ngpmach.c: the Neo Geo Pocket bridge over m0boy's runtime (targets/ngp).
// The runtime is compiled with -DNGP_TRACE -DM0_TL=__thread into this
// machine's archive; its trace hooks (below) feed the kernel's observers and
// its machine global is thread-local, so the same code serves the workers.
// The machine boots the cartridge directly (the synthesised BIOS), like every
// NGP emulator: the reset PC is the cartridge header's entry at 0x20001C.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ngpmach.h"
#include "ngp.h"     // the m0boy runtime's machine (ngp_t)

// ---- the observers the trace hooks feed -------------------------------------

static M0_TL const DpObservers *obs;
static M0_TL void *obs_user;
static M0_TL unsigned pad_armed, pad_seen;   // the pad probe's dry-run detection

void ngp_trace_step(uint32_t pc, uint8_t op) {
    if (obs && obs->on_step) obs->on_step(obs_user, pc, op, 0);
}
void ngp_trace_read(uint32_t addr, uint8_t v) {
    if (obs && obs->on_read) obs->on_read(obs_user, addr, v);
}
void ngp_trace_write(uint32_t addr, uint8_t v) {
    if (pad_armed && addr == 0x6F82u) pad_seen = 1;   // the BIOS's pad workspace byte
    if (obs && obs->on_write) obs->on_write(obs_user, addr, v);
}
void ngp_trace_dma(uint32_t addr, unsigned width) {
    if (obs && obs->on_copy) obs->on_copy(obs_user, addr, width, DP_COPY_DMA);
}

// ---- init / reset -------------------------------------------------------------

static M0_TL uint8_t *rom_copy;      // owned: the flash writes land here
static M0_TL uint32_t rom_bytes;
static M0_TL uint8_t *bios;          // the synthesised BIOS (owned)

int dp_ngp_init(const uint8_t *data, size_t size) {
    if (!data || !size || size > NGP_ROM_MAX) return 0;
    if (!bios) {
        bios = malloc(NGP_BIOS_BYTES);
        if (!bios) return 0;
        machine.bios = bios;
        ngp_bios_install();
    }
    free(rom_copy);
    rom_copy = malloc(size);
    if (!rom_copy) return 0;
    memcpy(rom_copy, data, size);
    rom_bytes = (uint32_t)size;
    ngp_load_rom(rom_copy, rom_bytes);
    ngp_reset();
    obs = NULL;
    return 1;
}

void dp_ngp_close(void) {
    free(rom_copy); rom_copy = NULL; rom_bytes = 0;
    free(bios); bios = NULL;
    machine.bios = NULL;
}

// ---- execution ------------------------------------------------------------------

int dp_ngp_step(void) {
    machine.cpu.fault = 0;
    // the step path never enters ngp_run_frame: count the V-INT boundary here,
    // and clear v_int like the next run_frame iteration does
    if (machine.cpu.halted) {   // HALT waits for the next line: tick it so the timers and the V-INT keep running
        ngp_tick(NGP_LINE_TICKS);
        if (machine.v_int) { machine.frames++; machine.v_int = 0; }
        return machine.cpu.fault ? 0 : 1;
    }
    int32_t cycles = ngp_step();
    if (machine.cpu.fault) return 0;   // a wild fetch, a faulted opcode: no progress
    ngp_tick((uint32_t)cycles);
    if (machine.v_int) { machine.frames++; machine.v_int = 0; }
    return 1;
}

uint32_t dp_ngp_pc(void) { return machine.cpu.pc; }

void dp_ngp_set_pc(uint32_t pc) { machine.cpu.pc = pc & 0xFFFFFF; }

uint8_t dp_ngp_peek(uint32_t addr) { return ngp_peek(addr); }

void dp_ngp_poke(uint32_t addr, uint8_t v) {
    addr &= 0xFFFFFF;
    if (addr >= 0x4000 && addr < 0x8000) machine.ram[addr - 0x4000] = v;
}

uint32_t dp_ngp_sp(void) { return machine.cpu.gpr[3]; }

uint32_t dp_ngp_reg(unsigned slot) {
    if (slot < 4) return machine.cpu.bank[(machine.cpu.sr >> 8) & 3][slot];
    if (slot < 8) return machine.cpu.gpr[slot - 4];
    return 0;
}

int dp_ngp_rom_visible(uint32_t addr) {
    return dp_ngp_rom_offset(addr) != 0xFFFFFFFFu;
}

uint32_t dp_ngp_rom_offset(uint32_t addr) {
    addr &= 0xFFFFFF;
    if (addr >= 0x200000 && addr < 0x400000) return addr - 0x200000;
    if (addr >= 0x800000 && addr < 0xA00000) return addr - 0x800000 + 0x200000;
    return 0xFFFFFFFFu;
}

void dp_ngp_set_input(uint32_t mask) {
    uint8_t b = 0;   // the pad bits the runtime holds: up down left right A B option
    if (mask & DP_IN_UP) b |= NGP_BTN_UP;
    if (mask & DP_IN_DOWN) b |= NGP_BTN_DOWN;
    if (mask & DP_IN_LEFT) b |= NGP_BTN_LEFT;
    if (mask & DP_IN_RIGHT) b |= NGP_BTN_RIGHT;
    if (mask & DP_IN_B1) b |= NGP_BTN_A;
    if (mask & DP_IN_B2) b |= NGP_BTN_B;
    if (mask & DP_IN_START) b |= NGP_BTN_OPTION;
    machine.buttons = b;
}

// ---- state ------------------------------------------------------------------

size_t dp_ngp_state_size(void) { return ngp_state_bytes(); }

void dp_ngp_save(void *buf) { ngp_state_io(buf, ngp_state_bytes(), 0); }

void dp_ngp_load(const void *buf) { ngp_state_io((void *)buf, ngp_state_bytes(), 1); }

// the state buffer's layout mirrors ngp_state_io: the tag then the spans; the
// clocks ride inside the spans, the hash zeroes them (B5)
static void state_hashes(const void *buf, uint64_t *full, uint64_t *cl) {
    const uint8_t *p = buf;
    size_t o_cpu = 4;
    size_t o_gfx = o_cpu + sizeof(ngp_cpu_t);
    size_t o_span = o_gfx + sizeof(ngp_gfx_t);
    size_t o_ram = o_span + (offsetof(ngp_t, ram) - offsetof(ngp_t, trun));
    size_t o_rtc = o_ram + NGP_RAM_BYTES;
    size_t o_sq = o_rtc + (offsetof(ngp_t, z80_enabled) - offsetof(ngp_t, rtc_latch))
                + (offsetof(ngp_t, z80_runtime) - offsetof(ngp_t, z80_enabled))
                + (offsetof(ngp_t, sq_period) - offsetof(ngp_t, z80_runtime));
    #define FNV(init, at, n) { const uint8_t *q = p + (at); for (size_t k = 0; k < (n); k++) { init ^= q[k]; init *= 0x01000193ull; } }
    uint64_t h = 0xCBF29CE484222325ull;
    FNV(h, o_cpu, sizeof(ngp_cpu_t));
    FNV(h, o_gfx, sizeof(ngp_gfx_t));
    FNV(h, o_span, o_ram - o_span);
    FNV(h, o_ram, NGP_RAM_BYTES);
    FNV(h, o_rtc, o_sq - o_rtc);
    FNV(h, o_sq, offsetof(ngp_t, flash_shadow) - offsetof(ngp_t, sq_period));
    // the Z80 registers close the buffer; its cycle counter is a clock
    const size_t o_z80 = ngp_state_bytes() - NGP_Z80_STATE_BYTES;
    FNV(h, o_z80, NGP_Z80_STATE_BYTES - sizeof(uint64_t));
    *full = h;
    // the clocks: cyc, frames (in the sq_period span), timer_clock/timer_hint
    // (in the trun span), z80_runtime, psg_runtime
    uint64_t c = h;
    FNV(c, o_span + (offsetof(ngp_t, timer_clock) - offsetof(ngp_t, trun)), sizeof machine.timer_clock + 4);
    FNV(c, o_sq + (offsetof(ngp_t, cyc) - offsetof(ngp_t, sq_period)), 4);
    FNV(c, o_sq + (offsetof(ngp_t, frames) - offsetof(ngp_t, sq_period)), 4);
    FNV(c, o_sq + (offsetof(ngp_t, psg_runtime) - offsetof(ngp_t, sq_period)), 4);
    FNV(c, o_rtc + (offsetof(ngp_t, z80_runtime) - offsetof(ngp_t, rtc_latch)), 4);
    *cl = c;
    #undef FNV
}

uint64_t dp_ngp_state_hash(void) {
    static M0_TL uint8_t *buf;
    if (!buf) buf = malloc(ngp_state_bytes());
    if (!buf) return 0;
    dp_ngp_save(buf);
    uint64_t full, cl;
    state_hashes(buf, &full, &cl);
    return cl;
}

void dp_ngp_state_set_int(void *buf, unsigned index) {
    // int_pending rides in the trun..ram span
    size_t o_cpu = 4;
    size_t o_gfx = o_cpu + sizeof(ngp_cpu_t);
    size_t o_span = o_gfx + sizeof(ngp_gfx_t);
    size_t at = o_span + (offsetof(ngp_t, int_pending) - offsetof(ngp_t, trun));
    if (index < 24) ((uint8_t *)buf)[at + index] = 1;
}

// C11: the register components (sr, the 20 wide registers, the shadow flags)
uint64_t dp_ngp_ctx_hash(void) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t *q;
    h ^= machine.cpu.sr & 0xFF; h *= 0x01000193ull;
    h ^= (machine.cpu.sr >> 8) & 0xFF; h *= 0x01000193ull;
    h ^= machine.cpu.f_dash; h *= 0x01000193ull;
    h ^= machine.cpu.halted; h *= 0x01000193ull;
    q = (const uint8_t *)machine.cpu.bank;
    for (size_t k = 0; k < sizeof machine.cpu.bank; k++) { h ^= q[k]; h *= 0x01000193ull; }
    q = (const uint8_t *)machine.cpu.gpr;
    for (size_t k = 0; k < sizeof machine.cpu.gpr; k++) { h ^= q[k]; h *= 0x01000193ull; }
    return h;
}

// ---- the watch side -----------------------------------------------------------

uint64_t dp_ngp_frame(void) { return machine.frames; }

unsigned dp_ngp_render(uint32_t *argb, unsigned cap, int *w, int *h) {
    if (cap < NGP_SCREEN_W * NGP_SCREEN_H) return 0;
    const uint16_t *fb = ngp_frame();
    for (unsigned i = 0; i < NGP_SCREEN_W * NGP_SCREEN_H; i++) {
        uint16_t px = fb[i];
        uint32_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
        argb[i] = 0xFF000000u | (r << 3) << 16 | (g << 2) << 8 | (b << 3);
    }
    *w = NGP_SCREEN_W; *h = NGP_SCREEN_H;
    return 1;
}

unsigned dp_ngp_audio_read(int16_t *stereo, unsigned max_frames) {
    return ngp_audio(stereo, max_frames, 44100);
}

void dp_ngp_observe(const DpObservers *o, void *user) {
    obs = o; obs_user = user;
}

void dp_ngp_pad_arm(int arm) { pad_armed = (unsigned)(arm != 0); pad_seen = 0; }
int dp_ngp_pad_seen(void) { return pad_seen; }
