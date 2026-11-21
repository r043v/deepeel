#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include "mdmach.h"
#include <stdlib.h>
#include <string.h>
#include "md_full.h"
#include "md_trace.h"
#include "md_dbg.h"

void dp_mdrom_set(const uint8_t *rom, size_t size);
uint32_t dp_mdrom_peek32(uint32_t a);
void dp_md_full_frame(MdFull *m, int native, int resume);

struct DpMdMach {
    MdFull *m;
    DpMdHooks h;
    uint64_t steps, budget;
    int resume_frame;
    uint32_t *screen;   // watch only: the frame drawn line by line (H-int palette swaps, e.g. Sonic 1 water line)
};

static __thread DpMdMach *cur;   // per thread: parallel workers each run their own machine
void dp_md_dma_observe(MdFull *m, uint32_t addr) {
    if (cur && cur->m == m && cur->h.on_dma_read &&
        !(m->sram_enable && addr >= 0x200000u && addr < 0x210000u))
        cur->h.on_dma_read(cur->h.user, addr, 2);
}

static uint8_t z80_peek(void *u, uint16_t a) {
    MdFull *m = u;
    if (a < 0x4000u) return m->zram[a & 0x1fffu];
    if (a >= 0x8000u) {
        uint32_t r = (m->z80_bank | (a & 0x7fffu)) & 0xffffffu;
        return r < md_rom_size ? md_rom[r] : 0xff;
    }
    return 0xff;
}
void dp_md_z80_begin(MdFull *m, z80_t *z) {
    if (!cur || cur->m != m || !cur->h.on_z80_step) return;
    z80_t t = *z;   /* decode on a copy with a side-effect-free bus */
    t.user = m; t.read = z80_peek; t.in = z80_peek;
    z80_decoded_t d;
    unsigned len = z80_decode(&t, &d) ? d.length : 1;
    uint32_t rom_pc = UINT32_MAX;
    if (z->pc >= 0x8000u) rom_pc = (m->z80_bank | (z->pc & 0x7fffu)) & 0xffffffu;
    cur->h.on_z80_step(cur->h.user, z->pc, len, rom_pc);
}
void dp_md_z80_end(MdFull *m) {
    if (cur && cur->m == m && cur->h.on_z80_end) cur->h.on_z80_end(cur->h.user);
}
void dp_md_z80_rom_read(MdFull *m, uint32_t a) {
    if (cur && cur->m == m && cur->h.on_z80_rom) cur->h.on_z80_rom(cur->h.user, a);
}
void dp_md_sound_store(MdFull *m, uint16_t addr, uint8_t value) {
    if (!cur || cur->m != m || !cur->h.on_sound_store) return;
    int ymreg = -1;
    if (addr >= 0x4000u && addr < 0x6000u && (addr & 1u)) ymreg = m->ym.addr[(addr >> 1) & 1u];
    else if ((addr & 0xfff9u) == 0x7f11u) ymreg = -2;
    cur->h.on_sound_store(cur->h.user, addr, value, ymreg);
}

static uint32_t vint_n; static const struct MdFull *vint_owner;
static uint32_t cur_ipc;   // PC of the instruction being executed (for the write log)
static void t_step(void *u, MdFull *m, uint32_t pc, uint16_t op) {
    DpMdMach *d = u;
    cur_ipc = pc;
    { static int init; static uint32_t va, vb, lo, hi;
      if (!init) { init = 1; const char *e = getenv("DP_MD_PCLOG_V"), *r = getenv("DP_MD_PCLOG");   // diagnostic: instruction PCs in V-int window a:b, optional pc range lo:hi
          if (!(e && sscanf(e, "%u:%u", &va, &vb) == 2)) vb = 0; if (!(r && sscanf(r, "%x:%x", &lo, &hi) == 2)) { lo = 0; hi = 0xffffff; } }
      if (vb && m == vint_owner && vint_n >= va && vint_n <= vb && pc >= lo && pc <= hi)
          fprintf(stderr, "I v=%u line=%u pc=%06x op=%04x cyc=%u d0=%08x a0=%08x a3=%08x e=%02x\n", vint_n, (unsigned)m->line, (unsigned)pc, op, (unsigned)m->cycles, (unsigned)m->d[0], (unsigned)m->a[0], (unsigned)m->a[3], (unsigned)m->ram[0xF13E]); }
    if (d->steps >= d->budget) {
        m->fault = DP_MD_FAULT_BUDGET;
        m->fault_pc = pc;
        m->slice_limit = 0;
        return;
    }
    ++d->steps;
    if (d->h.on_step) d->h.on_step(d->h.user, pc, op);
}

