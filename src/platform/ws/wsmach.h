// wsmach.h: the WonderSwan bridge over m0boy's runtime (targets/wonderswan).
// One heap machine per DpWs (the runtime has no globals), the observation
// points are the runtime's function-pointer sinks.  Boot matches the TAS
// watch (bizhawk timing, the Cygne start IO seeds, battery SRAM backing).
#ifndef DP_WS_WSMACH_H
#define DP_WS_WSMACH_H

#include <stddef.h>
#include <stdint.h>
#include "../../core/dpmachine.h"
#include "ws.h"

// the bridge machine: the runtime is a plain struct member (no globals), the
// sinks and the observers' context live beside it
typedef struct DpWs {
    Ws ws;
    uint8_t *rom_copy;
    uint32_t rom_bytes;
    uint8_t *sram;                 // the battery SRAM backing (owned)
    const DpObservers *obs;
    void *user;
    unsigned pad_armed, pad_seen;
    uint16_t *audio;               // ring of interleaved stereo samples at WS_AUDIO_RATE
    unsigned audio_cap, audio_head, audio_tail;
    unsigned audio_frames_total;
    unsigned watch_on;
} DpWs;

DpWs *dp_ws_open(const uint8_t *data, size_t size);
void dp_ws_close(DpWs *d);
int dp_ws_step(DpWs *d);
uint32_t dp_ws_pc(DpWs *d);
void dp_ws_set_pc(DpWs *d, uint32_t pc);
uint8_t dp_ws_peek(DpWs *d, uint32_t addr);
void dp_ws_poke(DpWs *d, uint32_t addr, uint8_t v);
uint32_t dp_ws_rom_offset(DpWs *d, uint32_t addr);
uint32_t dp_ws_sp(DpWs *d);
size_t dp_ws_state_size(DpWs *d);
void dp_ws_save(DpWs *d, uint8_t *buf);
void dp_ws_load(DpWs *d, const uint8_t *buf);
uint64_t dp_ws_state_hash(DpWs *d);   // the monotonic clocks excluded (B5)
uint64_t dp_ws_ctx_hash(DpWs *d);     // the CPU register projection (C11)
int dp_ws_get_regs(DpWs *d, DpRegState *out);
void dp_ws_set_input(DpWs *d, uint32_t mask);
void dp_ws_set_int(void *buf, unsigned bit);   // patch irq_pending in a saved blob
int dp_ws_invert_condition(DpWs *d, int condition);
uint64_t dp_ws_frame(DpWs *d);
int dp_ws_render(DpWs *d, uint32_t *argb, unsigned cap, int *w, int *h);
void dp_ws_observe(DpWs *d, const DpObservers *o, void *user);
void dp_ws_pad_arm(DpWs *d, int arm);   // the pad probe's dry-run detection
int dp_ws_pad_seen(DpWs *d);

#endif
