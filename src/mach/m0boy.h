/* The m0boy machine as memscan drives it: one MsMach per live machine
 * (engine explorer, DFS, session worker, exporter).  Execution runs inside
 * gbl_run(); every observation and cut goes through the hooks, which
 * longjmp back into ms_mach_run when the engine ends a path.  A state image
 * is what gbl_state_io streams (~17 KiB + cartridge RAM), the unit the fork
 * queue, the tree and the exported save states all store. */
#ifndef MS_MACH_M0BOY_H
#define MS_MACH_M0BOY_H

#include <setjmp.h>
#include <stdint.h>

#include "memscan/gbl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why a run returned.  The engine sets these through ms_mach_cut; m0boy.c
 * adds MS_CUT_HALT (halted with nothing left to wake it). */
enum {
    MS_CUT_NONE = 0,   /* returned without a cut (should not happen) */
    MS_CUT_STEPS,      /* per-path step budget */
    MS_CUT_CYCLES,     /* back-edge cap on a loop */
    MS_CUT_PAD,        /* slid into a padding run */
    MS_CUT_HALT,       /* no instruction for several frames */
    MS_CUT_SEGEND,     /* session segment over (frames, stagnation, death) */
    MS_CUT_QUIT,       /* global stop (SIGINT, budgets, completion) */
    MS_CUT_LOADED,     /* engine swapped the state, restart from gbr.pc */
};

typedef struct MsMach MsMach;

/* Engine callbacks.  on_step fires before each instruction with the opcode
 * byte (fetch excluded from on_read); it may cut.  on_read/on_write see
 * every CPU data access (the opcode and its operands come straight from the
 * mapped page).  on_vblank fires once per frame; inputs and frame counting
 * happen there, and it may cut. */
struct MsMach {
    const MsGblApi *api;
    int vi, ii;
    void *owner; /* engine back-pointer (the explorer / worker) */
    void (*on_step)(MsMach *m, uint16_t pc, uint8_t op, int entry);
    void (*on_read)(MsMach *m, uint16_t a, uint8_t v);
    void (*on_write)(MsMach *m, uint16_t a, uint8_t v);
    void (*on_vblank)(MsMach *m);

    /* screen, filled when render is on (gbl_skip_render == 0) */
    uint16_t line[160];
    uint8_t screen[144][160];
    int lines;
    int render;

    /* halt watchdog: frames without a single interpreted instruction */
    uint32_t last_instrs;
    int stalls;

    jmp_buf jmp;
    int cut;
};

/* Variant pick from a cartridge header (0x147 mapper, 0x149 RAM size);
 * returns -1 when no compiled variant covers it. */
int ms_mach_pick_variant(const uint8_t *header);

MsMach *ms_mach_open(int vi, int ii);
void ms_mach_close(MsMach *m);
/* wire the hooks before a run: one live machine instance serves several
 * users (fork explorer, worker, human lane, preview) at disjoint times, and
 * each attaches its own callbacks — the previous wiring never fires again */
void ms_mach_attach(MsMach *m, void *owner,
                    void (*on_step)(MsMach *, uint16_t, uint8_t, int),
                    void (*on_read)(MsMach *, uint16_t, uint8_t),
                    void (*on_write)(MsMach *, uint16_t, uint8_t),
                    void (*on_vblank)(MsMach *));
/* Share the ROM image with the instance (all instances of a run share one
 * buffer) and reset the machine. */
void ms_mach_set_rom(MsMach *m, const uint8_t *rom, uint32_t size);
void ms_mach_reset(MsMach *m);
/* Run until a hook cuts.  Re-entry continues from the live state without
 * reloading. */
int ms_mach_run(MsMach *m);
void ms_mach_cut(MsMach *m, int reason);

/* State images: engine-owned buffers of ms_mach_state_bytes(m). */
uint32_t ms_mach_state_bytes(MsMach *m);
void ms_mach_save(MsMach *m, uint8_t *buf);
void ms_mach_load(MsMach *m, const uint8_t *buf);

/* Byte at pc through the instance's read map (opcode fetches). */
uint8_t ms_mach_op(MsMach *m, uint16_t pc);

#ifdef __cplusplus
}
#endif

#endif