static void t_end(void *u, MdFull *m, uint32_t pc, uint32_t next) {
    DpMdMach *d = u;
    (void)m;
    if (!d->h.on_end) return;
    unsigned len = 2u + md_trace.fetched;
    d->h.on_end(d->h.user, pc, next, len);
}

static void t_rd(void *u, MdFull *m, uint32_t a, unsigned sz) {
    DpMdMach *d = u;
    (void)m;
    if (d->h.on_read) d->h.on_read(d->h.user, a, sz);
}

/* DP_MD_WLOG=lo:hi (hex work-RAM-style address range) with DP_MD_WLOG_V=a:b (V-int sample numbers): diagnostic log of the
 * CPU writes into the range, with the V-int count, frame, line and the writing instruction's PC (compare with Gens-rr's
 * memory.registerwrite, tools/gens_wlog.lua). */
static void wlog(MdFull *m, uint32_t a, unsigned sz, uint32_t v) {
    static int init; static uint32_t lo, hi, va, vb;
    if (!init) { init = 1; const char *e = getenv("DP_MD_WLOG"), *g = getenv("DP_MD_WLOG_V");
        if (e && sscanf(e, "%x:%x", &lo, &hi) == 2) { va = 0; vb = ~0u; if (g) sscanf(g, "%u:%u", &va, &vb); } else hi = 0; }
    if (!hi || m != vint_owner || a < lo || a > hi || vint_n < va || vint_n > vb) return;
    fprintf(stderr, "W v=%u frames=%u line=%u pc=%06x a=%06x sz=%u v=%0*x\n", vint_n, (unsigned)m->frames, (unsigned)m->line,
            (unsigned)cur_ipc, (unsigned)a, sz, (int)sz * 2, (unsigned)v);
}
static void t_wr(void *u, MdFull *m, uint32_t a, unsigned sz, uint32_t v) {
    DpMdMach *d = u;
    wlog(m, a, sz, v);
    if (d->h.on_write) d->h.on_write(d->h.user, a, sz, v);
}

DpMdMach *dp_mdmach_open(const uint8_t *rom, size_t size) {
    if (!rom || !size || size > DP_MD_ROM_MAX) return NULL;
    DpMdMach *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->m = calloc(1, sizeof *d->m);
    if (!d->m) { free(d); return NULL; }
    dp_mdrom_set(rom, size);
    return d;
}

void dp_mdmach_close(DpMdMach *d) {
    if (!d) return;
    if (cur == d) { memset(&md_trace, 0, sizeof md_trace); cur = NULL; }
    free(d->m);
    free(d->screen);
    free(d);
}

void dp_mdmach_set_hooks(DpMdMach *d, const DpMdHooks *h) { d->h = *h; }

/* DP_MD_DSY_VINT=<file>: diagnostic DSY1 writer sampling work RAM at the V-int vector like the Gens exec hook
 * (the reference side of `deepeel desync`); one sample per V-int, numbered from 0. */
