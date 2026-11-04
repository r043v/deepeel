/* m0boy machine host: context registry, hook dispatch, state images.
 * The generated instances file (cmake) owns the prefixed symbols: its
 * forwarders observe through dp_gbl_hook_* and then call the real runtime
 * symbol of that instance. */
#include "m0boy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DpMach *mach_at[DP_GBL_VARIANTS][DP_GBL_MACHINES];

/* cartridge header 0x147 -> MBC, 0x149 -> RAM size, clamped to what the
 * variant table compiles. */
int dp_mach_pick_variant(const uint8_t *header) {
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
    if (mbc == 1 && eram > 0x8000) eram = 0x8000; /* MBC1 banks 32 KiB at most */
    for (int v = 0; v < DP_GBL_VARIANTS; v++)
        if (dp_gbl_variant_mbc[v] == mbc && dp_gbl_variant_eram[v] == eram) return v;
    return -1;
}

DpMach *dp_mach_open(int vi, int ii) {
    if (vi < 0 || vi >= DP_GBL_VARIANTS || ii < 0 || ii >= DP_GBL_MACHINES) return NULL;
    if (mach_at[vi][ii]) return mach_at[vi][ii];
    DpMach *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->api = dp_gbl_inst[vi][ii];
    m->vi = vi;
    m->ii = ii;
    m->api->interp_only[0] = 1; /* the whole ROM goes through the interpreter */
    mach_at[vi][ii] = m;
    return m;
}

DpMach *dp_mach_open_exclusive(int vi, int ii) {
    if (vi < 0 || vi >= DP_GBL_VARIANTS || ii < 0 || ii >= DP_GBL_MACHINES) return NULL;
    if (mach_at[vi][ii]) return NULL; // the slot is taken (sessions, preview)
    return dp_mach_open(vi, ii);
}

void dp_mach_attach(DpMach *m, void *owner,
                    void (*on_step)(DpMach *, uint16_t, uint8_t, int),
                    void (*on_read)(DpMach *, uint16_t, uint8_t),
                    void (*on_write)(DpMach *, uint16_t, uint8_t),
                    void (*on_vblank)(DpMach *)) {
    m->owner = owner;
    m->on_step = on_step;
    m->on_read = on_read;
    m->on_write = on_write;
    m->on_vblank = on_vblank;
}

void dp_mach_close(DpMach *m) {
    if (!m) return;
    mach_at[m->vi][m->ii] = NULL;
    free(m);
}

void dp_mach_set_rom(DpMach *m, const uint8_t *rom, uint32_t size) {
    if (size > m->api->rom_max) size = m->api->rom_max; // the caller checked
    memcpy(m->api->rom, rom, size);
    /* the mapper wraps bank numbers at the cartridge's real size */
    uint32_t banks = 2;
    while (banks * 0x4000u < size) banks <<= 1;
    *m->api->rom_banks = banks;
    /* MBC1M: a 1 MiB cartridge carrying the Nintendo logo in two or more of
     * its 256 KiB sub-games (banks 0x10, 0x20, 0x30) is wired as a multicart */
    static const uint8_t logo[4] = {0xCE, 0xED, 0x66, 0x66};
    int games = 0;
    if (size == 0x100000)
        for (int g = 1; g < 4; g++)
            if (!memcmp(rom + (size_t)g * 0x40000 + 0x104, logo, 4)) games++;
    *m->api->mbc1m = (uint8_t)(games >= 2);
}

void dp_mach_reset(DpMach *m) {
    m->last_instrs = 0;
    m->stalls = 0;
    m->api->reset();
}

int dp_mach_run(DpMach *m) {
    m->cut = DP_CUT_NONE;
    if (setjmp(m->jmp)) return m->cut;
    m->api->run(); /* never returns; hooks longjmp */
    return DP_CUT_NONE;
}

void dp_mach_cut(DpMach *m, int reason) {
    m->cut = reason;
    longjmp(m->jmp, 1);
}

/* gbl_state_io streams the state in a fixed order; a plain cursor buffer is
 * the whole transport.  A load rebuilds the derived page maps, so execution
 * simply continues from the image's gbr.pc - no restart dance needed here. */
typedef struct { uint8_t *buf; uint32_t pos, cap; } DpStIo;

static void st_save(void *data, uint32_t bytes, void *ctx) {
    DpStIo *io = ctx;
    if (io->pos + bytes <= io->cap) memcpy(io->buf + io->pos, data, bytes);
    io->pos += bytes;
}

static void st_load(void *data, uint32_t bytes, void *ctx) {
    DpStIo *io = ctx;
    if (io->pos + bytes <= io->cap) memcpy(data, io->buf + io->pos, bytes);
    io->pos += bytes;
}

uint32_t dp_mach_state_bytes(DpMach *m) { return m->api->state_bytes(); }

void dp_mach_save(DpMach *m, uint8_t *buf) {
    DpStIo io = {buf, 0, dp_mach_state_bytes(m)};
    m->api->state_io(st_save, &io, 0);
}

