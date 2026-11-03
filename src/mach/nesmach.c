#include "nesmach.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "nes.h"
#include "nes_native.h"
#include "nes_trace.h"

/* the runtime's one machine (like MdFull, one ROM live per process) */
struct DpNesMach {
    DpNesHooks h;
    uint64_t steps, budget;
    int in_frame;      /* a frame is part-run: resume with nes_frame_rest */
    /* offsets into the state blob excluded from the exact-state hash */
    size_t off_cycles, off_frames;
    /* bridge-side mapper: 0 NROM, 2 UxROM (runtime), 3 CNROM, 7 AxROM */
    unsigned mapper;
    const uint8_t *chr_rom;
    unsigned chr_banks;      /* 8 KiB CHR banks (CNROM) */
    unsigned reg;            /* CNROM chr bank / AxROM prg bank */
};

#define MAPPER_TAIL 4u  /* register bytes appended to a state image */

static void mapper_apply(DpNesMach *d) {
    if (d->mapper == 3 && d->chr_rom) {
        unsigned b = d->reg % (d->chr_banks ? d->chr_banks : 1u);
        machine.chr = d->chr_rom + (size_t)b * 8192u;
        machine.ppu.chr = machine.chr;
    } else if (d->mapper == 7) {
        unsigned n32 = (unsigned)(machine.prg_size / 32768u);
        unsigned b = d->reg & 7u;
        if (n32) b %= n32;
        machine.cpu.bank_base = b * 32768u;
        machine.cpu.fixed_base = b * 32768u + 16384u;
        machine.cpu.bank = (uint8_t)(b * 2u);
    }
}

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
    if (a >= 0x8000u && (d->mapper == 3 || d->mapper == 7)) {
        d->reg = v;  /* bus conflicts ignored, as for UxROM */
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
    if (mapper_id != 0 && mapper_id != 2 && mapper_id != 3 && mapper_id != 7) {
        snprintf(err, errsz, "mapper %u: supported boards are NROM (0), "
                 "UxROM (2), CNROM (3) and AxROM (7)", mapper_id);
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
    d->chr_banks = chr / 8192u;
    if (mapper_id == 7) machine.cpu.banked = 0;  /* ours, not the UxROM write */
    mapper_apply(d);
    return d;
}

void dp_nesmach_close(DpNesMach *d) {
    memset(&nes_trace, 0, sizeof nes_trace);
    free(d);
}

void dp_nesmach_reset(DpNesMach *d) {
    nes_reset();
    d->reg = 0;
    mapper_apply(d);
    d->in_frame = 0;
    d->steps = 0;
}

void dp_nesmach_set_hooks(DpNesMach *d, const DpNesHooks *h) { d->h = *h; }

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
        else nes_frame();
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
    if (d->in_frame) { nes_frame_rest(); d->in_frame = 0; }
    nes_frame();
    if (d->h.on_frame) d->h.on_frame(d->h.user);
}

void dp_nesmach_set_input(DpNesMach *d, uint8_t pad0, uint8_t pad1) {
    (void)d;
    machine.input = pad0; /* pad 1 has no read path in the runtime yet */
}

size_t dp_nesmach_state_size(void) {
    return (size_t)nes_state_bytes() + MAPPER_TAIL;
}

void dp_nesmach_save(const DpNesMach *d, void *buf) {
    (void)d;
    uint8_t *p = buf;
    nes_state_io(io_copy, &p, 0);
    memcpy(p, &d->reg, MAPPER_TAIL);
}

void dp_nesmach_load(DpNesMach *d, const void *buf) {
    const uint8_t *p = buf;
    nes_state_io(io_paste, &p, 1);
    memcpy(&d->reg, p, MAPPER_TAIL);
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
    (void)d;
    if (addr < 0x8000u) return -1;
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
uint8_t *dp_nesmach_state_nmi_edge(void *buf) {
    return (uint8_t *)buf + offsetof(nes_t, cpu) + offsetof(nes6502_t, nmi_edge);
}

/* what a write to $8000+ would change, evaluated before the runtime or the
 * bridge applies it: the PRG window (UxROM/AxROM, in 16 KiB slots) or the
 * CHR bank (CNROM).  Returns 0 none, 1 PRG window, 2 CHR bank. */
int dp_nesmach_mapper_write(const DpNesMach *d, unsigned value,
                            unsigned *old_slot, unsigned *new_slot) {
    if (d->mapper == 2) {
        *old_slot = machine.cpu.bank;
        *new_slot = value & (machine.cpu.bank_count - 1u);
        return 1;
    }
    if (d->mapper == 7) {
        unsigned n32 = (unsigned)(machine.prg_size / 32768u);
        *old_slot = machine.cpu.bank_base / 16384u;
        *new_slot = ((value & 7u) % (n32 ? n32 : 1u)) * 2u;
        return 1;
    }
    if (d->mapper == 3) {
        unsigned n = d->chr_banks ? d->chr_banks : 1u;
        *old_slot = d->reg % n;
        *new_slot = value % n;
        return 2;
    }
    return 0;
}
unsigned dp_nesmach_mapper(const DpNesMach *d) { return d->mapper; }
