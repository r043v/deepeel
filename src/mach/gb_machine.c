// gb_machine.c: the m0boy runtime as a DpMachine (spec §7.2).  The explorer
// still drives DpMach directly; this wrapper is the kernel-facing view, built
// on the same instance so both see the same state.
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "../core/desync.h"
#include "m0boy.h"
#include "../explore/core.h"
#include "../explore/tas.h"
#include "../explore/gambatte_init.h"
#include <stddef.h>

static void gb_tas_tune(DpMach *mach, const DpTas *tas);


// The kernel-facing view of one m0boy instance.  Each wrapper owns its observer and its step
// counter, so several instances (parallel workers, one per compiled slot) run side by side.
typedef struct GbWrap {
    DpMach *d;
    DpMachine *machine;
    DpObservers o;
    void *user;
    unsigned seen;            // instructions started in the current gb_run
    int joy_armed, joy_seen;  // joypad-port read detection for the event probe
    int probing;              // suppress observations from a speculative dry run
    const uint8_t *rom; size_t rom_size;   // for clone()
    const DpTas *tas;         // movie whose recorder sync profile every instance takes
    int last_ei;              // the last executed instruction was EI: the IRQ check skips the next one
    uint8_t first_op;         // opcode of the instruction executed by the current gb_run
    uint32_t *ram_sources;    // CGB physical RAM provenance, part of each snapshot
    uint32_t ram_indices;     // includes physically mapped cartridge RAM
} GbWrap;
#define GB_CGB_RAM_INDICES 0x1C000u
static GbWrap *gbw(DpMachine *m) { return m->impl; }
// The PPU renders lines only while the watch is open.
static void gb_watch(DpMachine *m, int on) {
    DpMach *d = gbw(m)->d;
    d->render = on;
    d->api->skip_render[0] = !on;
    if (on) d->api->apu_set_rate(44100);
}