static FILE *vint_dsy;
static void vint_sample(void *u, const struct MdFull *m) {
    (void)u;
    static int64_t last_frame = -1;
    if (!vint_dsy || m != vint_owner) return;
    if ((int64_t)m->frames <= last_frame) return;   // forks restored on this machine replay earlier frames: the primary replay owns each frame
    last_frame = m->frames;
    { static unsigned stride = ~0u; if (stride == ~0u) { const char *e = getenv("DP_MD_DSY_STRIDE"); stride = e ? (unsigned)atoi(e) : 0; }
      if (stride > 1 && vint_n % stride && !getenv("DP_MD_DSY_FROM")) { vint_n++; return; } }   // DP_MD_DSY_STRIDE=N: write every N-th V-int only (long movies)
    { static long lo = -2, hi = -2; if (lo == -2) { const char *e = getenv("DP_MD_DSY_FROM"), *t = getenv("DP_MD_DSY_TO"); lo = e ? atol(e) : -1; hi = t ? atol(t) : -1; }
      if ((lo >= 0 && (long)vint_n < lo) || (hi >= 0 && (long)vint_n > hi)) { vint_n++; return; } }   // DP_MD_DSY_FROM/TO: a V-int window (stride 1 around a desync)
    { static long shot_lo = -2, shot_hi = -2; static const char *shot_pre; static MdFrameSel shot_sel;   /* DP_MD_SHOT=prefix DP_MD_SHOT_FROM/TO=V-int window: PPM of the rendered frame */
      if (shot_lo == -2) { shot_pre = getenv("DP_MD_SHOT"); const char *e = getenv("DP_MD_SHOT_FROM"), *t = getenv("DP_MD_SHOT_TO"); shot_lo = e ? atol(e) : -1; shot_hi = t ? atol(t) : shot_lo; }
      if (shot_pre && (getenv("DP_MD_SHOT_FRAMES") ? md_frame_sel(&shot_sel, "DP_MD_SHOT_FRAMES", vint_n) : (shot_lo >= 0 && (long)vint_n >= shot_lo && (long)vint_n <= shot_hi)) && (!getenv("DP_MD_SHOT_STEP") || vint_n % (unsigned)atoi(getenv("DP_MD_SHOT_STEP")) == 0)) {
          uint32_t *px = calloc(320u * 224u, sizeof *px);
          if (px) { char fn[512]; snprintf(fn, sizeof fn, "%s%u.ppm", shot_pre, vint_n); FILE *sf = fopen(fn, "wb");
              if (sf) { if (m->screen) memcpy(px, m->screen, 320u * 224u * sizeof *px); else md_full_render((MdFull *)m, px, 320); fprintf(sf, "P6\n320 224\n255\n");
                  for (int i = 0; i < 320 * 224; i++) { uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, sf); }
                  fclose(sf); }
              free(px); } } }
    // only the first machine (the primary replay), not shifted replays or forks
    if (getenv("DP_MD_VINT_LOG")) fprintf(stderr, "V n=%u frames=%u line=%u pc=%06x f62a=%02x\n", vint_n, (unsigned)m->frames, (unsigned)m->line, (unsigned)m->pc, m->ram[0xf62a]);
    uint16_t nb = 2; uint8_t nl = 4; uint32_t len = 0x10000u, rl = 68, regs[17];
    for (int i = 0; i < 8; i++) { regs[i] = m->d[i]; regs[8 + i] = m->a[i]; }
    { uint32_t sp = m->a[7]; regs[16] = ((uint32_t)m->ram[(sp + 2) & 0xffff] << 24) | ((uint32_t)m->ram[(sp + 3) & 0xffff] << 16) | ((uint32_t)m->ram[(sp + 4) & 0xffff] << 8) | m->ram[(sp + 5) & 0xffff]; }   // the interrupted PC (pushed by the interrupt), what Gens reports at the vector
    fwrite(&vint_n, 4, 1, vint_dsy); fwrite(&nb, 2, 1, vint_dsy); fwrite(&nl, 1, 1, vint_dsy); fwrite("wram", 1, 4, vint_dsy);
    fwrite(&len, 4, 1, vint_dsy); fwrite(m->ram, 1, len, vint_dsy);
    fwrite(&nl, 1, 1, vint_dsy); fwrite("regs", 1, 4, vint_dsy); fwrite(&rl, 4, 1, vint_dsy); fwrite(regs, 4, 17, vint_dsy); fflush(vint_dsy);
    vint_n++;
}

