// gp32_machine.c: the m0boy GP32 runtime (S3C2400 / ARM920T) as a DpMachine.
//
// Each instance owns a whole SoC.  A snapshot is the SoC without its SDRAM and
// host pixel buffers, plus one page-store id per KiB of SDRAM: only the pages
// written since the last snapshot are re-interned, only the pages that differ
// are copied back.  SmartMedia writes are kept per path as a chain of chunk
// records in the same store.
//
// The "ROM" the kernel indexes is an image of SDRAM: the loaded executable, or,
// for applications that unpack or relocate themselves, the RAM taken after a
// probe run.  A byte belongs to the image only while the RAM still holds the
// image's value.  With a BIOS, its 512 KiB follow the application in the index.
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/dpmachine.h"
#include "../explore/core.h"
#include "../platform/gp32/gp32_platform.h"
#include "../platform/gp32/pages.h"
#include "gp32_exec.h"
#include "gp32_hle.h"
#include "gp32_host.h"
#include "gp32_shot.h"

#define RAM_BASE S3C2400_SDRAM_BASE
#define NPAGES (GP32_RAM_SIZE / DP_GP32_PAGE)
#define WORDS (NPAGES / 64u)
#define HEAD_BYTES offsetof(S3c2400, ram)
#define TAIL_OFF offsetof(S3c2400, steps)
#define TAIL_BYTES (sizeof(S3c2400) - TAIL_OFF)
#define CARD_CHUNK 512u
#define AUDIO_FRAMES 16384u
#define T_BIT 0x20u

// shared, immutable after the probe
typedef struct {
    int refs;
    DpPageStore *pages;
    S3c2400 *boot;                        // the pristine machine
    uint8_t *bios;                        // NULL: HLE
    size_t bios_size;
    uint8_t *card; size_t card_size;      // the pristine card
    uint8_t *payload; size_t payload_size;
    uint32_t alias_start, alias_len, alias_to;   // an RW section the loader copied away from its file position
    uint8_t *image; size_t image_size, app_size;  // what the kernel indexes
    int snapshot;                         // the image is a RAM snapshot
    uint64_t probe_steps, probe_outside, unpack_steps;
    uint32_t boot_pc;
    uint64_t program;                     // names the application in shots
    int card_writable;
    uint64_t unhandled[64];               // unhandled HLE SWIs: (number << 32) | pc
    unsigned n_unhandled;
    unsigned irq_cap;                     // interrupt forks per source (0: no cap)
    unsigned irq_forks[32];
    unsigned long long irq_refused;
} Gp32Source;

typedef struct {
    Gp32Source *src;
    S3c2400 *soc;
    uint32_t cur_id[NPAGES];
    uint64_t dirty_id[WORDS], dirty_hash[WORDS];
    uint64_t page_hash[NPAGES], ram_hash;
    uint8_t *card;                        // this instance's card
    uint32_t card_node;                   // head of the write chain (0: pristine)
    uint64_t *chunk_hash;                 // by card chunk
    uint64_t *chunk_seen;
    const DpObservers *obs; void *user;
    bool (*swi)(void *, uint32_t, uint32_t); void *swi_context;
    unsigned pad_armed, pad_seen;
    int16_t *audio; unsigned audio_head, audio_count;
    // translations of 1 KiB blocks, valid for one TLB generation of one loaded state
    struct Xlat { uint32_t page, ttbr, gen, epoch, phys; } xlat[512];
    uint32_t epoch;
} Gp32;

#define GP32_APP_LIMIT 0x79c000u   // the largest application the loader accepts under the HLE firmware

// -- memory geometry ----------------------------------------------------------

// read-only mirror of the ARM920T walk (no side effects)
static int64_t walk(const Gp32 *g, uint32_t mva) {
    const Arm920t *cpu = &g->soc->cpu;
    S3c2400Access l1 = s3c2400_bus_peek(&g->soc->bus, cpu->ttbr | ((mva >> 18) & 0x3ffcu), 4);
    uint32_t desc = l1.value, type = desc & 3u;
    if (!l1.accepted || !type) return -1;
    if (type == 2) return (desc & 0xfff00000u) | (mva & 0xfffffu);
    uint32_t at = type == 1 ? (desc & 0xfffffc00u) | ((mva >> 10) & 0x3fcu) : (desc & 0xfffff000u) | ((mva >> 8) & 0xffcu);
    S3c2400Access l2 = s3c2400_bus_peek(&g->soc->bus, at, 4);
    desc = l2.value;
    if (!l2.accepted) return -1;
    switch (desc & 3u) {
    case 1: return (desc & 0xffff0000u) | (mva & 0xffffu);
    case 2: return (desc & 0xfffff000u) | (mva & 0xfffu);
    case 3: return type == 3 ? (int64_t)((desc & 0xfffffc00u) | (mva & 0x3ffu)) : -1;
    default: return -1;
    }
}

static inline int64_t translate(const Gp32 *g, uint32_t va) {
    const Arm920t *cpu = &g->soc->cpu;
    if (!(cpu->control & 1u)) return va;
    uint32_t mva = va < 0x02000000u ? va | cpu->pid : va;
    uint32_t page = mva >> 10;
    struct Xlat *x = &((Gp32 *)g)->xlat[(page ^ page >> 9) & 511u];
    if (x->epoch == g->epoch && x->page == page && x->gen == cpu->tlb_generation && x->ttbr == cpu->ttbr)
        return (int64_t)(x->phys | (mva & 0x3ffu));
    int64_t p = walk(g, mva);
    if (p >= 0) *x = (struct Xlat){ page, cpu->ttbr, cpu->tlb_generation, g->epoch, (uint32_t)p & ~0x3ffu };
    return p;
}

static inline uint8_t peek8(const Gp32 *g, uint32_t va) {
    int64_t p = translate(g, va);
    if (p < 0) return 0;
    uint32_t off = (uint32_t)p - RAM_BASE;
    if (off < GP32_RAM_SIZE) return g->soc->ram[off];
    if (g->soc->bus.boot_rom && (uint64_t)p < g->soc->bus.boot_rom_size) return g->soc->bus.boot_rom[p];
    S3c2400Access a = s3c2400_bus_peek(&g->soc->bus, (uint32_t)p, 1);
    return a.accepted ? (uint8_t)a.value : 0;
}

static uint8_t gp32_fetch(DpMachine *m, uint32_t a) { return peek8(m->impl, a & ~DP_ARM_THUMB); }

static void gp32_fetch_n(DpMachine *m, uint32_t a, uint8_t *o, unsigned n) {
    const Gp32 *g = m->impl;
    a &= ~DP_ARM_THUMB;
    uint32_t off = a - RAM_BASE;
    if (!(g->soc->cpu.control & 1u) && off < GP32_RAM_SIZE && n <= GP32_RAM_SIZE - off) { memcpy(o, g->soc->ram + off, n); return; }
    for (unsigned i = 0; i < n; i++) o[i] = peek8(g, a + i);
}

static int64_t gp32_rom_index(DpMachine *m, uint32_t a) {
    const Gp32 *g = m->impl;
    const Gp32Source *s = g->src;
    int64_t p = translate(g, a & ~DP_ARM_THUMB);
    if (p < 0) return -1;
    uint32_t off = (uint32_t)p - RAM_BASE;
    if (off < GP32_RAM_SIZE) {
        if (off < s->app_size && g->soc->ram[off] == s->image[off]) return off;
        if (!s->snapshot && off - s->alias_start < s->alias_len) {
            uint32_t at = s->alias_to + (off - s->alias_start);
            if (g->soc->ram[off] == s->image[at]) return at;
        }
        return -1;
    }
    if (s->bios && (uint64_t)p < GP32_BIOS_SIZE && s->app_size + (size_t)p < s->image_size) return (int64_t)(s->app_size + (size_t)p);
    return -1;
}

