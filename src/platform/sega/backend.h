#ifndef DP_SEGA_BACKEND_H
#define DP_SEGA_BACKEND_H
#include "../../cpu/z80/z80.h"
#include "sega.h"
#include <stddef.h>
#include <stdint.h>
// Runtime instances are singletons: exploration is sequential, not threaded.
enum { DP_SEGA_WRAM, DP_SEGA_SRAM, DP_SEGA_VRAM, DP_SEGA_CRAM };
enum { DP_SEGA_RAM_TAGS=8192+32768 };
enum { DP_SEGA_MAPPER_NONE, DP_SEGA_MAPPER_SEGA, DP_SEGA_MAPPER_CODEMASTERS, DP_SEGA_MAPPER_KOREAN150, DP_Z80_MAPPER_MSX_PLAIN, DP_Z80_MAPPER_MSX_ASCII8, DP_Z80_MAPPER_MSX_ASCII16, DP_Z80_MAPPER_MSX_KONAMI, DP_Z80_MAPPER_MSX_SCC };
enum { DP_SEGA_IRQ_FRAME=1, DP_SEGA_IRQ_LINE=2 };
typedef struct {
    unsigned space;
    uint16_t address;
    uint8_t mask,latch,pair,latch_value;
} DpSegaVdpTarget;
// Switchable ROM geometry for seeding, in 16 KiB physical banks. window_lo and
// window_hi are 16 KiB window bases (inclusive): a ROM word landing there carries
// no bank evidence and may seed every switchable bank's view of its target.
typedef struct {
    uint8_t banks;       // switchable 16 KiB banks (0: the profile never switches)
    uint16_t window_lo;  // lowest switchable window base
    uint16_t window_hi;  // highest switchable window base, inclusive
} DpSegaSeedGeometry;
typedef struct {
    int (*init)(const uint8_t *, size_t, uint8_t *, int);
    z80_t *(*cpu)(void);
    uint8_t (*read)(uint16_t);
    void (*write)(uint16_t,uint8_t);
    uint8_t (*in)(uint16_t);
    void (*out)(uint16_t,uint8_t);
    void (*advance)(unsigned);
    // Physical byte index, compatible with bank * $4000 + (addr & $3FFF).
    // Negative for RAM, unmapped addresses or cartridge RAM.
    int64_t (*rom_index)(uint16_t);
    size_t state_size;
    void (*save)(void *);
    void (*load)(const void *);
    // Normalized physical RAM cell; mirrors and SRAM banks share tags only
    // when they really refer to the same storage. Negative means no RAM.
    int32_t (*ram_index)(uint16_t);
    // Destination of the next data-port write, before its bus side effects.
    DpSegaVdpTarget (*vdp_target)(void);
    DpSegaInput (*input)(void);
    void (*set_input)(DpSegaInput);
    unsigned (*mapper)(void);
    // Inspect the command before its second byte is committed. Return 0 for
    // an address command/first byte, 1 for a register write, 2 if ignored.
    unsigned (*vdp_command)(uint8_t second,uint8_t *reg,uint8_t *value);
    // Enabled, pending VDP sources currently contributing to INT.
    unsigned (*irq_sources)(void);
    // Physical provenance storage geometry; SRAM tags follow WRAM tags.
    size_t ram_tags,wram_tags;
    uint8_t vdp_port_mask,vdp_data_port,vdp_control_port;
    void (*close)(void);
    // MSX: visible cartridge windows in 8 KiB units, including image padding.
    uint8_t (*cartridge_windows)(void);
    // Console memory-control port: current latch and bits effective in this profile.
    // Return zero for other ports. This query has no bus side effects.
    int (*memory_control)(uint16_t port,uint8_t *value,uint8_t *effect_mask);
    // Initialization only: select an explicit DP_SEGA_MAPPER_* board and reset it.
    int (*select_mapper)(unsigned mapper);
    // Query the switchable ROM geometry for seeding (always fills, zeroed when
    // the profile never switches banks). Returns 1, or 0 when unsupported.
    int (*seed_geometry)(DpSegaSeedGeometry *out);
    // Program the mapper so 16 KiB bank `bank` shows at the switchable window
    // covering `addr`, through raw machine writes: unobserved by the explorer
    // instrumentation and absent from the bank timeline. Returns 1 when the
    // latches were applied, 0 when this window/bank is not switchable. The
    // caller verifies the resulting view with rom_index before seeding.
    int (*seed_window)(uint16_t addr, unsigned bank);
} DpSegaBackend;
const DpSegaBackend *dp_sega_backend_sg(void);
const DpSegaBackend *dp_sega_backend_gg(void);
// Second instances (objcopy-namespaced runtime clones): the --jobs parallel
// drain runs one worker per instance. MSX has no clone: jobs fall back.
const DpSegaBackend *dp_sega_backend_sg2(void);
const DpSegaBackend *dp_sega_backend_gg2(void);
#endif