void dp_mdmach_reset(DpMdMach *d) {
    /* ClownMDEmu's clock (exact, phase 3) keeps real Sonic 1 films in step for 13 800+ frames where the 3420-master
     * default loses a frame near 1423 (docs/MD.fr.md); still overridable from the environment. */
    /* DP_MD_GENS=1: Gens-rr compatibility profile for TAS films recorded on Gens (not hardware fidelity): 488-cycle lines (3416 master),
     * Starscream's fixed bit-op/CLR/TAS/DIV times, Gens' DMA and input latch. Each knob stays overridable. ADDQ.W An is left out
     * (Sonic 1843S wants the hardware 8). */
    if (getenv("DP_MD_GENS")) {
        setenv("DP_MD_CLOWN_CLOCK", "3416", 0); setenv("DP_MD_CLOCK_OFF", "0", 0); setenv("DP_MD_PAD_LINE0", "1", 0); setenv("DP_MD_GENS_DMA", "1", 0);
        setenv("DP_MD_POWERON", "gens", 0); setenv("MD_IRQ_IMMEDIATE", "1", 0); setenv("DP_MD_STAR_TIME", "1", 0); setenv("DP_MD_STAR_DIV", "1", 0);
    }
    setenv("DP_MD_CLOWN_CLOCK", "exact", 0); setenv("DP_MD_CLOCK_OFF", "3", 0);
    md_full_reset(d->m);
    if ((getenv("DP_MD_SHOT") || getenv("DP_MD_LINESCREEN")) && !d->screen) { d->screen = calloc(320u * 224u, sizeof *d->screen); if (d->screen) d->m->screen = d->screen; }   /* per-line screen for DP_MD_SHOT (H-int palette swaps) */
    { const char *vp = getenv("DP_MD_DSY_VINT");
      if (vp && *vp && !vint_dsy) { vint_dsy = fopen(vp, "wb"); if (vint_dsy) fwrite("DSY1", 1, 4, vint_dsy); }
      if (vint_dsy && !vint_owner) vint_owner = d->m;
      if (vint_dsy) { d->m->phase_hook = vint_sample; d->m->phase_on_vint = 1; } }
    d->resume_frame = 0;
    d->steps = 0;
}

uint64_t dp_mdmach_run(DpMdMach *d, uint64_t max_steps, int *cut, int *fault) {
    memset(&md_trace, 0, sizeof md_trace);
    md_trace.user = d;
    md_trace.step = t_step;
    md_trace.end = t_end;
    md_trace.rd = t_rd;
    md_trace.wr = t_wr;
    cur = d;
    d->steps = 0;
    d->budget = max_steps;
    d->m->fault = 0;
    while (!d->m->fault && d->steps < d->budget) {
        dp_md_full_frame(d->m, 0, d->resume_frame);
        d->resume_frame = 1;
        if (!d->m->fault) d->resume_frame = 0;
        if (!d->m->fault && d->h.on_frame) d->h.on_frame(d->h.user);
    }
    // the budget fault is raised after the opcode words were consumed: put pc back on the cut instruction
    if (d->m->fault == DP_MD_FAULT_BUDGET) { d->m->pc = d->m->fault_pc; d->m->fault = 0; }
    if (cut) *cut = d->m->fault == DP_MD_FAULT_BUDGET || (!d->m->fault && d->steps >= d->budget);
    if (fault) *fault = (d->m->fault == DP_MD_FAULT_BUDGET || d->m->fault == DP_MD_FAULT_HALT) ? 0 : d->m->fault;
    return d->steps;
}

uint32_t dp_mdmach_pc(const DpMdMach *d) { return d->m->pc; }
uint32_t dp_mdmach_sp(const DpMdMach *d) { return d->m->a[7]; }   // A7 is the stack pointer
uint32_t dp_mdmach_fault_pc(const DpMdMach *d) { return d->m->fault_pc; }

