#include "nesmach.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "nes.h"
#include "nes_native.h"
#include "nes_trace.h"

typedef struct { uint8_t v[16]; uint8_t shift, cnt; } DpNesMapRegs;

/* the runtime's one machine (like MdFull, one ROM live per process) */
struct DpNesMach {
    DpNesHooks h;
    uint64_t steps, budget;
    int in_frame;      /* a frame is part-run: resume with nes_frame_rest */
    /* offsets into the state blob excluded from the exact-state hash */
    size_t off_cycles, off_frames;
    /* bridge-side mappers: 0 NROM, 2 UxROM (runtime), 1 MMC1, 3 CNROM,
     * 4 MMC3, 7 AxROM (composed windows) */
    unsigned mapper;
    const uint8_t *prg_rom, *chr_rom;
    unsigned prg8k, chr1k;   /* ROM sizes in 8 KiB PRG / 1 KiB CHR units */
    DpNesMapRegs rg;
    uint8_t prgbuf[32768];   /* composed $8000-$FFFF */
    uint8_t chrbuf[8192];    /* composed pattern space */
    uint8_t prg_map[4];      /* 8 KiB ROM slot behind each 8 KiB of $8000+ */
    uint16_t chr_map[8];     /* 1 KiB ROM slot behind each 1 KiB of CHR */
};

/* boards whose windows the bridge owns; 66 (GxROM) and 11 (Color Dreams) switch a 32 KiB PRG + 8 KiB CHR pair */
#define BRIDGED(m) ((m) == 1 || (m) == 3 || (m) == 4 || (m) == 5 || (m) == 7 || (m) == 11 || (m) == 66 || (m) == 71 || (m) == 94 || (m) == 180)

#define MAPPER_TAIL ((unsigned)sizeof(DpNesMapRegs))

