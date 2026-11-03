// pcemach.c: the PC Engine bridge over m0boy's machine.  m0boy's pce.c is
// compiled into this file (its statics — the timer tick, the IRQ latch
// update, the line carry — are the clock this bridge steps), with its bus
// renamed so every non-WRAM access lands here first: the CPU's direct ROM
// windows are switched off (rom_bytes = 0 in the core's view), the bridge
// maps the pages itself and tells the observer about each access.
//
// The line clock is pce_frame's, one instruction at a time: a line gives
// the CPU 1365 master clocks (455 cycles at CSH, 1365/12 at CSL, the
// remainder carried), an instruction runs while the line's budget is
// positive, the overshoot is carried into the next line exactly as
// huc6280_run(cpu, cycles + n) carries it — so a stepped run and m0boy's
// frame loop execute the same instruction stream.
#define machine dp_pce_machine
#define pce_bus_read dp_pce_hw_read
#define pce_bus_write dp_pce_hw_write
#include "pce.c"
#undef pce_bus_read
#undef pce_bus_write
#include "pce.h"
#include <stdlib.h>

uint8_t pce_bus_read(uint16_t at);
void pce_bus_write(uint16_t at, uint8_t v);

typedef struct {
    pce_t m;
    unsigned carry;     // master clocks carried between lines
    unsigned line;      // 0..261
    int line_left;      // CPU cycles left in this line
    unsigned line_budget;
} PceBridgeState;

static uint8_t *rom;           // owned, header stripped
static size_t rom_size;
static uint32_t rom_pages;
static uint8_t page_map[256];  // MPR value -> physical page, 0xFF: not ROM
static int page_valid[256];
static unsigned line_no, line_budget_;
static int line_left;
static DpPceObserver obs;
static int st_port;            // ST0/ST1/ST2 in flight: the write is the hardware's

static void build_page_map(void) {
    for (unsigned v = 0; v < 256; v++) {
        page_valid[v] = 0;
        if (v >= 0x80 || !rom_pages) continue;
        uint32_t phys;
        if (rom_pages == 0x30) // 384 KiB: 256 KiB at $00-$3F, the 128 KiB tail at $40-$7F
            phys = v < 0x40 ? (v & 0x1F) : 0x20 + (v & 0x0F);
        else
            phys = v % rom_pages;
        page_map[v] = (uint8_t)phys;
        page_valid[v] = 1;
    }
}

static void line_start(void) {
    master_carry += 1365;
    const unsigned div = machine.cpu.speed ? 3u : 12u;
    line_budget_ = master_carry / div;
    master_carry %= div;
    line_left += (int)line_budget_; // the overshoot carries, as in huc6280_run
}

int dp_pce_init(const uint8_t *data, size_t size) {
    if (!data || size < 0x2000) return 0;
    size_t off = (size % 0x2000) == 512 ? 512 : 0;
    size_t n = size - off;
    if (n % 0x2000 || n > 0x100000) return 0;
    uint8_t *copy = malloc(n);
    if (!copy) return 0;
    memcpy(copy, data + off, n);
    dp_pce_close();
    rom = copy;
    rom_size = n;
    rom_pages = (uint32_t)(n / 0x2000);
    build_page_map();
    memset(&machine, 0, sizeof machine);
    machine.cpu.wram = machine.wram;
    machine.cpu.rom = rom;
    machine.cpu.rom_bytes = (uint32_t)rom_size;
    huc6280_power(&machine.cpu);
    pce_init(rom, (uint32_t)rom_size);
    // every cartridge read goes through the bridge from now on
    machine.cpu.rom_bytes = 0;
    huc6280_map(&machine.cpu);
    line_no = 0;
    line_left = 0;
    line_start();
    return 1;
}

void dp_pce_close(void) {
    free(rom);
    rom = NULL;
    rom_size = 0;
    rom_pages = 0;
    memset(&obs, 0, sizeof obs);
}

size_t dp_pce_rom_size(void) { return rom_size; }
const uint8_t *dp_pce_rom(void) { return rom; }
void dp_pce_observe(const DpPceObserver *o) {
    if (o) obs = *o; else memset(&obs, 0, sizeof obs);
}

DpPceCpu dp_pce_cpu(void) {
    DpPceCpu c;
    const huc6280_t *k = &machine.cpu;
    c.a = k->a; c.x = k->x; c.y = k->y; c.s = k->s; c.p = k->p; c.pc = k->pc;
    memcpy(c.mpr, k->mpr, 8);
    c.irq_low = k->irq_low;
    c.irq_mask = k->irq_mask;
    c.speed = k->speed;
    return c;
}

