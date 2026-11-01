#include "msx.h"
#include "explore.h"
#include <sg1000.h>
#include <m0_msx_layout.h>
#include <stdlib.h>
#include <string.h>
_Static_assert((int)DP_MSX_PLAIN==M0_MSX_PLAIN && (int)DP_MSX_ASCII8==M0_MSX_ASCII8 &&
    (int)DP_MSX_ASCII16==M0_MSX_ASCII16 && (int)DP_MSX_KONAMI==M0_MSX_KONAMI &&
    (int)DP_MSX_KONAMI_SCC==M0_MSX_KONAMI_SCC,"MSX mapper IDs changed upstream");
static uint8_t *image;
static size_t image_size;
static uint64_t generation;
static DpMsxObserver observer;
static void *observer_user;
void msx_ay_write(unsigned reg,uint8_t value);
void dp_msx_observe(DpMsxObserver callback,void *user) { observer=callback; observer_user=user; }
static void event(unsigned kind,uint32_t address,uint8_t value) {
    if(observer) observer(observer_user,(DpMsxEvent){kind,address,value});
}
uint8_t dp_msx_bios_read(uint16_t a) { return machine.cpu.read(machine.cpu.user,a); }
void dp_msx_bios_write(uint16_t a,uint8_t value) { machine.cpu.write(machine.cpu.user,a,value); }
uint8_t dp_msx_bios_vdp_in(uint8_t port) { return machine.cpu.in(machine.cpu.user,port); }
void dp_msx_bios_vdp_out(uint8_t port,uint8_t value) { machine.cpu.out(machine.cpu.user,port,value); }
void dp_msx_bios_vdp_data(uint8_t value) { dp_msx_bios_vdp_out(0x98,value); }
uint8_t dp_msx_bios_rom_read(uint32_t offset,uint16_t address,uint8_t value) {
    if(image && offset<image_size && !(machine.scc_on && address>=0x9800 && address<0xa000))
        event(DP_MSX_ROM_READ,offset,value);
    return value;
}
uint8_t dp_msx_bios_ram_read(uint16_t a,uint8_t value) { if(image) event(DP_MSX_RAM_READ,a,value); return value; }
void dp_msx_bios_ram_written(uint16_t a,uint8_t value) { if(image) event(DP_MSX_RAM_WRITE,a,value); }
void dp_msx_bios_ay_write(unsigned reg,uint8_t value) {
    msx_ay_write(reg,value);
    if(image && reg<16) event(DP_MSX_AY_WRITE,reg,value);
}
typedef struct { sg1000_t machine; uint64_t generation; } Snapshot;
static void output_bus(void *user,uint16_t port,uint8_t value) { (void)user; dp_msx_out(port,value); }
int dp_msx_init(const uint8_t *rom,size_t size,int mapper) {
    if(!rom || mapper<DP_MSX_AUTO || mapper>DP_MSX_KONAMI_SCC) return 0;
    m0_msx_layout_t layout=m0_msx_layout(rom,size);
    if(!layout.valid || !layout.init || (mapper==DP_MSX_PLAIN && size>M0_MSX_MAX_UNBANKED)) return 0;
    size_t padded=m0_msx_padded(size);
    uint8_t *next=malloc(padded); if(!next) return 0;
    memset(next,0xff,padded); memcpy(next,rom,size);
    dp_msx_close(); image=next; image_size=size; generation++;
    msx_set_mapper(mapper); sg1000_init(image,size,NULL);
    // INIT must run in the cartridge's slot, including a mapped cartridge
    // whose header points into page 2 rather than the usual page 1.
    unsigned shift=(layout.init>>14)*2;
    sg1000_out(0xa8,(uint8_t)((machine.ppi_a&~(3u<<shift))|(1u<<shift)));
    if(dp_msx_rom_index(layout.init)<0) { dp_msx_close(); return 0; }
    machine.cpu.out=output_bus; return 1;
}
void dp_msx_close(void) {
    observer=NULL; observer_user=NULL;
    free(image); image=NULL; image_size=0; memset(&machine,0,sizeof(machine));
}
z80_t *dp_msx_cpu(void) { return image?&machine.cpu:NULL; }
uint8_t dp_msx_read(uint16_t a) { return image?sg1000_read(a):0xff; }
void dp_msx_write(uint16_t a,uint8_t value) { if(image) sg1000_write(a,value); }
uint8_t dp_msx_in(uint16_t port) { return image?sg1000_in((uint8_t)port):0xff; }
int dp_msx_bios_trap(uint16_t pc,uint16_t port) {
    return image && (port&255)<0x40 && !(machine.ppi_a&3) && pc<MSX_BIOS_BYTES-1 &&
           msx_bios[pc]==0xd3 && msx_bios[pc+1]==(port&255);
}
void dp_msx_out(uint16_t port,uint8_t value) {
    if(!image) return;
    if((port&255)<0x40) {
        // BIOS traps are private OUT stubs, not physical low-port devices.
        // The generic CPU advances PC before invoking the output callback.
        uint16_t stub=(uint16_t)(machine.cpu.pc-2);
        if(!dp_msx_bios_trap(stub,port)) return;
        event(DP_MSX_BIOS_BEGIN,port&255,0);
        sg1000_out((uint8_t)port,value);
        event(DP_MSX_BIOS_END,port&255,0); return;
    }
    sg1000_out((uint8_t)port,value);
}
void dp_msx_advance(unsigned cycles) { if(image) { machine.cyc+=cycles; sg_sync(); } }
int64_t dp_msx_rom_index(uint16_t a) {
    if(!image || a<0x4000 || a>=0xc000 || !(machine.cart_vis&(1u<<(a>>13)))) return -1;
    // The imported runtime has partial SCC read-back. Never attribute this
    // device overlay to cartridge ROM, even if its read falls through there.
    if(machine.msx_mapper==DP_MSX_KONAMI_SCC && machine.scc_on && a>=0x9800 && a<0xa000) return -1;
    uint32_t offset=(uint32_t)machine.page[a>>13]*0x2000u+(a&0x1fff);
    return offset<image_size?(int64_t)offset:-1;
}
int32_t dp_msx_ram_index(uint16_t a) { return image && machine.wr_win[a>>13]?(int32_t)a:-1; }
size_t dp_msx_state_size(void) { return sizeof(Snapshot); }
int dp_msx_save(void *state,size_t size) {
    if(!image || !state || size!=sizeof(Snapshot)) return 0;
    Snapshot *saved=state; memset(saved,0,size); saved->machine=machine; saved->generation=generation; return 1;
}
int dp_msx_load(const void *state,size_t size) {
    if(!image || !state || size!=sizeof(Snapshot)) return 0;
    // Snapshots are private to this ROM instance, never a portable file format.
    const Snapshot *saved=state;
    if(saved->generation!=generation || saved->machine.rom!=image || saved->machine.rom_size!=image_size) return 0;
    machine=saved->machine; msx_state_loaded(); sg1000_palette_changed(); return 1;
}
void dp_msx_buttons(unsigned player,uint8_t pressed) { if(image && player<2) sg1000_buttons(player,pressed&63); }
void dp_msx_return(int pressed) { if(image) sg1000_pause(pressed); }
DpMsxInfo dp_msx_info(void) {
    DpMsxInfo info={0}; if(!image) return info;
    info.base=machine.msx_base; info.init=machine.msx_init; info.mapper=machine.msx_mapper;
    memcpy(info.banks,machine.page,sizeof(machine.page));
    info.slots=machine.ppi_a; info.visible=machine.cart_vis; info.ppi_c=machine.ppi_c;
    info.ay_latch=machine.ay_latch; memcpy(info.ay,machine.ay_reg,sizeof(info.ay));
    memcpy(info.vdp,machine.vdp,sizeof(info.vdp)); info.status=machine.vdp_status;
    info.scc_on=machine.scc_on; info.frames=machine.frames; info.bios_calls=machine.msx_calls;
    info.bus_cycles=machine.cyc; info.frame_start=machine.frame_start; info.next_event=machine.next_evt; return info;
}

