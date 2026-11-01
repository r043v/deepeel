#include "backend.h"
#include <sg1000.h>
#include <string.h>
static int init(const uint8_t *rom,size_t n,uint8_t *ram,int sms) { (void)sms; sg1000_init(rom,n,ram); return 1; }
static z80_t *cpu(void) { return &machine.cpu; }
static uint8_t in(uint16_t p) { return sg1000_in((uint8_t)p); }
static void out(uint16_t p,uint8_t v) { sg1000_out((uint8_t)p,v); }
static void advance(unsigned cycles) { machine.cyc+=cycles; sg_sync(); }
static int64_t rom_index(uint16_t a) {
    if(a>=0xc000 || (a>=0x8000 && machine.cart_ram)) return -1;
    if(machine.rom_mask) a&=machine.rom_mask;
    return a<machine.rom_size ? a : -1;
}
static void save(void *out) { memcpy(out,&machine,sizeof(machine)); }
static void load(const void *in) { memcpy(&machine,in,sizeof(machine)); sg1000_palette_changed(); }
static int32_t ram_index(uint16_t a) {
    if(a>=0xc000) return a&1023;
    if(a>=0x8000 && machine.cart_ram) return 8192+(a&(SG_CART_RAM_SIZE-1));
    return -1;
}
static DpSegaVdpTarget vdp_target(void) {
    DpSegaVdpTarget t={DP_SEGA_VRAM,machine.vdp_address,0xff,0,0,0}; return t;
}
static DpSegaInput input(void) {
    DpSegaInput v={{machine.buttons[0],machine.buttons[1]},0,machine.pause,0}; return v;
}
static void set_input(DpSegaInput v) {
    sg1000_buttons(0,v.pad[0]); sg1000_buttons(1,v.pad[1]); sg1000_pause(v.pause);
}
static unsigned mapper(void) { return DP_SEGA_MAPPER_NONE; }
static unsigned vdp_command(uint8_t second,uint8_t *reg,uint8_t *value) {
    if(!machine.vdp_pending || !(second&128)) return 0;
    *reg=second&7; *value=machine.vdp_latch; return 1;
}
static unsigned irq_sources(void) {
    return (machine.vdp_status&128) && (machine.vdp[1]&32)?DP_SEGA_IRQ_FRAME:0;
}
const DpSegaBackend *dp_sega_backend_sg(void) {
    static const DpSegaBackend ops={init,cpu,sg1000_read,sg1000_write,in,out,advance,rom_index,sizeof(sg1000_t),save,load,ram_index,vdp_target,input,set_input,mapper,vdp_command,irq_sources,DP_SEGA_RAM_TAGS,8192,0xc1,0x80,0x81,NULL,NULL,NULL,NULL};
    return &ops;
}
