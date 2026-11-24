// CGB bank mapping, snapshots and hardware-copy observations; DMG isolation.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/dpmachine.h"
#include "../src/explore/core.h"
#include "../src/mach/m0boy.h"
#include "test_util.h"

static unsigned copies, bytes, last_channel;
static uint32_t first_src;
static void copied(void *u, uint32_t src, uint32_t len, unsigned channel) {
    (void)u;
    if (!copies) first_src = src;
    last_channel = channel;
    copies++; bytes += len;
}

#if TEST_CGB
static void check_gambatte_halted_frame(void) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "HALT ROM allocated");
    if (!rom) return;
    rom[0x143] = 0x80;
    rom[0x100] = 0x76; // HALT, followed by NOP
    DpMach *raw = dp_mach_open(dp_mach_pick_variant(rom), 0);
    CHECK_MSG(raw, "HALT instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "HALT wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    m->observe(m, &(DpObservers){0}, NULL);
    *raw->api->sync_vba_off_clock = 4;
    m->poke(m, 0xff40, 0);
    m->poke(m, 0xff00, 0x10); // select the buttons
    m->poke(m, 0xffff, 0x10);
    m->poke(m, 0xff0f, 0);
    raw->api->gbr->pc = 0x100;
    raw->api->gbr->ime = 0;
    CHECK_MSG(m->run(m) && m->frame(m) == 1 && m->pc(m) == 0x101,
              "a movie frame can end in HALT with no pending IRQ");
    uint8_t *snapshot = malloc(m->state_bytes(m));
    CHECK_MSG(snapshot, "halted frame snapshot allocated");
    if (snapshot) {
        m->save(m, snapshot);
        CHECK_MSG(m->run(m) && m->frame(m) == 2 && m->pc(m) == 0x101,
                  "the next frame advances without executing the NOP");
        m->load(m, snapshot);
        m->set_input(m, 0x10); // A
        CHECK_MSG(m->run(m) && m->frame(m) == 1 && m->pc(m) == 0x102,
                  "new frame input wakes the restored HALT");
        free(snapshot);
    }
    *raw->api->sync_vba_off_clock = 0;
    m->close(m);
    free(rom);
}

static void check_gambatte_timer_stop(void) {
    uint8_t rom[0x8000] = { [0x143] = 0x80 };
    DpMach *raw = dp_mach_open(dp_mach_pick_variant(rom), 0);
    CHECK_MSG(raw, "CGB timer instance opens");
    if (!raw) return;
    dp_mach_set_rom(raw, rom, sizeof rom);
    dp_mach_reset(raw);
    const DpGblApi *api = raw->api;
    *api->sync_vba_off_clock = 4;
    api->wr(0xff04, 0);
    api->wr(0xff05, 0xc8);
    api->wr(0xff07, 4);
    api->gbr->cyc += 836;
    api->wr(0xff07, 4);
    CHECK_MSG(api->rd(0xff05) == 0xc8, "an unchanged TAC leaves TIMA alone");
    api->wr(0xff07, 0);
    api->gbr->cyc += 36;
    api->wr(0xff07, 4);
    CHECK_MSG(api->rd(0xff05) == 0xc9, "Gambatte advances TIMA when TAC stops a high clock");
    *api->sync_vba_off_clock = 0;
    dp_mach_close(raw);
}

static void check_pcm_alu_probe(void) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "probe ROM allocated");
    if (!rom) return;
    rom[0x143] = 0x80;
    rom[0x100] = 0xae; // XOR (HL)
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "probe instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "probe wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    DpObservers observers = {0};
    m->observe(m, &observers, NULL);
    raw->api->gbr->pc = 0x100;
    raw->api->gbr->hl = 0xff76;
    raw->api->gbr->a = 0x55;
    uint8_t *snapshot = malloc(m->state_bytes(m));
    CHECK_MSG(snapshot, "probe snapshot allocated");
    if (snapshot) {
        const DpPlatformHooks *hooks = dp_machine_gb_hooks();
        const uint8_t instruction[] = {0xae, 0, 0};
        CHECK_MSG(hooks->event_probe(NULL, m, 0x100, instruction, 0xa5, snapshot),
                  "PCM probe handles arithmetic read");
        CHECK_MSG(raw->api->gbr->pc == 0x100 && raw->api->gbr->a == 0x55,
                  "probe restores the live machine");
        m->load(m, snapshot);
        CHECK_MSG(raw->api->gbr->a == 0xf0 && raw->api->gbr->pc == 0x101,
                  "alternative byte participates in the XOR instruction");
        free(snapshot);
    }
    m->close(m);
    free(rom);
}