static int64_t gp32_ram_index(DpMachine *m, uint32_t a) {
    int64_t p = translate(m->impl, a & ~DP_ARM_THUMB);
    if (p < 0) return -1;
    uint32_t off = (uint32_t)p - RAM_BASE;
    return off < GP32_RAM_SIZE ? (int64_t)off : -1;
}
static uint32_t gp32_ram_addr(DpMachine *m, uint32_t i) { (void)m; return RAM_BASE + i; }
static const uint8_t *gp32_ram_view(DpMachine *m) { return ((Gp32 *)m->impl)->soc->ram; }

// -- observation --------------------------------------------------------------

static inline void touch(Gp32 *g, uint32_t off) {
    uint32_t p = off / DP_GP32_PAGE;
    g->dirty_id[p / 64] |= 1ull << (p & 63);
    g->dirty_hash[p / 64] |= 1ull << (p & 63);
}

static void bus_access(void *context, const S3c2400Access *a) {
    Gp32 *g = context;
    if (a->region != S3C2400_SDRAM || !a->accepted) return;
    if (a->kind == S3C2400_STORE || a->kind == S3C2400_DMA_STORE || a->kind == S3C2400_CACHE_WRITEBACK) {
        touch(g, a->offset);
        touch(g, a->offset + a->width - 1);
    } else if (a->kind == S3C2400_LCD_LOAD && g->obs && g->obs->on_copy && a->offset < g->src->app_size) {
        g->obs->on_copy(g->user, a->address, a->width, DP_COPY_DMA);   // the LCD controller shows image bytes directly
    }
}

static void bus_copy(void *context, const S3c2400Access *source, const S3c2400Access *destination) {
    Gp32 *g = context;
    (void)destination;
    if (g->obs && g->obs->on_copy) g->obs->on_copy(g->user, source->address, source->width, DP_COPY_DMA);
}

static void cpu_access(void *context, const Arm920tAccess *e) {
    Gp32 *g = context;
    if (e->kind != S3C2400_LOAD && e->kind != S3C2400_STORE) return;
    // registers are named by their bus address, memory by the address the program used
    uint32_t a = e->physical >= 0x14000000u && e->physical < 0x20000000u ? e->physical : e->virtual_address;
    if (g->pad_armed && e->kind == S3C2400_LOAD && ((e->physical & ~3u) == 0x1560000cu || (e->physical & ~3u) == 0x15600030u))
        g->pad_seen = 1;
    if (!g->obs) return;
    void (*f)(void *, uint32_t, uint8_t) = e->kind == S3C2400_LOAD ? g->obs->on_read : g->obs->on_write;
    if (!f) return;
    for (unsigned i = 0; i < e->width; i++) f(g->user, a + i, (uint8_t)(e->value >> (8 * i)));
}

static void note_unhandled(Gp32Source *s, uint32_t number, uint32_t pc) {
    uint64_t key = (uint64_t)number << 32 | pc;
    unsigned n = __atomic_load_n(&s->n_unhandled, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n && i < 64; i++) if (__atomic_load_n(&s->unhandled[i], __ATOMIC_RELAXED) == key) return;
    unsigned at = __atomic_fetch_add(&s->n_unhandled, 1, __ATOMIC_ACQ_REL);
    if (at < 64) __atomic_store_n(&s->unhandled[at], key, __ATOMIC_RELAXED);
}

static void touch_range(Gp32 *g, uint32_t lo, uint32_t hi) {
    for (uint32_t off = lo; off < hi; off += DP_GP32_PAGE) touch(g, off);
}

// the HLE services that fill SDRAM without the bus: the application chain loader and the surface setup
static bool hle_swi(void *context, uint32_t number, uint32_t pc) {
    Gp32 *g = context;
    uint32_t service = g->soc->cpu.r[2];
    bool ok = g->swi(g->swi_context, number, pc);
    if (number == 0x05) touch_range(g, 0, GP32_RAM_SIZE);
    else if (number == 0x08 && service == 0) touch_range(g, 0x7b4000u, GP32_RAM_SIZE);
    if (!ok) note_unhandled(g->src, number, pc);
    return ok;
}

static void audio_sample(void *context, int16_t left, int16_t right, unsigned rate) {
    Gp32 *g = context;
    (void)rate;
    if (!g->audio || g->audio_count == AUDIO_FRAMES) return;
    unsigned at = (g->audio_head + g->audio_count++) % AUDIO_FRAMES;
    g->audio[2 * at] = left; g->audio[2 * at + 1] = right;
}

// point every pointer of a copied SoC at this instance
static void bind(Gp32 *g) {
    S3c2400 *s = g->soc;
    s->bus.ram = s->ram;
    s->bus.mmio.context = s;
    s->bus.hooks = (S3c2400Hooks){ .access = bus_access, .copy = bus_copy, .context = g };
    s->cpu.config.bus = &s->bus;
    s->cpu.config.trace = NULL;
    s->cpu.config.predicate = NULL;
    s->cpu.config.access = cpu_access;
    s->cpu.config.context = g;
    s->video = (S3c2400Video){ 0 };
    s->audio = (S3c2400Audio){ .sample = audio_sample, .context = g };
    s->lcd_capture = NULL;
    s->smc.image = g->card;
    if (g->swi) { s->cpu.config.software_interrupt = hle_swi; s->cpu.config.software_interrupt_context = g; }
}

// -- state images -------------------------------------------------------------

static uint32_t gp32_state_bytes(DpMachine *m) { (void)m; return (uint32_t)(HEAD_BYTES + TAIL_BYTES + 8 + NPAGES * 4); }

static void gp32_save(DpMachine *m, uint8_t *buf) {
    Gp32 *g = m->impl;
    memcpy(buf, g->soc, HEAD_BYTES); buf += HEAD_BYTES;
    memcpy(buf, (const uint8_t *)g->soc + TAIL_OFF, TAIL_BYTES); buf += TAIL_BYTES;
    uint32_t card[2] = { g->card_node, 0 };
    memcpy(buf, card, sizeof card); buf += sizeof card;
    for (unsigned w = 0; w < WORDS; w++) {
        for (uint64_t bits = g->dirty_id[w]; bits; bits &= bits - 1) {
            unsigned p = w * 64 + (unsigned)__builtin_ctzll(bits);
            const uint8_t *page = g->soc->ram + (size_t)p * DP_GP32_PAGE;
            g->cur_id[p] = dp_pages_intern(g->src->pages, page, dp_pages_hash(page));
        }
        g->dirty_id[w] = 0;
    }
    memcpy(buf, g->cur_id, sizeof g->cur_id);
}

typedef struct { uint32_t magic, parent, chunk, pad; uint8_t data[CARD_CHUNK]; } CardNode;
#define CARD_MAGIC 0x44524143u

