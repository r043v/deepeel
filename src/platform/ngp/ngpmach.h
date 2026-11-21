// ngpmach.h: the Neo Geo Pocket bridge over m0boy's runtime.  The runtime's
// files are compiled into this machine's archive with -DNGP_TRACE
// -DM0_TL=__thread and objcopy-prefixed, so one thread-local machine serves
// every worker thread.  The trace hooks route to the kernel's observers.
#ifndef DP_NGPMACH_H
#define DP_NGPMACH_H
#include <stdint.h>
#include <stddef.h>
#include "../../core/dpmachine.h"

int dp_ngp_init(const uint8_t *data, size_t size);   // owns a copy, installs the BIOS, resets
void dp_ngp_close(void);
int dp_ngp_step(void);                                // one instruction, 0 on fault (the explorer's run)
uint32_t dp_ngp_pc(void);
void dp_ngp_set_pc(uint32_t pc);
uint8_t dp_ngp_peek(uint32_t addr);                   // the bus view, no side effect
void dp_ngp_poke(uint32_t addr, uint8_t v);
size_t dp_ngp_state_size(void);
void dp_ngp_save(void *buf);
void dp_ngp_load(const void *buf);
uint64_t dp_ngp_state_hash(void);                     // B5: the clocks excluded
void dp_ngp_state_set_int(void *buf, unsigned index); // a pending interrupt in a saved state (the IRQ forks)
void dp_ngp_set_input(uint32_t mask);                 // the pad mask (DpMachine.set_input)
uint64_t dp_ngp_ctx_hash(void);                       // C11: the register components
unsigned dp_ngp_render(uint32_t *argb, unsigned cap, int *w, int *h);
unsigned dp_ngp_audio_read(int16_t *stereo, unsigned max_frames);
uint64_t dp_ngp_frame(void);                          // completed frames
int dp_ngp_rom_visible(uint32_t addr);                // the address is cartridge ROM
uint32_t dp_ngp_rom_offset(uint32_t addr);            // its offset in the image (0xFFFFFFFF: not ROM)
uint32_t dp_ngp_sp(void);                             // XSP
uint32_t dp_ngp_reg(unsigned slot);                   // a 32-bit register view (slots 0-7)
void dp_ngp_pad_arm(int arm);                         // the pad probe's dry-run detector
int dp_ngp_pad_seen(void);
void dp_ngp_observe(const struct DpObservers *o, void *user);   // the trace hooks' route

#endif