static void check_cartridge_ram_banks(void) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "banked cartridge ROM allocated");
    if (!rom) return;
    rom[0x143] = 0x80;
    rom[0x147] = 0x1a; // MBC5 + RAM
    rom[0x149] = 3;    // four 8 KiB banks
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "banked cartridge instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "banked cartridge wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    m->poke(m, 0x0000, 0x0a);
    m->poke(m, 0x4000, 0);
    m->poke(m, 0xa123, 0x31);
    int64_t first = m->ram_index(m, 0xa123);
    m->poke(m, 0x4000, 1);
    m->poke(m, 0xa123, 0x42);
    int64_t second = m->ram_index(m, 0xa123);
    CHECK_MSG(first >= 0 && second >= 0 && first != second,
              "cartridge RAM bank indices are distinct");
    if (first >= 0 && second >= 0) {
        m->ram_source_set(m, (uint32_t)first, 0x123);
        m->ram_source_set(m, (uint32_t)second, 0x456);
        CHECK_MSG(m->fetch(m, m->ram_addr(m, (uint32_t)first)) == 0x31 &&
                  m->fetch(m, m->ram_addr(m, (uint32_t)second)) == 0x42,
                  "physical cartridge RAM banks can be inspected independently");
        CHECK_MSG(m->ram_source(m, (uint32_t)first) == 0x123 &&
                  m->ram_source(m, (uint32_t)second) == 0x456,
                  "cartridge banks retain distinct ROM provenance");
    }
    m->poke(m, 0x0000, 0);
    CHECK_MSG(m->ram_index(m, 0xa123) < 0, "disabled cartridge RAM is not mapped");
    m->close(m);
    free(rom);
}

static void check_small_cartridge_ram(uint8_t type, uint8_t ram_size) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "small cartridge ROM allocated");
    if (!rom) return;
    rom[0x143] = 0x80;
    rom[0x147] = type;
    rom[0x149] = ram_size;
    rom[0x4000] = 0x0a;
    rom[0x4001] = 0xfa;
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "small cartridge instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "small cartridge wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    m->poke(m, 0x0000, 0x0a);
    m->poke(m, 0xa012, 0x0a);
    uint16_t mirror = type == 0x05 ? 0xa212 : 0xa812;
    int64_t first = m->ram_index(m, 0xa012);
    CHECK_MSG(first >= 0 && first == m->ram_index(m, mirror),
              "small cartridge RAM mirrors share a physical index");
    if (first >= 0) {
        uint8_t expected = type == 0x05 ? 0xfa : 0x0a;
        CHECK_MSG(m->fetch(m, m->ram_addr(m, (uint32_t)first)) == expected,
                  "physical small RAM inspection matches the bus value");
        if (type == 0x05) {
            m->ram_source_set(m, (uint32_t)first, 0x4001);
            CHECK_MSG(m->ram_source(m, (uint32_t)first) == 0,
                      "MBC2 discards provenance of transformed bytes");
            m->ram_source_set(m, (uint32_t)first, 0x4002);
            CHECK_MSG(m->ram_source(m, (uint32_t)first) == 0x4002,
                      "MBC2 retains provenance of unchanged bytes");
        }
    }
    m->close(m);
    free(rom);
}

#endif
typedef struct { uint32_t pc; uint8_t op; unsigned count; } StepCapture;
static void check_mapper_bank_seeds(int colour, uint8_t type, unsigned banks) {
    size_t size = (size_t)banks * 0x4000u;
    uint8_t *rom = calloc(1, size);
    CHECK_MSG(rom, "mapper seed ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    rom[0x147] = type;
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "mapper seed instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, (uint32_t)size);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "mapper seed wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, (uint32_t)size);
    if (type == 0x01) {
        CHECK_MSG(m->bank_view(m, 0x21) && m->rom_index(m, 0x4000) == 0x21 * 0x4000,
                  "MBC1 seeds both halves of the ROM bank");
        CHECK_MSG(!m->bank_view(m, 0x20), "MBC1 reports an unselectable bank");
    } else if (type == 0x05) {
        CHECK_MSG(m->bank_view(m, 7) && m->rom_index(m, 0x4000) == 7 * 0x4000,
                  "MBC2 seeds through the ROM bank register");
        CHECK_MSG(!m->bank_view(m, 0), "MBC2 reports its remapped bank zero");
    } else {
        CHECK_MSG(m->bank_view(m, 0x101) && m->rom_index(m, 0x4000) == 0x101 * 0x4000,
                  "MBC5 selects the ninth ROM-bank bit");
        CHECK_MSG(m->bank_view(m, 1) && m->rom_index(m, 0x4000) == 0x4000,
                  "MBC5 clears the ninth bit when selecting a lower bank");
    }
    m->close(m);
    free(rom);
}
static void check_time_in_state_hash(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "state hash ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "state hash instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "state hash wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    uint64_t before = m->state_hash(m);
    raw->api->gbr->cyc += 4;
    CHECK_MSG(m->state_hash(m) != before, "state hash distinguishes hardware cycle position");
    raw->api->gbr->cyc -= 4;
    CHECK_MSG(m->state_hash(m) == before, "restoring the cycle restores the state hash");
    ++*raw->api->frame;
    CHECK_MSG(m->state_hash(m) != before, "state hash distinguishes frame-dependent RTC time");
    m->close(m);
    free(rom);
}