static void card_switch(Gp32 *g, uint32_t node) {
    const Gp32Source *s = g->src;
    if (node == g->card_node || !g->card) return;
    CardNode n;
    for (uint32_t id = g->card_node; id; id = n.parent) {   // back to the pristine card
        memcpy(&n, dp_pages_get(s->pages, id), sizeof n);
        if (n.magic != CARD_MAGIC) break;
        size_t at = (size_t)n.chunk * CARD_CHUNK, len = s->card_size - at < CARD_CHUNK ? s->card_size - at : CARD_CHUNK;
        memcpy(g->card + at, s->card + at, len);
        g->chunk_hash[n.chunk] = 0;
    }
    memset(g->chunk_seen, 0, ((s->card_size / CARD_CHUNK) / 64 + 1) * 8);
    for (uint32_t id = node; id; id = n.parent) {   // newest record of each chunk wins
        memcpy(&n, dp_pages_get(s->pages, id), sizeof n);
        if (n.magic != CARD_MAGIC) break;
        if (g->chunk_seen[n.chunk / 64] >> (n.chunk & 63) & 1) continue;
        g->chunk_seen[n.chunk / 64] |= 1ull << (n.chunk & 63);
        size_t at = (size_t)n.chunk * CARD_CHUNK, len = s->card_size - at < CARD_CHUNK ? s->card_size - at : CARD_CHUNK;
        memcpy(g->card + at, n.data, len);
        uint8_t page[DP_GP32_PAGE] = { 0 }; memcpy(page, n.data, CARD_CHUNK);
        g->chunk_hash[n.chunk] = dp_pages_hash(page) | 1;
    }
    g->card_node = node;
}

// after a program or erase command: record the chunks of the addressed erase block that changed
static void card_record(Gp32 *g) {
    S3c2400 *soc = g->soc;
    const Gp32Source *s = g->src;
    const Gp32Smc *c = &soc->smc;
    soc->smc.dirty = false;
    if (!g->card || !c->block_pages) return;
    size_t first = c->row - c->row % c->block_pages;
    size_t lo = (c->offset + first * c->page_size) / CARD_CHUNK;
    size_t hi = (c->offset + (first + c->block_pages) * c->page_size + CARD_CHUNK - 1) / CARD_CHUNK;
    for (size_t k = lo; k < hi && k * CARD_CHUNK < s->card_size; k++) {
        size_t at = k * CARD_CHUNK, len = s->card_size - at < CARD_CHUNK ? s->card_size - at : CARD_CHUNK;
        if (!g->chunk_hash[k] && !memcmp(g->card + at, s->card + at, len)) continue;
        uint8_t page[DP_GP32_PAGE] = { 0 }; memcpy(page, g->card + at, len);
        uint64_t h = dp_pages_hash(page) | 1;
        if (h == g->chunk_hash[k]) continue;
        CardNode n = { .magic = CARD_MAGIC, .parent = g->card_node, .chunk = (uint32_t)k };
        memcpy(n.data, page, CARD_CHUNK);
        uint8_t record[DP_GP32_PAGE] = { 0 }; memcpy(record, &n, sizeof n);
        g->card_node = dp_pages_intern(s->pages, record, dp_pages_hash(record));
        g->chunk_hash[k] = h;
    }
}

static void gp32_restore(DpMachine *m, const uint8_t *buf) {
    Gp32 *g = m->impl;
    g->epoch++;
    memcpy(g->soc, buf, HEAD_BYTES); buf += HEAD_BYTES;
    memcpy((uint8_t *)g->soc + TAIL_OFF, buf, TAIL_BYTES); buf += TAIL_BYTES;
    uint32_t card[2]; memcpy(card, buf, sizeof card); buf += sizeof card;
    bind(g);   // a state written by another process carries stale pointers
    g->soc->irq_handler = g->src->boot->irq_handler;
    g->soc->bus.mmio.read = g->src->boot->bus.mmio.read;
    g->soc->bus.mmio.write = g->src->boot->bus.mmio.write;
    g->soc->bus.boot_rom = g->src->bios;
    g->soc->bus.absent_boot_rom_id = g->src->boot->bus.absent_boot_rom_id;
    for (unsigned w = 0; w < WORDS; w++) {   // only the pages written since the last save or named differently are copied
        const unsigned base = w * 64;
        if (!g->dirty_id[w] && !memcmp(buf + 4u * base, g->cur_id + base, 64 * 4)) continue;
        for (unsigned p = base; p < base + 64; p++) {
            uint32_t id; memcpy(&id, buf + 4u * p, 4);
            uint64_t bit = 1ull << (p & 63);
            if (!(g->dirty_id[w] & bit) && id == g->cur_id[p]) continue;
            memcpy(g->soc->ram + (size_t)p * DP_GP32_PAGE, dp_pages_get(g->src->pages, id), DP_GP32_PAGE);
            g->cur_id[p] = id;
            g->dirty_hash[w] |= bit;
        }
    }
    memset(g->dirty_id, 0, sizeof g->dirty_id);
    card_switch(g, card[0]);
}

// -- shots --------------------------------------------------------------------
// The portable state of the runtime: it carries the whole SoC and card, so that it restarts in another run of the
// explorer or in a player built from the same application.

static int gp32_shot_write(DpMachine *m, const char *path) {
    Gp32 *g = m->impl;
    const Gp32Host host = { .image = g->card, .image_size = g->src->card_size };
    s3c2400_sync(g->soc);   // the runtime expects no clocks pending between two runs
    return gp32_shot_save(g->soc, &host, g->src->program, path);
}

static int gp32_shot_read(DpMachine *m, const char *path) {
    Gp32 *g = m->impl;
    const Gp32Source *s = g->src;
    Gp32Host host = { .image = g->card, .image_size = s->card_size };
    if (!gp32_shot_load(g->soc, &host, s->program, path)) return 0;
    g->epoch++;
    bind(g);
    g->soc->smc.dirty = false; g->soc->eeprom_dirty = false;
    memset(g->dirty_id, 0xff, sizeof g->dirty_id);   // every page and chunk is new to this process
    memset(g->dirty_hash, 0xff, sizeof g->dirty_hash);
    if (g->card) {
        g->card_node = 0;
        memset(g->chunk_hash, 0, ((s->card_size + CARD_CHUNK - 1) / CARD_CHUNK) * sizeof *g->chunk_hash);
        for (size_t at = 0; at < s->card_size; at += CARD_CHUNK) {
            size_t len = s->card_size - at < CARD_CHUNK ? s->card_size - at : CARD_CHUNK;
            if (!memcmp(g->card + at, s->card + at, len)) continue;
            uint8_t page[DP_GP32_PAGE] = { 0 }; memcpy(page, g->card + at, len);
            CardNode n = { .magic = CARD_MAGIC, .parent = g->card_node, .chunk = (uint32_t)(at / CARD_CHUNK) };
            memcpy(n.data, page, CARD_CHUNK);
            uint8_t record[DP_GP32_PAGE] = { 0 }; memcpy(record, &n, sizeof n);
            g->card_node = dp_pages_intern(s->pages, record, dp_pages_hash(record));
            g->chunk_hash[n.chunk] = dp_pages_hash(page) | 1;
        }
    }
    return 1;
}

static inline uint64_t mix(uint64_t h, uint64_t v) { h = (h ^ v) * 0x9E3779B97F4A7C15ull; return h ^ (h >> 31); }

static uint64_t gp32_ctx_reg_hash(DpMachine *m) {
    const Arm920t *c = &((Gp32 *)m->impl)->soc->cpu;
    uint64_t h = 0x243F6A8885A308D3ull;
    for (unsigned i = 0; i < 15; i++) h = mix(h, c->r[i]);
    return mix(h, c->cpsr | (uint64_t)c->control << 32);
}