/* pure: the maps a register set implies */
static void maps_of(const DpNesMach *d, const DpNesMapRegs *r, uint8_t prg[4], uint16_t chr[8]) {
    unsigned n = d->prg8k ? d->prg8k : 1u, c = d->chr1k ? d->chr1k : 1u;
    unsigned last = n - 1u;
    switch (d->mapper) {
    case 5: {   /* MMC5 (bridged subset): PRG mode $5100, CHR mode $5101, PRG $5114-$5117, CHR $5120-$5127 */
        unsigned pm = r->v[0] & 3u, cm = r->v[1] & 3u, R[4];
        for (unsigned i = 0; i < 4; i++) R[i] = r->v[2 + i] & 0x7Fu;
        if (pm == 0) { for (unsigned i = 0; i < 4; i++) prg[i] = (uint8_t)(((R[3] & ~3u) + i) % n); }
        else if (pm == 1) {
            for (unsigned i = 0; i < 2; i++) { prg[i] = (uint8_t)(((R[1] & ~1u) + i) % n); prg[2 + i] = (uint8_t)(((R[3] & ~1u) + i) % n); }
        } else if (pm == 2) {
            prg[0] = (uint8_t)((R[1] & ~1u) % n); prg[1] = (uint8_t)(((R[1] & ~1u) + 1u) % n);
            prg[2] = (uint8_t)(R[2] % n); prg[3] = (uint8_t)(R[3] % n);
        } else for (unsigned i = 0; i < 4; i++) prg[i] = (uint8_t)(R[i] % n);
        if (d->chr_rom) {
            unsigned step = cm == 0 ? 8u : cm == 1 ? 4u : cm == 2 ? 2u : 1u;
            for (unsigned i = 0; i < 8; i++) {
                unsigned slot = (i / step + 1u) * step - 1u;   /* the register that owns this 1 KiB */
                chr[i] = (uint16_t)((r->v[6 + slot] * step + (i % step)) % c);
            }
        }
        return;
    }
    case 7: {
        unsigned b = (r->v[0] & 7u) * 4u;
        for (unsigned i = 0; i < 4; i++) prg[i] = (uint8_t)((b + i) % n);
        break;
    }
    case 71: case 94: {   /* Camerica / UN1ROM: 16 KiB switchable at $8000, last bank fixed at $C000 */
        unsigned b = d->mapper == 94 ? (r->v[0] >> 2) & 7u : r->v[0], n16 = (n + 1u) / 2u;
        b %= n16 ? n16 : 1u;
        prg[0] = (uint8_t)((b * 2u) % n); prg[1] = (uint8_t)((b * 2u + 1u) % n);
        prg[2] = (uint8_t)(last - 1u + (n < 2)); prg[3] = (uint8_t)last;
        return;
    }
    case 180: {   /* Crazy Climber: bank 0 fixed at $8000, 16 KiB switchable at $C000 */
        unsigned b = r->v[0] & 7u, n16 = (n + 1u) / 2u;
        b %= n16 ? n16 : 1u;
        prg[0] = 0; prg[1] = (uint8_t)(1u % n);
        prg[2] = (uint8_t)((b * 2u) % n); prg[3] = (uint8_t)((b * 2u + 1u) % n);
        return;
    }
    case 66: case 11: {
        unsigned pb = d->mapper == 66 ? (r->v[0] >> 4) & 3u : r->v[0] & 3u;
        unsigned cb = d->mapper == 66 ? r->v[0] & 3u : (r->v[0] >> 4) & 15u;
        for (unsigned i = 0; i < 4; i++) prg[i] = (uint8_t)((pb * 4u + i) % n);
        for (unsigned i = 0; i < 8; i++) chr[i] = (uint16_t)((cb * 8u + i) % c);
        return;
    }
    case 3:
        for (unsigned i = 0; i < 8; i++) chr[i] = (uint16_t)(((r->v[0] * 8u) + i) % c);
        for (unsigned i = 0; i < 4; i++) prg[i] = (uint8_t)(i % n);
        return;
    case 1: {
        unsigned ctrl = r->v[0], mode = (ctrl >> 2) & 3, p = r->v[3] & 15u;
        if (mode < 2) { for (unsigned i = 0; i < 4; i++) prg[i] = (uint8_t)((((p & ~1u) * 2u) + i) % n); }
        else if (mode == 2) {
            prg[0] = 0; prg[1] = (uint8_t)(1u % n);
            prg[2] = (uint8_t)((p * 2u) % n); prg[3] = (uint8_t)((p * 2u + 1u) % n);
        } else {
            prg[0] = (uint8_t)((p * 2u) % n); prg[1] = (uint8_t)((p * 2u + 1u) % n);
            prg[2] = (uint8_t)(last - 1u + (n < 2)); prg[3] = (uint8_t)last;
        }
        if (d->chr_rom) {
            if (ctrl & 16) {
                for (unsigned i = 0; i < 4; i++) {
                    chr[i] = (uint16_t)((r->v[1] * 4u + i) % c);
                    chr[4 + i] = (uint16_t)((r->v[2] * 4u + i) % c);
                }
            } else for (unsigned i = 0; i < 8; i++) chr[i] = (uint16_t)((((r->v[1] & ~1u) * 4u) + i) % c);
        }
        return;
    }
    case 4: {
        unsigned cmd = r->v[0], R[8];
        for (unsigned i = 0; i < 8; i++) R[i] = r->v[2 + i];
        unsigned s6 = R[6] & 63u, s7 = R[7] & 63u, pen = last - 1u + (n < 2);
        if (cmd & 0x40) { prg[0] = (uint8_t)(pen); prg[1] = (uint8_t)(s7 % n); prg[2] = (uint8_t)(s6 % n); }
        else { prg[0] = (uint8_t)(s6 % n); prg[1] = (uint8_t)(s7 % n); prg[2] = (uint8_t)pen; }
        prg[3] = (uint8_t)last;
        if (d->chr_rom) {
            unsigned k[8] = { R[0] & ~1u, R[0] | 1u, R[1] & ~1u, R[1] | 1u, R[2], R[3], R[4], R[5] };
            for (unsigned i = 0; i < 8; i++) chr[i] = (uint16_t)(k[(cmd & 0x80) ? (i + 4) & 7 : i] % c);
        }
        return;
    }
    default: return;
    }
}

/* power-on register values: MMC1 control = 3, MMC5 modes 3 with the last bank under $E000 */
static void mapper_regs_init(DpNesMach *d) {
    memset(&d->rg, 0, sizeof d->rg);
    if (d->mapper == 1) { d->rg.v[0] = 0x0C; d->rg.shift = 0x10; }
    if (d->mapper == 5) { d->rg.v[0] = 3; d->rg.v[1] = 3; d->rg.v[5] = (uint8_t)(0x80u | ((d->prg8k ? d->prg8k : 1u) - 1u)); }
}

static void mapper_apply(DpNesMach *d) {
    if (d->mapper < 3 && d->mapper != 1) return;  /* 0/2: the runtime's own */
    maps_of(d, &d->rg, d->prg_map, d->chr_map);
    if (d->mapper != 3) {
        for (unsigned i = 0; i < 4; i++)
            memcpy(d->prgbuf + i * 8192u, d->prg_rom + (size_t)d->prg_map[i] * 8192u, 8192u);
        machine.prg = machine.cpu.prg = d->prgbuf;
        machine.cpu.bank_base = 0;
        machine.cpu.fixed_base = 16384u;
        machine.cpu.bank = (uint8_t)(d->prg_map[0] / 2u);
    }
    if (d->chr_rom && d->mapper != 7) {
        for (unsigned i = 0; i < 8; i++)
            memcpy(d->chrbuf + i * 1024u, d->chr_rom + (size_t)d->chr_map[i] * 1024u, 1024u);
        machine.chr = machine.ppu.chr = d->chrbuf;
    }
}

