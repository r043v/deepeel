#include "../src/platform/msx/msx.h"
#include "../src/platform/msx/explore.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
static uint8_t rom[0x10000],flags[0x10000];
static DpSegaClosureStats stats;
static void fixture(uint16_t init) {
    memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags));
    rom[0]='A'; rom[1]='B'; rom[2]=(uint8_t)init; rom[3]=(uint8_t)(init>>8);
    memset(rom+4,0,12);
}
static DpSegaClosure *create(int mapper,size_t size,DpSegaClosureOptions options) {
    memset(flags,0,sizeof(flags));
    const DpSegaBackend *b=dp_msx_backend(); CHECK(b->init(rom,size,NULL,mapper));
    DpSegaClosure *c=dp_sega_closure_create(b,dp_msx_system(),rom,size,flags,options,&stats);
    CHECK(c); return c;
}
static DpSegaClosureOptions limits(void) { return (DpSegaClosureOptions){256,32,8,16}; }
static void finish(DpSegaClosure *c) {
    z80_t before=*dp_msx_cpu(); DpMsxInfo info=dp_msx_info();
    dp_sega_closure_observe(c); dp_sega_closure_run(c);
    DpMsxInfo after=dp_msx_info();
    CHECK(!memcmp(&before,dp_msx_cpu(),sizeof(before)));
    CHECK(info.slots==after.slots && info.visible==after.visible && info.frames==after.frames);
    CHECK(info.bios_calls==after.bios_calls && info.bus_cycles==after.bus_cycles &&
          info.frame_start==after.frame_start && info.next_event==after.next_event);
    CHECK(!memcmp(info.banks,after.banks,sizeof(info.banks)) &&
          !memcmp(info.vdp,after.vdp,sizeof(info.vdp)) && !memcmp(info.ay,after.ay,sizeof(info.ay)));
    CHECK(stats.nodes==stats.processed && stats.nodes<=stats.limits.nodes);
    uint64_t processed=stats.processed; dp_sega_closure_run(c); CHECK(stats.processed==processed);
    dp_sega_closure_free(c); dp_msx_close();
}
static void ram_stack(void) {
    for(unsigned variant=0;variant<5;variant++) {
        fixture(0x4010);
        uint16_t sp=variant==4?1:0x4000;
        const uint8_t entry[]={0x31,(uint8_t)sp,(uint8_t)(sp>>8),0xcd,0x00,0x42,0x76};
        memcpy(rom+0x10,entry,sizeof(entry)); rom[0x200]=0xc9;
        if(variant==2 || variant==3 || variant==4) {
            uint16_t address=variant==3?0xcffe:variant==4?0:0x3ffe;
            const uint8_t corrupt[]={0x32,(uint8_t)address,(uint8_t)(address>>8),0xc9};
            memcpy(rom+0x200,corrupt,sizeof(corrupt));
        }
        DpSegaClosure *c=create(DP_MSX_PLAIN,0x8000,limits());
        if(variant) { dp_msx_out(0xa8,0xf7); dp_sega_closure_invalidate(c); }
        finish(c);
        CHECK(flags[0x200]==DP_SEGA_STATIC);
        if(variant==1 || variant==3) { CHECK(flags[0x16]==DP_SEGA_STATIC && stats.returns_resolved==1); }
        else { CHECK(!flags[0x16]); if(variant==2 || variant==4) CHECK(stats.stack_invalidations); }
    }
    // Wrapped stack word $FFFF/$0000 is valid only when both pages are RAM.
    fixture(0x4010); const uint8_t wrapped[]={0x31,1,0,0xcd,0,0x42,0x76};
    memcpy(rom+0x10,wrapped,sizeof(wrapped)); rom[0x200]=0xc9;
    DpSegaClosure *c=create(DP_MSX_PLAIN,0x8000,limits()); dp_msx_out(0xa8,0xf7); finish(c);
    CHECK(flags[0x16]==DP_SEGA_STATIC && stats.returns_resolved==1);
}
static void views_and_hidden_ram(void) {
    fixture(0x4010); const uint8_t entry[]={0x31,0,0x40,0xcd,0,0x42,0x76};
    memcpy(rom+0x10,entry,sizeof(entry)); rom[0x200]=0xc9;
    DpSegaClosure *c=create(DP_MSX_PLAIN,0x8000,limits());
    dp_sega_closure_observe(c);
    dp_msx_out(0xa8,0xf7); dp_sega_closure_invalidate(c); dp_sega_closure_observe(c);
    CHECK(stats.views==2); finish(c);
    CHECK(stats.returns_resolved==1 && flags[0x16]==DP_SEGA_STATIC);
    // Place code in page 2 and stack in page 1; low RAM is not a Sega mirror.
    fixture(0x8010); const uint8_t low[]={0x31,0,0x80,0xcd,0,0x82,0x76};
    memcpy(rom+0x4010,low,sizeof(low)); rom[0x4200]=0xc9;
    c=create(DP_MSX_PLAIN,0x8000,limits()); dp_msx_out(0xa8,0xdc); finish(c);
    CHECK(flags[0x4016]==DP_SEGA_STATIC && stats.returns_resolved==1);
    // With ROM visible instead, CALL cannot establish a RAM return proof.
    memset(flags,0,sizeof(flags)); c=create(DP_MSX_PLAIN,0x8000,limits()); finish(c);
    CHECK(!flags[0x4016]);
}
static void mappers(void) {
    const uint16_t reg[]={0,0x6800,0x7000,0x6000,0x9000};
    const uint16_t target[]={0,0x6100,0x8100,0x6100,0x8100};
    for(int mapper=DP_MSX_ASCII8;mapper<=DP_MSX_KONAMI_SCC;mapper++) {
        for(unsigned safe=0;safe<2;safe++) {
            fixture(0x4010); uint16_t address=safe?0x5800:reg[mapper],pc=target[mapper];
            const uint8_t code[]={0x32,(uint8_t)address,(uint8_t)(address>>8),0xc3,(uint8_t)pc,(uint8_t)(pc>>8)};
            memcpy(rom+0x10,code,sizeof(code));
            for(unsigned bank=0;bank<sizeof(rom)/0x2000;bank++) rom[bank*0x2000+0x100]=0x76;
            DpSegaClosure *c=create(mapper,sizeof(rom),limits()); dp_msx_out(0xa8,0xd4);
            int64_t index=dp_msx_rom_index(pc); CHECK(index>=0); if(index<0) exit(1);
            CHECK(dp_msx_read(pc)==0x76);
            finish(c);
            CHECK(flags[0x13]==DP_SEGA_STATIC);
            if(safe) { CHECK(flags[index]==DP_SEGA_STATIC && !stats.map_barriers); }
            else { CHECK(!flags[index] && stats.map_barriers && stats.ambiguous); }
        }
    }
    // A mapper decoder in a hidden cartridge slot receives a RAM store.
    fixture(0x8010); const uint8_t hidden[]={0x32,0,0x68,0x76};
    memcpy(rom+0x10,hidden,sizeof(hidden));
    DpSegaClosure *c=create(DP_MSX_ASCII8,sizeof(rom),limits()); dp_msx_out(0xa8,0xdc); finish(c);
    CHECK(flags[0x13]==DP_SEGA_STATIC && !stats.map_barriers);
    // Plain ROM cannot change banks through memory writes, including unknown ones.
    fixture(0x4010); const uint8_t plain[]={0x77,0x76}; memcpy(rom+0x10,plain,sizeof(plain));
    c=create(DP_MSX_PLAIN,0x8000,limits()); finish(c);
    CHECK(flags[0x11]==DP_SEGA_STATIC && !stats.map_barriers);
    // Konami's fixed first bank survives an unknown memory store.
    c=create(DP_MSX_KONAMI,sizeof(rom),limits()); finish(c);
    CHECK(flags[0x11]==DP_SEGA_STATIC);
}
static void ports_and_bios(void) {
    const uint8_t ports[]={0x98,0x99,0xa0,0xa1,0xa8,0xab,1};
    for(unsigned i=0;i<sizeof(ports);i++) {
        fixture(0x4010); const uint8_t code[]={0xd3,ports[i],0x76}; memcpy(rom+0x10,code,sizeof(code));
        DpSegaClosure *c=create(DP_MSX_PLAIN,0x8000,limits()); finish(c);
        if(ports[i]==0xa8) CHECK(!flags[0x12] && stats.map_barriers);
        else CHECK(flags[0x12]==DP_SEGA_STATIC && !stats.map_barriers);
    }
    for(unsigned variant=0;variant<3;variant++) {
        fixture(0x4010); const uint8_t code[]={0xed,0x79,0x76}; memcpy(rom+0x10,code,sizeof(code));
        if(variant==2) { // EXX loses the constant C before OUT (C),A.
            rom[0x10]=0xd9; rom[0x11]=0xed; rom[0x12]=0x79; rom[0x13]=0x76;
        }
        DpSegaClosure *c=create(DP_MSX_PLAIN,0x8000,limits());
        dp_msx_cpu()->c=variant?0x99:0xa8;
        finish(c);
        if(variant==1) CHECK(flags[0x12]==DP_SEGA_STATIC);
        else CHECK(!flags[variant==2?0x13:0x12] && stats.map_barriers);
    }
    fixture(0x4010); const uint8_t bios[]={0xcd,0x5c,0,0x76}; memcpy(rom+0x10,bios,sizeof(bios));
    DpSegaClosure *c=create(DP_MSX_PLAIN,0x8000,limits()); finish(c);
    CHECK(!flags[0x13] && stats.unmapped && !flags[0x5c]);
}
static void bounds_and_overlay(void) {
    fixture(0x4010); rom[0x10]=0x76;
    DpSegaClosureOptions o=limits(); o.views=1;
    DpSegaClosure *c=create(DP_MSX_PLAIN,0x8000,o); dp_sega_closure_observe(c);
    dp_msx_out(0xa8,0xf7); dp_sega_closure_invalidate(c); finish(c);
    CHECK(stats.views==1 && stats.view_skips);
    fixture(0x4010); const uint8_t recursive[]={0xcd,0x10,0x40,0xc9}; memcpy(rom+0x10,recursive,sizeof(recursive));
    c=create(DP_MSX_PLAIN,0x8000,limits()); finish(c);
    CHECK(stats.stack_cuts || stats.context_cuts || stats.node_cuts);
    fixture(0x4010); const uint8_t jump[]={0xc3,0,0x98}; memcpy(rom+0x10,jump,sizeof(jump));
    c=create(DP_MSX_KONAMI_SCC,sizeof(rom),limits()); dp_msx_out(0xa8,0xd4); dp_msx_write(0x9000,0x3f);
    CHECK(dp_msx_rom_index(0x9800)<0); finish(c); CHECK(stats.unmapped);
}
static void partial_rom(void) {
    fixture(0x4010); const uint8_t code[]={0xc3,0x10,0x60};
    memcpy(rom+0x10,code,sizeof(code)); rom[0x2010]=0x76;
    DpSegaClosure *c=create(DP_MSX_PLAIN,0x2011,limits()); finish(c);
    CHECK(flags[0x2010]==DP_SEGA_STATIC && !flags[0x2011]);
    // A bank register remains active when its CPU byte lies in image padding.
    const uint8_t banked[]={0x32,0,0x68,0xc3,0x10,0x60};
    memcpy(rom+0x10,banked,sizeof(banked));
    c=create(DP_MSX_ASCII8,0x2011,limits()); dp_msx_write(0x6800,1);
    CHECK(dp_msx_rom_index(0x6800)<0 && dp_msx_rom_index(0x6010)==0x2010);
    finish(c); CHECK(!flags[0x2010] && stats.map_barriers && stats.ambiguous);
    memcpy(rom+0x10,code,sizeof(code));
    rom[0x2010]=0x3e; // Truncated LD A,n must not mark its first byte as code.
    c=create(DP_MSX_PLAIN,0x2011,limits()); finish(c);
    CHECK(!flags[0x2010] && stats.decode && stats.unmapped);
}
static void exploration_cutoff(void) {
    fixture(0x4010); const uint8_t entry[]={0xc3,0,0x42};
    const uint8_t target[]={0x3a,0,0x60,0xd3,0x98,0x76};
    memcpy(rom+0x10,entry,sizeof(entry)); memcpy(rom+0x200,target,sizeof(target));
    DpSegaOptions o=dp_msx_options(); o.msx_mapper=DP_MSX_PLAIN;
    o.max_total=1; o.input_total=0; DpSegaResults a,b;
    CHECK(dp_sega_explore(dp_msx_system(),rom,0x8000,&o,&a));
    o.closure.nodes=0; CHECK(dp_sega_explore(dp_msx_system(),rom,0x8000,&o,&b));
    CHECK(a.steps==b.steps && a.steps==1 && a.paths==b.paths);
    CHECK(a.flags[0x200]==DP_SEGA_STATIC && !b.flags[0x200]);
    CHECK(a.closure.new_bytes==sizeof(target));
    CHECK(!a.vdp_copies && !a.ram_copies && !a.data_reads && !a.exec_outside);
    CHECK(!memcmp(a.io_write,b.io_write,sizeof(a.io_write)) && !a.bios_calls[15]);
    dp_sega_results_free(&a); dp_sega_results_free(&b);
    // A static CALL into the simulated BIOS has no invented continuation.
    const uint8_t bios[]={0xcd,0x5c,0,0x76}; memcpy(rom+0x200,bios,sizeof(bios));
    o.closure=dp_msx_options().closure;
    CHECK(dp_sega_explore(dp_msx_system(),rom,0x8000,&o,&a));
    CHECK(a.flags[0x200]==DP_SEGA_STATIC && !a.flags[0x203] && !a.bios_calls[15]);
    dp_sega_results_free(&a);
}
int main(void) {
    ram_stack(); views_and_hidden_ram(); mappers(); ports_and_bios(); bounds_and_overlay(); partial_rom(); exploration_cutoff();
    TEST_DONE("msx_closure");
}
