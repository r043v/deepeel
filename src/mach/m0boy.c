/* m0boy machine host: context registry, hook dispatch, state images.
 * The generated instances file (cmake) owns the prefixed symbols: its
 * forwarders observe through ms_gbl_hook_* and then call the real runtime
 * symbol of that instance. */
#include "m0boy.h"

#include <stdlib.h>
#include <string.h>

static MsMach *mach_at[MS_GBL_VARIANTS][MS_GBL_MACHINES];

/* cartridge header 0x147 -> MBC, 0x149 -> RAM size, clamped to what the
 * variant table compiles. */
int ms_mach_pick_variant(const uint8_t *header) {
    static const struct { uint8_t lo, hi, mbc; } map[] = {
        {0x00, 0x00, 0}, {0x01, 0x03, 1}, {0x05, 0x06, 2},
        {0x0F, 0x13, 3}, {0x19, 0x1E, 5},
    };
    int mbc = -1;
    for (unsigned k = 0; k < sizeof map / sizeof map[0]; k++)
        if (header[0x147] >= map[k].lo && header[0x147] <= map[k].hi) mbc = map[k].mbc;
    if (mbc < 0) return -1;
    static const uint32_t ram_of[] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    uint32_t eram = header[0x149] < 6 ? ram_of[header[0x149]] : 0;
    if (mbc == 0) eram = 0;
    if (mbc == 2) eram = 0x200; /* fixed 512x4-bit */
    if (mbc == 1 && eram > 0x2000) eram = 0x2000;
    for (int v = 0; v < MS_GBL_VARIANTS; v++)
        if (ms_gbl_variant_mbc[v] == mbc && ms_gbl_variant_eram[v] == eram) return v;
    return -1;
}

MsMach *ms_mach_open(int vi, int ii) {
    if (vi < 0 || vi >= MS_GBL_VARIANTS || ii < 0 || ii >= MS_GBL_MACHINES) return NULL;
    if (mach_at[vi][ii]) return mach_at[vi][ii];
    MsMach *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->api = ms_gbl_inst[vi][ii];
    m->vi = vi;
    m->ii = ii;
    m->api->interp_only[0] = 1; /* the whole ROM goes through the interpreter */
    mach_at[vi][ii] = m;
    return m;
}

void ms_mach_attach(MsMach *m, void *owner,
                    void (*on_step)(MsMach *, uint16_t, uint8_t, int),
                    void (*on_read)(MsMach *, uint16_t, uint8_t),
                    void (*on_write)(MsMach *, uint16_t, uint8_t),
                    void (*on_vblank)(MsMach *)) {
    m->owner = owner;
    m->on_step = on_step;
    m->on_read = on_read;
    m->on_write = on_write;
    m->on_vblank = on_vblank;
}

void ms_mach_close(MsMach *m) {
    if (!m) return;
    mach_at[m->vi][m->ii] = NULL;
    free(m);
}

void ms_mach_set_rom(MsMach *m, const uint8_t *rom, uint32_t size) {
    if (size > m->api->rom_max) size = m->api->rom_max; // the caller checked
    memcpy(m->api->rom, rom, size);
}

void ms_mach_reset(MsMach *m) {
    m->last_instrs = 0;
    m->stalls = 0;
    m->api->reset();
}

int ms_mach_run(MsMach *m) {
    m->cut = MS_CUT_NONE;
    if (setjmp(m->jmp)) return m->cut;
    m->api->run(); /* never returns; hooks longjmp */
    return MS_CUT_NONE;
}

void ms_mach_cut(MsMach *m, int reason) {
    m->cut = reason;
    longjmp(m->jmp, 1);
}

/* gbl_state_io streams the state in a fixed order; a plain cursor buffer is
 * the whole transport.  A load rebuilds the derived page maps, so execution
 * simply continues from the image's gbr.pc - no restart dance needed here. */
typedef struct { uint8_t *buf; uint32_t pos, cap; } MsStIo;

static void st_save(void *data, uint32_t bytes, void *ctx) {
    MsStIo *io = ctx;
    if (io->pos + bytes <= io->cap) memcpy(io->buf + io->pos, data, bytes);
    io->pos += bytes;
}

static void st_load(void *data, uint32_t bytes, void *ctx) {
    MsStIo *io = ctx;
    if (io->pos + bytes <= io->cap) memcpy(data, io->buf + io->pos, bytes);
    io->pos += bytes;
}

uint32_t ms_mach_state_bytes(MsMach *m) { return m->api->state_bytes(); }

void ms_mach_save(MsMach *m, uint8_t *buf) {
    MsStIo io = {buf, 0, ms_mach_state_bytes(m)};
    m->api->state_io(st_save, &io, 0);
}

void ms_mach_load(MsMach *m, const uint8_t *buf) {
    MsStIo io = {(uint8_t *)buf, 0, ms_mach_state_bytes(m)};
    m->api->state_io(st_load, &io, 1);
}

uint8_t ms_mach_op(MsMach *m, uint16_t pc) {
    const uint8_t *page = m->api->rmap[pc >> 12];
    return page ? page[pc] : 0xFF;
}

/* ---- hooks --------------------------------------------------------------- */

void ms_gbl_hook_coverage(int v, int i, uint16_t bank, uint16_t addr, int entry) {
    (void)v;
    (void)i;
    (void)bank;
    (void)addr;
    (void)entry; /* exec marking happens in the engine's on_step */
}

void ms_gbl_hook_trace(int v, int i, uint16_t pc) {
    MsMach *m = mach_at[v][i];
    if (!m || !m->on_step) return;
    m->on_step(m, pc, ms_mach_op(m, pc), 0);
}

void ms_gbl_hook_read(int v, int i, uint16_t a, uint8_t val) {
    MsMach *m = mach_at[v][i];
    if (m && m->on_read) m->on_read(m, a, val);
}

void ms_gbl_hook_write(int v, int i, uint16_t a, uint8_t val) {
    MsMach *m = mach_at[v][i];
    if (m && m->on_write) m->on_write(m, a, val);
}

uint16_t *ms_gbl_hook_line_begin(int v, int i, uint8_t ly) {
    MsMach *m = mach_at[v][i];
    if (!m || !m->render || ly >= 144) return NULL;
    return m->line;
}

void ms_gbl_hook_line_end(int v, int i, uint8_t ly) {
    MsMach *m = mach_at[v][i];
    if (!m || !m->render) return;
    if (ly < 144) {
        for (int x = 0; x < 160; x++) m->screen[ly][x] = (uint8_t)m->line[x];
        m->lines++;
    }
}

void ms_gbl_hook_vblank(int v, int i) {
    MsMach *m = mach_at[v][i];
    if (!m) return;
    uint32_t instrs = *m->api->interp_instrs;
    if (instrs == m->last_instrs) {
        /* a frame without a single interpreted instruction: halted with
         * nothing left that can wake it */
        if (++m->stalls >= 8) {
            m->stalls = 0;
            ms_mach_cut(m, MS_CUT_HALT);
            return;
        }
    } else {
        m->last_instrs = instrs;
        m->stalls = 0;
    }
    if (m->on_vblank) m->on_vblank(m);
}

void ms_gbl_hook_safe(int v, int i) {
    (void)v;
    (void)i; /* state changes happen from the engine loop, never mid-path */
}