void dp_pce_set_flags(uint8_t p) { machine.cpu.p = p; }
void dp_pce_set_pc(uint16_t pc) { machine.cpu.pc = pc; }

// map the physical ROM page into the window of pc and start there; 0 when no MPR value maps it
int dp_pce_enter(uint32_t rom_page, uint16_t pc) {
    for (unsigned v = 0; v < 0x80; v++)
        if (page_valid[v] && page_map[v] == rom_page) {
            machine.cpu.mpr[pc >> 13] = (uint8_t)v;
            machine.cpu.pc = pc;
            return 1;
        }
    return 0;
}

int64_t dp_pce_rom_index_mpr(const uint8_t *mpr, uint16_t addr) {
    uint8_t v = mpr[addr >> 13];
    if (!page_valid[v]) return -1;
    return (int64_t)page_map[v] * 0x2000 + (addr & 0x1FFF);
}

int64_t dp_pce_rom_index(uint16_t addr) { return dp_pce_rom_index_mpr(machine.cpu.mpr, addr); }

uint8_t dp_pce_peek(uint16_t addr) {
    uint8_t v = machine.cpu.mpr[addr >> 13];
    if (page_valid[v]) return rom[(size_t)page_map[v] * 0x2000 + (addr & 0x1FFF)];
    if (v >= 0xF8 && v <= 0xFB) return machine.wram[addr & 0x1FFF];
    return 0xFF;
}

uint8_t pce_bus_read(uint16_t at) {
    uint8_t v = machine.cpu.mpr[at >> 13];
    if (page_valid[v]) {
        uint32_t cell = (uint32_t)page_map[v] * 0x2000 + (at & 0x1FFF);
        if (obs.rom_read) obs.rom_read(obs.user, cell, at);
        return rom[cell];
    }
    if (v == 0xFF) {
        uint8_t r = dp_pce_hw_read(at & 0x1FFF);
        if (obs.io_read) obs.io_read(obs.user, at & 0x1FFF, r);
        return r;
    }
    return 0xFF; // unmapped pages (no CD, no expansion): open bus
}

void pce_bus_write(uint16_t at, uint8_t v) {
    uint8_t page = st_port ? 0xFF : machine.cpu.mpr[at >> 13];
    if (page == 0xFF) {
        if (obs.io_write) obs.io_write(obs.user, at & 0x1FFF, v);
        dp_pce_hw_write(at & 0x1FFF, v);
    }
    // ROM pages and open bus ignore writes (HuCards without a mapper)
}

int dp_pce_irq_due(void) {
    return (machine.cpu.irq_low & machine.cpu.irq_mask) && !(machine.cpu.p & PCE_P_I);
}

unsigned dp_pce_step(int *frame_done) {
    huc6280_t *c = &machine.cpu;
    uint8_t op = dp_pce_peek(c->pc);
    st_port = !dp_pce_irq_due() && (op == 0x03 || op == 0x13 || op == 0x23);
    uint32_t before = c->cycles;
    huc6280_run(c, c->cycles + 1);
    st_port = 0;
    unsigned spent = c->cycles - before;
    if (frame_done) *frame_done = 0;
    line_left -= (int)spent;
    if (line_left <= 0) {
        vdc_line(&machine.vdc, line_no);
        machine.timer_clock += line_budget_;
        while (machine.timer_clock >= 1024) {
            machine.timer_clock -= 1024;
            timer_tick();
        }
        update_irq();
        if (++line_no == 262) {
            line_no = 0;
            machine.frames++;
            if (frame_done) *frame_done = 1;
        }
        line_start();
    }
    return spent;
}

void dp_pce_raise(int source) {
    if (source == 1) machine.vdc.status |= VDCS_VD;
    else if (source == 2) machine.timer_irq = 1;
    update_irq();
}

void dp_pce_input(uint8_t buttons) { machine.input = buttons; }
uint8_t dp_pce_input_get(void) { return machine.input; }
uint32_t dp_pce_frames(void) { return machine.frames; }

size_t dp_pce_state_size(void) { return sizeof(PceBridgeState); }

void dp_pce_save(void *dst) {
    PceBridgeState *s = dst;
    memcpy(&s->m, &machine, sizeof machine);
    s->carry = master_carry;
    s->line = line_no;
    s->line_left = line_left;
    s->line_budget = line_budget_;
}

