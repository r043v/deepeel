// wsmach.c: the WonderSwan bridge over m0boy's runtime.  The machine is a
// plain heap struct (the runtime has no globals), so every clone is its own
// instance and workers need no thread-local trickery.  The kernel's
// observers ride the runtime's sinks: instruction, memory read/write and
// port I/O.  The boot matches the TAS watch's (bizhawk timing, the Cygne
// start IO seeds, battery SRAM backing the cartridge's SRAM window).
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "wsmach.h"
#include "ws.h"
#include "ws_bizhawk_boot.h"
#include "../../cpu/v30mz/v30mz.h"

enum { WS_LINEAR_W = 224, WS_LINEAR_H = 144 };

uint8_t dp_ws_rom_read(void *ctx, uint32_t offset) {
    return ((const uint8_t *)ctx)[offset];
}

// ---- the sinks the runtime calls -------------------------------------------

static void sink_insn(void *ctx, uint32_t pc, uint32_t rom_offset) {
    DpWs *d = ctx;
    (void)rom_offset;
    if (d->obs && d->obs->on_step)
        d->obs->on_step(d->user, pc, 0, 0);
}

static void sink_read(void *ctx, uint32_t addr, uint8_t v) {
    DpWs *d = ctx;
    if (d->obs && d->obs->on_read)
        d->obs->on_read(d->user, addr, v);
}

static void sink_write(void *ctx, uint32_t addr, uint8_t v) {
    DpWs *d = ctx;
    if (d->obs && d->obs->on_write)
        d->obs->on_write(d->user, addr, v);
}

static void sink_io(void *ctx, uint16_t port, uint8_t v, int write) {
    DpWs *d = ctx;
    if (!write && d->pad_armed && (port == 0xb5 || port == 0xb6))
        d->pad_seen = 1;
    if (d->obs && d->obs->on_io)
        d->obs->on_io(d->user, port, v, write);
}

static void sink_audio(void *ctx, uint8_t speaker, int16_t left, int16_t right) {
    DpWs *d = ctx;
    (void)speaker;
    unsigned used = (d->audio_tail + 1) % d->audio_cap;
    if (used == d->audio_head) return;
    d->audio[d->audio_tail * 2] = left;
    d->audio[d->audio_tail * 2 + 1] = right;
    d->audio_tail = used;
    d->audio_frames_total++;
}

// ---- boot -------------------------------------------------------------------

DpWs *dp_ws_open(const uint8_t *data, size_t size) {
    if (!data || !size) { fprintf(stderr, "deepeel: ws: open: null image\n"); return NULL; }
    DpWs *d = calloc(1, sizeof *d);
    if (!d) { fprintf(stderr, "deepeel: ws: open: out of memory\n"); return NULL; }
    d->rom_copy = malloc(size);
    if (!d->rom_copy) { fprintf(stderr, "deepeel: ws: open: out of memory (%zu)\n", size); free(d); return NULL; }
    memcpy(d->rom_copy, data, size);
    d->rom_bytes = (uint32_t)size;
    if (!ws_boot_model(&d->ws, (WsRom){dp_ws_rom_read, d->rom_copy, d->rom_bytes}, true, NULL)) {
        fprintf(stderr, "deepeel: ws: open: boot refused (%zu bytes)\n", size);
        free(d->rom_copy); free(d);
        return NULL;
    }
    d->ws.bizhawk_timing = true;
    d->ws.line = WS_HEIGHT + 1;
    memcpy(d->ws.ram + 0x75ac, "A_C1n_c1", 8);
    ws_boot_bizhawk(&d->ws);
    if (d->ws.cart_sram_size) {
        d->sram = calloc(1, d->ws.cart_sram_size);
        if (!d->sram || !ws_attach_sram(&d->ws, d->sram, d->ws.cart_sram_size)) {
            free(d->rom_copy); free(d->sram); free(d);
            return NULL;
        }
    }
    d->audio_cap = WS_AUDIO_RATE;
    d->audio = calloc(d->audio_cap, 2 * sizeof(uint16_t));
    if (!d->audio) { free(d->rom_copy); free(d->sram); free(d); return NULL; }
    d->ws.audio.sink = sink_audio;
    d->ws.audio.context = d;
    d->ws.instruction_sink = sink_insn;
    d->ws.instruction_context = d;
    d->ws.memory_read_sink = sink_read;
    d->ws.memory_read_context = d;
    d->ws.memory_write_sink = sink_write;
    d->ws.memory_write_context = d;
    d->ws.io_sink = sink_io;
    d->ws.io_context = d;
    return d;
}