/* a $8000+ write against a register set (MMC1 serial port, MMC3 pairs) */
static void regs_write(const DpNesMach *d, DpNesMapRegs *r, unsigned a, unsigned v) {
    switch (d->mapper) {
    case 3: case 7: case 11: case 66: case 94: case 180: r->v[0] = (uint8_t)v; break;
    case 71: if (a >= 0xC000u) r->v[0] = (uint8_t)v; break;
    case 5:
        if (a == 0x5100u) r->v[0] = (uint8_t)v;
        else if (a == 0x5101u) r->v[1] = (uint8_t)v;
        else if (a >= 0x5114u && a <= 0x5117u) r->v[2 + (a - 0x5114u)] = (uint8_t)v;
        else if (a >= 0x5120u && a <= 0x5127u) r->v[6 + (a - 0x5120u)] = (uint8_t)v;
        break;
    case 1:
        if (v & 0x80) { r->shift = 0x10; r->cnt = 0; r->v[0] |= 0x0C; break; }
        r->shift = (uint8_t)((r->shift >> 1) | ((v & 1) << 4));
        if (++r->cnt == 5) {
            unsigned reg = (a >> 13) & 3;  /* ctrl, chr0, chr1, prg */
            r->v[reg] = r->shift;
            r->shift = 0x10; r->cnt = 0;
        }
        break;
    case 4:
        if (!(a & 0x6000)) {
            if (!(a & 1)) r->v[0] = (uint8_t)v;
            else r->v[2 + (r->v[0] & 7)] = (uint8_t)v;
        }
        break;
    }
}

static uint16_t watch_buf[160 * 128];   /* panel picture, see dp_nesmach_watch_enable */
static int watch_on;

static void io_copy(void *data, uint32_t bytes, void *ctx) {
    uint8_t **p = ctx;
    memcpy(*p, data, bytes);
    *p += bytes;
}

static void io_paste(void *data, uint32_t bytes, void *ctx) {
    const uint8_t **p = ctx;
    memcpy(data, *p, bytes);
    *p += bytes;
}

static void t_step(void *u, unsigned pc, unsigned op) {
    DpNesMach *d = u;
    if (d->steps >= d->budget) { nes_trace.stop = 1; return; }
    ++d->steps;
    if (d->h.on_step) d->h.on_step(d->h.user, pc, op);
}

static void t_rd(void *u, unsigned a) {
    DpNesMach *d = u;
    if (d->h.on_read) d->h.on_read(d->h.user, a);
}

static void t_wr(void *u, unsigned a, unsigned v) {
    DpNesMach *d = u;
    if (d->h.on_write) d->h.on_write(d->h.user, a, v);
    if (d->mapper == 5 && a >= 0x5100u && a < 0x5128u) {
        regs_write(d, &d->rg, a, v);
        mapper_apply(d);
    } else if (a >= 0x8000u && BRIDGED(d->mapper)) {
        regs_write(d, &d->rg, a, v);
        mapper_apply(d);
    }
}

static void t_interrupt(void *u, unsigned vector) {
    DpNesMach *d = u;
    if (d->h.on_interrupt) d->h.on_interrupt(d->h.user, vector);
}

__attribute__((unused)) static void t_frame(void *u) {
    DpNesMach *d = u;
    if (d->h.on_frame) d->h.on_frame(d->h.user);
}