void dp_pce_load(const void *src) {
    const PceBridgeState *s = src;
    memcpy(&machine, &s->m, sizeof machine);
    master_carry = s->carry;
    line_no = s->line;
    line_left = s->line_left;
    line_budget_ = s->line_budget;
    // the windows point into this process's machine (same global): rebuild
    // them anyway so a snapshot from another build layout stays coherent
    machine.cpu.wram = machine.wram;
    machine.cpu.rom_bytes = 0;
    huc6280_map(&machine.cpu);
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

uint64_t dp_pce_state_hash(void) {
    const huc6280_t *c = &machine.cpu;
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t regs[16] = { c->a, c->x, c->y, c->s, c->p, (uint8_t)c->pc, (uint8_t)(c->pc >> 8),
                         c->irq_low, c->irq_mask, (uint8_t)c->speed, machine.input, machine.pad_sel,
                         machine.timer_enable, machine.timer_load, machine.timer_irq, (uint8_t)line_no };
    h = fnv(h, regs, sizeof regs);
    h = fnv(h, c->mpr, 8);
    h = fnv(h, machine.wram, sizeof machine.wram);
    const vdc_t *v = &machine.vdc;
    h = fnv(h, &v->select, (size_t)((const uint8_t *)&v->linebuf - (const uint8_t *)&v->select));
    return h;
}

uint64_t dp_pce_vram_hash(void) {
    return fnv(0xcbf29ce484222325ull, machine.vdc.vram, sizeof machine.vdc.vram);
}

uint8_t *dp_pce_wram(void) { return machine.wram; }
uint8_t dp_pce_mpr(unsigned i) { return machine.cpu.mpr[i & 7]; }
void dp_pce_reset(void) {
    pce_reset();
    machine.cpu.rom_bytes = 0;
    huc6280_map(&machine.cpu);
    line_no = 0;
    line_left = 0;
    line_start();
}

// --- fast frames: the native loop, one line per huc6280_run, the trace
// hook (huc6280.c built with PCE_TRACE) reporting each instruction
static DpPceTraceFn trace_fn;
static void *trace_user;

void dp_pce_trace(DpPceTraceFn fn, void *user) { trace_fn = fn; trace_user = user; }

void pce_trace_step(uint16_t pc) {
    uint8_t op = dp_pce_peek(pc);
    st_port = op == 0x03 || op == 0x13 || op == 0x23;
    if (trace_fn) trace_fn(trace_user, pc, op);
}

void pce_trace_dyn(uint16_t pc) { (void)pc; }

void dp_pce_run_frame(void) {
    huc6280_t *c = &machine.cpu;
    for (;;) {
        uint32_t before = c->cycles;
        huc6280_run(c, c->cycles + (uint32_t)(line_left > 0 ? line_left : 1));
        st_port = 0;
        line_left -= (int)(c->cycles - before);
        if (line_left > 0) continue;
        vdc_line(&machine.vdc, line_no);
        machine.timer_clock += line_budget_;
        while (machine.timer_clock >= 1024) {
            machine.timer_clock -= 1024;
            timer_tick();
        }
        update_irq();
        int done = 0;
        if (++line_no == 262) {
            line_no = 0;
            machine.frames++;
            done = 1;
        }
        line_start();
        if (done) return;
    }
}

// a coarse screen fingerprint (Go-Explore cells): the visible part of the
// BAT (its size from MWR, its origin from the scroll registers) on a 4x4
// tile grid, tile numbers quantised, and the vertical scroll in 32-pixel
// steps
uint64_t dp_pce_screen_key(void) {
    const vdc_t *d = &machine.vdc;
    static const unsigned widths[4] = { 32, 64, 128, 128 };
    unsigned w = widths[(d->mwr >> 4) & 3], h = (d->mwr & 0x40) ? 64 : 32;
    unsigned x0 = (d->bxr >> 3) & (w - 1), y0 = (d->byr >> 3) & (h - 1);
    uint64_t hsh = 0xcbf29ce484222325ull ^ (uint64_t)(d->byr >> 5);
    hsh *= 0x100000001b3ull;
    for (unsigned r = 0; r < 28; r += 4)
        for (unsigned c = 0; c < 32; c += 4) {
            unsigned at = ((y0 + r) & (h - 1)) * w + ((x0 + c) & (w - 1));
            hsh ^= (uint64_t)((d->vram[at & (VDC_VRAM_WORDS - 1)] & 0x7FF) >> 4);
            hsh *= 0x100000001b3ull;
        }
    return hsh;
}