static unsigned gb_audio_read(DpMachine *m, int16_t *stereo, unsigned cap) {
    const DpMach *d = gbw(m)->d;
    if (!d->render) return 0;
    uint8_t mono[44100 / 60];
    unsigned count = cap < sizeof mono ? cap : sizeof mono;
    d->api->apu_render(mono, count);
    for (unsigned i = 0; i < count; ++i) {
        int16_t sample = (int16_t)(((int)mono[i] - 128) * 256);
        stereo[2 * i] = stereo[2 * i + 1] = sample;
    }
    return count;
}
static int gb_render(DpMachine *m, uint32_t *argb, unsigned cap, int *w, int *h) {
    const GbWrap *gb = gbw(m);
    const DpMach *d = gb->d;
    if (cap < 160u * 144u || !d->render) return 0;
    const int colour = DP_GBL_COLOR_BUILD && gb->rom_size > 0x143 && (gb->rom[0x143] & 0x80);
    for (int y = 0; y < 144; y++) {
        for (int x = 0; x < 160; x++) {
            uint32_t r, g, b;
            if (colour) {
                uint16_t pixel = d->screen16[y][x];
                r = (pixel >> 11) * 255u / 31u;
                g = ((pixel >> 5) & 63u) * 255u / 63u;
                b = (pixel & 31u) * 255u / 31u;
            } else {
                r = g = b = 255u - d->screen[y][x] * 85u;
            }
            argb[y * 160 + x] = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
    *w = 160; *h = 144; return 1;
}

// the frame counter rides at the end of the saved state so a restored fork resumes at its own frame
static uint32_t gb_state_bytes(DpMachine *m) { return dp_mach_state_bytes(gbw(m)->d) + 8 + gbw(m)->ram_indices * sizeof(uint32_t); }
// B5: an exact-state candidate must include time. Timer, PPU, DMA, serial,
// RTC and pending-input deadlines are interpreted relative to the CPU clock;
// equal saved deadlines at different cycles are not equivalent states.
static uint64_t gb_state_hash(DpMachine *m) {
    uint32_t mb = dp_mach_state_bytes(gbw(m)->d);
    uint8_t *img = malloc(mb ? mb : 1);
    if (!img) return 0;
    dp_mach_save(gbw(m)->d, img);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < mb; i++) h = (h ^ img[i]) * 0x100000001b3ull;
    h = (h ^ (uint8_t)gbw(m)->last_ei) * 0x100000001b3ull;
    if (gbw(m)->d->cgb_bios_mapped) h = (h ^ 1u) * 0x100000001b3ull;
    uint64_t frame = *gbw(m)->d->api->frame;
    for (unsigned i = 0; i < sizeof frame; ++i) {
        h = (h ^ (uint8_t)frame) * 0x100000001b3ull;
        frame >>= 8;
    }
    if (gbw(m)->ram_sources) {
        const uint8_t *p = (const uint8_t *)gbw(m)->ram_sources;
        for (size_t i = 0; i < gbw(m)->ram_indices * sizeof(uint32_t); i++) h = (h ^ p[i]) * 0x100000001b3ull;
    }
    free(img);
    return h;
}
static void gb_save(DpMachine *m, uint8_t *buf) {
    DpMach *d = gbw(m)->d;
    dp_mach_save(d, buf);
    uint64_t f = *d->api->frame | ((uint64_t)gbw(m)->last_ei << 63)
                               | ((uint64_t)d->cgb_bios_mapped << 62);
    memcpy(buf + dp_mach_state_bytes(d), &f, 8);
    if (gbw(m)->ram_sources) memcpy(buf + dp_mach_state_bytes(d) + 8, gbw(m)->ram_sources, gbw(m)->ram_indices * sizeof(uint32_t));
}
static void gb_load(DpMachine *m, const uint8_t *buf) {
    DpMach *d = gbw(m)->d;
    dp_mach_load(d, buf);
    uint64_t f; memcpy(&f, buf + dp_mach_state_bytes(d), 8);
    gbw(m)->last_ei = (int)(f >> 63);
    dp_mach_map_cgb_bios(d, (int)((f >> 62) & 1));
    f &= (1ull << 62) - 1;
    *d->api->frame = (typeof(*d->api->frame))f;
    if (gbw(m)->ram_sources) memcpy(gbw(m)->ram_sources, buf + dp_mach_state_bytes(d) + 8, gbw(m)->ram_indices * sizeof(uint32_t));
}
static uint32_t gb_ram_source(DpMachine *m, uint32_t i) {
    return i < gbw(m)->ram_indices ? gbw(m)->ram_sources[i] : 0;
}
static void gb_ram_source_set(DpMachine *m, uint32_t i, uint32_t source) {
    // MBC2 stores four bits and reads the upper nibble as ones. Only retain
    // an origin when the ROM byte survives that transformation unchanged.
    if (source && i >= GB_CGB_RAM_INDICES &&
        dp_gbl_variant_mbc[gbw(m)->d->api->variant] == 2 &&
        (source > gbw(m)->rom_size || (gbw(m)->rom[source - 1] & 0xf0u) != 0xf0u))
        source = 0;
    if (i < gbw(m)->ram_indices) gbw(m)->ram_sources[i] = source;
}
// Side-effect-free view through the current memory mapping, including CGB WRAM banks.
static uint8_t gb_fetch(DpMachine *m, uint32_t a) {
    DpMach *d = gbw(m)->d;
    // Synthetic addresses expose all physical CGB WRAM banks to the core's
    // dense RAM indices without changing the CPU-visible 16-bit bus.
    if (a >= 0x10000u && a < 0x18000u) return d->api->wram[a - 0x10000u];
    if (a >= 0x18000u && a < 0x1C000u) return d->api->vram[a - 0x18000u];
    if (a >= GB_CGB_RAM_INDICES && a < gbw(m)->ram_indices) {
        uint8_t v = d->api->eram[a - GB_CGB_RAM_INDICES];
        return dp_gbl_variant_mbc[d->api->variant] == 2 ? v | 0xf0u : v;
    }
    if (a >= 0x8000 && a < 0xA000) {
        unsigned bank = DP_GBL_COLOR_BUILD && gbw(m)->rom_size > 0x143 &&
                        (gbw(m)->rom[0x143] & 0x80) ? d->api->rd(0xFF4F) & 1u : 0;
        return d->api->vram[(a - 0x8000) + bank * 0x2000u];
    }
    if (a >= 0xFE00 && a < 0xFEA0) return d->api->oam[a - 0xFE00];
    if (a >= 0xFF80 && a < 0xFFFF && d->api->hram) return d->api->hram[a - 0xFF80];
    return dp_mach_op(d, (uint16_t)a);
}
// The instruction window must see the CPU bus, including OAM-DMA conflicts
// and PPU access restrictions. Keep fetch() as the side-effect-free physical
// inspection view used by provenance and static analysis. Avoid speculative
// reads of I/O registers beyond the current instruction.
static void gb_fetch_instruction(DpMachine *m, uint32_t a, uint8_t *out, unsigned n) {
    const DpGblApi *api = gbw(m)->d->api;
    for (unsigned i = 0; i < n; ++i) {
        uint16_t addr = (uint16_t)(a + i);
        out[i] = addr >= 0xFF00u && addr < 0xFF80u
                     ? gb_fetch(m, addr) : api->rd(addr);
    }
}
static int64_t gb_cgb_ram_index(DpMachine *m, uint32_t a) {
    if (a >= 0xA000u && a < 0xC000u) {
        const DpGblApi *api = gbw(m)->d->api;
        int32_t offset = api->eram_index((uint16_t)a);
        return offset >= 0 ? GB_CGB_RAM_INDICES + (int64_t)offset : -1;
    }
    if (a >= 0x8000u && a < 0xA000u) {
        unsigned bank = gbw(m)->d->api->rd(0xFF4F) & 1u;
        return 0x18000u + bank * 0x2000u + (a - 0x8000u);
    }
    if (a >= 0xE000u && a < 0xFE00u) a -= 0x2000u;
    if (a >= 0xD000u && a < 0xE000u) {
        unsigned bank = gbw(m)->d->api->rd(0xFF70) & 7u;
        if (!bank) bank = 1;
        return 0x10000u + bank * 0x1000u + (a - 0xD000u);
    }
    return a < 0x10000u ? (int64_t)a : -1;
}
static int64_t gb_dmg_ram_index(DpMachine *m, uint32_t a) {
    (void)m;
    if (a >= 0xE000u && a < 0xFE00u) a -= 0x2000u;
    return a < 0x10000u ? (int64_t)a : -1;
}
static uint32_t gb_cgb_ram_addr(DpMachine *m, uint32_t i) { (void)m; return i; }
static uint32_t dsy_crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void gb_dump_dsy(DpMachine *m, FILE *file, uint32_t frame) {
    const DpGblApi *api = gbw(m)->d->api;
    const gbl_cpu_t *cpu = api->gbr;
    const unsigned variant = (unsigned)api->variant;
    const uint8_t flags = (cpu->zr == 0 ? 0x80 : 0) | (cpu->nf ? 0x40 : 0) |
                          (cpu->hf ? 0x20 : 0) | (cpu->cf ? 0x10 : 0);
    // CPU-visible WRAM uses the mapper's active bank. Export physical banks separately.
    uint8_t wram[0x2000];
    memcpy(wram, api->wram, 0x1000);
    memcpy(wram + 0x1000, api->rmap[0xD] + 0xD000u, 0x1000);
    const uint8_t regs[] = {
        cpu->pc, cpu->pc >> 8, cpu->sp, cpu->sp >> 8, cpu->a, flags,
        cpu->bc, cpu->bc >> 8, cpu->de, cpu->de >> 8, cpu->hl, cpu->hl >> 8
    };
    // DEEPEEL_DSY_DIGEST=1: long-run scans keep regs/hram/ie and a CRC-32 of each large block (tools/gb_sync_check.py --digest)
    static int digest = -1;
    if (digest < 0) { const char *e = getenv("DEEPEEL_DSY_DIGEST"); digest = e && *e && *e != '0'; }
    if (digest) {
        enum { MAXD = 8 + 2 + 1 + 1 };   // 8 WRAM banks, 2 VRAM banks, OAM, SRAM
        uint32_t crc[MAXD]; char nm[MAXD][16]; const char *dn[3 + MAXD] = {"regs", "hram", "ie"};
        const uint8_t *dd[3 + MAXD] = {regs, api->hram, api->ie};
        uint32_t dl[3 + MAXD] = {sizeof regs, 0x7F, 1};
        unsigned n = 3, k = 0, wb = DP_GBL_COLOR_BUILD ? 8 : 2, vb = DP_GBL_COLOR_BUILD ? 2 : 1;
        for (unsigned i = 0; i < wb; i++, k++) { crc[k] = dsy_crc32(api->wram + i * 0x1000, 0x1000); snprintf(nm[k], 16, "wram%u_crc", i); }
        for (unsigned i = 0; i < vb; i++, k++) { crc[k] = dsy_crc32(api->vram + i * 0x2000, 0x2000); snprintf(nm[k], 16, "vram%u_crc", i); }
        crc[k] = dsy_crc32(api->oam, 0xA0); snprintf(nm[k++], 16, "oam_crc");
        if (dp_gbl_variant_eram[variant]) { crc[k] = dsy_crc32(api->eram, dp_gbl_variant_eram[variant]); snprintf(nm[k++], 16, "sram_crc"); }
        for (unsigned i = 0; i < k; i++, n++) { dn[n] = nm[i]; dd[n] = (const uint8_t *)&crc[i]; dl[n] = 4; }
        DpDsyWriter dw = {file};
        dp_dsy_frame(&dw, frame, n, dn, dd, dl);
        return;
    }
    const char *names[] = {"wram", "wram_banks", "vram", "oam", "hram", "regs", "io_raw", "ie", "sram"};
    const uint8_t *data[] = {wram, api->wram, api->vram, api->oam, api->hram, regs, api->io, api->ie, api->eram};
    const uint32_t len[] = {
        sizeof wram, DP_GBL_COLOR_BUILD ? 0x8000 : 0x2000,
        DP_GBL_COLOR_BUILD ? 0x4000 : 0x2000, 0xA0, 0x7F,
        sizeof regs, 0x80, 1, dp_gbl_variant_eram[variant]
    };
    DpDsyWriter writer = {file};
    dp_dsy_frame(&writer, frame, len[8] ? 9 : 8, names, data, len);
}

// compact ROM index bank * 0x4000 + (addr & 0x3FFF); the window bank comes from the mapper state
static int64_t gb_rom_index(DpMachine *m, uint32_t a) {
    DpMach *d = gbw(m)->d;
    if (a >= 0x8000 || dp_mach_in_cgb_bios(d, a)) return -1;
    uint32_t bank = dp_gbl_bank_of(d->api, (uint16_t)a);
    int64_t idx = (int64_t)bank * 0x4000 + (a & 0x3FFF);
    return gbw(m)->rom_size && (size_t)idx >= gbw(m)->rom_size ? -1 : idx;
}
// --- observe: DpObservers over the engine hooks ---------------------------------
// one instruction per gb_run: the hook of the second instruction cuts before it executes
static void gb_on_step(DpMach *m, uint16_t pc, uint8_t op, int entry) {
    GbWrap *g = m->owner;
    if (g->seen == 0) g->first_op = op;
    if (g->seen++ >= 1) dp_mach_cut(m, DP_CUT_STEPS);
    if (!g->probing && g->o.on_step) g->o.on_step(g->user, pc, op, entry);
}
static void gb_on_read(DpMach *m, uint16_t a, uint8_t v) {
    GbWrap *g = m->owner;
    if (g->joy_armed && a == 0xFF00) g->joy_seen = 1;
    if (!g->probing && g->o.on_read) g->o.on_read(g->user, a, v);
}
static void gb_on_write(DpMach *m, uint16_t a, uint8_t v) {
    GbWrap *g = m->owner;
    if (g->probing && g->ram_sources) {
        /* A speculative instruction changes its own saved RAM as well. The
         * global observer is suppressed, so discard stale local provenance. */
        int64_t index = g->machine->ram_index(g->machine, a);
        if (index >= 0) gb_ram_source_set(g->machine, (uint32_t)index, 0);
    }
    if (!g->probing && g->o.on_write) g->o.on_write(g->user, a, v);
}
static void gb_on_copy(DpMach *m, uint16_t src, uint16_t dst, uint16_t len, unsigned channel) {
    GbWrap *g = m->owner;
    /* The CGB OAM DMA runtime reads $E000-$FFFF through the WRAM echo.
     * Report the address actually read, including $FE00-$FFFF where the
     * CPU's own address space would instead select OAM or I/O. */
    if (g->ram_sources && channel == DP_COPY_GB_OAM && src >= 0xE000u)
        src = (uint16_t)(src - 0x2000u);
    if (g->ram_sources) {
        for (unsigned i = 0; i < len; i++) {
            uint16_t s = (uint16_t)(src + i), d = (uint16_t)(dst + i);
            int64_t from_rom = g->machine->rom_index(g->machine, s);
            int64_t from_ram = from_rom < 0 ? g->machine->ram_index(g->machine, s) : -1;
            int64_t to_ram = g->machine->ram_index(g->machine, d);
            uint32_t provenance = from_rom >= 0 ? (uint32_t)from_rom + 1u :
                from_ram >= 0 ? gb_ram_source(g->machine, (uint32_t)from_ram) : 0;
            if (to_ram >= 0) gb_ram_source_set(g->machine, (uint32_t)to_ram, provenance);
        }
    }
    if (!g->probing && g->o.on_copy) g->o.on_copy(g->user, src, len, channel);
}
static void gb_attach_hooks(DpMachine *m) {
    GbWrap *g = gbw(m);
    dp_mach_attach(g->d, g, gb_on_step, gb_on_read, gb_on_write, NULL);
    g->d->on_copy = gb_on_copy;
}
static void gb_observe(DpMachine *m, const DpObservers *o, void *user) {
    GbWrap *g = gbw(m);
    g->o = *o;
    g->user = user;
    gb_attach_hooks(m);
}
// one instruction; a halted machine with nothing to wake it reports no progress
static int gb_run(DpMachine *m) {
    GbWrap *g = gbw(m);
    g->seen = 0;
    // the interpreter keeps the EI delay in a local that a cut loses: after an EI, hide the pending
    // event for this run so the instruction after EI still executes before the interrupt
    volatile uint32_t *ne = &g->d->api->gbr->next_evt;
    uint32_t saved = *ne, far = g->d->api->gbr->cyc + 0x40000000u;
    if (g->last_ei) *ne = far;
    int cut = dp_mach_run(g->d);
    if (g->last_ei && *ne == far) *ne = saved;
    if (g->seen > 0) g->last_ei = (g->first_op == 0xFB);
    return g->seen > 0 || cut == DP_CUT_FRAME;
}
static uint32_t gb_pc(DpMachine *m) { return gbw(m)->d->api->gbr->pc; }
static void gb_set_pc(DpMachine *m, uint32_t pc) { gbw(m)->d->api->gbr->pc = (uint16_t)pc; }
static int gb_invert_condition(DpMachine *m, int condition) {
    gbl_cpu_t *c = gbw(m)->d->api->gbr;
    if (condition < 0 || condition > 3) return 0;
    if (condition & 2) c->cf = !c->cf;
    else c->zr = c->zr ? 0 : 1;
    return 1;
}
// register seeds (music rips): the SM83 registers are directly writable through the engine
static int gb_seed_regs(DpMachine *m, uint8_t a, uint16_t sp) {
    if (a != 0xFF) gbw(m)->d->api->gbr->a = a;
    if (sp != 0xFFFF) gbw(m)->d->api->gbr->sp = sp;
    return 1;
}
static void gb_poke(DpMachine *m, uint32_t a, uint8_t v) {
    GbWrap *g = gbw(m);
    uint16_t addr = (uint16_t)a;
    /* Exploration mutations bypass the CPU write hook. Keep provenance only
     * when the physical byte actually remains unchanged (including rejected
     * writes and mirrored/banked RAM), without creating a coverage event. */
    int memory = (addr >= 0x8000u && addr < 0xFEA0u) ||
                 (addr >= 0xFF80u && addr < 0xFFFFu);
    int64_t index = g->ram_sources && memory ? m->ram_index(m, addr) : -1;
    uint8_t old = index >= 0 ? gb_fetch(m, m->ram_addr(m, (uint32_t)index)) : 0;
    g->d->api->wr(addr, v);
    if (index >= 0 && old != gb_fetch(m, m->ram_addr(m, (uint32_t)index)))
        gb_ram_source_set(m, (uint32_t)index, 0);
}
// per-bank seeding: the $4000 window shows 16 KiB block `bank` (the kernel's seed_banks);
// bank 0 included — the reset view is not assumed, the boot may have selected another bank
static int gb_bank_view(DpMachine *m, unsigned bank) {
    DpMach *d = gbw(m)->d;
    unsigned mbc = dp_gbl_variant_mbc[d->api->variant];
    if (bank >= *d->api->rom_banks) return 0;
    switch (mbc) {
    case 0:
        break; /* fixed bank 1 */
    case 1: {
        unsigned shift = *d->api->mbc1m ? 4u : 5u;
        unsigned low = bank & ((1u << shift) - 1u);
        /* MBC1M can select low nibble zero with bit 4 set; ordinary
         * MBC1 instead remaps each low-zero bank to its successor. */
        if (*d->api->mbc1m && !low) low = 0x10;
        gb_poke(m, 0x2000, (uint8_t)low);
        gb_poke(m, 0x4000, (uint8_t)(bank >> shift));
        break;
    }
    case 2:
        gb_poke(m, 0x2100, (uint8_t)bank); /* address bit 8 selects ROM */
        break;
    case 3:
        gb_poke(m, 0x2000, (uint8_t)bank);
        break;
    case 5:
        gb_poke(m, 0x2000, (uint8_t)bank);
        gb_poke(m, 0x3000, (uint8_t)(bank >> 8));
        break;
    default:
        return 0;
    }
    /* Report unavailable or bus-blocked mappings instead of seeding an
     * unintended bank. MBC1/2/3 cannot select every nominal bank. */
    return d->api->gbr->rom_bank == bank;
}
// Echo RAM mirrors executable WRAM; only the OAM/unusable window is cut.
// A stack outside WRAM/HRAM pops garbage.
static int gb_cut_pc(DpMachine *m, uint32_t pc) { (void)m; return pc >= 0xFE00 && pc < 0xFF00; }
// shared DP_DEST_* knowledge: where a write lands in the hardware (the GB's
// finer DP_DF_* types project onto these same destinations)
static uint8_t gb_data_dest(DpMachine *m, uint32_t a) {
    (void)m;
    if (a >= 0x8000 && a < 0xA000) return DP_DEST_VRAM;
    if (a >= 0xFE00 && a < 0xFEA0) return DP_DEST_OAM;
    if (a >= 0xFF10 && a <= 0xFF25) return DP_DEST_MUSIC;
    if (a >= 0xFF30 && a <= 0xFF3F) return DP_DEST_SAMPLE;
    if (a == 0xFF47 || a == 0xFF48 || a == 0xFF49 || a == 0xFF69 || a == 0xFF6B) return DP_DEST_CRAM;
    if (a >= 0xA000 && a < 0xC000) return DP_DEST_SRAM;
    return DP_DEST_NONE;
}
static int gb_cut_sp(DpMachine *m) { uint16_t sp = gbw(m)->d->api->gbr->sp; return sp < 0xC000 || sp > 0xFFFE; }
static uint64_t gb_frame(DpMachine *m) { return *gbw(m)->d->api->frame; }
// kernel mask (up down left right button1 button2 start select) -> the engine's set_buttons layout
// (A=1 B=2 Select=4 Start=8 Right=16 Left=32 Up=64 Down=128, what the movies and the old engine feed)
static void gb_set_input(DpMachine *m, uint32_t mask) {
    uint8_t pad = 0;
    if (mask & 0x01) pad |= 0x40;   // up
    if (mask & 0x02) pad |= 0x80;   // down
    if (mask & 0x04) pad |= 0x20;   // left
    if (mask & 0x08) pad |= 0x10;   // right
    if (mask & 0x10) pad |= 0x01;   // a
    if (mask & 0x20) pad |= 0x02;   // b
    if (mask & 0x40) pad |= 0x08;   // start
    if (mask & 0x80) pad |= 0x04;   // select
    gbw(m)->d->api->set_buttons(pad);
}

// --- ctx_components: what a section's behaviour can depend on ---------------------
static const DpCtxComp gb_ctx[] = {
    { "a", DP_CC_REG, 0, 1 },    { "f", DP_CC_REG, 1, 1 },    { "b", DP_CC_REG, 2, 1 },
    { "c", DP_CC_REG, 3, 1 },    { "d", DP_CC_REG, 4, 1 },    { "e", DP_CC_REG, 5, 1 },
    { "h", DP_CC_REG, 6, 1 },    { "l", DP_CC_REG, 7, 1 },    { "sp", DP_CC_REG, 8, 2 },
    { "ime", DP_CC_REG, 9, 1 },  { "ie", DP_CC_REG, 10, 1 },  { "rom bank", DP_CC_REG, 11, 2 },
    { "ram gate", DP_CC_REG, 12, 1 }, { "vram bank", DP_CC_REG, 13, 1 },
    { "wram", DP_CC_PAGES, -1, 0x2000 }, { "vram", DP_CC_PAGES, -1, 0x2000 },
    { "sram", DP_CC_PAGES, -1, 0x4000 }, { "hram", DP_CC_PAGES, -1, 0x7F },
    { "oam", DP_CC_PAGES, -1, 0xA0 },    { "io", DP_CC_PAGES, -1, 0x80 },
};
static uint64_t gb_ctx_reg_hash(DpMachine *m) {   // the register block before the clocks (includes pc, sp, flags, ime, rom bank)
    // CGB banking and DMA state are outside gbl_cpu_t. Preserve the full
    // machine state in the projection key rather than merging banked states.
    if (DP_GBL_COLOR_BUILD && gbw(m)->rom_size > 0x143 && (gbw(m)->rom[0x143] & 0x80))
        return gb_state_hash(m);
    const uint8_t *b = (const uint8_t *)gbw(m)->d->api->gbr;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < offsetof(gbl_cpu_t, cyc); i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
// C3: the concrete SM83 registers (every byte known) for the static closure's bootstrapping
static int gb_get_regs(DpMachine *m, DpRegState *out) {
    const gbl_cpu_t *c = gbw(m)->d->api->gbr;
    dp_regstate_clear(out);
    out->v[DP_SM83R_BC] = c->bc; out->v[DP_SM83R_DE] = c->de; out->v[DP_SM83R_HL] = c->hl; out->v[DP_SM83R_SP] = c->sp;
    out->v[DP_SM83R_A] = c->a;
    out->known[DP_SM83R_BC] = out->known[DP_SM83R_DE] = out->known[DP_SM83R_HL] = out->known[DP_SM83R_SP] = 3;
    out->known[DP_SM83R_A] = 1;
    return 1;
}
static int gb_ctx_components(DpMachine *m, const DpCtxComp **out) {
    (void)m;
    *out = gb_ctx;
    return (int)(sizeof gb_ctx / sizeof *gb_ctx);
}

static int gb_probe_run(DpMachine *m) {
    GbWrap *g = gbw(m);
    g->probing = 1;
    int ran = gb_run(m);
    g->probing = 0;
    return ran;
}


// technique 2 on GB: an instruction that reads the joypad port ($FF00) forks one state per button
// held; the held pad stays in the saved state.  The instruction is dry-run once to see the read.
static int gb_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    static const uint8_t pads[] = {0x80, 0x10, 0x20, 0x40, 0x04, 0x08, 0x02, 0x01};
    GbWrap *g = gbw(m);
    // Externally supplied IR and DAC samples are not determined by the ROM.
    // Substitute the bus read during the instruction, so arithmetic, bit
    // operations and read-modify-write instructions see the alternative too.
    uint32_t read_addr = 0x10000u;
    if (bytes[0] == 0xF0) read_addr = 0xFF00u | bytes[1];
    else if (bytes[0] == 0xFA) read_addr = (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8);
    else if (bytes[0] == 0xF2) read_addr = 0xFF00u | (g->d->api->gbr->bc & 0xFFu);
    else if (bytes[0] == 0x0A) read_addr = g->d->api->gbr->bc;
    else if (bytes[0] == 0x1A) read_addr = g->d->api->gbr->de;
    else if (bytes[0] == 0x2A || bytes[0] == 0x3A) read_addr = g->d->api->gbr->hl;
    else if ((bytes[0] & 0xC7u) == 0x46u && bytes[0] != 0x76u) {
        read_addr = g->d->api->gbr->hl;
    }
    else if ((bytes[0] >= 0x80 && bytes[0] <= 0xbf && (bytes[0] & 7u) == 6u) ||
             bytes[0] == 0x34 || bytes[0] == 0x35 ||
             (bytes[0] == 0xcb && (bytes[1] & 7u) == 6u))
        read_addr = g->d->api->gbr->hl;
    else if ((bytes[0] & 0xcfu) == 0xc1u || bytes[0] == 0xc9 || bytes[0] == 0xd9 ||
             bytes[0] == 0xc0 || bytes[0] == 0xc8 || bytes[0] == 0xd0 || bytes[0] == 0xd8) {
        uint16_t sp = g->d->api->gbr->sp;
        if (sp == 0xff01 || sp == 0xff02 || sp == 0xff41 || sp == 0xff44 ||
            sp == 0xff56 || sp == 0xff76 || sp == 0xff77)
            read_addr = sp;
        else read_addr = (uint16_t)(sp + 1u);
    }
    int cgb = DP_GBL_COLOR_BUILD && g->rom_size > 0x143 && (g->rom[0x143] & 0x80);
    int rly = read_addr == 0xFF44;
    int rsta = read_addr == 0xFF41;
    int ir = cgb && read_addr == 0xFF56 && (g->d->api->io[0x56] & 0xC0) == 0xC0;
    int pcm = cgb && (read_addr == 0xFF76 || read_addr == 0xFF77);
    // A headless run has no serial peer. Explore received bytes on either
    // clock, including reads after the completion IRQ has been acknowledged.
    uint8_t sc = g->d->api->io[0x02];
    int serial = (read_addr == 0xFF01 && ((sc & 0x80u) || *g->d->api->serial_received)) ||
                 (read_addr == 0xFF02 && (sc & 0x81u) == 0x80u);
    if (rly || rsta || ir || pcm || serial) {
        int lcd_on = (g->d->api->io[0x40] & 0x80u) != 0;
        unsigned count = rly ? (lcd_on ? 154u : 1u) :
                         rsta ? (lcd_on ? 8u : 1u) :
                         ir || (serial && read_addr == 0xFF02) ? 2u : 256u;
        if (k >= count) return 0;
        uint8_t *pre = malloc(m->state_bytes(m));
        if (!pre) return 0;
        m->save(m, pre);
        uint8_t value = rsta ? (uint8_t)(lcd_on ? (g->d->api->rd(0xFF41) & 0xF8u) | k :
                                                   g->d->api->rd(0xFF41) & 0xFCu) :
                        ir ? (uint8_t)((g->d->api->rd(0xFF56) & ~2u) | (k << 1)) : (uint8_t)k;
        if (serial && read_addr == 0xFF02) {
            value = (uint8_t)((g->d->api->rd(0xFF02) & 0x7Fu) | (k ? 0x80u : 0));
            if (!k) {
                g->d->api->io[0x02] &= 0x7Fu;
                g->d->api->io[0x0F] |= 8u;
                *g->d->api->serial_received = 1;
            }
        } else if (serial) {
            g->d->api->io[0x01] = value;
        }
        g->d->probe_address = (uint16_t)read_addr;
        g->d->probe_value = value;
        g->d->probe_hit = 0;
        g->d->probe_enabled = 1;
        int ran = gb_probe_run(m);
        g->d->probe_enabled = 0;
        if (!ran || !g->d->probe_hit) { m->load(m, pre); free(pre); return 0; }
        m->save(m, save);
        m->load(m, pre);
        free(pre);
        return 1;
    }
    if (k >= sizeof pads) return 0;
    uint8_t *pre = malloc(m->state_bytes(m));
    if (!pre) return 0;
    m->save(m, pre);
    g->joy_armed = 1; g->joy_seen = 0;
    gb_probe_run(m);
    g->joy_armed = 0;
    int hit = g->joy_seen;
    m->load(m, pre);
    if (hit) { g->d->api->set_buttons(pads[k]); m->save(m, save); m->load(m, pre); }
    free(pre);
    return hit;
}
// Explore each enabled interrupt line independently, preserving the concrete
// requests and priority. The snapshot is restored before the dispatcher runs.
static void gb_irq_request(DpMach *d, unsigned source, uint8_t bit) {
    if (source == 3 && (d->api->io[0x02] & 0x81u) == 0x80u) {
        /* A peer clock can finish an externally clocked transfer while the
         * CPU waits for its IRQ. Leave the received byte open to subsequent
         * SB probes and make SC reflect the completed transfer. */
        d->api->wr(0xFF02, d->api->io[0x02] & 0x7Fu);
        d->api->io[0x01] = 0xFF;
        *d->api->serial_received = 1;
    }
    d->api->io[0x0F] |= bit;
}
static int gb_irq_source_alt(void *user, DpMachine *m, unsigned source, unsigned delay, uint8_t *save) {
    (void)user;
    DpMach *d = gbw(m)->d;
    if (source >= 5 || delay < 1 || delay > 3 || !d->api->gbr->ime || !d->api->ie) return 0;
    uint8_t bit = (uint8_t)(1u << source);
    if (!(*d->api->ie & bit) || (d->api->io[0x0F] & bit)) return 0;
    /* Jitter the arrival itself, keeping the intervening concrete machine
     * state and leaving the live path and its observations untouched. */
    uint8_t *before = malloc(m->state_bytes(m));
    if (!before) return 0;
    m->save(m, before);
    int ok = 1;
    for (unsigned i = 1; i < delay && ok; i++) ok = gb_probe_run(m);
    if (ok && d->api->gbr->ime && (*d->api->ie & bit) && !(d->api->io[0x0F] & bit)) {
        gb_irq_request(d, source, bit);
        m->save(m, save);
    } else ok = 0;
    m->load(m, before);
    free(before);
    return ok;
}
static const DpPlatformHooks gb_hooks = {
    .event_probe = gb_event_probe, .irq_source_count = 5, .irq_source_alt = gb_irq_source_alt
};
const DpPlatformHooks *dp_machine_gb_hooks(void) { return &gb_hooks; }

static void gb_close(DpMachine *m) {
    if (!m) return;
    dp_mach_close(gbw(m)->d);
    free(gbw(m)->ram_sources);
    free(m->impl);
    free(m);
}

static DpMachine *gb_wrap(DpMach *d);
// a clone is another compiled instance of the same variant, reset to power-on
static DpMachine *gb_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    GbWrap *o = gbw(m);
    if (!o->rom) return NULL;
    for (int ii = 0; ii < DP_GBL_MACHINES; ii++) {
        DpMach *d = dp_mach_open_exclusive(o->d->vi, ii);
        if (!d) continue;
        dp_mach_set_rom(d, o->rom, (uint32_t)o->rom_size);
        dp_mach_reset(d);
        if (o->tas) gb_tas_tune(d, o->tas);
        DpMachine *c = gb_wrap(d);
        if (!c) { dp_mach_close(d); return NULL; }
        dp_machine_gb_source(c, o->rom, o->rom_size);
        gbw(c)->tas = o->tas;
        return c;
    }
    return NULL;   // every compiled instance is taken
}
void dp_machine_gb_source(DpMachine *m, const uint8_t *rom, size_t size) {
    gbw(m)->rom = rom; gbw(m)->rom_size = size;
    m->ram_index = gb_dmg_ram_index;
    if (DP_GBL_COLOR_BUILD && size > 0x143 && (rom[0x143] & 0x80)) {
        uint32_t indices = GB_CGB_RAM_INDICES + dp_gbl_variant_eram[gbw(m)->d->api->variant];
        if (gbw(m)->ram_indices != indices) {
            free(gbw(m)->ram_sources);
            gbw(m)->ram_sources = calloc(indices, sizeof(uint32_t));
            if (!gbw(m)->ram_sources) {
                fprintf(stderr, "deepeel: cannot allocate CGB RAM provenance\n");
                abort();
            }
            gbw(m)->ram_indices = indices;
        }
        m->ram_index = gb_cgb_ram_index;
        m->ram_addr = gb_cgb_ram_addr;
        m->ram_size = gbw(m)->ram_indices;
        m->ram_source = gb_ram_source;
        m->ram_source_set = gb_ram_source_set;
    } else {
        free(gbw(m)->ram_sources);
        gbw(m)->ram_sources = NULL;
        gbw(m)->ram_indices = 0;
        m->ram_addr = NULL;
        m->ram_size = 0;
        m->ram_source = NULL;
        m->ram_source_set = NULL;
    }
    m->clone = gb_clone;
}