static uint64_t gp32_state_hash(DpMachine *m) {
    Gp32 *g = m->impl;
    for (unsigned w = 0; w < WORDS; w++) {
        for (uint64_t bits = g->dirty_hash[w]; bits; bits &= bits - 1) {
            unsigned p = w * 64 + (unsigned)__builtin_ctzll(bits);
            uint64_t h = mix(dp_pages_hash(g->soc->ram + (size_t)p * DP_GP32_PAGE), p + 1);
            g->ram_hash ^= g->page_hash[p] ^ h;
            g->page_hash[p] = h;
        }
        g->dirty_hash[w] = 0;
    }
    const S3c2400 *s = g->soc;
    uint64_t h = mix(gp32_ctx_reg_hash(m), s->cpu.r[15]);
    for (unsigned i = 0; i < 6; i++) h = mix(h, s->irq[i] | (uint64_t)s->cpu.spsr[i] << 32);
    for (unsigned i = 0; i < 21; i++) h = mix(h, s->lcd[i]);
    for (unsigned i = 0; i < 4; i++) h = mix(h, s->dma[i].regs[6] | (uint64_t)s->dma[i].active << 32);
    h = mix(h, s->buttons | (uint64_t)g->card_node << 16);
    return mix(h, g->ram_hash);
}

// -- execution ----------------------------------------------------------------

static uint32_t gp32_pc(DpMachine *m) {
    const Arm920t *c = &((Gp32 *)m->impl)->soc->cpu;
    return c->r[15] | ((c->cpsr & T_BIT) ? DP_ARM_THUMB : 0);
}
static void gp32_set_pc(DpMachine *m, uint32_t pc) {
    Arm920t *c = &((Gp32 *)m->impl)->soc->cpu;
    c->cpsr = (c->cpsr & ~T_BIT) | ((pc & DP_ARM_THUMB) ? T_BIT : 0);
    c->r[15] = pc & ~DP_ARM_THUMB;
    c->waiting = false;
}

static int step_one(Gp32 *g) {
    S3c2400 *s = g->soc;
    for (unsigned wait = 0; wait < 200000u; wait++) {
        if (s->hle_exited || s->hle_unhandled) return 0;
        Arm920tStep r = s3c2400_step_batched(s);
        if (s->smc.dirty) card_record(g);
        if (r != ARM920T_WAITING) return !(s->hle_exited || s->hle_unhandled);
        if (!s->cpu.irq && !s->cpu.fiq_line && s->cycles_per_instruction) {   // asleep until the next peripheral event
            unsigned skip = s->idle_budget / s->cycles_per_instruction;
            s->idle_budget -= skip * s->cycles_per_instruction;
            s->pending_clocks += skip * s->cycles_per_instruction;
            s->steps += skip;
        }
    }
    return 0;   // asleep with no interrupt coming
}

static int gp32_run(DpMachine *m) {
    Gp32 *g = m->impl;
    // `mov lr, pc` and the jump after it are one call for the decoder
    uint32_t pc = gp32_pc(m);
    uint8_t b[8];
    gp32_fetch_n(m, pc, b, sizeof b);
    DpFlowInfo f;
    m->isa->flow(b, pc, &f);
    unsigned half = (pc & DP_ARM_THUMB) ? 2 : 4;
    if (!step_one(g)) return 0;
    if (f.kind == DP_FLOW_CALL && f.length == 2 * half && gp32_pc(m) == pc + half) return step_one(g);
    return 1;
}

// force the negation of an ARM condition by editing the flags
static int gp32_invert_condition(DpMachine *m, int cond) {
    Arm920t *c = &((Gp32 *)m->impl)->soc->cpu;
    const uint32_t N = 1u << 31, Z = 1u << 30, C = 1u << 29, V = 1u << 28;
    uint32_t f = c->cpsr;
    int n = !!(f & N), z = !!(f & Z), cy = !!(f & C), v = !!(f & V);
    switch (cond) {
    case 0: case 1: f ^= Z; break;
    case 2: case 3: f ^= C; break;
    case 4: case 5: case 10: case 11: f ^= N; break;
    case 6: case 7: f ^= V; break;
    case 8: case 9: if (cy && !z) f &= ~C; else f = (f | C) & ~Z; break;
    case 12: case 13: if (!z && n == v) f |= Z; else f = (f & ~(Z | N)) | (v ? N : 0); break;
    default: return 0;
    }
    c->cpsr = f;
    return 1;
}

static int gp32_cut_pc(DpMachine *m, uint32_t pc) {
    const Gp32 *g = m->impl;
    int64_t p = translate(g, pc & ~DP_ARM_THUMB);
    if (p < 0) return 1;
    if ((uint32_t)p - RAM_BASE < GP32_RAM_SIZE) return 0;
    return !(g->src->bios && (uint64_t)p < g->soc->bus.boot_rom_size);
}
static int gp32_cut_sp(DpMachine *m) {
    const Gp32 *g = m->impl;
    uint32_t sp = g->soc->cpu.r[13];
    if (g->soc->cpu.control & 1u) return translate(g, sp - 4) < 0;
    return sp - 4 - RAM_BASE >= GP32_RAM_SIZE;
}

// the HLE interrupt table and timer callbacks: handlers the application registered
static unsigned gp32_vector_seeds(DpMachine *m, uint32_t *out, unsigned max) {
    const Gp32 *g = m->impl;
    if (g->src->bios) return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < 36 && n < max; i++) {
        uint32_t h;
        if (i < 32) memcpy(&h, g->soc->ram + (0x0c7ac000u - RAM_BASE) + 4 * i, 4);
        else h = g->soc->hle_timer_handler[i - 32];
        if (!h || (h & 3u) || h - RAM_BASE >= g->src->app_size) continue;
        unsigned dup = 0;
        for (unsigned j = 0; j < n; j++) dup |= out[j] == h;
        if (!dup) out[n++] = h;
    }
    return n;
}

static uint8_t gp32_data_dest(DpMachine *m, uint32_t a) {
    const Gp32 *g = m->impl;
    if (a >= 0x14a00400u && a < 0x14a00800u) return DP_DEST_CRAM;
    if ((a & ~3u) == 0x15508010u) return DP_DEST_SAMPLE;
    if ((a & ~3u) == 0x1560000cu) return DP_DEST_SRAM;   // the SmartMedia data port
    int64_t p = translate(g, a);
    if (p < 0) return DP_DEST_NONE;
    const uint32_t *lcd = g->soc->lcd;
    if (lcd[0] & 1u) {   // the frame buffer the LCD controller is fetching
        uint32_t base = (lcd[5] & 0xfffffffu) << 1, end = (base & ~0x3fffffu) | ((lcd[6] & 0x1fffffu) << 1);
        if (end <= base) end += 0x400000u;
        if ((uint32_t)p >= base && (uint32_t)p < end) return DP_DEST_VRAM;
    }
    return DP_DEST_NONE;
}

static uint16_t buttons_of(uint32_t mask) {
    static const struct { uint32_t in; uint16_t pad; } map[] = {
        { DP_IN_UP, GP32_UP }, { DP_IN_DOWN, GP32_DOWN }, { DP_IN_LEFT, GP32_LEFT }, { DP_IN_RIGHT, GP32_RIGHT },
        { DP_IN_B1, GP32_A }, { DP_IN_B2, GP32_B }, { DP_IN_START, GP32_START }, { DP_IN_SELECT, GP32_SELECT },
        { DP_IN_C, GP32_L }, { DP_IN_X, GP32_R },
    };
    uint16_t b = 0;
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) if (mask & map[i].in) b |= map[i].pad;
    return b;
}
static void gp32_set_input(DpMachine *m, uint32_t mask) { s3c2400_set_buttons(((Gp32 *)m->impl)->soc, buttons_of(mask)); }

// Random and play sessions: every GP32 key (R included, absent from the default catalogue), with walking and
// attacking forward repeated so that action games advance.  Idle comes first.
static const uint32_t gp32_pads[] = {
    0, DP_IN_B1, DP_IN_START, DP_IN_B2, DP_IN_UP, DP_IN_DOWN, DP_IN_LEFT, DP_IN_RIGHT, DP_IN_SELECT, DP_IN_C, DP_IN_X,
    DP_IN_B1, DP_IN_RIGHT, DP_IN_RIGHT | DP_IN_B1, DP_IN_RIGHT | DP_IN_B2,
};