DpNesMach *dp_nesmach_open(const uint8_t *data, size_t size, int mapper,
                           char *err, size_t errsz) {
    if (size < 16 || memcmp(data, "NES\x1a", 4)) {
        snprintf(err, errsz, "not an iNES image");
        return NULL;
    }
    unsigned mapper_id = (unsigned)((data[6] >> 4) | (data[7] & 0xF0));
    if (mapper >= 0) mapper_id = (unsigned)mapper;
    if (mapper_id != 0 && mapper_id != 1 && mapper_id != 2 && mapper_id != 3 &&
        mapper_id != 4 && mapper_id != 5 && mapper_id != 7 && mapper_id != 11 && mapper_id != 66 && mapper_id != 71 && mapper_id != 94 && mapper_id != 180) {
        snprintf(err, errsz, "mapper %u: supported boards are NROM (0), MMC1 (1), "
                 "UxROM (2), CNROM (3), MMC3 (4), MMC5 (5), AxROM (7), Color Dreams (11), GxROM (66), Camerica (71), UN1ROM (94) and 180", mapper_id);
        return NULL;
    }
    size_t body = size - 16;
    if (data[6] & 4) { /* trainer sits between the header and the PRG */
        snprintf(err, errsz, "iNES trainer images are not supported");
        return NULL;
    }
    unsigned prg = (unsigned)data[4] * 16384u, chr = (unsigned)data[5] * 8192u;
    if (prg == 0 || prg > body || chr > body - prg) {
        snprintf(err, errsz, "iNES sizes out of range (prg %u, chr %u, body %zu)",
                 prg, chr, body);
        return NULL;
    }
    if (mapper_id == 3 && (chr == 0 || prg != 16384u * (prg / 16384u) ||
                           (prg != 16384u && prg != 32768u))) {
        snprintf(err, errsz, "CNROM wants 16/32 KiB PRG and CHR ROM");
        return NULL;
    }
    if ((mapper_id == 1 || mapper_id == 4 || mapper_id == 5 || mapper_id == 71 || mapper_id == 94 || mapper_id == 180) && (prg % 16384u || prg < 32768u)) {
        snprintf(err, errsz, "MMC1/MMC3 want >=32 KiB of PRG in 16 KiB units");
        return NULL;
    }
    if ((mapper_id == 11 || mapper_id == 66) && (prg % 32768u || !chr)) {
        snprintf(err, errsz, "Color Dreams/GxROM want PRG in 32 KiB banks and CHR ROM");
        return NULL;
    }
    if (mapper_id == 7 && (prg % 32768u)) {
        snprintf(err, errsz, "AxROM wants PRG in 32 KiB banks");
        return NULL;
    }
    if (mapper_id == 0 && prg != 16384u && prg != 32768u) {
        snprintf(err, errsz, "NROM wants 16 or 32 KiB of PRG, image has %u KiB",
                 prg / 1024);
        return NULL;
    }
    if (mapper_id == 2 && (prg % 16384u || prg <= 32768u)) {
        snprintf(err, errsz, "UxROM wants >32 KiB of PRG in 16 KiB banks, "
                 "image has %u KiB", prg / 1024);
        return NULL;
    }
    DpNesMach *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    nes_init(data + 16, prg, data + 16 + prg, chr, (data[6] & 1) != 0);
    d->off_cycles = offsetof(nes_t, cpu) + offsetof(nes6502_t, cycles);
    d->off_frames = offsetof(nes_t, frames);
    d->mapper = mapper_id;
    d->chr_rom = chr ? data + 16 + prg : NULL;
    d->prg_rom = data + 16;
    d->prg8k = prg / 8192u;
    d->chr1k = chr / 1024u;
    if (BRIDGED(mapper_id))
        machine.cpu.banked = 0;  /* the bridge owns the windows, not the UxROM write */
    memset(&d->rg, 0, sizeof d->rg);
    mapper_regs_init(d);
    mapper_apply(d);
    return d;
}

void dp_nesmach_close(DpNesMach *d) {
    memset(&nes_trace, 0, sizeof nes_trace);
    free(d);
}

void dp_nesmach_reset(DpNesMach *d) {
    nes_reset();
    memset(&d->rg, 0, sizeof d->rg);
    mapper_regs_init(d);
    mapper_apply(d);
    d->in_frame = 0;
    d->steps = 0;
}

void dp_nesmach_set_hooks(DpNesMach *d, const DpNesHooks *h) { d->h = *h; }

/* ends the current run slice at the next instruction boundary (the frame
 * hook of an external driver uses it; same contract as dp_mach_cut) */
void dp_nesmach_cut(DpNesMach *d) {
    (void)d;
    nes_trace.stop = 1;
}

/* the live PPU status byte, without a read's side effects (the fidelity
 * harness traces what the game sees at its $2002 polls) */
unsigned dp_nesmach_ppu_status(const DpNesMach *d) {
    (void)d;
    return machine.ppu.status;
}

uint64_t dp_nesmach_run(DpNesMach *d, uint64_t max_steps) {
    nes_trace.user = d;
    nes_trace.step = t_step;
    nes_trace.rd = t_rd;
    nes_trace.wr = t_wr;
    nes_trace.interrupt = t_interrupt;
    nes_trace.stop = 0;
    d->steps = 0;
    d->budget = max_steps;
    while (d->steps < max_steps) {
        if (d->in_frame) nes_frame_rest();
        else {
            if (watch_on) machine.ppu.panel_out = watch_buf;
            nes_frame();
        }
        /* a completed frame (line 262) reports on_frame even when a hook
         * stop raced its end; a mid-frame stop resumes through rest() */
        if (machine.line >= 262u) {
            d->in_frame = 0;
            if (d->h.on_frame) d->h.on_frame(d->h.user);
        } else {
            d->in_frame = 1;
        }
        if (nes_trace.stop) break;
    }
    return d->steps;
}