static void check_video_event_probe(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "video probe ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    rom[0x100] = 0xae; /* XOR (HL) */
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "video probe instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "video probe wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    DpObservers observers = {0};
    m->observe(m, &observers, NULL);
    raw->api->wr(0xff40, 0x80);
    raw->api->gbr->pc = 0x100;
    raw->api->gbr->hl = 0xff44;
    raw->api->gbr->a = 0x55;
    uint8_t *snapshot = malloc(m->state_bytes(m));
    CHECK_MSG(snapshot, "video probe snapshot allocated");
    if (snapshot) {
        const uint8_t instruction[] = {0xae, 0, 0};
        const DpPlatformHooks *hooks = dp_machine_gb_hooks();
        CHECK_MSG(hooks->event_probe(NULL, m, 0x100, instruction, 123, snapshot),
                  "LY probe accepts an intermediate scanline through XOR (HL)");
        CHECK_MSG(raw->api->gbr->a == 0x55 && raw->api->gbr->pc == 0x100,
                  "video probe restores the live state");
        m->load(m, snapshot);
        CHECK_MSG(raw->api->gbr->a == (0x55 ^ 123) && raw->api->gbr->pc == 0x101,
                  "LY alternative participates in the instruction");
        raw->api->gbr->pc = 0x100;
        raw->api->gbr->hl = 0xff41;
        raw->api->gbr->a = 0x55;
        uint8_t stat_high = raw->api->rd(0xff41) & 0xf8;
        CHECK_MSG(hooks->event_probe(NULL, m, 0x100, instruction, 7, snapshot),
                  "STAT probe accepts coincidence and mode through XOR (HL)");
        m->load(m, snapshot);
        CHECK_MSG(raw->api->gbr->a == (uint8_t)(0x55 ^ (stat_high | 7)),
                  "STAT probe preserves the upper register bits");
        free(snapshot);
    }
    m->close(m);
    free(rom);
}

static void capture_step(void *user, uint32_t pc, uint8_t op, int entry) {
    (void)entry;
    StepCapture *capture = user;
    if (!capture->count++) { capture->pc = pc; capture->op = op; }
}

static void check_ram_instruction_trace(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "RAM execution ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "RAM execution instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "RAM execution wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    StepCapture capture = {0};
    DpObservers observer = {.on_step = capture_step};
    m->observe(m, &observer, &capture);
    raw->api->wr(0xff80, 0x04); /* INC B */
    raw->api->wr(0xff81, 0x00);
    raw->api->gbr->pc = 0xff80;
    CHECK_MSG(m->run(m), "HRAM instruction executes");
    CHECK_MSG(capture.count && capture.pc == 0xff80 && capture.op == 0x04,
              "HRAM trace observes the executed opcode");
#if TEST_CGB
    if (colour) {
        capture = (StepCapture){0};
        raw->api->wr(0xff40, 0);
        raw->api->wr(0xff4f, 1);
        raw->api->wr(0x8000, 0x3c); /* INC A */
        raw->api->wr(0x8001, 0x00);
        raw->api->gbr->pc = 0x8000;
        CHECK_MSG(m->run(m), "VRAM instruction executes");
        CHECK_MSG(capture.count && capture.pc == 0x8000 && capture.op == 0x3c,
                  "VRAM trace observes the selected bank's opcode");
    }
#endif
    m->close(m);
    free(rom);
}

