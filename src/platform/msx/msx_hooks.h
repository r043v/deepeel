#ifndef DP_MSX_HOOKS_H
#define DP_MSX_HOOKS_H
#include <sg1000.h>
#include <stdint.h>
// Included only by the generated, instrumented MSX BIOS translation unit.
uint8_t dp_msx_bios_read(uint16_t address);
void dp_msx_bios_write(uint16_t address,uint8_t value);
uint8_t dp_msx_bios_vdp_in(uint8_t port);
void dp_msx_bios_vdp_out(uint8_t port,uint8_t value);
void dp_msx_bios_vdp_data(uint8_t value);
uint8_t dp_msx_bios_rom_read(uint32_t offset,uint16_t address,uint8_t value);
uint8_t dp_msx_bios_ram_read(uint16_t address,uint8_t value);
void dp_msx_bios_ram_written(uint16_t address,uint8_t value);
void dp_msx_bios_ay_write(unsigned reg,uint8_t value);
#define sg1000_read dp_msx_bios_read
#define sg1000_write dp_msx_bios_write
#define msx_vdp_in dp_msx_bios_vdp_in
#define msx_vdp_out dp_msx_bios_vdp_out
#define msx_vdp_data dp_msx_bios_vdp_data
#define msx_ay_write dp_msx_bios_ay_write
#endif