void dp_nesmach_frame(DpNesMach *d) {
    nes_trace.user = d;
    nes_trace.step = t_step;
    nes_trace.rd = t_rd;
    nes_trace.wr = t_wr;
    nes_trace.interrupt = t_interrupt;
    nes_trace.stop = 0;
    d->steps = 0;
    d->budget = UINT64_MAX;   /* the step hook stops on budget otherwise:
        * a zeroed instance never executed a single instruction (found by
        * the fidelity harness: the DUT sat at its reset loop forever) */
    if (d->in_frame) { nes_frame_rest(); d->in_frame = 0; }
    nes_frame();
    if (d->h.on_frame) d->h.on_frame(d->h.user);
}

void dp_nesmach_set_input(DpNesMach *d, uint8_t pad0, uint8_t pad1) {
    (void)d;
    machine.input = pad0; /* pad 1 has no read path in the runtime yet */
}

const uint8_t *dp_nesmach_prg_rom(const DpNesMach *d) { return d->prg_rom; }

unsigned dp_nesmach_input(const DpNesMach *d) { (void)d; return machine.input; }

size_t dp_nesmach_state_size(void) {
    return (size_t)nes_state_bytes() + MAPPER_TAIL;
}

void dp_nesmach_save(const DpNesMach *d, void *buf) {
    (void)d;
    uint8_t *p = buf;
    nes_state_io(io_copy, &p, 0);
    memcpy(p, &d->rg, MAPPER_TAIL);
}

void dp_nesmach_load(DpNesMach *d, const void *buf) {
    const uint8_t *p = buf;
    nes_state_io(io_paste, &p, 1);
    memcpy(&d->rg, p, MAPPER_TAIL);
    mapper_apply(d);
}

const unsigned dp_nesmach_off_cycles = offsetof(nes_t, cpu) + offsetof(nes6502_t, cycles);
const unsigned dp_nesmach_off_frames = offsetof(nes_t, frames);

uint64_t dp_nesmach_state_hash(const void *buf) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = buf;
    size_t size = (size_t)nes_state_bytes();
    for (size_t i = 0; i < size; i++) {
        if ((i >= dp_nesmach_off_cycles && i < dp_nesmach_off_cycles + 4) ||
            (i >= dp_nesmach_off_frames && i < dp_nesmach_off_frames + 4))
            continue;
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    for (unsigned i = 0; i < MAPPER_TAIL; i++) h = (h ^ b[size + i]) * 0x100000001b3ull;
    return h;
}

int64_t dp_nesmach_rom_index(const DpNesMach *d, unsigned addr) {
    if (addr < 0x8000u) return -1;
    if (BRIDGED(d->mapper))
        return (int64_t)d->prg_map[(addr - 0x8000u) >> 13] * 8192 + (addr & 0x1FFF);
    return (int64_t)(addr >= 0xC000u ? machine.cpu.fixed_base : machine.cpu.bank_base)
         + (addr & 0x3FFF);
}

uint8_t dp_nesmach_read(const DpNesMach *d, unsigned addr) {
    (void)d;
    if (addr < 0x2000u) return machine.ram[addr & 0x7FF];
    if (addr >= 0x8000u)
        return machine.prg[(addr & 0x3FFFu) +
                           (addr >= 0xC000u ? machine.cpu.fixed_base : machine.cpu.bank_base)];
    return nes_bus_read((uint16_t)addr);
}

unsigned dp_nesmach_pc(const DpNesMach *d) { (void)d; return machine.cpu.pc; }
unsigned dp_nesmach_bank(const DpNesMach *d) { (void)d; return machine.cpu.bank; }
unsigned dp_nesmach_p(const DpNesMach *d) { (void)d; return machine.cpu.p; }
unsigned dp_nesmach_nmi_pending(const DpNesMach *d) { (void)d; return machine.cpu.nmi_edge; }
unsigned dp_nesmach_banked(const DpNesMach *d) { (void)d; return machine.cpu.banked; }
unsigned dp_nesmach_bank_count(const DpNesMach *d) { (void)d; return machine.cpu.bank_count; }
size_t dp_nesmach_prg_size(const DpNesMach *d) { (void)d; return machine.prg_size; }
unsigned dp_nesmach_frames(const DpNesMach *d) { (void)d; return machine.frames; }
const uint8_t *dp_nesmach_oam(const DpNesMach *d) { (void)d; return machine.oam; }

/* the saved blob starts with a verbatim copy of nes_t (nes_state_io), so a
 * fork patch lands at the fixed offsets of the runtime's struct */
