/* Bridge to m0boy's NES runtime (targets/nes/runtime): one global machine
 * per process, like MdFull.  Observation goes through m0boy's NES_TRACE
 * hooks; a cut is the hooks' stop flag, honoured at the next instruction
 * boundary with only the cycles actually consumed.  A state image is a
 * nes_state_io blob (~10 KiB); mid-frame images are legal — machine.line
 * carries the next un-run line, nes_frame_rest resumes them. */
#ifndef DP_NESMACH_H
#define DP_NESMACH_H
#include <stddef.h>
#include <stdint.h>

typedef struct DpNesMach DpNesMach;

typedef struct {
    void *user;
    /* before the instruction at pc executes (opcode fetched) */
    void (*on_step)(void *user, unsigned pc, unsigned op);
    /* data accesses only (the opcode fetch and the stack stay out) */
    void (*on_read)(void *user, unsigned addr);
    void (*on_write)(void *user, unsigned addr, unsigned value);
    /* an NMI ($FFFA) or IRQ/BRK ($FFFE) sequence starts */
    void (*on_interrupt)(void *user, unsigned vector);
    /* after a completed frame (262 lines) */
    void (*on_frame)(void *user);
} DpNesHooks;

/* iNES image (header + PRG + CHR); mappers 0 (NROM), 2 (UxROM) — the
 * runtime's boards — and 3 (CNROM), 7 (AxROM) — the bridge's.  Returns NULL with a human message in `err`. */
DpNesMach *dp_nesmach_open(const uint8_t *data, size_t size, int mapper,
                           char *err, size_t errsz);
void dp_nesmach_close(DpNesMach *m);
void dp_nesmach_reset(DpNesMach *m);
void dp_nesmach_set_hooks(DpNesMach *m, const DpNesHooks *h);

/* Runs instructions until max_steps, a hook stop or a completed frame
 * boundary the caller's on_frame handles; returns the steps executed. */
uint64_t dp_nesmach_run(DpNesMach *m, uint64_t max_steps);
/* One full frame: finishes the current one, then runs a new one. */
void dp_nesmach_frame(DpNesMach *m);
void dp_nesmach_set_input(DpNesMach *m, uint8_t pad0, uint8_t pad1);

size_t dp_nesmach_state_size(void);
void dp_nesmach_save(const DpNesMach *m, void *buf);
void dp_nesmach_load(DpNesMach *m, const void *buf);
/* exact-state hash for the fork dedup: the volatile clock fields
 * (cpu.cycles, frames) are excluded */
uint64_t dp_nesmach_state_hash(const void *buf);

/* the canonical cartridge cell behind a CPU address with the live bank
 * mapping: bank * $4000 + (addr & $3FFF), -1 outside the PRG */
int64_t dp_nesmach_rom_index(const DpNesMach *m, unsigned addr);
/* a CPU bus read through the machine (RAM, PPU, APU, pads, PRG windows) */
uint8_t dp_nesmach_read(const DpNesMach *m, unsigned addr);
unsigned dp_nesmach_pc(const DpNesMach *m);
unsigned dp_nesmach_p(const DpNesMach *m);       /* live flags byte */
unsigned dp_nesmach_nmi_pending(const DpNesMach *m);
unsigned dp_nesmach_banked(const DpNesMach *m);
unsigned dp_nesmach_bank(const DpNesMach *m);
unsigned dp_nesmach_bank_count(const DpNesMach *m);
size_t dp_nesmach_prg_size(const DpNesMach *m);
unsigned dp_nesmach_frames(const DpNesMach *m);
const uint8_t *dp_nesmach_oam(const DpNesMach *m);
/* the saved state's P flag byte (a fork force patches it in place) */
uint8_t *dp_nesmach_state_p(void *buf);
/* the saved state's pending-NMI edge (an interrupt fork clears it) */
uint8_t *dp_nesmach_state_nmi_edge(void *buf);

/* mapper the bridge runs (0, 2, 3, 7) and what a $8000+ write changes:
 * 0 nothing, 1 the PRG window (16 KiB slots), 2 the CHR bank (CNROM) */
unsigned dp_nesmach_mapper(const DpNesMach *m);
int dp_nesmach_mapper_write(const DpNesMach *m, unsigned addr, unsigned value,
                            unsigned *old_slot, unsigned *new_slot);

/* CHR ROM as an indexed space (bank * 8 KiB + pattern address; size 0 and
 * index -1 on CHR-RAM boards).  chr_tiles fills out[] with
 * (cell << 1 | is_sprite) for the tiles the PPU state refers to now. */
size_t dp_nesmach_chr_size(const DpNesMach *m);
int64_t dp_nesmach_chr_index(const DpNesMach *m, unsigned addr);
size_t dp_nesmach_chr_tiles(const DpNesMach *m, uint64_t *out, size_t cap);

unsigned dp_nesmach_ppu_v(const DpNesMach *m);  /* the PPU address ($2006/$2007) */

#endif