// wraps an open DpMach (ownership moves to the machine); NULL on failure
DpMachine *dp_machine_gb(DpMach *impl) { return gb_wrap(impl); }
static DpMachine *gb_wrap(DpMach *impl) {
    if (!impl) return NULL;
    DpMachine *m = calloc(1, sizeof *m);
    GbWrap *g = calloc(1, sizeof *g);
    if (!m || !g) { free(m); free(g); return NULL; }
    g->d = impl;
    g->machine = m;
    m->name = "gb";
    m->work_ram_base = 0xC000u; m->work_ram_len = 0x2000u;
    m->dump_dsy = gb_dump_dsy;
    m->save_ram_en = 1; m->save_ram_en_addr = 0x0000u; m->save_ram_en_val = 0x0A;
    m->save_ram_base = 0xA000u; m->save_ram_len = 0x2000u;
    m->plat = dp_platform_gb();
    m->bank_shift = 14;   // the SM83's 16 KiB pointer-table window (the static closure's geometry, E5)
    m->rom_top_win = 2; m->bank_window = 0x4000u;   // P10: the switchable window maps its bank at $4000   // $0000 fixed, $4000 switchable: a code target never sits at $8000 and beyond
    m->isa = dp_isa_sm83();
    m->impl = g;
    m->state_bytes = gb_state_bytes;
    m->save = gb_save;
    m->state_hash = gb_state_hash;
    m->load = gb_load;
    m->rom_index = gb_rom_index;
    m->fetch = gb_fetch;
    m->fetch_n = gb_fetch_instruction;
    m->run = gb_run;
    m->pc = gb_pc;
    m->set_pc = gb_set_pc;
    m->invert_condition = gb_invert_condition;
    m->seed_regs = gb_seed_regs;
    m->bank_view = gb_bank_view;
    m->cut_pc = gb_cut_pc;
    m->cut_sp = gb_cut_sp;
    m->data_dest = gb_data_dest;
    m->poke = gb_poke;
    m->set_input = gb_set_input;
    m->frame = gb_frame;
    m->watch = gb_watch; m->render = gb_render;
    m->audio_read = gb_audio_read;
    m->observe = gb_observe;
    m->ctx_components = gb_ctx_components;
    m->ctx_reg_hash = gb_ctx_reg_hash;
    m->get_regs = gb_get_regs;
    m->close = gb_close;
    return m;
}