uint8_t *dp_nesmach_state_p(void *buf) {
    return (uint8_t *)buf + offsetof(nes_t, cpu) + offsetof(nes6502_t, p);
}
uint8_t *dp_nesmach_state_ram(void *buf) { return (uint8_t *)buf + offsetof(nes_t, ram); }
unsigned dp_nesmach_sp(const DpNesMach *d) { (void)d; return machine.cpu.s; }
unsigned dp_nesmach_state_pc(const void *buf) {
    uint16_t pc;
    memcpy(&pc, (const uint8_t *)buf + offsetof(nes_t, cpu) + offsetof(nes6502_t, pc), sizeof pc);
    return pc;
}
uint64_t dp_nesmach_state_ctx_hash(const void *buf) {
    // coarse context: CPU registers, pc and the mapper registers; RAM and clocks excluded
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = buf;
    size_t o = offsetof(nes_t, cpu);
    const unsigned f[] = { offsetof(nes6502_t, a), offsetof(nes6502_t, x), offsetof(nes6502_t, y),
                           offsetof(nes6502_t, s), offsetof(nes6502_t, p), offsetof(nes6502_t, pc),
                           offsetof(nes6502_t, pc) + 1 };
    for (unsigned i = 0; i < sizeof f / sizeof *f; i++) h = (h ^ b[o + f[i]]) * 0x100000001b3ull;
    size_t size = (size_t)nes_state_bytes();
    for (unsigned i = 0; i < MAPPER_TAIL; i++) h = (h ^ b[size + i]) * 0x100000001b3ull;
    return h;
}
/* C2 projection: the coarse context plus only the 256-byte work-RAM pages whose bit is set in
 * `pages` (what the section about to run was seen to read); mask 0xFF = the whole RAM */
uint64_t dp_nesmach_state_ctx_hash_ram(const void *buf, unsigned pages) {
    uint64_t h = dp_nesmach_state_ctx_hash(buf);
    const uint8_t *ram = (const uint8_t *)buf + offsetof(nes_t, ram);
    for (unsigned pg = 0; pg < 8; pg++) {
        if (!(pages & (1u << pg))) continue;
        for (unsigned i = 0; i < 256; i++) h = (h ^ ram[pg * 256 + i]) * 0x100000001b3ull;
        h ^= pg + 1;
    }
    return h;
}
/* C2 projection (docs/COUVERTURE.fr.md 2.5): the context a section cannot
 * distinguish, projected onto what it actually reads: CPU registers, pc and
 * the mapper registers (coarse), the NMI edge (IRQ in the context), the
 * work-RAM and PRG-RAM pages whose bit is set, and the PPU / controller / APU
 * state when the section was seen reading them.  Clocks stay excluded: an IRQ
 * that fires later is the irq fork's variant, as on GB. */
uint64_t dp_nesmach_state_ctx_hash_live(const void *buf, unsigned ram_pages, unsigned wram_pages,
                                        unsigned io_mask) {
    uint64_t h = dp_nesmach_state_ctx_hash(buf);
    const uint8_t *b = buf;
    h = (h ^ b[offsetof(nes_t, cpu) + offsetof(nes6502_t, nmi_edge)]) * 0x100000001b3ull;
    const uint8_t *ram = b + offsetof(nes_t, ram);
    for (unsigned pg = 0; pg < 8; pg++) {
        if (!(ram_pages & (1u << pg))) continue;
        for (unsigned i = 0; i < 256; i++) h = (h ^ ram[pg * 256 + i]) * 0x100000001b3ull;
        h ^= pg + 1;
    }
    const uint8_t *wram = b + offsetof(nes_t, wram);
    for (unsigned pg = 0; pg < 32; pg++) {
        if (!(wram_pages & (1u << pg))) continue;
        for (unsigned i = 0; i < 256; i++) h = (h ^ wram[pg * 256 + i]) * 0x100000001b3ull;
        h ^= 0x40 + pg;
    }
    if (io_mask & DP_NES_LIVE_PPU) {  // PPU registers + OAM: $2002/$2004 readers
        const uint8_t *ppu = b + offsetof(nes_t, ppu);
        for (size_t i = 0; i < sizeof(nes_ppu_t); i++) h = (h ^ ppu[i]) * 0x100000001b3ull;
        const uint8_t *oam = b + offsetof(nes_t, oam);
        for (unsigned i = 0; i < 256; i++) h = (h ^ oam[i]) * 0x100000001b3ull;
    }
    if (io_mask & DP_NES_LIVE_PAD) {  // controller state: $4016/$4017 readers
        for (unsigned i = 0; i < 4; i++) {
            h = (h ^ b[offsetof(nes_t, input) + i]) * 0x100000001b3ull;  // input, shift[2], strobe
        }
    }
    if (io_mask & DP_NES_LIVE_OTHER) {  // APU / $4015 readers
        const uint8_t *apu = b + offsetof(nes_t, apu);
        for (size_t i = 0; i < sizeof(nes_apu_t); i++) h = (h ^ apu[i]) * 0x100000001b3ull;
    }
    return h;
}
uint8_t *dp_nesmach_state_nmi_edge(void *buf) {
    return (uint8_t *)buf + offsetof(nes_t, cpu) + offsetof(nes6502_t, nmi_edge);
}