static void gp32_poke(DpMachine *m, uint32_t a, uint8_t v) {
    Gp32 *g = m->impl;
    int64_t p = translate(g, a);
    if (p < 0) return;
    uint32_t off = (uint32_t)p - RAM_BASE;
    if (off < GP32_RAM_SIZE) { g->soc->ram[off] = v; touch(g, off); }
    else s3c2400_bus_write(&g->soc->bus, S3C2400_STORE, (uint32_t)p, 1, v);
}

static void gp32_observe(DpMachine *m, const DpObservers *o, void *user) { Gp32 *g = m->impl; g->obs = o; g->user = user; }

static uint64_t gp32_frame(DpMachine *m) { return ((Gp32 *)m->impl)->soc->frames; }

static int gp32_get_regs(DpMachine *m, DpRegState *out) {
    const Arm920t *c = &((Gp32 *)m->impl)->soc->cpu;
    dp_regstate_clear(out);
    for (unsigned i = 0; i < DP_ARMR_NREGS && i < DP_REG_N; i++) { out->v[i] = c->r[i]; out->known[i] = 0x0F; }
    return 1;
}

static unsigned gp32_audio_read(DpMachine *m, int16_t *st, unsigned max) {
    Gp32 *g = m->impl;
    if (!g->audio) { g->audio = malloc(AUDIO_FRAMES * 4); return 0; }   // capture starts with the first reader
    unsigned n = 0;
    for (; n < max && g->audio_count; n++, g->audio_count--) {
        memcpy(st + 2 * n, g->audio + 2 * g->audio_head, 4);
        g->audio_head = (g->audio_head + 1) % AUDIO_FRAMES;
    }
    return n;
}

// the panel is a 240 x 320 column buffer: 320 x 240 once turned upright
static int gp32_render(DpMachine *m, uint32_t *argb, unsigned cap, int *w, int *h) {
    const Gp32 *g = m->impl;
    if (cap < 320u * 240u) return 0;
    for (unsigned y = 0; y < 240; y++)
        for (unsigned x = 0; x < 320; x++) argb[y * 320 + x] = g->soc->pixels[x * 240 + 239 - y];
    *w = 320; *h = 240;
    return 1;
}

// -- hooks --------------------------------------------------------------------

// technique 2: an instruction that reads the button ports forks one state per pad held
static int gp32_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    static const uint32_t pads[] = { 0, DP_IN_B1, DP_IN_B2, DP_IN_B1 | DP_IN_B2, DP_IN_UP, DP_IN_DOWN, DP_IN_LEFT,
                                     DP_IN_RIGHT, DP_IN_START, DP_IN_SELECT, DP_IN_C, DP_IN_X };
    unsigned nfull = m->pad_full ? dp_pad_count(m->input_bits) : 0;
    if (k >= (nfull ? nfull : sizeof pads / sizeof *pads)) return 0;
    // only loads can read a port: skip the dry run for everything else
    if (pc & DP_ARM_THUMB) {
        unsigned op = bytes[0] | bytes[1] << 8, top = op >> 11;
        if (!(top == 0x0b || top == 0x0d || top == 0x0f || top == 0x11 || (op >> 9) == 0x2b)) return 0;
    } else {
        uint32_t op = bytes[0] | bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
        int single = (op & 0x0c100000u) == 0x04100000u, half = (op & 0x0e100090u) == 0x00100090u;
        if ((!single && !half) || (op >> 16 & 15u) >= 13u) return 0;
    }
    Gp32 *g = m->impl;
    uint32_t n = gp32_state_bytes(m);
    uint8_t *pre = malloc(n);
    if (!pre) return 0;
    const DpObservers *obs = g->obs;
    g->obs = NULL;   // the dry run is not part of the exploration
    gp32_save(m, pre);
    g->pad_armed = 1; g->pad_seen = 0;
    int progress = gp32_run(m);
    int hit = g->pad_seen;
    g->pad_armed = 0;
    gp32_restore(m, pre);
    if (progress && hit) {
        gp32_set_input(m, nfull ? dp_pad_subset(m->input_bits, k) : pads[k]);
        gp32_run(m);   // the probe instruction runs again, with the pad held
        gp32_save(m, save);
        gp32_restore(m, pre);
    }
    g->obs = obs;
    free(pre);
    return progress && hit;
}

// IRQ forks: one state per unmasked source pending
static int gp32_irq_source_alt(void *user, DpMachine *m, unsigned source, unsigned delay, uint8_t *save) {
    (void)user;
    Gp32 *g = m->impl;
    S3c2400 *s = g->soc;
    if (delay != 1 || source >= 32 || (s->irq[2] >> source & 1u) || (s->cpu.cpsr & 0x80u) || (s->irq[0] >> source & 1u)) return 0;
    uint32_t irq[6];
    const bool line = s->cpu.irq, fast = s->cpu.fiq_line;
    memcpy(irq, s->irq, sizeof irq);
    s3c2400_set_irq(s, source);
    gp32_save(m, save);
    memcpy(s->irq, irq, sizeof irq);   // raising a source touches nothing but the controller and the two CPU lines
    s->cpu.irq = line; s->cpu.fiq_line = fast;
    __atomic_add_fetch(&g->src->irq_forks[source], 1, __ATOMIC_RELAXED);
    return 1;
}

// the unmasked sources not yet pending, while the CPU accepts IRQs, under their cap
static uint64_t gp32_irq_source_ready(void *user, DpMachine *m) {
    (void)user;
    const Gp32 *g = m->impl;
    const S3c2400 *s = g->soc;
    if (s->cpu.cpsr & 0x80u) return 0;
    uint32_t ready = ~(s->irq[2] | s->irq[0]);
    if (g->src->irq_cap)
        for (uint32_t bits = ready; bits; bits &= bits - 1) {
            unsigned source = (unsigned)__builtin_ctz(bits);
            if (__atomic_load_n(&g->src->irq_forks[source], __ATOMIC_RELAXED) >= g->src->irq_cap) ready &= ~(1u << source);
        }
    return ready;
}

static const DpPlatformHooks gp32_hooks = { .event_probe = gp32_event_probe, .irq_source_count = 32, .irq_source_alt = gp32_irq_source_alt,
                                            .irq_source_ready = gp32_irq_source_ready };
const DpPlatformHooks *dp_machine_gp32_hooks(void) { return &gp32_hooks; }
const DpPlatform *dp_machine_gp32_platform(void) { return dp_platform_gp32(); }

// -- life cycle ---------------------------------------------------------------

static void source_release(Gp32Source *s) {
    if (!s || __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL)) return;
    dp_pages_close(s->pages);
    free(s->boot); free(s->bios); free(s->card); free(s->payload); free(s->image);
    free(s);
}

static void gp32_close(DpMachine *m) {
    Gp32 *g = m->impl;
    if (g) {
        source_release(g->src);
        free(g->soc); free(g->card); free(g->chunk_hash); free(g->chunk_seen); free(g->audio); free(g);
    }
    free(m);
}

// back to the pristine machine
static void restart(Gp32 *g) {
    Gp32Source *s = g->src;
    memcpy(g->soc, s->boot, sizeof *g->soc);
    g->epoch++;
    if (g->card) memcpy(g->card, s->card, s->card_size);
    if (g->chunk_hash) memset(g->chunk_hash, 0, (s->card_size / CARD_CHUNK + 1) * 8);
    g->card_node = 0;
    bind(g);
    memset(g->dirty_id, 0xff, sizeof g->dirty_id);
    memset(g->dirty_hash, 0xff, sizeof g->dirty_hash);
    memset(g->page_hash, 0, sizeof g->page_hash);
    g->ram_hash = 0;
}