size_t dp_mdmach_state_size(void) { return sizeof(MdFull); }
void dp_mdmach_save(const DpMdMach *d, void *buf) { memcpy(buf, d->m, sizeof(MdFull)); }
void dp_mdmach_load(DpMdMach *d, const void *buf) {
    // A state read back from a spool was written by another process: keep this process's own
    // pointers (Z80 bus callbacks, hooks, coverage, screen) instead of the stale saved ones.
    MdFull *m = d->m;
    z80_t cpu = m->sound_cpu;
    void (*ym_log)(void *, uint64_t, unsigned, uint8_t, uint8_t) = m->ym_log;
    void *ym_log_user = m->ym_log_user;
    uint8_t *coverage = m->coverage;
    uint32_t *screen = m->screen;
    void (*line_hook)(void *, const struct MdFull *, unsigned) = m->line_hook;
    void *hook_user = m->hook_user;
    memcpy(m, buf, sizeof(MdFull));
    m->sound_cpu.user = cpu.user;
    m->sound_cpu.read = cpu.read;
    m->sound_cpu.write = cpu.write;
    m->sound_cpu.in = cpu.in;
    m->sound_cpu.out = cpu.out;
    m->ym_log = ym_log; m->ym_log_user = ym_log_user;
    m->coverage = coverage; m->screen = screen;
    m->line_hook = line_hook; m->hook_user = hook_user;
    // a state saved between frames (frame wrapped, line still 261) restarts at line 0: resuming at line 261 would replay that
    // line and skip the frame-start work (the V-int request)
    d->resume_frame = !(m->line == 261 && m->frame_cycles < 100000u);
    md_full_cram_sync(d->m);
}
void dp_mdmach_state_set_pc(void *buf, uint32_t pc) { ((MdFull *)buf)->pc = pc; }
void dp_mdmach_state_set_an(void *buf, unsigned n, uint32_t v) { ((MdFull *)buf)->a[n & 7] = v; }
void dp_mdmach_state_set_pad(void *buf, uint8_t pad) { ((MdFull *)buf)->pad = pad; }
// ext: bit 7 = six-button pad plugged in, bits 0-3 = Z,Y,X,Mode pressed; resets the TH phase counter
void dp_mdmach_state_set_pad_ext(void *buf, uint8_t ext) { MdFull *m = buf; m->pad_ext = ext; m->pad_cnt = 0; }
static int condition_true(unsigned cc, unsigned f) {
    int c = !!(f & 1), v = !!(f & 2), z = !!(f & 4), n = !!(f & 8);
    switch (cc & 15) {
    case 0: return 1; case 1: return 0; case 2: return !c && !z; case 3: return c || z;
    case 4: return !c; case 5: return c; case 6: return !z; case 7: return z;
    case 8: return !v; case 9: return v; case 10: return !n; case 11: return n;
    case 12: return n == v; case 13: return n != v; case 14: return !z && n == v;
    default: return z || n != v;
    }
}
void dp_mdmach_state_force_cc(void *buf, unsigned condition, int taken) {
    MdFull *s = buf;
    unsigned old = s->sr & 15, best = old, cost = 5;
    for (unsigned f = 0; f < 16; f++) {
        unsigned bits = old ^ f, n = 0;
        for (; bits; bits >>= 1) n += bits & 1;
        if (condition_true(condition, f) == !!taken && n < cost) { best = f; cost = n; }
    }
    s->sr = (uint16_t)((s->sr & ~15u) | best);
}
void dp_mdmach_state_force_dbcc(void *buf, unsigned condition, unsigned reg, int taken,
                                uint32_t old_dn, uint16_t old_sr) {
    MdFull *s = buf;
    s->sr = old_sr;
    s->d[reg & 7] = old_dn;
    if (taken) {
        dp_mdmach_state_force_cc(s, condition, 0);
        uint16_t old = (uint16_t)old_dn;
        dp_mdmach_state_set_dn_word(s, reg, old ? (uint16_t)(old - 1) : 0);
    } else if (condition == 1) {
        dp_mdmach_state_set_dn_word(s, reg, 0xFFFF);
    } else {
        dp_mdmach_state_force_cc(s, condition, 1);
        s->d[reg & 7] = old_dn;
    }
}
void dp_mdmach_state_set_dn_word(void *buf, unsigned n, uint16_t w) {
    MdFull *s = buf;
    s->d[n & 7] = (s->d[n & 7] & 0xFFFF0000u) | w;
}
uint32_t dp_mdmach_state_pc(const void *buf) { return ((const MdFull *)buf)->pc; }
void dp_mdmach_halt(DpMdMach *d) {
    d->m->fault = DP_MD_FAULT_HALT;
    d->m->slice_limit = 0;
}
uint16_t dp_mdmach_vdp_reg(const DpMdMach *d, unsigned r) { return d->m->vdp_reg[r % 24]; }
uint16_t dp_mdmach_sr(const DpMdMach *d) { return d->m->sr; }
uint32_t dp_mdmach_dn(const DpMdMach *d, unsigned n) { return d->m->d[n & 7]; }
uint32_t dp_mdmach_an(const DpMdMach *d, unsigned n) { return d->m->a[n & 7]; }
// DP_MD_PAD_LINE0=1: the input of a movie frame is latched at the first active line (Gens-rr), not at the V-int that opens the frame
static int pad_line0(void) { static int v = -1; if (v < 0) { const char *e = getenv("DP_MD_PAD_LINE0"); v = e && *e && *e != '0'; } return v; }
void dp_mdmach_set_pad(DpMdMach *d, uint8_t pad) { if (pad_line0()) { d->m->pad_pend = pad; d->m->pend_set = 1; } else d->m->pad = pad; }
// port 2 (second three-button pad): on = plugged in; off keeps every read at $7F
void dp_mdmach_set_pad2(DpMdMach *d, int on, uint8_t pad) { if (pad_line0()) { d->m->pad2_on_pend = on != 0; d->m->pad2_pend = pad; d->m->pend_set = 1; } else { d->m->pad2_on = on != 0; d->m->pad2 = pad; } }
// C12: six-button pad: ext bit 7 plugged, bits 0-3 Z,Y,X,Mode pressed (0 keeps the three-button pad)
void dp_mdmach_set_pad_ext(DpMdMach *d, uint8_t ext) { if (d->m->pad_ext != ext) { d->m->pad_ext = ext; d->m->pad_cnt = 0; } }