void dp_ws_close(DpWs *d) {
    if (!d) return;
    free(d->rom_copy);
    free(d->sram);
    free(d->audio);
    free(d);
}

// ---- execution ----------------------------------------------------------------

int dp_ws_step(DpWs *d) {
    return ws_step_with_video(&d->ws, NULL, d) == WS_STEP_OK;
}

uint32_t dp_ws_pc(DpWs *d) { return ws_address(d->ws.cs, d->ws.ip); }

void dp_ws_set_pc(DpWs *d, uint32_t pc) {
    d->ws.ip = (uint16_t)pc;
    d->ws.cs = (uint16_t)(pc >> 4);
}

uint8_t dp_ws_peek(DpWs *d, uint32_t addr) { return ws_peek(&d->ws, addr); }

void dp_ws_poke(DpWs *d, uint32_t addr, uint8_t v) {
    addr &= 0xFFFFFu;
    if (addr < 0x10000u)
        d->ws.ram[addr] = v;
}

uint32_t dp_ws_rom_offset(DpWs *d, uint32_t addr) { return ws_rom_offset(&d->ws, addr); }

uint32_t dp_ws_sp(DpWs *d) { return d->ws.reg[4]; }

// ---- state ----------------------------------------------------------------------

size_t dp_ws_state_size(DpWs *d) { (void)d; return sizeof(Ws); }

void dp_ws_save(DpWs *d, uint8_t *buf) { memcpy(buf, &d->ws, sizeof(Ws)); }

// The explorer's states cross instances (worker threads, --from-state, --resume): the saved image
// carries the saver's host pointers, so this instance's own are kept.
#define WS_HOST_FIELDS(X) \
    X(rom) X(instruction_sink) X(instruction_context) X(memory_write_sink) X(memory_write_context) \
    X(memory_read_sink) X(memory_read_context) X(io_sink) X(io_context) X(flash.store) X(flash.context) \
    X(boot_rom) X(audio.sink) X(audio.context) X(uart_send) X(uart_context) X(internal_eeprom.data) \
    X(cart_eeprom.data) X(sram) X(sram_context) X(sram_read) X(sram_write)

void dp_ws_load(DpWs *d, const uint8_t *buf) {
#define SIZE(f) + sizeof d->ws.f
    uint8_t keep[0 WS_HOST_FIELDS(SIZE)], *k = keep;
#define SAVE(f) memcpy(k, &d->ws.f, sizeof d->ws.f); k += sizeof d->ws.f;
    WS_HOST_FIELDS(SAVE)
    memcpy(&d->ws, buf, sizeof(Ws));
    k = keep;
#define RESTORE(f) memcpy(&d->ws.f, k, sizeof d->ws.f); k += sizeof d->ws.f;
    WS_HOST_FIELDS(RESTORE)
#undef SIZE
#undef SAVE
#undef RESTORE
}

#define FNV_AT(h, at, n) { const uint8_t *q = (const uint8_t *)&d->ws + (at); \
    for (size_t k = 0; k < (n); k++) { h ^= q[k]; h *= 0x01000193ull; } }