static DpMachine *instance(Gp32Source *src) {
    DpMachine *m = calloc(1, sizeof *m);
    Gp32 *g = calloc(1, sizeof *g);
    if (!m || !g) { free(m); free(g); return NULL; }
    m->impl = g;
    g->src = src;
    g->epoch = 1;
    __atomic_add_fetch(&src->refs, 1, __ATOMIC_ACQ_REL);
    g->soc = malloc(sizeof *g->soc);
    if (src->card) {
        g->card = malloc(src->card_size);
        g->chunk_hash = malloc((src->card_size / CARD_CHUNK + 1) * 8);
        g->chunk_seen = malloc((src->card_size / CARD_CHUNK / 64 + 1) * 8);
    }
    if (!g->soc || (src->card && (!g->card || !g->chunk_hash || !g->chunk_seen))) { gp32_close(m); return NULL; }
    if (src->boot->cpu.config.software_interrupt) {
        g->swi = src->boot->cpu.config.software_interrupt;
        g->swi_context = g->soc;
    }
    restart(g);
    m->name = "gp32";
    m->plat = dp_platform_gp32();
    m->isa = dp_isa_arm920t();
    m->work_ram_base = RAM_BASE; m->work_ram_len = GP32_RAM_SIZE;
    m->ram_index = gp32_ram_index; m->ram_addr = gp32_ram_addr; m->ram_size = GP32_RAM_SIZE;
    m->ram_view = gp32_ram_view; m->ram_view_len = GP32_RAM_SIZE;
    m->input_bits = DP_IN_UP | DP_IN_DOWN | DP_IN_LEFT | DP_IN_RIGHT | DP_IN_B1 | DP_IN_B2 | DP_IN_START | DP_IN_SELECT | DP_IN_C | DP_IN_X;
    m->state_bytes = gp32_state_bytes;
    m->save = gp32_save;
    m->load = gp32_restore;
    m->shot_save = gp32_shot_write;
    m->shot_load = gp32_shot_read;
    m->fetch = gp32_fetch;
    m->fetch_n = gp32_fetch_n;
    m->vector_seeds = gp32_vector_seeds;
    m->poke = gp32_poke;
    m->rom_index = gp32_rom_index;
    m->linear = 1;
    m->code_base = RAM_BASE;
    m->run = gp32_run;
    m->pc = gp32_pc;
    m->set_pc = gp32_set_pc;
    m->invert_condition = gp32_invert_condition;
    m->cut_pc = gp32_cut_pc;
    m->cut_sp = gp32_cut_sp;
    m->data_dest = gp32_data_dest;
    m->state_hash = gp32_state_hash;
    m->ctx_reg_hash = gp32_ctx_reg_hash;
    m->frame = gp32_frame;
    m->set_input = gp32_set_input;
    m->play_pads = gp32_pads; m->n_play_pads = sizeof gp32_pads / sizeof *gp32_pads;
    m->observe = gp32_observe;
    m->get_regs = gp32_get_regs;
    m->audio_read = gp32_audio_read;
    m->render = gp32_render;
    m->close = gp32_close;
    return m;
}

static DpMachine *gp32_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    DpMachine *c = instance(((Gp32 *)m->impl)->src);
    if (c) c->clone = gp32_clone;
    return c;
}

static uint8_t *read_file(const char *path, size_t limit, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *d = NULL;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (n > 0 && (size_t)n <= limit && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)n)) && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *size = (size_t)n;
    return d;
}

#define UNPACK_FOOTPRINT 1024u       // distinct instructions below which the application is still a stub
#define UNPACK_LIMIT 400000000u

// run the boot for `frames` frames and decide what the kernel's image is
static int probe(DpMachine *m, const DpGp32Options *o) {
    Gp32 *g = m->impl;
    Gp32Source *s = g->src;
    S3c2400 *soc = g->soc;
    uint32_t hi = 0;
    uint64_t total = 0, outside = 0;
    uint8_t *seen = NULL;                   // executed halfwords
    uint8_t *ran_as = NULL, *late = NULL;   // an unpacker's instructions as they ran; what still runs after it
    unsigned frames = o->probe_frames ? o->probe_frames : (s->bios ? 600 : 120);
    if (o->image != DP_GP32_IMAGE_PAYLOAD || s->bios) {
        uint64_t limit = (uint64_t)frames * 2000000u;
        // A self-unpacking application spends the whole window in a small stub:
        // while the executed footprint stays that small, keep going, then judge
        // a fresh window of the unpacked code.
        seen = s->bios || o->probe_frames ? NULL : calloc(GP32_APP_LIMIT / 16 + 1, 1);   // one bit per halfword
        if (seen) { ran_as = malloc(GP32_APP_LIMIT); late = calloc(GP32_APP_LIMIT / 16 + 1, 1); }
        if (!ran_as || !late) { free(seen); seen = NULL; }
        unsigned footprint = 0, end = frames;
        int unpacking = 0;
        for (uint64_t ran = 0;;) {
            if (soc->frames >= end || ran >= limit) {
                if (seen && footprint < UNPACK_FOOTPRINT && ran < UNPACK_LIMIT) { unpacking = 1; end += frames; limit = UNPACK_LIMIT; }
                else break;
            }
            // a few presses get past title screens that hold the unpacking back
            if (!(ran & 0xfffff)) s3c2400_set_buttons(soc, soc->frames % 64 < 8 && soc->frames > 60 ? GP32_START : 0);
            int64_t p = translate(g, soc->cpu.r[15]);
            // the top of SDRAM belongs to the HLE firmware: its stubs are not the application
            if (p >= 0 && (uint32_t)p - RAM_BASE < (s->bios ? GP32_RAM_SIZE - 4 : GP32_APP_LIMIT)) {
                uint32_t off = (uint32_t)p - RAM_BASE;
                total++; ran++;
                unsigned n = (soc->cpu.cpsr & T_BIT) ? 2 : 4;
                if (off + n > s->payload_size || memcmp(soc->ram + off, s->payload + off, n)) outside++;
                if (off + n > hi) hi = off + n;
                if (seen && s->unpack_steps) {
                    late[off >> 4] |= 1u << (off >> 1 & 7);
                    if (!(soc->cpu.cpsr & T_BIT)) late[(off + 2) >> 4] |= 1u << ((off + 2) >> 1 & 7);
                }
                if (seen && !(seen[off >> 4] & 1u << (off >> 1 & 7))) {
                    seen[off >> 4] |= 1u << (off >> 1 & 7);
                    if (off + n <= GP32_APP_LIMIT) memcpy(ran_as + off, soc->ram + off, n);
                    if (n == 4) seen[(off + 2) >> 4] |= 1u << ((off + 2) >> 1 & 7);
                    if (++footprint == UNPACK_FOOTPRINT && unpacking) {
                        unpacking = 0; s->unpack_steps = ran;
                        total = outside = 0;
                        end = soc->frames + frames; limit = ran + (uint64_t)frames * 2000000u;
                    }
                }
            }
            if (!step_one(g)) break;
        }
    }
    s->probe_steps = total; s->probe_outside = outside;
    s->snapshot = o->image == DP_GP32_IMAGE_SNAPSHOT || (o->image == DP_GP32_IMAGE_AUTO && (s->bios || outside * 20 > total));
    size_t app = s->payload_size;
    if (s->snapshot) {
        size_t top = ((size_t)hi + 4095) & ~(size_t)4095;
        if (top > app) app = top;
        if (!app) fprintf(stderr, "gp32: the probe run executed nothing from SDRAM%s\n",
                          s->bios ? " (the firmware did not start the application: pad input, --card or a longer --probe-frames may be needed)" : "");
    }
    s->app_size = app;
    s->image_size = app + (s->bios ? GP32_BIOS_SIZE : 0);
    s->image = s->image_size ? calloc(s->image_size, 1) : NULL;
    if (s->image) {
        if (app) memcpy(s->image, s->snapshot ? soc->ram : s->payload, app);
        // an unpacker is usually overwritten by what it unpacks: the image keeps
        // the bytes it ran wherever nothing ran there afterwards
        if (s->unpack_steps && s->snapshot)
            for (size_t off = 0; off + 2 <= app && off + 2 <= GP32_APP_LIMIT; off += 2)
                if ((seen[off >> 4] & 1u << (off >> 1 & 7)) && !(late[off >> 4] & 1u << (off >> 1 & 7)))
                    memcpy(s->image + off, ran_as + off, 2);
        if (s->bios) memcpy(s->image + app, s->bios, GP32_BIOS_SIZE);
    }
    free(seen); free(late); free(ran_as);
    if (!s->image) return 0;
    restart(g);
    return 1;
}

