#include "mdmach.h"
#include <stdlib.h>
#include <string.h>
#include "md_full.h"
#include "md_trace.h"

void dp_mdrom_set(const uint8_t *rom, size_t size);
uint32_t dp_mdrom_peek32(uint32_t a);
void dp_md_full_frame(MdFull *m, int native, int resume);

struct DpMdMach {
    MdFull *m;
    DpMdHooks h;
    uint64_t steps, budget;
    int resume_frame;
};

static DpMdMach *cur;
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

static void t_step(void *u, MdFull *m, uint32_t pc, uint16_t op) {
    DpMdMach *d = u;
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

static void t_wr(void *u, MdFull *m, uint32_t a, unsigned sz, uint32_t v) {
    DpMdMach *d = u;
    (void)m;
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
    free(d);
}

void dp_mdmach_set_hooks(DpMdMach *d, const DpMdHooks *h) { d->h = *h; }

void dp_mdmach_reset(DpMdMach *d) {
    md_full_reset(d->m);
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
    if (cut) *cut = d->m->fault == DP_MD_FAULT_BUDGET || (!d->m->fault && d->steps >= d->budget);
    if (fault) *fault = (d->m->fault == DP_MD_FAULT_BUDGET || d->m->fault == DP_MD_FAULT_HALT) ? 0 : d->m->fault;
    return d->steps;
}

uint32_t dp_mdmach_pc(const DpMdMach *d) { return d->m->pc; }
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
    d->resume_frame = 1;
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
void dp_mdmach_set_pad(DpMdMach *d, uint8_t pad) { d->m->pad = pad; }

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
    return !(d->m->sram_enable && addr >= 0x200000u && addr < 0x210000u);
}
const uint8_t *dp_mdmach_ram(const DpMdMach *d) { return d->m->ram; }
void dp_mdmach_render(const DpMdMach *d, uint32_t *pixels) { md_full_render(d->m, pixels, 320); }
static uint64_t fnv_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
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
    uint8_t flags[] = { m->vdp_code, m->vdp_pending, m->dma_fill, m->pad, m->pad_th, m->pad_control, m->pad_ext, m->pad_cnt,
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
uint8_t dp_mdmach_ram8(const DpMdMach *d, uint32_t a) { return d->m->ram[a & 0xFFFFu]; }