static void check_irq_sources(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "IRQ source ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    rom[0x100] = 0x77; /* LD (HL),A */
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "IRQ source instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "IRQ source wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    StepCapture capture = {0};
    DpObservers observer = {.on_step = capture_step};
    m->observe(m, &observer, &capture);
    const DpPlatformHooks *hooks = dp_machine_gb_hooks();
    uint8_t *snapshot = malloc(m->state_bytes(m));
    CHECK_MSG(snapshot && hooks->irq_source_alt && hooks->irq_source_count == 5,
              "five independent IRQ sources available");
    if (snapshot && hooks->irq_source_alt) {
        for (unsigned source = 0; source < 5; source++) {
            raw->api->gbr->pc = 0x100;
            raw->api->gbr->sp = 0xfffe;
            raw->api->gbr->ime = 1;
            raw->api->wr(0xffff, 0x1f);
            raw->api->wr(0xff0f, 0);
            capture = (StepCapture){0};
            if (source == 2) {
                raw->api->gbr->hl = 0xc000;
                int64_t index = m->ram_index ? m->ram_index(m, 0xc000) : -1;
                if (index >= 0 && m->ram_source_set) m->ram_source_set(m, (uint32_t)index, 0x101);
                CHECK_MSG(hooks->irq_source_alt(NULL, m, source, 2, snapshot),
                          "IRQ jitter creates a delayed arrival");
                CHECK_MSG(raw->api->gbr->pc == 0x100 && capture.count == 0,
                          "IRQ jitter restores the live machine without observations");
                if (index >= 0 && m->ram_source)
                    CHECK_MSG(m->ram_source(m, (uint32_t)index) == 0x101,
                              "jitter preserves live path provenance");
                m->load(m, snapshot);
                if (index >= 0 && m->ram_source)
                    CHECK_MSG(m->ram_source(m, (uint32_t)index) == 0,
                              "jitter snapshot invalidates overwritten provenance");
                CHECK_MSG(raw->api->gbr->pc == 0x101 && (raw->api->io[0x0f] & 4),
                          "delayed IRQ snapshot contains the intervening instruction");
                raw->api->gbr->pc = 0x100;
                raw->api->gbr->ime = 1;
                raw->api->wr(0xff0f, 0);
            }
            CHECK_MSG(hooks->irq_source_alt(NULL, m, source, 1, snapshot),
                      "enabled IRQ source produces a snapshot");
            CHECK_MSG((raw->api->io[0x0f] & 0x1f) == 0,
                      "IRQ fork leaves the live request register unchanged");
            m->load(m, snapshot);
            CHECK_MSG(m->run(m), "forked IRQ reaches its handler");
            CHECK_MSG(capture.count && capture.pc == 0x40 + source * 8,
                      "fork reaches the corresponding IRQ vector");
        }
        free(snapshot);
    }
    m->close(m);
    free(rom);
}

static void check_internal_serial(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "serial timing ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "serial timing instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "serial timing wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    raw->api->wr(0xff0f, 0);
    raw->api->wr(0xff01, 0);
    raw->api->wr(0xff02, 0x81);
    raw->api->gbr->cyc += 1024;
    CHECK_MSG(raw->api->rd(0xff01) == 3, "two serial clocks shift two incoming bits");
    uint8_t *snapshot = malloc(m->state_bytes(m));
    CHECK_MSG(snapshot, "serial timing snapshot allocated");
    if (snapshot) {
        m->save(m, snapshot);
        raw->api->gbr->cyc += 3072;
        CHECK_MSG(raw->api->rd(0xff01) == 0xff && (raw->api->io[0x0f] & 8),
                  "eight serial clocks finish and request an interrupt");
        m->load(m, snapshot);
        raw->api->wr(0xff02, 0);
        raw->api->gbr->cyc += 4096;
        CHECK_MSG(raw->api->rd(0xff01) == 3 && !(raw->api->io[0x0f] & 8),
                  "cancelled serial transfer stays stopped after snapshot restore");
        free(snapshot);
    }
#if TEST_CGB
    if (colour) {
        raw->api->wr(0xff01, 0);
        raw->api->wr(0xff02, 0x83);
        raw->api->gbr->cyc += 16;
        CHECK_MSG(raw->api->rd(0xff01) == 1, "CGB fast clock shifts after 16 cycles");
    }
#endif
    m->close(m);
    free(rom);
}

static void check_external_serial_probe(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "serial probe ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    rom[0x100] = 0xf0; rom[0x101] = 0x01; /* LDH A,($01) */
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "serial probe instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "serial probe wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    DpObservers observer = {0};
    m->observe(m, &observer, NULL);
    raw->api->wr(0xff02, 0x80); /* external-clock transfer armed */
    raw->api->gbr->pc = 0x100;
    uint8_t *snapshot = malloc(m->state_bytes(m));
    CHECK_MSG(snapshot, "serial probe snapshot allocated");
    if (snapshot) {
        const uint8_t instruction[] = {0xf0, 0x01, 0};
        const DpPlatformHooks *hooks = dp_machine_gb_hooks();
        raw->api->gbr->ime = 1;
        raw->api->wr(0xffff, 8);
        raw->api->wr(0xff0f, 0);
        CHECK_MSG(hooks->irq_source_alt(NULL, m, 3, 1, snapshot),
                  "external serial clock can finish before its IRQ");
        CHECK_MSG((raw->api->io[0x02] & 0x80) && !(raw->api->io[0x0f] & 8),
                  "serial IRQ alternative preserves the live transfer");
        m->load(m, snapshot);
        CHECK_MSG(!(raw->api->io[0x02] & 0x80) && (raw->api->io[0x0f] & 8),
                  "serial IRQ alternative completes the transfer");
        raw->api->gbr->ime = 0; /* handler has acknowledged entry */
        raw->api->wr(0xff0f, 0);
        CHECK_MSG(hooks->event_probe(NULL, m, 0x100, instruction, 0x5a, snapshot),
                  "serial IRQ alternative still explores the peer byte");
        m->load(m, snapshot);
        CHECK_MSG(raw->api->gbr->a == 0x5a,
                  "handler can observe an alternative serial byte");
        raw->api->wr(0xff02, 0x80);
        raw->api->gbr->pc = 0x100;
        CHECK_MSG(hooks->event_probe(NULL, m, 0x100, instruction, 0xa5, snapshot),
                  "external serial input offers every received value");
        m->load(m, snapshot);
        CHECK_MSG(raw->api->gbr->a == 0xa5 && raw->api->gbr->pc == 0x102,
                  "serial variant executes with the supplied input");
        raw->api->wr(0xff01, 0);
        raw->api->wr(0xff02, 0x81);
        raw->api->gbr->cyc += 4096;
        (void)raw->api->rd(0xff01);
        raw->api->wr(0xff0f, 0); /* handler may already have acknowledged IRQ */
        raw->api->gbr->pc = 0x100;
        CHECK_MSG(hooks->event_probe(NULL, m, 0x100, instruction, 0x3c, snapshot),
                  "completed internally clocked transfer explores peer data");
        m->load(m, snapshot);
        CHECK_MSG(raw->api->gbr->a == 0x3c,
                  "received byte remains variable after interrupt acknowledgement");
        free(snapshot);
    }
    m->close(m);
    free(rom);
}