DpMachine *dp_machine_gp32(const DpGp32Options *o) {
    Gp32Source *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->refs = 1;
    s->irq_cap = o->irq_cap;
    DpMachine *m = NULL;
    uint8_t *file = NULL, *bios = NULL;
    size_t file_size = 0;
    Gp32Host host = { .exec_path = o->exec_path, .card_path = o->card_path, .readonly = true, .hle = !o->bios_path };
    S3c2400 *soc = s->boot = malloc(sizeof *soc);
    if (!soc) goto fail;
    file = read_file(o->exec_path, 64u << 20, &file_size);
    if (!file) { fprintf(stderr, "gp32: cannot read %s\n", o->exec_path); goto fail; }
    // a raw firmware dump given as the application boots from its own reset vector, alone or with a card
    int firmware = 0;
    if (file_size == GP32_BIOS_SIZE && file[3] == 0xea) {
        S3c2400 *scratch = calloc(1, sizeof *scratch);
        size_t loaded = 0;
        if (!scratch) goto fail;
        firmware = gp32_exec_load_ram(scratch, file, file_size, GP32_APP_LIMIT, &loaded) != NULL;
        free(scratch);
    }
    if (firmware) {
        host.exec_path = NULL; host.hle = false;
        bios = file; file = NULL;
    } else if (o->bios_path) {
        size_t n = 0;
        bios = read_file(o->bios_path, GP32_BIOS_SIZE, &n);
        if (!bios || n != GP32_BIOS_SIZE) { fprintf(stderr, "gp32: %s: expected a %u-byte BIOS\n", o->bios_path, GP32_BIOS_SIZE); goto fail; }
    }
    if (!gp32_host_boot(&host, soc, &bios, (Arm920tConfig){ 0 }, (S3c2400Hooks){ 0 }, (S3c2400Video){ 0 })) goto fail;
    s->bios = bios; bios = NULL;
    s->bios_size = s->bios ? soc->bus.boot_rom_size : 0;
    if (host.image) {
        s->card = malloc(host.image_size);
        if (!s->card) goto fail_host;
        memcpy(s->card, host.image, host.image_size);
        s->card_size = host.image_size;
        soc->smc.writable = !o->card_readonly;
    }
    soc->smc.dirty = false; soc->eeprom_dirty = false;
    soc->cycles_per_instruction = o->cpi ? o->cpi : 16u;   // fewer instructions spent waiting for the next frame
    gp32_host_close(&host, NULL);
    soc->smc.image = NULL;
    // the executable as the loader lays it out
    if (firmware) { s->payload = calloc(1, 1); if (!s->payload) goto fail; }
    else {
        S3c2400 *scratch = calloc(1, sizeof *scratch);
        size_t loaded = 0;
        const char *error = scratch ? gp32_exec_load_ram(scratch, file, file_size, GP32_APP_LIMIT, &loaded) : "out of memory";
        if (!error) {
            s->payload = malloc(loaded ? loaded : 1);
            if (s->payload) { memcpy(s->payload, scratch->ram, loaded); s->payload_size = loaded; }
            if (s->payload && loaded >= 28) {
                uint32_t h[7]; memcpy(h, s->payload, sizeof h);
                uint32_t ro = h[2] - RAM_BASE, rw = h[3] - RAM_BASE, end = h[4] - RAM_BASE;
                if (rw >= loaded && rw < GP32_RAM_SIZE && end >= rw && ro <= loaded) {
                    s->alias_start = rw; s->alias_to = ro;
                    s->alias_len = end - rw < loaded - ro ? end - rw : (uint32_t)(loaded - ro);
                }
            }
        }
        free(scratch);
        if (error || !s->payload) { fprintf(stderr, "gp32: %s: %s\n", o->exec_path, error ? error : "out of memory"); goto fail; }
    }
    free(file); file = NULL;
    s->program = gp32_shot_program(o->exec_path);
    {
        char path[1100];
        const char *pages = NULL;
        if (o->state_dir) {
            mkdir(o->state_dir, 0777);
            if (snprintf(path, sizeof path, "%s/gp32pages.bin", o->state_dir) >= (int)sizeof path) goto fail;
            pages = path;
        }
        s->pages = dp_pages_open(pages);
        if (!s->pages) goto fail;
    }
    m = instance(s);
    source_release(s);   // the instance holds the reference now
    if (!m) return NULL;
    m->clone = gp32_clone;
    if (!probe(m, o)) { gp32_close(m); return NULL; }
    return m;
fail_host:
    gp32_host_close(&host, NULL);
fail:
    free(file); free(bios);
    source_release(s);
    return NULL;
}

const uint8_t *dp_gp32_image(DpMachine *m, size_t *size) {
    const Gp32Source *s = ((Gp32 *)m->impl)->src;
    *size = s->image_size;
    return s->image;
}

int64_t dp_gp32_bios_offset(DpMachine *m) {
    const Gp32Source *s = ((Gp32 *)m->impl)->src;
    return s->bios ? (int64_t)s->app_size : -1;
}

// Static register references: a linear sweep of the image, in ARM then in Thumb,
// following the constants loaded in the low registers until the next return or
// unconditional branch.  An address counts when a literal holds it or a load or
// store reaches it from a known base.  Candidates only: nothing says the code runs.
static uint32_t word_at(const uint8_t *image, size_t size, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= size) memcpy(&v, image + at, 4);
    return v;
}

static void note_ref(const DpPlatform *p, uint8_t *refs, uint32_t address) {
    int i = p->find(address);
    if (i >= 0) refs[i] = 1;
}

static uint32_t rotated_imm(uint32_t op) {
    unsigned r = (op >> 7) & 30u;
    uint32_t v = op & 255u;
    return r ? v >> r | v << (32 - r) : v;
}

