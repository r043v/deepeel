/* The m0boy machine as deepeel drives it: one DpMach per live machine
 * (engine explorer, DFS, session worker, exporter).  Execution runs inside
 * gbl_run(); every observation and cut goes through the hooks, which
 * longjmp back into dp_mach_run when the engine ends a path.  A state image
 * is what gbl_state_io streams (~17 KiB + cartridge RAM), the unit the fork
 * queue, the tree and the exported save states all store. */
#ifndef DP_MACH_M0BOY_H
#define DP_MACH_M0BOY_H

#include <setjmp.h>
#include <stdint.h>

#include "deepeel/gbl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why a run returned.  The engine sets these through dp_mach_cut; m0boy.c
 * adds DP_CUT_HALT (halted with nothing left to wake it). */
enum {
    DP_CUT_NONE = 0,   /* returned without a cut (should not happen) */
    DP_CUT_STEPS,      /* per-path step budget */
    DP_CUT_CYCLES,     /* back-edge cap on a loop */
    DP_CUT_PAD,        /* slid into a padding run */
    DP_CUT_HALT,       /* no instruction for several frames */
    DP_CUT_SEGEND,     /* session segment over (frames, stagnation, death) */
    DP_CUT_QUIT,       /* global stop (SIGINT, budgets, completion) */
    DP_CUT_LOADED,     /* engine swapped the state, restart from gbr.pc */
};

typedef struct DpMach DpMach;

/* Engine callbacks.  on_step fires before each instruction with the opcode
 * byte (fetch excluded from on_read); it may cut.  on_read/on_write see
 * every CPU data access (the opcode and its operands come straight from the
 * mapped page).  on_vblank fires once per frame; inputs and frame counting
 * happen there, and it may cut. */
struct DpMach {
    const DpGblApi *api;
    int vi, ii;
    void *owner; /* engine back-pointer (the explorer / worker) */
    void (*on_step)(DpMach *m, uint16_t pc, uint8_t op, int entry);
    void (*on_read)(DpMach *m, uint16_t a, uint8_t v);
    void (*on_write)(DpMach *m, uint16_t a, uint8_t v);
    void (*on_vblank)(DpMach *m);

    /* screen, filled when render is on (gbl_skip_render == 0) */
    uint16_t line[160];
    uint8_t screen[144][160];
    uint16_t screen16[144][160]; /* the line's raw pixels (RGB565 in CGB colour mode) */
    int lines;
    int render;

    /* halt watchdog: frames without a single interpreted instruction */
    uint32_t last_instrs;
    int stalls;

    jmp_buf jmp;
    int cut;
    int in_vblank;     /* inside the vblank hook: the CPU may be mid-instruction (a STAT/LY read synced the PPU) */
    int cut_pending;   /* SEGEND asked there; taken at the next instruction boundary */
};

/* Variant pick from a cartridge header (0x147 mapper, 0x149 RAM size);
 * returns -1 when no compiled variant covers it. */
int dp_mach_pick_variant(const uint8_t *header);

DpMach *dp_mach_open(int vi, int ii);
// same, but NULL when the instance slot is already taken (a session worker,
// the preview): the parallel drain only runs on free instances
DpMach *dp_mach_open_exclusive(int vi, int ii);
void dp_mach_close(DpMach *m);
/* wire the hooks before a run: one live machine instance serves several
 * users (fork explorer, worker, human lane, preview) at disjoint times, and
 * each attaches its own callbacks — the previous wiring never fires again */
void dp_mach_attach(DpMach *m, void *owner,
                    void (*on_step)(DpMach *, uint16_t, uint8_t, int),
                    void (*on_read)(DpMach *, uint16_t, uint8_t),
                    void (*on_write)(DpMach *, uint16_t, uint8_t),
                    void (*on_vblank)(DpMach *));
/* Share the ROM image with the instance (all instances of a run share one
 * buffer) and reset the machine. */
void dp_mach_set_rom(DpMach *m, const uint8_t *rom, uint32_t size);
void dp_mach_reset(DpMach *m);
/* Run until a hook cuts.  Re-entry continues from the live state without
 * reloading. */
int dp_mach_run(DpMach *m);
void dp_mach_cut(DpMach *m, int reason);

/* State images: engine-owned buffers of dp_mach_state_bytes(m). */
uint32_t dp_mach_state_bytes(DpMach *m);
void dp_mach_save(DpMach *m, uint8_t *buf);
void dp_mach_load(DpMach *m, const uint8_t *buf);

/* Byte at pc through the instance's read map (opcode fetches). */
uint8_t dp_mach_op(DpMach *m, uint16_t pc);

#ifdef __cplusplus
}
#endif

#endif