static void check_pending_input_snapshot(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "input snapshot ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "input snapshot instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "input snapshot wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    raw->api->wr(0xff00, 0x10);
    *raw->api->sync_pad_delay = 100;
    raw->api->set_buttons(1);
    uint8_t *snapshot = malloc(m->state_bytes(m));
    CHECK_MSG(snapshot, "pending input snapshot allocated");
    if (snapshot) {
        m->save(m, snapshot);
        raw->api->set_buttons(2);
        m->load(m, snapshot);
        raw->api->gbr->cyc += 100;
        CHECK_MSG((raw->api->rd(0xff00) & 3) == 2,
                  "snapshot restores pending input and its latch time");
        free(snapshot);
    }
    *raw->api->frame = 321;
    dp_mach_reset(raw);
    raw->api->wr(0xff00, 0x10);
    CHECK_MSG(*raw->api->frame == 0 && (raw->api->rd(0xff00) & 0x0f) == 0x0f,
              "reset clears the frame counter and prior held input");
    *raw->api->sync_pad_delay = 0;
    m->close(m);
    free(rom);
}

static void check_mbc3_rtc(int present, int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "MBC3 ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    rom[0x147] = present ? 0x10 : 0x12;
    rom[0x149] = 3;
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = vi >= 0 ? dp_mach_open(vi, 0) : NULL;
    CHECK_MSG(raw, "MBC3 instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "MBC3 wrapper opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    m->poke(m, 0x0000, 0x0a);
    m->poke(m, 0x4000, 0);
    m->poke(m, 0xa000, 0x5a);
    m->poke(m, 0x4000, 8);
    CHECK_MSG(raw->api->eram_index(0xa000) < 0, "RTC selection is not cartridge RAM");
    m->poke(m, 0xa000, 37);
    m->poke(m, 0x6000, 0); m->poke(m, 0x6000, 1);
    CHECK_MSG(raw->api->rd(0xa000) == (present ? 37 : 0xff),
              "RTC registers are present only on timer cartridges");
    uint32_t bytes = m->state_bytes(m);
    uint8_t *snapshot = malloc(bytes);
    CHECK_MSG(snapshot, "RTC snapshot allocated");
    if (snapshot) {
        m->save(m, snapshot);
        m->poke(m, 0xa000, 12);
        m->poke(m, 0x6000, 0); m->poke(m, 0x6000, 1);
        m->load(m, snapshot);
        CHECK_MSG(raw->api->rd(0xa000) == (present ? 37 : 0xff),
                  "snapshot restores the latched RTC");
        free(snapshot);
    }
    if (present) {
        m->poke(m, 0xff40, 0); /* RTC must advance with LCD disabled. */
        m->poke(m, 0xa000, 0);
        raw->api->gbr->cyc += 2097152u;
        m->poke(m, 0x6000, 0); m->poke(m, 0x6000, 1);
        CHECK_MSG(raw->api->rd(0xa000) == 0, "RTC preserves half a second");
        raw->api->gbr->cyc -= 4;
        m->poke(m, 0x6000, 0); m->poke(m, 0x6000, 1);
        CHECK_MSG(raw->api->rd(0xa000) == 0, "RTC ignores a temporary cycle rewind");
        raw->api->gbr->cyc += 4;
        snapshot = malloc(m->state_bytes(m));
        CHECK_MSG(snapshot, "fractional RTC snapshot allocated");
        if (snapshot) {
            m->save(m, snapshot);
            raw->api->gbr->cyc += 2097152u;
            m->poke(m, 0x6000, 0); m->poke(m, 0x6000, 1);
            CHECK_MSG(raw->api->rd(0xa000) == 1, "RTC advances in CPU time with LCD off");
            m->load(m, snapshot);
            raw->api->gbr->cyc += 2097152u;
            m->poke(m, 0x6000, 0); m->poke(m, 0x6000, 1);
            CHECK_MSG(raw->api->rd(0xa000) == 1, "snapshot restores fractional RTC time");
            free(snapshot);
        }
    }
    m->poke(m, 0x4000, 0);
    CHECK_MSG(raw->api->rd(0xa000) == 0x5a, "RAM mapping resumes after RTC access");
    m->close(m);
    free(rom);
}

static void check_machine(int colour) {
    uint8_t *rom = calloc(1, 0x8000);
    CHECK_MSG(rom, "ROM allocated");
    if (!rom) return;
    rom[0x143] = colour ? 0x80 : 0;
    for (unsigned i = 0; i < 256; i++) rom[0x4000 + i] = (uint8_t)(i ^ 0xa5);
    int vi = dp_mach_pick_variant(rom);
    DpMach *raw = dp_mach_open(vi, 0);
    CHECK_MSG(raw, "instance opens");
    if (!raw) { free(rom); return; }
    dp_mach_set_rom(raw, rom, 0x8000);
    dp_mach_reset(raw);
    DpMachine *m = dp_machine_gb(raw);
    CHECK_MSG(m, "machine opens");
    if (!m) { dp_mach_close(raw); free(rom); return; }
    dp_machine_gb_source(m, rom, 0x8000);
    CHECK_MSG(m->ram_index(m, 0xc123) == m->ram_index(m, 0xe123),
              "WRAM and its echo share provenance in both modes");
    CHECK_MSG(!m->cut_pc(m, 0xe123), "WRAM echo remains executable");
    DpObservers ob = { .on_copy = copied };
    m->observe(m, &ob, NULL);
    copies = bytes = first_src = 0;

    m->poke(m, 0xff40, 0); // unrestricted VRAM
    m->poke(m, 0xff70, 1);
    m->poke(m, 0xd000, 0x12);
    m->poke(m, 0xff70, 3);
    m->poke(m, 0xd000, 0x34);
    if (colour) {
        m->poke(m, 0xff56, 0xc1);
        CHECK_MSG(raw->api->rd(0xff56) == 0xff, "infrared port masks unused and receiver bits");
        m->poke(m, 0xff75, 0xff);
        CHECK_MSG(raw->api->rd(0xff75) == 0xff, "CGB FF75 writable bits set");
        m->poke(m, 0xff75, 0);
        CHECK_MSG(raw->api->rd(0xff75) == 0x8f, "CGB FF75 unused bits read high");
        m->poke(m, 0xff72, 0x52);
        CHECK_MSG(raw->api->rd(0xff72) == 0x52, "CGB scratch register retained");
        m->poke(m, 0xff26, 0);
        CHECK_MSG(raw->api->rd(0xff76) == 0 && raw->api->rd(0xff77) == 0,
                  "powered-off CGB PCM channels output zero");
        CHECK_MSG(m->fetch(m, 0xd000) == 0x34, "fetch reads selected WRAM bank");
        uint64_t bank3 = m->ctx_reg_hash(m);
        uint32_t n = m->state_bytes(m);
        uint8_t *saved = malloc(n);
        CHECK_MSG(saved, "snapshot allocated");
        if (!saved) { m->close(m); free(rom); return; }
        m->save(m, saved);
        m->poke(m, 0xff70, 1);
        CHECK_MSG(m->fetch(m, 0xd000) == 0x12, "WRAM bank 1 retained");
        int64_t bank1_index = m->ram_index(m, 0xd000);
        CHECK_MSG(m->ctx_reg_hash(m) != bank3, "projection distinguishes WRAM banks");
        m->load(m, saved);
        CHECK_MSG(m->fetch(m, 0xd000) == 0x34, "snapshot restores WRAM bank");
        int64_t bank3_index = m->ram_index(m, 0xd000);
        CHECK_MSG(bank1_index != bank3_index, "provenance distinguishes physical WRAM banks");
        CHECK_MSG(m->ram_source && m->ram_source_set, "CGB has path-local provenance");
        if (m->ram_source && m->ram_source_set) {
            m->ram_source_set(m, (uint32_t)bank3_index, 0x4001);
            m->save(m, saved);
            m->ram_source_set(m, (uint32_t)bank3_index, 0);
            m->load(m, saved);
            CHECK_MSG(m->ram_source(m, (uint32_t)bank3_index) == 0x4001,
                      "snapshot restores path-local ROM provenance");
            m->poke(m, 0xf000, 0x34);
            CHECK_MSG(m->ram_source(m, (uint32_t)bank3_index) == 0x4001,
                      "unchanged mirrored RAM keeps its provenance");
            m->poke(m, 0xf000, 0x35);
            CHECK_MSG(m->ram_source(m, (uint32_t)bank3_index) == 0,
                      "mutation through echo RAM invalidates physical provenance");
            m->load(m, saved);
        }
        CHECK_MSG(m->fetch(m, m->ram_addr(m, (uint32_t)bank1_index)) == 0x12 &&
                  m->fetch(m, m->ram_addr(m, (uint32_t)bank3_index)) == 0x34,
                  "physical WRAM indices read their own banks");
        free(saved);

        m->poke(m, 0xff4f, 0);
        m->poke(m, 0x8000, 0x45);
        m->poke(m, 0xff4f, 1);
        m->poke(m, 0x8000, 0x67);
        int64_t vram1_index = m->ram_index(m, 0x8000);
        CHECK_MSG(m->fetch(m, 0x8000) == 0x67, "VRAM bank 1 selected");
        m->poke(m, 0xff4f, 0);
        CHECK_MSG(m->fetch(m, 0x8000) == 0x45, "VRAM bank 0 retained");
        int64_t vram0_index = m->ram_index(m, 0x8000);
        CHECK_MSG(vram0_index != vram1_index &&
                  m->fetch(m, m->ram_addr(m, (uint32_t)vram0_index)) == 0x45 &&
                  m->fetch(m, m->ram_addr(m, (uint32_t)vram1_index)) == 0x67,
                  "provenance and inspection distinguish VRAM banks");
        m->poke(m, 0xff51, 0x40); m->poke(m, 0xff52, 0);
        m->poke(m, 0xff53, 0); m->poke(m, 0xff54, 0);
        m->poke(m, 0xff55, 0x0f);
        CHECK_MSG(copies == 16 && bytes == 256 && first_src == 0x4000 && last_channel == DP_COPY_GB_VRAM, "GDMA observes actual blocks");
        CHECK_MSG(!memcmp(raw->api->vram, rom + 0x4000, 256), "GDMA moves source bytes");
        copies = bytes = 0;
        raw->api->vram[0] = 0x77;
        m->poke(m, 0xff51, 0x40); m->poke(m, 0xff52, 0);
        m->poke(m, 0xff53, 0x1f); m->poke(m, 0xff54, 0xf0);
        m->poke(m, 0xff55, 1);
        CHECK_MSG(copies == 1 && bytes == 16, "GDMA stops at VRAM destination boundary");
        CHECK_MSG(raw->api->vram[0] == 0x77, "GDMA does not wrap into VRAM start");
        m->poke(m, 0xff4f, 1);
        m->poke(m, 0xff51, 0x40); m->poke(m, 0xff52, 0x10);
        m->poke(m, 0xff53, 0); m->poke(m, 0xff54, 0x20);
        m->poke(m, 0xff55, 0);
        CHECK_MSG(!memcmp(raw->api->vram + 0x2020, rom + 0x4010, 16),
                  "GDMA writes the selected CGB VRAM bank");
        CHECK_MSG(m->ram_source(m, (uint32_t)m->ram_index(m, 0x8020)) == 0x4011,
                  "GDMA records ROM provenance in the selected VRAM bank");
        CHECK_MSG(raw->api->rd(0xff55) == 0xff, "completed GDMA reports idle");
        m->poke(m, 0xff51, 0x40); m->poke(m, 0xff52, 0);
        m->poke(m, 0xff53, 0); m->poke(m, 0xff54, 0x40);
        m->poke(m, 0xff55, 0x81);
        CHECK_MSG(raw->api->rd(0xff55) == 1, "HDMA stays pending with LCD off");
        n = m->state_bytes(m);
        saved = malloc(n);
        CHECK_MSG(saved, "HDMA snapshot allocated");
        if (saved) {
            m->save(m, saved);
            m->poke(m, 0xff55, 0);
            CHECK_MSG(raw->api->rd(0xff55) == 0x81, "stopped HDMA reports remaining blocks");
            m->load(m, saved);
            CHECK_MSG(raw->api->rd(0xff55) == 1, "HDMA snapshot restores active transfer");
            m->poke(m, 0xff55, 0);
            free(saved);
        }
    } else {
        m->poke(m, 0xff75, 0);
        CHECK_MSG(raw->api->rd(0xff75) == 0xff, "DMG does not expose CGB scratch registers");
        CHECK_MSG(raw->api->rd(0xff76) == 0xff && raw->api->rd(0xff77) == 0xff,
                  "DMG does not expose CGB PCM registers");
        CHECK_MSG(m->fetch(m, 0xd000) == 0x34, "DMG ignores SVBK");
        m->poke(m, 0xff51, 0x40); m->poke(m, 0xff55, 0);
        CHECK_MSG(copies == 0, "DMG does not perform CGB DMA");
    }
    copies = bytes = 0;
    m->poke(m, 0xff46, 0x40);
    if (colour) {
        CHECK_MSG(copies == 0, "CGB OAM DMA is not instantaneous");
        raw->api->gbr->cyc += 8 + 4 * 79;
        (void)raw->api->rd(0xff00);
        CHECK_MSG(copies == 80 && bytes == 80, "CGB DMA observes only transferred bytes");
        uint32_t n = m->state_bytes(m);
        uint8_t *saved = malloc(n);
        CHECK_MSG(saved, "DMA snapshot allocated");
        if (saved) {
            m->save(m, saved);
            raw->api->gbr->cyc += 4 * 80;
            (void)raw->api->rd(0xff00);
            CHECK_MSG(bytes == 0xa0, "CGB OAM DMA finishes over time");
            m->load(m, saved);
            bytes = copies = 0;
            raw->api->gbr->cyc += 4 * 80;
            (void)raw->api->rd(0xff00);
            CHECK_MSG(bytes == 80, "CGB OAM DMA resumes after snapshot");
            free(saved);
        }
    } else {
        CHECK_MSG(copies == 1 && bytes == 0xa0 && first_src == 0x4000 && last_channel == DP_COPY_GB_OAM,
                  "DMG OAM DMA observes ROM source");
    }
    CHECK_MSG(!memcmp(raw->api->oam, rom + 0x4000, 0xa0), "OAM DMA copies source bytes");
    if (colour) {
        m->poke(m, 0xde00, rom[0x4000]);
        int64_t source = m->ram_index(m, 0xde00);
        int64_t destination = m->ram_index(m, 0xfe00);
        m->ram_source_set(m, (uint32_t)source, 0x4001);
        copies = bytes = first_src = 0;
        m->poke(m, 0xff46, 0xfe);
        raw->api->gbr->cyc += 8;
        (void)raw->api->rd(0xff00);
        CHECK_MSG(copies == 1 && first_src == 0xde00,
                  "CGB OAM DMA reports the actual WRAM source behind FE00");
        CHECK_MSG(raw->api->oam[0] == rom[0x4000] &&
                  m->ram_source(m, (uint32_t)destination) == 0x4001,
                  "CGB OAM DMA propagates the WRAM source provenance");
    }
    m->close(m);
    free(rom);
}

static void check_timer_counter_wrap(void) {
    uint8_t rom[0x8000] = {0};
    DpMach *raw = dp_mach_open(dp_mach_pick_variant(rom), 0);
    CHECK_MSG(raw, "timer instance opens");
    if (!raw) return;
    dp_mach_set_rom(raw, rom, sizeof rom);
    dp_mach_reset(raw);
    const DpGblApi *api = raw->api;
    api->wr(0xff04, 0);
    api->gbr->cyc = 0x7ffffff0u;
    (void)api->rd(0xff05);
    api->gbr->cyc = 0xffffffe0u;
    (void)api->rd(0xff05);
    api->io[0x05] = 0x20;
    api->io[0x07] = 0x05; // TIMA enabled, one tick per 16 CPU cycles
    api->io[0x0f] = 0;
    api->gbr->cyc = 16;
    CHECK_MSG(api->rd(0xff05) == 0x23 && !(api->io[0x0f] & 4),
              "TIMA counts three ticks across the wrap without requesting an IRQ");
    api->gbr->cyc = 12;
    CHECK_MSG(api->rd(0xff05) == 0x23, "TIMA ignores a temporary cycle rewind");
    dp_mach_close(raw);
}

int main(void) {
    check_mapper_bank_seeds(0, 0x01, 128);
    check_mapper_bank_seeds(0, 0x05, 16);
    check_mapper_bank_seeds(0, 0x19, 512);
    check_machine(0);
    check_pending_input_snapshot(0);
    check_irq_sources(0);
    check_external_serial_probe(0);
    check_internal_serial(0);
    check_time_in_state_hash(0);
    check_video_event_probe(0);
    check_ram_instruction_trace(0);
    check_mbc3_rtc(0, 0);
    check_mbc3_rtc(1, 0);
    check_timer_counter_wrap();
#if TEST_CGB
    check_gambatte_timer_stop();
    check_mapper_bank_seeds(1, 0x01, 128);
    check_mapper_bank_seeds(1, 0x05, 16);
    check_mapper_bank_seeds(1, 0x19, 512);
    check_machine(1);
    check_pending_input_snapshot(1);
    check_irq_sources(1);
    check_external_serial_probe(1);
    check_internal_serial(1);
    check_time_in_state_hash(1);
    check_video_event_probe(1);
    check_ram_instruction_trace(1);
    check_gambatte_halted_frame();
    check_pcm_alu_probe();
    check_cartridge_ram_banks();
    check_small_cartridge_ram(0x05, 0); // MBC2 internal 512 nibbles
    check_small_cartridge_ram(0x12, 1); // MBC3 2 KiB
    check_mbc3_rtc(0, 1);
    check_mbc3_rtc(1, 1);
#endif
    TEST_DONE("gb_banks_dma");
}