static void arm_refs(const DpPlatform *p, uint8_t *refs, const uint8_t *image, size_t size) {
    uint32_t value[16];
    unsigned known = 0;
    for (size_t at = 0; at + 4 <= size; at += 4) {
        uint32_t op = word_at(image, size, at);
        if (op >> 28 == 15u) { known = 0; continue; }
        unsigned rn = (op >> 16) & 15u, rd = (op >> 12) & 15u;
        if ((op & 0x0f7f0000u) == 0x051f0000u) {   // ldr rd, [pc, #imm]
            size_t literal = op & 0x00800000u ? at + 8 + (op & 0xfffu) : at + 8 - (op & 0xfffu);
            if (literal + 4 <= size) {
                value[rd] = word_at(image, size, literal);
                known |= 1u << rd;
                note_ref(p, refs, value[rd]);
            } else known &= ~(1u << rd);
        } else if ((op & 0x0e000000u) == 0x04000000u) {   // ldr, str with an immediate offset
            if (known & 1u << rn) {
                uint32_t offset = op & 0xfffu, base = value[rn];
                uint32_t moved = op & 0x00800000u ? base + offset : base - offset;
                int pre = (op >> 24) & 1u;
                note_ref(p, refs, pre ? moved : base);
                if (!pre || (op & 0x00200000u)) value[rn] = moved;
            }
            if (op & 0x00100000u) known &= ~(1u << rd);
        } else if ((op & 0x0e400090u) == 0x00400090u && (op & 0x60u)) {   // halfword, signed byte, immediate
            if ((known & 1u << rn) && (op & 0x01000000u)) {
                uint32_t offset = (op >> 4 & 0xf0u) | (op & 15u);
                note_ref(p, refs, op & 0x00800000u ? value[rn] + offset : value[rn] - offset);
            }
            if (op & 0x00100000u) known &= ~(1u << rd);
        } else if ((op & 0x0e000000u) == 0x02000000u) {   // data processing, immediate
            unsigned code = (op >> 21) & 15u;
            uint32_t imm = rotated_imm(op);
            if (code == 13u) { value[rd] = imm; known |= 1u << rd; }
            else if ((code == 4u || code == 2u || code == 12u) && (known & 1u << rn)) {
                value[rd] = code == 4u ? value[rn] + imm : code == 2u ? value[rn] - imm : value[rn] | imm;
                known |= 1u << rd;
            } else if (code < 8u || code > 11u) known &= ~(1u << rd);
        } else if ((op & 0x0c000000u) == 0) {
            known &= ~(1u << rd);
        } else if ((op & 0x0e000000u) == 0x08000000u) {   // block transfer
            if (op & 0x00100000u) known &= ~(op & 0xffffu);
            if (op & 0x00200000u) known &= ~(1u << rn);
            if ((op & 0xf0108000u) == 0xe0108000u) known = 0;
        } else if ((op & 0x0f000000u) == 0x0b000000u) {
            known &= ~0x500fu;   // a call clobbers r0-r3, ip, lr
        } else if ((op & 0xff000000u) == 0xea000000u) {
            known = 0;
        }
        if (op == 0xe12fff1eu || op == 0xe1a0f00eu) known = 0;
    }
}

static void thumb_refs(const DpPlatform *p, uint8_t *refs, const uint8_t *image, size_t size) {
    uint32_t value[8];
    unsigned known = 0;
    for (size_t at = 0; at + 2 <= size; at += 2) {
        uint16_t op = (uint16_t)(image[at] | image[at + 1] << 8);
        unsigned low = op & 7u, base = (op >> 3) & 7u, high = (op >> 8) & 7u, imm5 = (op >> 6) & 31u;
        if ((op & 0xf800u) == 0x4800u) {
            size_t literal = ((at + 4) & ~(size_t)3) + (op & 255u) * 4u;
            if (literal + 4 <= size) {
                value[high] = word_at(image, size, literal);
                known |= 1u << high;
                note_ref(p, refs, value[high]);
            } else known &= ~(1u << high);
        } else if (op >= 0x6000u && op < 0x9000u) {
            unsigned scale = op < 0x7000u ? 4u : op < 0x8000u ? 1u : 2u;
            if (known & 1u << base) note_ref(p, refs, value[base] + imm5 * scale);
            if (op & 0x0800u) known &= ~(1u << low);
        } else if ((op & 0xf800u) == 0x3000u) {
            value[high] += op & 255u;
        } else if ((op & 0xf800u) == 0x2000u) {
            value[high] = op & 255u;
            known |= 1u << high;
        } else if ((op & 0xff00u) == 0x4700u || (op & 0xff00u) == 0xbd00u || (op & 0xf800u) == 0xe000u) {
            known = 0;
        } else if ((op & 0xf800u) == 0xf800u) {
            known &= ~15u;
        } else if (op < 0x6000u) {
            known &= ~(1u << (op >= 0x2000u && op < 0x4000u ? high : low));
        }
    }
}

void dp_gp32_report_refs(DpMachine *m, FILE *out, const uint64_t *reads, const uint64_t *writes) {
    const Gp32Source *s = ((Gp32 *)m->impl)->src;
    const DpPlatform *p = dp_platform_gp32();
    uint8_t *refs = calloc((size_t)p->io_reg_count, 1);
    if (!refs || !s->image) { free(refs); return; }
    arm_refs(p, refs, s->image, s->image_size);
    thumb_refs(p, refs, s->image, s->image_size);
    int referenced = 0, only = 0, either = 0;
    for (int i = 0; i < p->io_reg_count; i++) {
        int touched = reads[i] || writes[i];
        referenced += refs[i];
        only += refs[i] && !touched;
        either += refs[i] || touched;
    }
    fprintf(out, "gp32: static register references: %d of %d entries referenced by the image, %d of them never touched at run time; %d covered by one or the other\n",
            referenced, p->io_reg_count, only, either);
    for (int pass = 0; pass < 2; pass++) {
        int n = 0;
        for (int i = 0; i < p->io_reg_count; i++) {
            if (reads[i] || writes[i] || refs[i] == (pass != 0)) continue;
            if (!n++) fprintf(out, "gp32: %s:", pass ? "neither touched nor referenced" : "referenced, never touched");
            fprintf(out, " %s", p->io_regs[i].name);
        }
        if (n) fputc('\n', out);
    }
    free(refs);
}

void dp_gp32_report(DpMachine *m, FILE *out) {
    const Gp32Source *s = ((Gp32 *)m->impl)->src;
    fprintf(out, "gp32: %s firmware, image %zu bytes (%s", s->bios ? "BIOS" : "HLE", s->image_size,
            s->snapshot ? "SDRAM snapshot after the probe run" : "loaded executable");
    if (s->bios) fprintf(out, ", the BIOS from offset 0x%zx", s->app_size);
    fprintf(out, "), probe: %llu instructions, %llu outside the loaded executable\n",
            (unsigned long long)s->probe_steps, (unsigned long long)s->probe_outside);
    if (s->unpack_steps) fprintf(out, "gp32: self-unpacking application, the probe ran %llu instructions to get past the unpacker\n", (unsigned long long)s->unpack_steps);
    fprintf(out, "gp32: %u distinct pages stored (%u KiB)\n", dp_pages_count(s->pages), dp_pages_count(s->pages) * (DP_GP32_PAGE / 1024));
    unsigned forks = 0, capped = 0;
    for (unsigned i = 0; i < 32; i++) { forks += s->irq_forks[i]; capped += s->irq_cap && s->irq_forks[i] >= s->irq_cap; }
    if (forks) fprintf(out, "gp32: %u interrupt forks, %u sources stopped at the cap of %u\n", forks, capped, s->irq_cap);
    unsigned n = s->n_unhandled < 64 ? s->n_unhandled : 64;
    if (n) {
        fprintf(out, "gp32: %u unhandled HLE calls%s:", n, s->n_unhandled > 64 ? " or more" : "");
        for (unsigned i = 0; i < n; i++) fprintf(out, " swi %llx @%08llx", (unsigned long long)(s->unhandled[i] >> 32), (unsigned long long)(s->unhandled[i] & 0xffffffffu));
        fputc('\n', out);
    }
}
