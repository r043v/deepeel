// Sega Z80 hardware metadata. Memory and port buses are distinct.
#ifndef DP_SEGA_H
#define DP_SEGA_H
#include <stdint.h>
#include <stdio.h>
typedef struct {
    uint8_t pad[2]; // up/down/left/right/button1/button2 = bits 0..5
    uint8_t start,pause; // GG Start is polled; SMS/SG Pause raises NMI
    uint8_t reset; // SMS Reset is polled; it is not the CPU reset pin
} DpSegaInput;
typedef struct {
    uint16_t mask, value; // register selected when (address & mask) == value
    uint8_t rw; // 1 read, 2 write
    const char *name, *effect;
} DpSegaPort;
typedef struct {
    const char *id, *name;
    const DpSegaPort *ports;
    unsigned port_count;
    unsigned ram_bytes, vdp_registers, cram_bytes;
    const char *memory, *mapper, *interrupts, *limitations;
} DpSegaSystem;
const DpSegaSystem *dp_sega_system(const char *id);
const DpSegaPort *dp_sega_port(const DpSegaSystem *system, uint16_t port, int write);
// the port tables as native DpIoReg entries (the GB/NES/PCE/MD model)
struct DpPlatform;
const struct DpPlatform *dp_platform_sega(const DpSegaSystem *system);
const struct DpPlatform *dp_platform_msx(void);
void dp_sega_regs(FILE *out, const DpSegaSystem *system);
int dp_z80_command(int argc, char **argv);
#endif
