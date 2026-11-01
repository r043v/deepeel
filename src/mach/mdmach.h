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

uint32_t dp_mdmach_pc(const DpMdMach *m);
uint32_t dp_mdmach_peek32(const DpMdMach *m, uint32_t addr);
#endif
