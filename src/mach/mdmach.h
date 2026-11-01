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
uint32_t dp_mdmach_state_pc(const void *buf);
void dp_mdmach_state_set_irq(void *buf, int vint, int hint);
/* VDP command latch: 1 if the next ctrl-port word is the second one. */
int dp_mdmach_vdp_pending(const DpMdMach *m);
uint16_t dp_mdmach_vdp_first(const DpMdMach *m);
/* Stops the current run after the instruction in progress (fault HALT). */
void dp_mdmach_halt(DpMdMach *m);
/* Live-state accessors used by the fork logic. */
uint16_t dp_mdmach_vdp_reg(const DpMdMach *m, unsigned r);
uint16_t dp_mdmach_sr(const DpMdMach *m);
uint32_t dp_mdmach_dn(const DpMdMach *m, unsigned n);
void dp_mdmach_set_pad(DpMdMach *m, uint8_t pad);
uint32_t dp_mdmach_pc(const DpMdMach *m);
uint32_t dp_mdmach_peek32(const DpMdMach *m, uint32_t addr);
#endif