static void state_hashes(DpWs *d, uint64_t *full, uint64_t *cl) {
    // the plain-data members the machine's behaviour depends on; the clocks
    // (frames, line clock, timers, audio phases) and the pointers ride out
    uint64_t h = 0xCBF29CE484222325ull;
    FNV_AT(h, offsetof(Ws, reg), sizeof ((Ws *)0)->reg)
    FNV_AT(h, offsetof(Ws, cs), 2 * 6)                 // cs ip ds es ss flags
    FNV_AT(h, offsetof(Ws, timer_reload), 4)           // the reloads are programmed
    FNV_AT(h, offsetof(Ws, timer_control), 1)
    FNV_AT(h, offsetof(Ws, irq_base), 3)               // base enable pending
    FNV_AT(h, offsetof(Ws, halted), 3)                 // halted irq_shadow stack_shadow
    FNV_AT(h, offsetof(Ws, nmi_enabled), 2)            // nmi_enabled low_battery
    FNV_AT(h, offsetof(Ws, system_control), 1)
    FNV_AT(h, offsetof(Ws, video_mode), 1)
    FNV_AT(h, offsetof(Ws, dma_control), 1)
    FNV_AT(h, offsetof(Ws, dma_source), 8)             // dma_source target length
    FNV_AT(h, offsetof(Ws, display), sizeof ((Ws *)0)->display)
    FNV_AT(h, offsetof(Ws, sprites), sizeof ((Ws *)0)->sprites)
    FNV_AT(h, offsetof(Ws, ram), sizeof ((Ws *)0)->ram)
    FNV_AT(h, offsetof(Ws, keys), 2)
    FNV_AT(h, offsetof(Ws, key_select), 1)
    FNV_AT(h, offsetof(Ws, mapper_2003), 2)            // mapper_2003 rom_in_sram_window
    FNV_AT(h, offsetof(Ws, gpo_direction), 3)          // gpo_direction gpo_data rom_linear
    FNV_AT(h, offsetof(Ws, rom_bank), 4)               // rom_bank rom_bank_mask
    FNV_AT(h, offsetof(Ws, rom_window), sizeof ((Ws *)0)->rom_window)
    FNV_AT(h, offsetof(Ws, internal_nv), sizeof ((Ws *)0)->internal_nv)
    FNV_AT(h, offsetof(Ws, cart_nv), sizeof ((Ws *)0)->cart_nv)
    FNV_AT(h, offsetof(Ws, flash.phase), 4)            // phase identify fast exit_fast
    FNV_AT(h, offsetof(Ws, uart_control), 4)           // control status tx rx
    // the audio io page and the programmed audio state (the phases ride out)
    FNV_AT(h, offsetof(Ws, audio.index), 4)
    FNV_AT(h, offsetof(Ws, audio.io), sizeof ((WsAudio *)0)->io)
    FNV_AT(h, offsetof(Ws, audio.dma_source), 4)
    FNV_AT(h, offsetof(Ws, audio.dma_control), 4)      // dma_control pcm_control pcm_mode pcm_clock
    FNV_AT(h, offsetof(Ws, audio.pcm_channel), 1)
    // the rtc's programmed state (the second clock and delay ride out)
    FNV_AT(h, offsetof(Ws, rtc.date), sizeof ((WsRtc *)0)->date * 3)   // date alarm payload
    FNV_AT(h, offsetof(Ws, rtc.status), 5)             // status command data index length
    FNV_AT(h, offsetof(Ws, rtc.present), 4)            // present busy ready
    *full = h;
    *cl = h;
    (void)full;
}

uint64_t dp_ws_state_hash(DpWs *d) {
    uint64_t full, cl;
    state_hashes(d, &full, &cl);
    return cl;
}

// C11: the CPU register projection
uint64_t dp_ws_ctx_hash(DpWs *d) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned i = 0; i < 8; i++) { h ^= d->ws.reg[i]; h *= 0x01000193ull; }
    h ^= d->ws.cs; h *= 0x01000193ull;
    h ^= d->ws.ip; h *= 0x01000193ull;
    h ^= d->ws.ds; h *= 0x01000193ull;
    h ^= d->ws.es; h *= 0x01000193ull;
    h ^= d->ws.ss; h *= 0x01000193ull;
    h ^= d->ws.flags; h *= 0x01000193ull;
    h ^= d->ws.halted; h *= 0x01000193ull;
    return h;
}

// patch irq_pending inside a saved state blob (the IRQ fork hook)
void dp_ws_set_int(void *buf, unsigned bit) {
    if (bit < 8)
        ((Ws *)buf)->irq_pending |= (uint8_t)(1u << bit);
}