/* what a write to $8000+ would change, evaluated before the runtime or the
 * bridge applies it: the PRG window (UxROM/AxROM, in 16 KiB slots) or the
 * CHR bank (CNROM).  Returns 0 none, 1 PRG window, 2 CHR bank. */
int dp_nesmach_mapper_write(const DpNesMach *d, unsigned addr, unsigned value,
                            unsigned *old_slot, unsigned *new_slot) {
    if (d->mapper == 2) {
        *old_slot = machine.cpu.bank;
        *new_slot = value & (machine.cpu.bank_count - 1u);
        return *old_slot != *new_slot ? 1 : 0;
    }
    if (d->mapper == 0) return 0;
    DpNesMapRegs r = d->rg;
    uint8_t p0[4], p1[4];
    uint16_t c0[8], c1[8];
    memcpy(p0, d->prg_map, 4);
    memcpy(c0, d->chr_map, sizeof c0);
    memcpy(p1, p0, 4);
    memcpy(c1, c0, sizeof c1);
    regs_write(d, &r, addr, value);
    maps_of(d, &r, p1, c1);
    if (d->mapper == 5)
        for (unsigned i = 0; i < 4; i++)
            if (p0[i] != p1[i]) { *old_slot = p0[i]; *new_slot = p1[i]; return 1 + (int)(i / 2u) * 2; }
    for (unsigned w = 0; w < 2; w++)
        if (p0[w * 2] != p1[w * 2]) {
            *old_slot = p0[w * 2] / 2u; *new_slot = p1[w * 2] / 2u;
            return 1 + (int)w * 2;
        }
    for (unsigned i = 0; i < 8; i++)
        if (c0[i] != c1[i]) { *old_slot = c0[i] / 8u; *new_slot = c1[i] / 8u; return 2; }
    return 0;
}
unsigned dp_nesmach_mapper(const DpNesMach *d) { return d->mapper; }

/* Puts the CPU at `addr` with PRG bank `bank` in the switchable window
 * (UxROM; NROM accepts bank 0 only).  Bridged boards (3, 7) are refused:
 * their windows belong to the bridge.  Returns 1 on success. */
int dp_nesmach_enter(DpNesMach *d, unsigned bank, unsigned addr) {
    if (d->mapper == 2) {
        if (bank >= machine.cpu.bank_count) return 0;
        machine.cpu.bank = (uint8_t)bank;
        machine.cpu.bank_base = (uint32_t)bank * 16384u;
    } else if (d->mapper == 1) {   /* MMC1: 16 KiB switchable $8000 slot (control mode 3), set as the serial port would */
        if (bank >= (unsigned)(machine.prg_size >> 14) || bank > 15) return 0;
        d->rg.v[0] |= 0x0C; d->rg.v[3] = (uint8_t)bank;
        mapper_apply(d);
    } else if (d->mapper == 71 || d->mapper == 94 || d->mapper == 180) {   /* Camerica / UN1ROM: a 16 KiB switchable $8000 slot */
        if (bank >= (unsigned)(machine.prg_size >> 14) || bank > 7) return 0;
        d->rg.v[0] = (uint8_t)(d->mapper == 94 ? bank << 2 : bank);
        mapper_apply(d);
    } else if (d->mapper != 0 || bank != 0) return 0;
    machine.cpu.pc = (uint16_t)addr;
    return 1;
}

/* MMC3: map an 8 KiB PRG bank under the switchable slot `addr` falls in ($8000-$BFFF in PRG mode 0, $C000-$DFFF in mode 1) */
int dp_nesmach_cross16(const DpNesMach *d) { return d->mapper == 1 || d->mapper == 2 || d->mapper == 71 || d->mapper == 94 || d->mapper == 180; }
int dp_nesmach_mmc3_mode1(const DpNesMach *d) { return d->mapper == 4 && (d->rg.v[0] & 0x40); }
/* the switchable PRG window a stream pointer must fall in: [lo, hi) */
void dp_nesmach_window(const DpNesMach *d, unsigned *lo, unsigned *hi) {
    *lo = 0x8000u; *hi = 0xC000u;
    if (d->mapper == 180) { *lo = 0xC000u; *hi = 0x10000u; }
    else if (dp_nesmach_mmc3_mode1(d)) { *lo = 0xC000u; *hi = 0xE000u; }
}
int dp_nesmach_map_8k(DpNesMach *d, unsigned addr, unsigned bank8) {
    if (d->mapper != 4 || addr < 0x8000u || addr >= 0xE000u || bank8 >= (unsigned)(machine.prg_size >> 13) || bank8 > 63) return 0;
    if (d->rg.v[0] & 0x40) {   /* PRG mode 1: R6 is the $C000 window, $8000 is fixed */
        if (addr < 0xC000u) return 0;
        d->rg.v[2 + (addr >= 0xE000u ? 7 : 6)] = (uint8_t)bank8;
    } else {
        if (addr >= 0xC000u) return 0;
        d->rg.v[2 + (addr >= 0xA000u ? 7 : 6)] = (uint8_t)bank8;
    }
    mapper_apply(d);
    return 1;
}