// applies the movie's recorder sync profile to this instance and to every clone
void dp_machine_gb_set_tas(DpMachine *m, const struct DpTas *tas) {
    gbw(m)->tas = tas;
    gb_tas_tune(gbw(m)->d, tas);
}

// per-recorder machine tuning for the movie loaded in w (sync profiles, frame 0 input)
static void gb_tas_tune(DpMach *mach, const DpTas *tas) {
    int gambatte = tas->recorder == DP_TAS_REC_BIZHAWK && tas->gambatte;
    int vblank_clock = gambatte && !tas->equal_frames && !getenv("DEEPEEL_TAS_NOMODE3");
    int legacy_cgb_clock = vblank_clock && tas->bizhawk_23 && tas->is_cgb && tas->cgb_bios_loaded;
    if (tas->cgb_bios_loaded) dp_mach_start_cgb_bios(mach, tas->cgb_bios, legacy_cgb_clock ? 8u : 0u);
    if (vblank_clock) *mach->api->sync_vba_off_clock = legacy_cgb_clock ? 4 : 3;
    // BizHawk 2.3.2 Gambatte: RTC counted in CPU cycles, divisor 0x400000 + movie offset. The emulator
    // identity comes from the TASVideos API (tools/tasmeta.py exports DEEPEEL_TAS_RTCOFF), never the header.
    if (legacy_cgb_clock && getenv("DEEPEEL_TAS_RTCOFF"))
        *mach->api->sync_rtc_div = (uint32_t)(0x400000 + atoi(getenv("DEEPEEL_TAS_RTCOFF")));
    if (gambatte) {
        const char *skew = getenv("DEEPEEL_TAS_DIVSKEW");
        // The older CGB core starts its CPU counter at 8 with DIV based at 0.
        // Other movies retain the read correction measured on SML2.
        *mach->api->sync_div_skew = skew ? (uint8_t)atoi(skew) : (legacy_cgb_clock ? 0 : 40);
    }
    if (tas->cgb_bios_loaded && getenv("DEEPEEL_DEBUG_CGB_WRAM")) {
        FILE *f = fopen(getenv("DEEPEEL_DEBUG_CGB_WRAM"), "rb");
        if (f) { (void)fread(mach->api->wram, 1, 0x8000, f); fclose(f); }
    }
    if (tas->cgb_bios_loaded && getenv("DEEPEEL_DEBUG_CGB_HRAM")) {
        FILE *f = fopen(getenv("DEEPEEL_DEBUG_CGB_HRAM"), "rb");
        if (f) { (void)fread(mach->api->hram, 1, 0x7F, f); fclose(f); }
    }
    // the recording emulator's timing quirks (gbl.h movie sync
    // knobs), for this worker's machine only and while the movie
    // plays: random play after it runs on hardware timing again
    // BizHawk (Gambatte) powers a cartridge up with blank battery RAM
    // filled with $FF: games that read their save slot (Mario Land 2)
    // branch on it, so a zeroed RAM replays another game
    if ((tas->recorder == DP_TAS_REC_BIZHAWK || (tas->recorder == DP_TAS_REC_VBA && tas->recorder_rev == 0 && !getenv("DEEPEEL_TAS_NOINIT"))) && mach->api->eram)
        memset(mach->api->eram, 0xFF, dp_gbl_variant_eram[mach->api->variant]);
    // VBA-rr's V8 core (VBM minor version 1) powers WRAM up with a fixed
    // pattern ($FF x8 / $0F x8, blocks of $800 alternating), HRAM with $FF
    // (observed on the VBA-rr headless harness); the V7 core (minor 0)
    // powers up zeroed, like m0boy: uninitialised reads depend on it
    if (tas->recorder == DP_TAS_REC_VBA && tas->recorder_rev >= 1 && !getenv("DEEPEEL_TAS_NOINIT")) {
        for (int i = 0; i < 0x2000; i++)
            mach->api->wram[i] = (((i >> 3) ^ (i >> 11)) & 1) ? 0x0F : 0xFF;
        memset(mach->api->hram, 0xFF, 0x7F);
    }
    // BizHawk's Gambatte starts from a fixed power-on RAM dump, not zeros
    if (tas->recorder == DP_TAS_REC_BIZHAWK && tas->gambatte &&
        !tas->cgb_bios_loaded && !getenv("DEEPEEL_TAS_NOINIT")) {
        memcpy(mach->api->wram, dp_gambatte_wram, 0x2000);
        memcpy(mach->api->hram, dp_gambatte_hram, 0x7F);
    }
    // BizHawk's GBHawk (gbl.h): the pad is latched at the vblank
    // edge inside its fixed frame (65,664 cycles to LY 144); the
    // worker applies the next mask just after the m0boy vblank, so
    // the pad needs a 14,481-cycle delay (measured window
    // 14,470..14,550; the PPU phase below moves the m0boy vblank
    // edge but the pad window keeps 14,481).  The divider phase at
    // $0100 is 0xFE4D, and GBHawk is 405 cycles into line 153
    // there (LY zeroed by its cycle-6 rule; gbl.h, sync_ppu_phase:
    // applied to the loaded state, it makes every LY read match).
    // GBHawk keeps the WRAM zero (no power-on dump) and its frames
    // are always 70224 cycles, like the vblank clock: no mode-3
    // clock change applies.
    if (tas->recorder == DP_TAS_REC_BIZHAWK && tas->gbhawk) {
        *mach->api->sync_pad_delay = getenv("DEEPEEL_TAS_PADDELAY")
            ? (uint32_t)strtoul(getenv("DEEPEEL_TAS_PADDELAY"), NULL, 0) : 14481u;
        *mach->api->sync_div_extra = getenv("DEEPEEL_TAS_DIVEXTRA")
            ? (uint16_t)strtoul(getenv("DEEPEEL_TAS_DIVEXTRA"), NULL, 0) : 0xFE4Du;
        *mach->api->sync_ppu_phase = getenv("DEEPEEL_TAS_PPUPHASE")
            ? (uint16_t)strtoul(getenv("DEEPEEL_TAS_PPUPHASE"), NULL, 0) : 405u;
        mach->api->sync_ppu_apply();
        fprintf(stderr, "deepeel: TAS %s: GBHawk sync profile (pad latch +14481 cycles, DIV phase $FE4D, PPU phase 405)\n",
                tas->source);
    }
    if (tas->echo_old && !getenv("DEEPEEL_TAS_NOECHO")) {
        *mach->api->sync_echo_old = 1;
        mach->api->sync_echo_apply();
        fprintf(stderr, "deepeel: TAS %s: VBA echoRAMFix off ($E000-$FDFF is separate memory)\n", tas->source);
    }
    int vba_old = tas->recorder == DP_TAS_REC_VBA && tas->recorder_rev == 0;
    if (vba_old) {
        // VBA 1.7.2 based recorders (VBM minor version 0): input
        // read at each vblank, no frame while the LCD is off, a
        // ten-line line 0 after an enable.  Partial: their CPU
        // timing quirks (cycle table, HALT wake-up) stay hardware
        *mach->api->sync_vba_off_clock = getenv("DEEPEEL_TAS_OFFCLOCK")
            ? (uint8_t)atoi(getenv("DEEPEEL_TAS_OFFCLOCK")) : 2;
        // V7 reads LY at the start of the reading instruction (gbl.h, sync_ly_lag)
        *mach->api->sync_ly_lag = getenv("DEEPEEL_TAS_LYLAG")
            ? (int16_t)atoi(getenv("DEEPEEL_TAS_LYLAG")) : 1;
        // V7 counts no tick for the interrupt dispatch (20 cycles on the hardware)
        *mach->api->sync_irq_trim = getenv("DEEPEEL_TAS_IRQTRIM")
            ? (int16_t)atoi(getenv("DEEPEEL_TAS_IRQTRIM")) : 20;
        // V7 wakes a halted CPU on its 96-cycle sound grid (gbl.h, sync_halt_snd);
        // DEEPEEL_TAS_HALTSND = grid phase in cycles, -1 = hardware wake-up
        {
            const char *hs = getenv("DEEPEEL_TAS_HALTSND");
            *mach->api->sync_halt_snd = (int16_t)((hs ? atoi(hs) : 0) + 1);
        }
        fprintf(stderr, "deepeel: TAS %s: VBA 1.7.2 sync profile (input at vblank, no LCD-off frame, "
                        "ten-line enable)\n", tas->source);
    } else if (tas->recorder == DP_TAS_REC_VBA) {
        *mach->api->sync_div_skew = 4;
        *mach->api->sync_vba_off_clock = 1;
        fprintf(stderr, "deepeel: TAS %s: VBA sync profile (DIV read +4 cycles, VBA LCD-off frame clock)\n",
                tas->source);
    }
}
