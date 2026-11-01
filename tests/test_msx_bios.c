#include "../src/platform/msx/msx.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
static uint8_t rom[0x8000];
static unsigned data_reads,ram_writes,vdp_reads,vdp_writes,control_writes;
static unsigned events[DP_MSX_AY_WRITE+1];
static DpMsxEvent last[DP_MSX_AY_WRITE+1];
static unsigned active,errors;
static void observe(void *user,DpMsxEvent event) {
    CHECK(user==rom); CHECK(event.kind<=DP_MSX_AY_WRITE);
    if(event.kind>DP_MSX_AY_WRITE) return;
    events[event.kind]++; last[event.kind]=event;
    if(event.kind==DP_MSX_BIOS_BEGIN) { if(active) errors++; active=1; }
    if(event.kind==DP_MSX_BIOS_END) { if(!active) errors++; active=0; }
}
static uint8_t read_bus(void *u,uint16_t a) {
    CHECK(u==rom); if(a>=0x8000 && a<0x8004) data_reads++;
    return dp_msx_read(a);
}
static void write_bus(void *u,uint16_t a,uint8_t v) {
    CHECK(u==rom); if(a>=0xc000 && a<0xc004) ram_writes++;
    dp_msx_write(a,v);
}
static uint8_t in_bus(void *u,uint16_t p) {
    CHECK(u==rom); if((p&255)==0x98) vdp_reads++;
    return dp_msx_in(p);
}
static void out_bus(void *u,uint16_t p,uint8_t v) {
    CHECK(u==rom);
    if((p&255)==0x98) vdp_writes++;
    if((p&255)==0x99) control_writes++;
    dp_msx_out(p,v);
}
static void counters(void) {
    data_reads=ram_writes=vdp_reads=vdp_writes=control_writes=0;
    memset(events,0,sizeof(events)); memset(last,0,sizeof(last)); active=errors=0;
}
static void init(void) {
    memset(rom,0,sizeof(rom)); rom[0]='A'; rom[1]='B'; rom[2]=0x10; rom[3]=0x40; rom[0x10]=0x76;
    for(unsigned i=0;i<4;i++) rom[0x4000+i]=(uint8_t)(0x50+i);
    CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_PLAIN));
    dp_msx_observe(observe,rom);
    z80_t *c=dp_msx_cpu(); c->user=rom; c->read=read_bus; c->write=write_bus; c->in=in_bus; c->out=out_bus;
    counters();
}
static void call(uint16_t entry) {
    z80_t *c=dp_msx_cpu(); c->halted=0; c->pc=entry; c->sp=0xf000;
    dp_msx_write(0xf000,0x10); dp_msx_write(0xf001,0x40);
    for(unsigned step=0;step<32 && !c->halted && !c->fault;step++) {
        unsigned cycles=z80_step(c); dp_msx_advance(cycles);
    }
    CHECK(c->halted && !c->fault); CHECK(!active && !errors);
    CHECK(events[DP_MSX_BIOS_BEGIN]==1 && events[DP_MSX_BIOS_END]==1);
    CHECK(last[DP_MSX_BIOS_BEGIN].address==last[DP_MSX_BIOS_END].address);
}
static void copies(void) {
    init(); dp_msx_out(0xa8,0xd4);
    z80_t *c=dp_msx_cpu(); c->hl=0x8000; c->de=0x1234; c->bc=4; call(0x5c);
    CHECK(data_reads==4 && vdp_writes==4 && control_writes==2);
    CHECK(events[DP_MSX_ROM_READ]==0); // mapped reads use the CPU bus once
    dp_msx_out(0x99,0x34); dp_msx_out(0x99,0x12);
    for(unsigned i=0;i<4;i++) CHECK(dp_msx_in(0x98)==0x50+i);
    counters(); c->hl=0x1234; c->de=0xc000; c->bc=4; call(0x59);
    CHECK(vdp_reads==4 && ram_writes==4 && control_writes==2);
    CHECK(!vdp_writes && !data_reads);
    for(unsigned i=0;i<4;i++) CHECK(dp_msx_read(0xc000+i)==0x50+i);
    counters(); c->hl=0x2000; c->bc=0; c->a=0x66; call(0x56);
    CHECK(!vdp_writes && control_writes==2); // BC=0 is zero bytes in this simulated BIOS
    counters(); c->hl=0x2000; c->bc=5; c->a=0x66; call(0x56); CHECK(vdp_writes==5);
}
static void slots(void) {
    init(); z80_t *c=dp_msx_cpu();
    CHECK(dp_msx_rom_index(0x8000)==-1); // page 2 is RAM in the CPU view
    c->a=1; c->hl=0x8002; call(0x0c);
    CHECK(c->a==0x52 && !data_reads && events[DP_MSX_ROM_READ]==1);
    CHECK(last[DP_MSX_ROM_READ].address==0x4002 && last[DP_MSX_ROM_READ].value==0x52);
    counters(); c->a=3; c->hl=0x4000; c->e=0x7b; call(0x14);
    CHECK(events[DP_MSX_RAM_WRITE]==1 && last[DP_MSX_RAM_WRITE].address==0x4000);
    CHECK(dp_msx_read(0x4000)=='A'); // the visible cartridge was not written
    counters(); c->a=3; c->hl=0x4000; call(0x0c);
    CHECK(c->a==0x7b && events[DP_MSX_RAM_READ]==1 && !events[DP_MSX_ROM_READ]);
    CHECK(last[DP_MSX_RAM_READ].address==0x4000);
    CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_KONAMI_SCC));
    dp_msx_observe(observe,rom); c=dp_msx_cpu();
    c->user=rom; c->read=read_bus; c->write=write_bus; c->in=in_bus; c->out=out_bus;
    dp_msx_out(0xa8,0xd4); dp_msx_write(0x9000,0x3f); counters();
    c->a=1; c->hl=0x9800; call(0x0c);
    CHECK(!events[DP_MSX_ROM_READ]); // partial SCC read-back must not invent a ROM origin
}
static void ay_and_lifecycle(void) {
    init(); z80_t *c=dp_msx_cpu(); c->a=8; c->e=0x0f; call(0x93);
    CHECK(events[DP_MSX_AY_WRITE]==1 && last[DP_MSX_AY_WRITE].address==8);
    CHECK(last[DP_MSX_AY_WRITE].value==15 && dp_msx_info().ay[8]==15);
    counters(); call(0x90); CHECK(events[DP_MSX_AY_WRITE]==15);
    counters(); dp_msx_out(0xa0,9); dp_msx_out(0xa1,7);
    CHECK(events[DP_MSX_AY_WRITE]==1 && !events[DP_MSX_BIOS_BEGIN]);
    size_t bytes=dp_msx_state_size(); void *state=malloc(bytes); CHECK(state); if(!state) exit(1);
    CHECK(dp_msx_save(state,bytes)); dp_msx_observe(NULL,NULL);
    CHECK(dp_msx_load(state,bytes)); free(state); // restoring a path must not restore its observer
    dp_msx_out(0xa1,8); CHECK(events[DP_MSX_AY_WRITE]==1);
    dp_msx_observe(observe,rom); dp_msx_close(); counters();
    CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_PLAIN)); dp_msx_out(0xa0,9); dp_msx_out(0xa1,7);
    CHECK(!events[DP_MSX_AY_WRITE]); // observer does not survive another ROM init
    dp_msx_close();
}
int main(void) {
    copies(); slots(); ay_and_lifecycle(); TEST_DONE("msx_bios");
}
