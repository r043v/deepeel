#include "backend.h"
#include <gg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
// The bridge steps the generic CPU and instruments its bus callbacks. The
// runtime's separate block interpreter/profiler is not used by this bridge.
void gg_rom_trace(uint32_t offset) { (void)offset; }
void gg_rom_trace_fetch(uint32_t offset) { (void)offset; }
void gg_rom_trace_insn(uint32_t offset) { (void)offset; }
void gg_cart_trace(uint32_t offset,unsigned write) { (void)offset; (void)write; }
// The imported runtime has only the first pad. Extra SMS pins belong to the
// bridge snapshot too; the generic CPU reaches them through this input bus.
static uint8_t second_pad,reset_button,memory_control;
typedef struct { gg_t core; uint8_t second_pad,reset_button,memory_control; } State;
static int cartridge_enabled(void) { return !GG_IS_SMS || !(memory_control&0x40); }
static int memory_port(uint16_t p) { return GG_IS_SMS?(p&0xc1)==0:(p&255)==0x3e; }
static uint8_t read(uint16_t a) {
    if(a>=0xc000) return memory_control&0x10?0xff:machine.ram[a&0x1fff];
    return cartridge_enabled()?gg_read(a):0xff;
}
static void write(uint16_t a,uint8_t v) {
    static int wr_trace=-1;
    if(wr_trace<0) wr_trace=getenv("GG_WR_TRACE")?1:0;
    if(wr_trace&&a>=0xc0c0&&a<=0xc0d0)
        fprintf(stderr,"wr %04x=%02x ctl=%02x pc=%04x\n",a,v,memory_control,machine.cpu.pc);
    if(a>=0xc000) {
        if(!(memory_control&0x10)) machine.ram[a&0x1fff]=v;
        else if(wr_trace&&a>=0xc0c0&&a<=0xc0d0) fprintf(stderr,"wr %04x DROPPED ctl=%02x\n",a,memory_control);
        // The cartridge mapper and internal RAM have independent enables.
        if(cartridge_enabled() && (GG_KOREAN150?(a==0xfff3 || a==0xfffc):
            (a>=0xfffc && !GG_CODEMASTERS))) gg_mapper_write(a,v);
    } else if(cartridge_enabled()) gg_write(a,v);
}
static uint8_t read_bus(void *u,uint16_t a) { (void)u; return read(a); }
static void write_bus(void *u,uint16_t a,uint8_t v) { (void)u; write(a,v); }
static void out(uint16_t p,uint8_t v);
static void output_bus(void *u,uint16_t p,uint8_t v) { (void)u; out(p,v); }
static uint8_t in(uint16_t p);
static uint8_t input_bus(void *user,uint16_t p) { (void)user; return in(p); }
static int init(const uint8_t *rom,size_t n,uint8_t *ram,int sms) {
    second_pad=reset_button=0; gg_init_system(rom,n,ram,sms);
    // BIOS-less post-boot profile: absent BIOS/card/expansion, cartridge and RAM on.
    memory_control=sms?0xab:0xa8;
    machine.cpu.in=input_bus; machine.cpu.out=output_bus;
    machine.cpu.read=read_bus; machine.cpu.write=write_bus;
    return 1;
}
static z80_t *cpu(void) { return &machine.cpu; }
static uint8_t in(uint16_t p) {
    if(GG_IS_SMS && (p&0xc0)==0xc0) {
        if(memory_control&4) return 0xff;
        unsigned control=machine.io_ctl;
        if(!(p&1)) {
            uint8_t value=(uint8_t)~((machine.buttons&63)|((second_pad&3)<<6));
            if(!(control&1)) value=(uint8_t)((value&~0x20)|((control&0x10)<<1));
            return value;
        }
        uint8_t value=(uint8_t)(0x20|(!reset_button<<4)|((~(second_pad>>2))&15));
        if(!(control&4)) value=(uint8_t)((value&~8)|((control&0x40)>>3));
        unsigned tha=(control&2)?1:(control>>5)&1,thb=(control&8)?1:(control>>7)&1;
        return (uint8_t)(value|(tha<<6)|(thb<<7));
    }
    return gg_in((uint8_t)p);
}
static void out(uint16_t p,uint8_t v) {
    if(memory_port(p)) memory_control=v; else gg_out((uint8_t)p,v);
}
static void advance(unsigned cycles) { machine.cyc+=cycles; sg_sync(); }
static int64_t rom_index(uint16_t a) {
    if(!cartridge_enabled() || a>=0xc000 || (!GG_KOREAN150 && a>=0x8000 && (machine.ram_ctl&8))) return -1;
    uint32_t index = a<0x400 && GG_FIXED_KIB ? a :
        ((uint32_t)machine.page[a>>GG_BANK_SHIFT]<<GG_BANK_SHIFT)+(a&GG_BANK_MASK);
    return index<machine.rom_size ? (int64_t)index : -1;
}
static void save(void *out) {
    State *s=out; memset(s,0,sizeof(*s)); s->core=machine; s->second_pad=second_pad; s->reset_button=reset_button; s->memory_control=memory_control;
}
static DpSegaWatch W;
static uint16_t watch_store[GG_ROW_STRIDE];
static int watch_draw(void *u,unsigned y) { (void)u; (void)y; return 1; }
static void watch_row(void *u,unsigned y,uint16_t *p) {
    (void)u; dp_sega_watch_row(&W,y-GG_WIN_TOP,p,GG_WIDTH,(int)GG_WIN_LINES);
}
static void watch_frame(void *u) { (void)u; dp_sega_watch_frame(&W); }
static void hook(void) {
    if(!W.every) return;
    machine.row_out=watch_store+GG_ROW_PAD; machine.row_tmp=NULL; machine.draw_line=watch_draw; machine.row=watch_row; machine.frame=watch_frame;
}
static void watch(unsigned every,uint64_t *published) { W.every=every; W.published=published; hook(); }
static void load(const void *in) {
    const State *s=in; machine=s->core; second_pad=s->second_pad; reset_button=s->reset_button; memory_control=s->memory_control;
    hook(); gg_remap(); gg_vdp_restore();
}
static int32_t ram_index(uint16_t a) {
    if(a>=0xc000) return memory_control&0x10?-1:(int32_t)(a&0x1fff);
    if(!cartridge_enabled()) return -1;
    if(!GG_KOREAN150 && a>=0x8000 && (machine.ram_ctl&8) && machine.cart_ram)
        return 8192+(int32_t)gg_cart_offset(a,machine.ram_ctl);
    return -1;
}
static DpSegaVdpTarget vdp_target(void) {
    DpSegaVdpTarget t={DP_SEGA_VRAM,machine.vdp_address,0xff,0,0,0};
    if(machine.vdp_code==3) {
        t.space=DP_SEGA_CRAM;
        t.address=machine.vdp_address&(GG_IS_SMS?31:63);
        if(GG_IS_SMS) t.mask=0x3f;
        else if(t.address&1) { t.mask=0x0f; t.pair=1; t.latch_value=machine.cram_latch; }
        else t.latch=1;
    }
    return t;
}
static DpSegaInput input(void) {
    DpSegaInput v={{machine.buttons&63,GG_IS_SMS?second_pad:0},!GG_IS_SMS && !!(machine.buttons&GG_START),
        GG_IS_SMS && !!(machine.buttons&GG_PAUSE),GG_IS_SMS?reset_button:0}; return v;
}
static void set_input(DpSegaInput v) {
    gg_buttons((v.pad[0]&63)|(!GG_IS_SMS && v.start?GG_START:0)|(GG_IS_SMS && v.pause?GG_PAUSE:0));
    if(GG_IS_SMS) { second_pad=v.pad[1]&63; reset_button=!!v.reset; }
}
static unsigned mapper(void) {
    return GG_KOREAN150?DP_SEGA_MAPPER_KOREAN150:GG_CODEMASTERS?DP_SEGA_MAPPER_CODEMASTERS:DP_SEGA_MAPPER_SEGA;
}
static unsigned vdp_command(uint8_t second,uint8_t *reg,uint8_t *value) {
    if(!machine.vdp_pending || (second>>6)!=2) return 0;
    *reg=second&15; *value=machine.vdp_latch; return *reg<=10?1:2;
}
static unsigned irq_sources(void) {
    return (((machine.vdp_status&128) && (machine.vdp[1]&32))?DP_SEGA_IRQ_FRAME:0)|
        ((machine.line_irq && (machine.vdp[0]&16))?DP_SEGA_IRQ_LINE:0);
}
static int control_state(uint16_t port,uint8_t *value,uint8_t *effect_mask) {
    if(!memory_port(port)) return 0;
    *value=memory_control; *effect_mask=GG_IS_SMS?0x54:0x10; return 1;
}
static int select_mapper(unsigned selected) {
    if(selected<DP_SEGA_MAPPER_SEGA || selected>DP_SEGA_MAPPER_KOREAN150 ||
       (selected==DP_SEGA_MAPPER_KOREAN150 && machine.rom_size>0x200000)) return 0;
    machine.mapper=(uint8_t)(selected-DP_SEGA_MAPPER_SEGA);
    gg_reset(); return 1;
}
// Sega mapper: $FFFD-$FFFF switch their own 16 KiB slot (the first $400 stays
// at bank 0). Codemasters switches all three slots from $0000/$4000/$8000.
// Korean150 switches the low 32 KiB in 8 KiB steps through $FFF3/$FFFC.
static int seed_geometry(DpSegaSeedGeometry *g) {
    memset(g,0,sizeof(*g));
    unsigned banks=machine.rom_size>>14;
    if(!banks) return 1;
    g->banks=(uint8_t)(banks>255?255:banks);
    if(GG_KOREAN150) { g->window_lo=0x0000; g->window_hi=0x4000; }
    else if(GG_CODEMASTERS) { g->window_lo=0x0000; g->window_hi=0x8000; }
    else { g->window_lo=0x4000; g->window_hi=0x8000; }
    return 1;
}
static int seed_window(uint16_t addr, unsigned bank) {
    if(addr>=0xc000 || bank>255) return 0;
    if(GG_KOREAN150) {
        if(addr>=0x8000) return 0;
        // Mode $00 keeps ROM enabled and shows 8 KiB page 2*bank+(s&1) at slot
        // s: the requested 16 KiB bank lands in both halves of its window.
        write(0xfffc,0x00); write(0xfff3,(uint8_t)bank); return 1;
    }
    if(GG_CODEMASTERS) { write((uint16_t)(addr&0xc000),(uint8_t)bank); return 1; }
    if(addr<0x400) return 0; // fixed bank 0
    write((uint16_t)(0xfffd+(addr>>14)),(uint8_t)bank); return 1;
}
static uint64_t screen_key(void) {
    if(!machine.sms) return dp_sega_name_key(machine.vram,(machine.vdp[2]&14u)<<10,1792,2,machine.vdp[8],machine.vdp[9]);
    return dp_sega_name_key(machine.vram,(machine.vdp[2]&14u)<<10,1792,2,machine.vdp[8],machine.vdp[9]);
}
static const uint8_t *ram_view(size_t *n) { *n=sizeof(machine.ram); return machine.ram; }
const DpSegaBackend *dp_sega_backend_gg(void) {
    static const DpSegaBackend ops={init,cpu,read,write,in,out,advance,rom_index,sizeof(State),save,load,ram_index,vdp_target,input,set_input,mapper,vdp_command,irq_sources,DP_SEGA_RAM_TAGS,8192,0xc1,0x80,0x81,NULL,NULL,control_state,select_mapper,seed_geometry,seed_window,NULL,NULL,NULL,screen_key,ram_view,watch};
    return &ops;
}