size_t dp_nesmach_chr_size(const DpNesMach *d) { return d->chr_rom ? (size_t)d->chr1k * 1024u : 0; }

/* canonical CHR cell behind a PPU pattern address under the live bank */
int64_t dp_nesmach_chr_index(const DpNesMach *d, unsigned addr) {
    if (!d->chr_rom) return -1;
    if (d->mapper == 1 || d->mapper == 3 || d->mapper == 4 || d->mapper == 5 || d->mapper == 11 || d->mapper == 66)
        return (int64_t)d->chr_map[(addr >> 10) & 7] * 1024 + (addr & 0x3FF);
    return (int64_t)(machine.chr - d->chr_rom) + (addr & 0x1FFFu);
}

/* tiles the PPU state refers to right now: background tiles of the two
 * nametables (when BG rendering is on) and OAM sprites (when sprites are
 * on).  Fills out[] with (cell << 1 | is_sprite); returns the count. */
size_t dp_nesmach_chr_tiles(const DpNesMach *d, uint64_t *out, size_t cap) {
    if (!d->chr_rom) return 0;
    size_t n = 0;
    const nes_ppu_t *p = &machine.ppu;
    if (p->mask & 0x08) {
        unsigned page = (p->ctrl & 0x10) ? 0x1000u : 0u;
        for (unsigned nt = 0; nt < 2; nt++)
            for (unsigned i = 0; i < 960 && n < cap; i++) {
                int64_t c = dp_nesmach_chr_index(d, page + p->nametable[nt * 1024u + i] * 16u);
                out[n++] = (uint64_t)c << 1;
            }
    }
    if (p->mask & 0x10) {
        int tall = (p->ctrl & 0x20) != 0;
        for (unsigned s = 0; s < 64 && n < cap; s++) {
            const uint8_t *o = p->oam + s * 4;
            if (o[0] >= 0xEF) continue;
            unsigned a = tall ? ((o[1] & 1) ? 0x1000u : 0u) + (o[1] & 0xFEu) * 16u
                              : o[1] * 16u + ((p->ctrl & 0x08) ? 0x1000u : 0u);
            for (unsigned t = 0; t < (tall ? 2u : 1u) && n < cap; t++)
                out[n++] = ((uint64_t)dp_nesmach_chr_index(d, a + t * 16u) << 1) | 1u;
        }
    }
    return n;
}
static void watch_row(void *host, unsigned row) {
    (void)host;
    machine.ppu.panel_out = watch_buf + (size_t)((row + 1) % 128) * 160;
}
void dp_nesmach_watch_enable(DpNesMach *d, int on) {
    (void)d;
    watch_on = on;
    machine.ppu.panel = on ? watch_row : 0;
    machine.ppu.panel_out = watch_buf;
}
void dp_nesmach_render(const DpNesMach *d, uint32_t *argb) {
    (void)d;
    for (unsigned i = 0; i < 160 * 128; i++) {
        uint16_t v = watch_buf[i];
        unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        argb[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
}
const uint8_t *dp_nesmach_ram(const DpNesMach *d) { (void)d; return machine.ram; }
uint64_t dp_nesmach_screen_key(const DpNesMach *d) {
    (void)d;
    const nes_ppu_t *p = &machine.ppu;
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(p->ctrl & 3);
    h *= 0x100000001b3ull;
    h ^= (uint64_t)(((p->t & 0x1F) >> 3) | (((p->t >> 5) & 0x1F) >> 3) << 2);
    h *= 0x100000001b3ull;
    for (unsigned nt = 0; nt < 2; nt++)
        for (unsigned r = 0; r < 30; r += 4)
            for (unsigned c = 0; c < 32; c += 4) {
                h ^= (uint64_t)(p->nametable[nt * 1024u + r * 32u + c] >> 3);
                h *= 0x100000001b3ull;
            }
    return h;
}
unsigned dp_nesmach_ppu_v(const DpNesMach *d) { (void)d; return machine.ppu.v & 0x3FFF; }