int dp_ws_invert_condition(DpWs *d, int condition) {
    enum { CF = 1, PF = 4, ZF = 64, SF = 128, OF = 2048 };
    uint16_t f = d->ws.flags;
    switch (condition) {
        case 0: case 1: f ^= OF; break;                    // O <-> NO
        case 2: case 3: f ^= CF; break;                    // B <-> NB
        case 4: case 5: f ^= ZF; break;                    // Z <-> NZ
        case 6: f &= (uint16_t)~(CF | ZF); break;          // BE -> A: neither set
        case 7: f |= CF; break;                            // A -> BE
        case 8: case 9: f ^= SF; break;                    // S <-> NS
        case 10: case 11: f ^= PF; break;                  // P <-> NP
        case 12: case 13: f ^= SF; break;                  // L <-> NL (SF != OF flips)
        case 14: f &= (uint16_t)~ZF; f ^= SF; break;       // LE -> G
        case 15: f |= ZF; break;                           // G -> LE
        default: return 0;                                 // the CX conditions
    }
    d->ws.flags = f;
    return 1;
}

int dp_ws_get_regs(DpWs *d, DpRegState *out) {
    dp_regstate_clear(out);
    for (unsigned i = 0; i < 8; i++) {
        out->v[DP_V30R_AX + i] = d->ws.reg[i];
        out->known[DP_V30R_AX + i] = 0x03;
    }
    out->v[DP_V30R_FL] = d->ws.flags & 0xFF;
    out->known[DP_V30R_FL] = 0x03;
    return 1;
}

// ---- input / video / observers --------------------------------------------------

void dp_ws_set_input(DpWs *d, uint32_t mask) {
    uint16_t keys = 0;
    // the runtime's key word (Cygne layout, active-low reads): X1 0x010,
    // X2 0x020, X3 0x040, X4 0x080, Y1 0x100, Y2 0x200, Y3 0x400, Y4 0x800,
    // Start 0x002, A 0x004, B 0x008
    if (mask & DP_IN_UP) keys |= 0x0100;      // Y1
    if (mask & DP_IN_DOWN) keys |= 0x0400;    // Y3
    if (mask & DP_IN_LEFT) keys |= 0x0010;    // X1
    if (mask & DP_IN_RIGHT) keys |= 0x0040;   // X3
    if (mask & DP_IN_B1) keys |= 0x0004;      // A
    if (mask & DP_IN_B2) keys |= 0x0008;      // B
    if (mask & DP_IN_START) keys |= 0x0002;   // Start
    if (mask & DP_IN_SELECT) keys |= 0x0020;  // X2
    if (mask & DP_IN_X) keys |= 0x0200;       // Y2
    if (mask & DP_IN_Y) keys |= 0x0080;       // X4
    if (mask & DP_IN_Z) keys |= 0x0800;       // Y4
    ws_set_keys(&d->ws, keys);
}

uint64_t dp_ws_frame(DpWs *d) { return d->ws.frames; }

int dp_ws_render(DpWs *d, uint32_t *argb, unsigned cap, int *w, int *h) {
    if (cap < (unsigned)(WS_LINEAR_W * WS_LINEAR_H)) return 0;
    for (unsigned line = 0; line < WS_LINEAR_H; line++) {
        uint16_t row[WS_WIDTH];
        ws_render_line_rgb(&d->ws, line, row);
        for (unsigned x = 0; x < WS_LINEAR_W; x++) {
            unsigned r = (row[x] >> 8) & 15, g = (row[x] >> 4) & 15, b = row[x] & 15;
            argb[line * WS_LINEAR_W + x] = 0xFF000000u | (r * 17 << 16) | (g * 17 << 8) | b * 17;
        }
    }
    *w = WS_LINEAR_W; *h = WS_LINEAR_H;
    return 1;
}

void dp_ws_observe(DpWs *d, const DpObservers *o, void *user) {
    d->obs = o;
    d->user = user;
}

void dp_ws_pad_arm(DpWs *d, int arm) {
    d->pad_armed = (unsigned)(arm != 0);
    d->pad_seen = 0;
}
int dp_ws_pad_seen(DpWs *d) { return (int)d->pad_seen; }
