/* Bridge to m0boy's MdFull (Mega Drive reference machine).
 * MdFull reads the ROM from the globals md_rom/md_rom_size, so one ROM is
 * live per process (milestone 1 is single-threaded). Hooks come from the
 * MD_TRACE build of m0boy's 68000 core. */
#ifndef DP_MDMACH_H
#define DP_MDMACH_H
#include <stddef.h>
#include <stdint.h>

#define DP_MD_ROM_MAX 0x400000u
#define DP_MD_FAULT_BUDGET 0x7E /* MdFull.fault value used to cut by budget */
#define DP_MD_FAULT_HALT 0x7D   /* path stopped on request (dp_mdmach_halt) */

typedef struct DpMdMach DpMdMach;

typedef struct {
    void *user;
    /* before the instruction at pc executes */
    void (*on_step)(void *user, uint32_t pc, uint16_t op);
    /* after it: len = bytes consumed (opcode + extension words), next = new pc */
    void (*on_end)(void *user, uint32_t pc, uint32_t next, unsigned len);
    /* data accesses only (instruction fetches are filtered out) */
    void (*on_read)(void *user, uint32_t addr, unsigned size);
    void (*on_write)(void *user, uint32_t addr, unsigned size, uint32_t value);
    /* Called after a completed frame, never after a cut or machine fault. */
    void (*on_frame)(void *user);
    /* Successful 68k-bus word read by VDP DMA, not a CPU read. */
    void (*on_dma_read)(void *user, uint32_t addr, unsigned size);
    /* Z80 sound CPU. Before an instruction: pc, length, ROM address of pc
     * when it executes from the banked window (UINT32_MAX otherwise). */
    void (*on_z80_step)(void *user, uint16_t pc, unsigned len, uint32_t rom_pc);
    void (*on_z80_end)(void *user);
    /* Z80 read of a ROM byte through the $8000 bank window (fetches included). */
    void (*on_z80_rom)(void *user, uint32_t rom_addr);
    /* Store on the sound bus (Z80 RAM, YM2612, bank register, PSG), from the
     * 68000 or from the Z80. ymreg: YM register selected by a data-port
     * store, -1 otherwise, -2 PSG. */
    void (*on_sound_store)(void *user, uint16_t addr, uint8_t value, int ymreg);
} DpMdHooks;

DpMdMach *dp_mdmach_open(const uint8_t *rom, size_t size); /* copies the ROM */
void dp_mdmach_close(DpMdMach *m);
void dp_mdmach_reset(DpMdMach *m);
void dp_mdmach_set_hooks(DpMdMach *m, const DpMdHooks *h);

/* Runs up to max_steps instructions. Returns the steps executed.
 * *cut = 1 if the budget stopped the run, 0 otherwise (*fault tells why). */
uint64_t dp_mdmach_run(DpMdMach *m, uint64_t max_steps, int *cut, int *fault);

/* Whole-machine snapshots (fork support). A snapshot is a plain byte copy of
 * the machine; the host-only pointers of MdFull are always NULL here. */
size_t dp_mdmach_state_size(void);
void dp_mdmach_save(const DpMdMach *m, void *buf);
void dp_mdmach_load(DpMdMach *m, const void *buf);
void dp_mdmach_state_set_pc(void *buf, uint32_t pc);
void dp_mdmach_state_set_dn_word(void *buf, unsigned n, uint16_t w);
/* Force a 68000 condition by changing the fewest CCR bits possible. */
void dp_mdmach_state_force_cc(void *buf, unsigned condition, int taken);
void dp_mdmach_state_force_dbcc(void *buf, unsigned condition, unsigned reg, int taken,
                                uint32_t old_dn, uint16_t old_sr);
uint32_t dp_mdmach_state_pc(const void *buf);
void dp_mdmach_state_set_irq(void *buf, int vint, int hint);
void dp_mdmach_state_set_pad(void *buf, uint8_t pad);
void dp_mdmach_state_set_an(void *buf, unsigned n, uint32_t v);
/* VDP command latch: 1 if the next ctrl-port word is the second one. */
int dp_mdmach_vdp_pending(const DpMdMach *m);
uint16_t dp_mdmach_vdp_first(const DpMdMach *m);
uint8_t dp_mdmach_vdp_code(const DpMdMach *m);
int dp_mdmach_rom_visible(const DpMdMach *m, uint32_t addr);
/* Stops the current run after the instruction in progress (fault HALT). */
void dp_mdmach_halt(DpMdMach *m);
/* Live-state accessors used by the fork logic. */
uint16_t dp_mdmach_vdp_reg(const DpMdMach *m, unsigned r);
uint16_t dp_mdmach_sr(const DpMdMach *m);
uint32_t dp_mdmach_dn(const DpMdMach *m, unsigned n);
uint32_t dp_mdmach_an(const DpMdMach *m, unsigned n);
void dp_mdmach_set_pad(DpMdMach *m, uint8_t pad);
uint32_t dp_mdmach_pc(const DpMdMach *m);
uint32_t dp_mdmach_fault_pc(const DpMdMach *m);
uint32_t dp_mdmach_peek32(const DpMdMach *m, uint32_t addr);
const uint8_t *dp_mdmach_ram(const DpMdMach *m);   /* the 64 KiB work RAM */
/* coarse screen fingerprint (plane A/B name tables: tile >> 3 sampled every 4th, scroll >> 5) */
/* render the current VDP state: 320x224 ARGB into pixels (stride 320) */
void dp_mdmach_render(const DpMdMach *m, uint32_t *pixels);
uint64_t dp_mdmach_screen_key(const DpMdMach *m);
uint8_t dp_mdmach_ram8(const DpMdMach *m, uint32_t addr); /* work RAM byte */
#endif
