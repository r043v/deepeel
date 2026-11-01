#ifndef DP_MSX_H
#define DP_MSX_H
#include "../../cpu/z80/z80.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
enum { DP_MSX_AUTO=-1,DP_MSX_PLAIN,DP_MSX_ASCII8,DP_MSX_ASCII16,DP_MSX_KONAMI,DP_MSX_KONAMI_SCC };
typedef struct {
    uint16_t base,init,banks[8];
    uint8_t mapper,slots,visible,ppi_c,ay_latch,ay[16],vdp[8],status,scc_on;
    uint32_t frames,bios_calls,bus_cycles,frame_start,next_event;
} DpMsxInfo;
enum { DP_MSX_BIOS_BEGIN,DP_MSX_BIOS_END,DP_MSX_ROM_READ,DP_MSX_RAM_READ,DP_MSX_RAM_WRITE,DP_MSX_AY_WRITE };
typedef struct { unsigned kind; uint32_t address; uint8_t value; } DpMsxEvent;
typedef void (*DpMsxObserver)(void *user,DpMsxEvent event);
// Read-only observers are not snapshotted. Register after init; close clears
// them. Slot-access events use physical ROM/RAM keys, not the current CPU view.
void dp_msx_observe(DpMsxObserver observer,void *user);
// This MSX1 instance is independent of the SG/SMS/GG instances. The ROM is
// copied and page-padded internally. A cartridge with an INIT routine is required.
int dp_msx_init(const uint8_t *rom,size_t size,int mapper);
void dp_msx_close(void);
z80_t *dp_msx_cpu(void);
uint8_t dp_msx_read(uint16_t address);
void dp_msx_write(uint16_t address,uint8_t value);
uint8_t dp_msx_in(uint16_t port);
void dp_msx_out(uint16_t port,uint8_t value);
// True only for a private BIOS OUT stub visible at the given instruction PC.
int dp_msx_bios_trap(uint16_t pc,uint16_t port);
void dp_msx_advance(unsigned cycles);
int64_t dp_msx_rom_index(uint16_t address);
int32_t dp_msx_ram_index(uint16_t address);
size_t dp_msx_state_size(void);
int dp_msx_save(void *state,size_t size);
int dp_msx_load(const void *state,size_t size);
void dp_msx_buttons(unsigned player,uint8_t pressed);
void dp_msx_return(int pressed);
DpMsxInfo dp_msx_info(void);
void dp_msx_regs(FILE *out);
#endif