void dp_mach_load(DpMach *m, const uint8_t *buf) {
    DpStIo io = {(uint8_t *)buf, 0, dp_mach_state_bytes(m)};
    m->api->state_io(st_load, &io, 1);
}

uint8_t dp_mach_op(DpMach *m, uint16_t pc) {
    const uint8_t *page = m->api->rmap[pc >> 12];
    return page ? page[pc] : 0xFF;
}

/* ---- hooks --------------------------------------------------------------- */

void dp_gbl_hook_coverage(int v, int i, uint16_t bank, uint16_t addr, int entry) {
    (void)v;
    (void)i;
    (void)bank;
    (void)addr;
    (void)entry; /* exec marking happens in the engine's on_step */
}

int dp_dbg_trace;
FILE *dp_dbg_trace_f;
static FILE *dp_instr_f;
void dp_gbl_hook_trace(int v, int i, uint16_t pc) {
    DpMach *m = mach_at[v][i];
    if (dp_dbg_trace && dp_dbg_trace_f) { // desync diagnostics: one frame's pc sequence
        const gbl_cpu_t *c = m->api->gbr;
        fprintf(dp_dbg_trace_f, "%d.%d %04x %02x %04x %04x %04x %04x\n", v, i, pc, c->a, c->bc, c->de, c->hl, c->sp);
    }
    // DEEPEEL_TAS_INSTR=<file>: one "<cycle> <pc> <a> <f> <bc> <de> <hl> <sp> <ime>"
    // per executed instruction (--tas-solo: the TAS worker's machine is the only
    // one), the same columns tools/gbhawk_ref --instr writes
    {
        if (!dp_instr_f) {
            const char *p = getenv("DEEPEEL_TAS_INSTR");
            if (p) dp_instr_f = fopen(p, "w");
        }
        if (dp_instr_f) {
            const gbl_cpu_t *c = m->api->gbr;
            uint8_t f = (uint8_t)((c->zr == 0 ? 0x80 : 0) | (c->nf ? 0x40 : 0) | (c->hf ? 0x20 : 0) | (c->cf ? 0x10 : 0));
            fprintf(dp_instr_f, "%u %04x %02x %02x %04x %04x %04x %04x %u\n", (unsigned)c->cyc, pc, c->a, f, c->bc, c->de, c->hl, c->sp, c->ime);
        }
    }
    if (!m || !m->on_step) return;
    m->on_step(m, pc, dp_mach_op(m, pc), 0);
}

void dp_gbl_hook_read(int v, int i, uint16_t a, uint8_t val) {
    DpMach *m = mach_at[v][i];
    static long watchr = -2;
    if (watchr == -2) {
        const char *e = getenv("DEEPEEL_DEBUG_WATCHR"); // desync diagnostics
        watchr = e ? strtol(e, NULL, 16) : -1;
    }
    if (m && watchr == a) fprintf(stderr, "R v=%u a=%04x val=%02x pc=%04x\n", (unsigned)*m->api->frame, a, val, m->api->gbr->pc);
    if (m && m->on_read) m->on_read(m, a, val);
}

void dp_gbl_hook_write(int v, int i, uint16_t a, uint8_t val) {
    DpMach *m = mach_at[v][i];
    static long watchw = -2;
    if (watchw == -2) {
        const char *e = getenv("DEEPEEL_DEBUG_WATCHW"); // desync diagnostics
        watchw = e ? strtol(e, NULL, 16) : -1;
    }
    if (m && watchw == a) {
        const gbl_cpu_t *c = m->api->gbr;
        fprintf(stderr, "W v=%u a=%04x val=%02x pc=%04x af=%02x bc=%04x de=%04x hl=%04x sp=%04x bank=%u\n",
                (unsigned)*m->api->frame, a, val, c->pc, c->a, c->bc, c->de, c->hl, c->sp, c->rom_bank);
    }
    if (m && m->on_write) m->on_write(m, a, val);
}

uint16_t *dp_gbl_hook_line_begin(int v, int i, uint8_t ly) {
    DpMach *m = mach_at[v][i];
    if (!m || !m->render || ly >= 144) return NULL;
    return m->line;
}

void dp_gbl_hook_line_end(int v, int i, uint8_t ly) {
    DpMach *m = mach_at[v][i];
    if (!m || !m->render) return;
    if (ly < 144) {
        for (int x = 0; x < 160; x++) m->screen[ly][x] = (uint8_t)m->line[x];
        m->lines++;
    }
}

void dp_gbl_hook_vblank(int v, int i) {
    DpMach *m = mach_at[v][i];
    if (!m) return;
    uint32_t instrs = *m->api->interp_instrs;
    if (instrs == m->last_instrs) {
        /* a frame without a single interpreted instruction: halted with
         * nothing left that can wake it */
        if (++m->stalls >= 8) {
            m->stalls = 0;
            dp_mach_cut(m, DP_CUT_HALT);
            return;
        }
    } else {
        m->last_instrs = instrs;
        m->stalls = 0;
    }
    if (m->on_vblank) m->on_vblank(m);
}

void dp_gbl_hook_safe(int v, int i) {
    (void)v;
    (void)i; /* state changes happen from the engine loop, never mid-path */
}