// Adapter for the shared bounded Z80 explorer. The runtime itself stays isolated.
static int explore_init(const uint8_t *rom,size_t size,uint8_t *ram,int mapper) {
    (void)ram; return dp_msx_init(rom,size,mapper);
}
static void explore_save(void *state) { dp_msx_save(state,sizeof(Snapshot)); }
static void explore_load(const void *state) { dp_msx_load(state,sizeof(Snapshot)); }
static DpSegaVdpTarget explore_target(void) {
    return (DpSegaVdpTarget){DP_SEGA_VRAM,machine.vdp_address,0xff,0,0,0};
}
static DpSegaInput explore_input(void) {
    return (DpSegaInput){{machine.buttons[0],machine.buttons[1]},machine.pause,0,0};
}
static void explore_set_input(DpSegaInput input) {
    dp_msx_buttons(0,input.pad[0]); dp_msx_buttons(1,input.pad[1]); dp_msx_return(input.start);
}
static unsigned explore_mapper(void) {
    return DP_Z80_MAPPER_MSX_PLAIN+machine.msx_mapper;
}
static unsigned explore_command(uint8_t second,uint8_t *reg,uint8_t *value) {
    if(!machine.vdp_pending || !(second&128)) return 0;
    *reg=second&7; *value=machine.vdp_latch; return 1;
}
static unsigned explore_irq(void) {
    return (machine.vdp_status&128) && (machine.vdp[1]&32)?DP_SEGA_IRQ_FRAME:0;
}
static uint8_t explore_windows(void) { return machine.cart_vis; }
const DpSegaBackend *dp_msx_backend(void) {
    static const DpSegaBackend ops={explore_init,dp_msx_cpu,dp_msx_read,dp_msx_write,
        dp_msx_in,dp_msx_out,dp_msx_advance,dp_msx_rom_index,sizeof(Snapshot),explore_save,explore_load,
        dp_msx_ram_index,explore_target,explore_input,explore_set_input,explore_mapper,explore_command,
        explore_irq,65536,65536,0xff,0x98,0x99,dp_msx_close,explore_windows,NULL,NULL};
    return &ops;
}
