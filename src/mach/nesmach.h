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
/* the linear PRG image (cell index = bank * $4000 + offset) */
const uint8_t *dp_nesmach_prg_rom(const DpNesMach *m);
unsigned dp_nesmach_input(const DpNesMach *m);   /* live pad 0 mask */
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
/* the saved state's 2 KiB CPU RAM, and the live stack pointer */
uint8_t *dp_nesmach_state_ram(void *buf);
unsigned dp_nesmach_sp(const DpNesMach *m);
unsigned dp_nesmach_state_pc(const void *buf);
/* coarse context hash (registers, pc, mapper registers; RAM and clocks excluded) */
uint64_t dp_nesmach_state_ctx_hash(const void *buf);
/* the saved state's pending-NMI edge (an interrupt fork clears it) */
uint8_t *dp_nesmach_state_nmi_edge(void *buf);

/* mapper the bridge runs (0, 2, 3, 7) and what a $8000+ write changes:
 * 0 nothing, 1 the PRG window (16 KiB slots), 2 the CHR bank (CNROM) */
/* explicit entry: PC + switchable bank (UxROM/NROM only), 1 on success */
int dp_nesmach_enter(DpNesMach *m, unsigned bank, unsigned addr);
/* MMC3: an 8 KiB bank under the R6/R7 slot holding addr (PRG mode 0); 1 on success */
/* boards whose 16 KiB $8000 slot dp_nesmach_enter can set (UxROM, MMC1, Camerica, UN1ROM) */
int dp_nesmach_cross16(const DpNesMach *m);
int dp_nesmach_mmc3_mode1(const DpNesMach *m);   /* MMC3 in PRG mode 1 ($C000 window switchable) */
int dp_nesmach_map_8k(DpNesMach *m, unsigned addr, unsigned bank8);
unsigned dp_nesmach_mapper(const DpNesMach *m);
int dp_nesmach_mapper_write(const DpNesMach *m, unsigned addr, unsigned value,
                            unsigned *old_slot, unsigned *new_slot);

/* CHR ROM as an indexed space (bank * 8 KiB + pattern address; size 0 and
 * index -1 on CHR-RAM boards).  chr_tiles fills out[] with
 * (cell << 1 | is_sprite) for the tiles the PPU state refers to now. */
size_t dp_nesmach_chr_size(const DpNesMach *m);
int64_t dp_nesmach_chr_index(const DpNesMach *m, unsigned addr);
size_t dp_nesmach_chr_tiles(const DpNesMach *m, uint64_t *out, size_t cap);

/* the 2 KiB CPU RAM, and a coarse screen fingerprint (nametable tiles >> 3 sampled every 4th,
 * coarse scroll, nametable select): equal keys = the same screen cell (Go-Explore) */
/* the 160x128 panel picture of the last frame (RGB565 words as the renderer writes them);
 * enabling it makes the PPU render rows, which costs time: off by default */
void dp_nesmach_watch_enable(DpNesMach *m, int on);
void dp_nesmach_render(const DpNesMach *m, uint32_t *argb);   /* 160x128 */
const uint8_t *dp_nesmach_ram(const DpNesMach *m);
uint64_t dp_nesmach_screen_key(const DpNesMach *m);
unsigned dp_nesmach_ppu_v(const DpNesMach *m);  /* the PPU address ($2006/$2007) */

#endif
