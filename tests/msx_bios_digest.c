#include "../src/platform/msx/msx.h"
#include <stdio.h>
#include <string.h>
static uint8_t rom[0x8000];
static uint64_t mix(uint64_t h,uint64_t v) { return (h^v)*1099511628211ull; }
static int call(uint16_t entry) {
    z80_t *c=dp_msx_cpu(); c->halted=0; c->iff1=c->iff2=0; c->pc=entry; c->sp=0xf000;
    dp_msx_write(0xf000,0x10); dp_msx_write(0xf001,0x40);
    for(unsigned steps=0;steps<64 && !c->fault && !c->halted;steps++) {
        unsigned cycles=z80_step(c); dp_msx_advance(cycles);
    }
    return c->halted && !c->fault;
}
static void digest(unsigned checkpoint) {
    uint64_t h=14695981039346656037ull; z80_t *c=dp_msx_cpu();
    const uint64_t regs[]={c->bc,c->de,c->hl,c->a,c->f,c->bc2,c->de2,c->hl2,c->af2,c->ix,c->iy,c->sp,c->pc,
        c->i,c->r,c->im,c->iff1,c->iff2,c->halted,c->ei_delay,c->irq,c->nmi,c->irq_vector,c->fault,c->r7,c->cycles};
    for(unsigned j=0;j<sizeof(regs)/sizeof(regs[0]);j++) h=mix(h,regs[j]);
    DpMsxInfo info=dp_msx_info();
    h=mix(h,info.slots); h=mix(h,info.ppi_c); h=mix(h,info.frames); h=mix(h,info.bios_calls);
    h=mix(h,info.status); h=mix(h,info.ay_latch);
    h=mix(h,info.bus_cycles); h=mix(h,info.frame_start); h=mix(h,info.next_event);
    for(unsigned j=0;j<8;j++) h=mix(h,info.vdp[j]);
    for(unsigned j=0;j<16;j++) h=mix(h,info.ay[j]);
    for(unsigned a=0;a<65536;a++) h=mix(h,dp_msx_read((uint16_t)a));
    dp_msx_out(0xa8,0xff);
    for(unsigned a=0;a<65536;a++) h=mix(h,dp_msx_read((uint16_t)a));
    dp_msx_out(0xa8,info.slots);
    dp_msx_out(0x99,0); dp_msx_out(0x99,0);
    for(unsigned a=0;a<16384;a++) h=mix(h,dp_msx_in(0x98));
    printf("%u %016llx\n",checkpoint,(unsigned long long)h);
}
int main(void) {
    memset(rom,0,sizeof(rom)); rom[0]='A'; rom[1]='B'; rom[2]=0x10; rom[3]=0x40; rom[0x10]=0x76;
    for(unsigned j=0x4000;j<sizeof(rom);j++) rom[j]=(uint8_t)(j^(j>>3));
    if(!dp_msx_init(rom,sizeof(rom),DP_MSX_PLAIN)) return 1;
    z80_t *c=dp_msx_cpu(); dp_msx_out(0xa8,0xd4); digest(0);
    c->hl=0x8000; c->de=0x1234; c->bc=257; if(!call(0x5c)) return 1; digest(1);
    c->hl=0x3fe0; c->bc=64; c->a=0x55; if(!call(0x56)) return 1; digest(2);
    c->hl=0x3fe0; c->de=0xc100; c->bc=64; if(!call(0x59)) return 1; digest(3);
    c->a=1; c->hl=0x8000; if(!call(0x0c)) return 1; digest(4);
    c->a=3; c->hl=0x4000; c->e=0xaa; if(!call(0x14)) return 1; digest(5);
    c->a=8; c->e=15; if(!call(0x93)) return 1; digest(6);
    if(!call(0x90)) return 1; digest(7);
    if(!call(0x6f)) return 1; digest(8);
    if(!call(0x69)) return 1; digest(9);
    c->a=3; if(!call(0x5f)) return 1; digest(10);
    dp_msx_close(); return 0;
}