uint32_t dp_mdmach_peek32(const DpMdMach *d, uint32_t a) {
    (void)d;
    return dp_mdrom_peek32(a);
}

void dp_mdmach_state_set_irq(void *buf, int vint, int hint) {
    MdFull *s = buf;
    if (vint) s->irq_pending = 1;
    if (hint) s->hint_pending = 1;
}
int dp_mdmach_vdp_pending(const DpMdMach *d) { return d->m->vdp_pending; }
uint16_t dp_mdmach_vdp_first(const DpMdMach *d) { return (uint16_t)d->m->vdp_first; }
uint8_t dp_mdmach_vdp_code(const DpMdMach *d) { return d->m->vdp_code; }
int dp_mdmach_rom_visible(const DpMdMach *d, uint32_t addr) {
    return addr < 0x400000u && !(d->m->sram_enable && addr >= 0x200000u && addr < 0x210000u);
}
const uint8_t *dp_mdmach_ram(const DpMdMach *d) { return d->m->ram; }
void dp_mdmach_render(const DpMdMach *d, uint32_t *pixels) {
    DpMdMach *w = (DpMdMach *)d;
    // First call (the watch is the only caller): from now on the kernel draws each active line as it is emulated,
    // so mid-frame CRAM writes (H-int palette swap) show; before that, fall back to the end-of-frame palette.
    if (!w->screen) {
        w->screen = calloc(320u * 224u, sizeof *w->screen);
        if (w->screen) w->m->screen = w->screen;
    }
    if (w->screen && w->m->screen == w->screen && w->screen[0] != 0u) { memcpy(pixels, w->screen, 320u * 224u * sizeof *pixels); return; }
    md_full_render(d->m, pixels, 320);
}
static uint64_t fnv_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
uint64_t dp_mdmach_ctx_hash(const DpMdMach *d) {   // C11: the 68000 registers (no clocks, no RAM)
    const MdFull *m = d->m;
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv_bytes(h, m->d, sizeof m->d); h = fnv_bytes(h, m->a, sizeof m->a);
    h = fnv_bytes(h, &m->pc, sizeof m->pc); h = fnv_bytes(h, &m->usp, sizeof m->usp);
    h = fnv_bytes(h, &m->sr, sizeof m->sr); h = fnv_bytes(h, &m->z80_bank, sizeof m->z80_bank);
    return h;
}
uint64_t dp_mdmach_state_hash(const DpMdMach *d) {
    const MdFull *m = d->m;
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv_bytes(h, m->d, sizeof m->d); h = fnv_bytes(h, m->a, sizeof m->a);
    h = fnv_bytes(h, &m->pc, sizeof m->pc); h = fnv_bytes(h, &m->usp, sizeof m->usp);
    h = fnv_bytes(h, &m->sr, sizeof m->sr);
    h = fnv_bytes(h, m->ram, sizeof m->ram); h = fnv_bytes(h, m->vram, sizeof m->vram);
    h = fnv_bytes(h, m->vsram, sizeof m->vsram); h = fnv_bytes(h, m->cram, sizeof m->cram);
    h = fnv_bytes(h, m->vdp_reg, sizeof m->vdp_reg);
    h = fnv_bytes(h, &m->vdp_addr, sizeof m->vdp_addr); h = fnv_bytes(h, &m->vdp_first, sizeof m->vdp_first);
    uint8_t flags[] = { m->vdp_code, m->vdp_pending, m->dma_fill, m->pad, m->pad_th, m->pad_control, m->pad_ext, m->pad_cnt, m->pad2, m->pad2_th, m->pad2_on,
                        m->z80_bus, m->z80_reset, m->sram_enable, m->sram_protect, m->stopped, m->fault,
                        m->irq_pending, m->hint_pending, m->hint_counter };
    h = fnv_bytes(h, flags, sizeof flags);
    h = fnv_bytes(h, &m->line, sizeof m->line);
    h = fnv_bytes(h, m->sram, sizeof m->sram); h = fnv_bytes(h, m->zram, sizeof m->zram);
    h = fnv_bytes(h, &m->z80_bank, sizeof m->z80_bank);
    return h;
}
uint64_t dp_mdmach_state_ctx_hash_live(const DpMdMach *d, uint8_t live) {
    const MdFull *m = d->m;
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv_bytes(h, m->d, sizeof m->d); h = fnv_bytes(h, m->a, sizeof m->a);
    h = fnv_bytes(h, &m->usp, sizeof m->usp); h = fnv_bytes(h, &m->sr, sizeof m->sr);
    if (live & DP_MDL_VIDEO) {
        h = fnv_bytes(h, m->vdp_reg, sizeof m->vdp_reg);
        h = fnv_bytes(h, m->vsram, sizeof m->vsram);
        h = fnv_bytes(h, m->cram, sizeof m->cram);
        h = fnv_bytes(h, &m->vdp_addr, sizeof m->vdp_addr);
        h = fnv_bytes(h, &m->vdp_first, sizeof m->vdp_first);
        uint8_t vf[] = { m->vdp_code, m->vdp_pending, m->dma_fill };
        h = fnv_bytes(h, vf, sizeof vf);
    }
    if (live & DP_MDL_SOUND) {
        h = fnv_bytes(h, &m->z80_bus, sizeof m->z80_bus);
        h = fnv_bytes(h, &m->z80_reset, sizeof m->z80_reset);
        h = fnv_bytes(h, &m->z80_bank, sizeof m->z80_bank);
        h = fnv_bytes(h, m->zram, sizeof m->zram);
    }
    if (live & DP_MDL_BANK) {
        uint8_t bk[] = { m->sram_enable, m->sram_protect };
        h = fnv_bytes(h, bk, sizeof bk);
    }
    if (live & DP_MDL_SYS) {
        uint8_t sys[] = { m->irq_pending, m->hint_pending, m->hint_counter, m->stopped };
        h = fnv_bytes(h, sys, sizeof sys);
    }
    return h;
}
uint64_t dp_mdmach_screen_key(const DpMdMach *d) {
    const MdFull *m = d->m;
    unsigned wsel = m->vdp_reg[16] & 3, w = wsel == 0 ? 32 : wsel == 1 ? 64 : 128;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned pl = 0; pl < 2; pl++) {
        unsigned base = pl ? (unsigned)(m->vdp_reg[4] & 7) << 13 : (unsigned)(m->vdp_reg[2] & 0x38) << 10;
        h ^= (uint64_t)(((m->vsram[pl * 2] << 8 | m->vsram[pl * 2 + 1]) & 0x3FF) >> 5);
        h *= 0x100000001b3ull;
        for (unsigned r = 0; r < 28; r += 4)
            for (unsigned c = 0; c < 32; c += 4) {
                unsigned at = (base + (r * w + c) * 2u) & 0xFFFEu;
                unsigned word = m->vram[at] << 8 | m->vram[at + 1];
                h ^= (uint64_t)((word & 0x7FF) >> 3);
                h *= 0x100000001b3ull;
            }
    }
    return h;
}
void dp_mdmach_poke(DpMdMach *d, uint32_t a, uint8_t v) { if (a >= 0xFF0000u) d->m->ram[a & 0xFFFFu] = v; }
unsigned dp_mdmach_audio_read(DpMdMach *d, int16_t *stereo, unsigned max_frames) { return md_full_audio_read(d->m, stereo, max_frames); }
uint8_t dp_mdmach_ram8(const DpMdMach *d, uint32_t a) { return d->m->ram[a & 0xFFFFu]; }


/* Debug: writes VRAM (64K), CRAM (64 words LE), VSRAM (80), VDP regs (24 words LE) to path. */
void dp_mdmach_dump_video(const DpMdMach *m, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(m->m->vram, 1, 65536, f); fwrite(m->m->cram, 2, 64, f);
    fwrite(m->m->vsram, 1, 80, f); fwrite(m->m->vdp_reg, 2, 24, f); fwrite(m->m->ram, 1, 65536, f);
    fclose(f);
}
