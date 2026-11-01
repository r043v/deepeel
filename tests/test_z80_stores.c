#include "../src/platform/msx/msx.h"
#include "../src/platform/msx/explore.h"
#include "test_util.h"
#include <string.h>
static uint8_t rom[0x8000],flags[0x8000];
static const DpSegaBackend *backend(unsigned system) {
    return system==0?dp_sega_backend_sg():system==3?dp_msx_backend():dp_sega_backend_gg();
}
static const DpSegaSystem *profile(unsigned system) {
    return system==3?dp_msx_system():dp_sega_system(system==0?"sg":system==1?"sms":"gg");
}
static void fixture(unsigned system,const uint8_t *code,size_t length) {
    memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags));
    rom[0]='A'; rom[1]='B'; rom[2]=0; rom[3]=0x45; memset(rom+4,0,12);
    memcpy(rom+0x500,code,length); rom[0x700]=0; rom[0x701]=0xd1;
    CHECK(backend(system)->init(rom,sizeof(rom),NULL,system==3?DP_MSX_PLAIN:system==1));
    backend(system)->cpu()->pc=system==3?0x4500:0x500;
    backend(system)->cpu()->sp=0xd000;
}
static void run(unsigned system,DpSegaClosureStats *base,DpSegaClosureStats *targets) {
    const DpSegaBackend *b=backend(system); DpSegaClosureOptions options={256,16,4,16};
    DpSegaClosure *c=dp_sega_closure_create(b,profile(system),rom,sizeof(rom),flags,options,base);
    CHECK(c); CHECK(dp_sega_closure_targets(c,targets));
    z80_t before=*b->cpu(); dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(!memcmp(&before,b->cpu(),sizeof(before)));
    CHECK(base->nodes==base->processed && targets->nodes==targets->processed);
    CHECK(base->nodes<=options.nodes && targets->nodes<=options.nodes);
    dp_sega_closure_free(c); if(b->close) b->close();
}
static void stores(unsigned system,unsigned variant) {
    uint8_t code[32]; size_t length; int refined=1;
#define CODE(...) do { const uint8_t bytes[]={__VA_ARGS__}; memcpy(code,bytes,sizeof(bytes)); length=sizeof(bytes); } while(0)
    switch(variant) {
    case 0: CODE(0x21,0,0xd1,0x77,0x76); break;
    case 1: CODE(0x21,0,0xd1,0x36,0,0x76); break;
    case 2: CODE(0x21,0,0xd1,0x34,0x76); break;
    case 3: CODE(0x21,0,0xd1,0x35,0x76); break;
    case 4: CODE(0xdd,0x21,1,0xd1,0xdd,0x77,0xff,0x76); break;
    case 5: CODE(0xfd,0x21,0xff,0xff,0xfd,0x36,1,0,0x76); break;
    case 6: CODE(0x21,0,0xd1,0xcb,0x06,0x76); break;
    case 7: CODE(0x21,0,0xd1,0xcb,0x86,0x76); break;
    case 8: CODE(0x21,0,0xd1,0xcb,0xc6,0x76); break;
    case 9: CODE(0xdd,0x21,1,0xd1,0xdd,0xcb,0xff,0x04,0x76); break;
    case 10: CODE(0x21,0,0xd1,0xed,0x6f,0x76); break;
    case 11: CODE(0x21,0,0xd1,0xed,0x67,0x76); break;
    case 12: CODE(0x21,0,0xd1,0xed,0xa2,0x76); break;
    case 13: CODE(0x21,0,0xd1,0xed,0xaa,0x76); break;
    case 14: CODE(0x21,0,0xd1,0xcb,0x46,0x76); refined=0; break; // BIT is not a write.
    case 15: CODE(0x21,0,0xd1,0x60,0x77,0x76); refined=0; break;
    case 16: CODE(0x21,0,0xd1,0x29,0x77,0x76); refined=0; break;
    case 17: CODE(0x21,0,0xd1,0xdd,0x21,0xfd,0xff,0xdd,0xed,0x6f,0x76); break;
    default: {
        uint16_t table=system==3?0x4700:0x700;
        CODE(0x2a,(uint8_t)table,(uint8_t)(table>>8),0x77,0x76); break;
    }
    }
#undef CODE
    fixture(system,code,length); DpSegaClosureStats base,targets; run(system,&base,&targets);
    CHECK(targets.memory_resolved==(uint64_t)refined);
    if((variant==15 || variant==16) && (system==1 || system==2)) {
        CHECK(!flags[0x500+length-1] && targets.map_barriers);
    } else CHECK(flags[0x500+length-1]==DP_SEGA_STATIC);
}
static void stack(unsigned system,unsigned variant) {
    uint16_t target=system==3?0x4600:0x600;
    const uint8_t entry[]={0xcd,(uint8_t)target,(uint8_t)(target>>8),0x76};
    fixture(system,entry,sizeof(entry));
    uint16_t address=variant==1?0xcffe:variant==2?0xeffe:variant==3?0xd3fe:0xd100;
    const uint8_t callee[]={0x21,(uint8_t)address,(uint8_t)(address>>8),0x77,0xc9};
    // Reinitialize after editing the ROM: MSX owns its copied cartridge image.
    if(backend(system)->close) backend(system)->close();
    if(variant==4) {
        const uint8_t indexed[]={0xdd,0x21,0x7e,0xd0,0xdd,0x77,0x80,0xc9};
        memcpy(rom+0x600,indexed,sizeof(indexed)); // $D07E - 128 = $CFFE.
    } else memcpy(rom+0x600,callee,sizeof(callee));
    CHECK(backend(system)->init(rom,sizeof(rom),NULL,system==3?DP_MSX_PLAIN:system==1));
    backend(system)->cpu()->pc=system==3?0x4500:0x500; backend(system)->cpu()->sp=0xd000;
    DpSegaClosureStats base,targets; run(system,&base,&targets);
    int corrupt=variant==1 || variant==4 || (variant==2 && system!=3) || (variant==3 && system==0);
    CHECK(targets.memory_resolved==1);
    if(corrupt) CHECK(!flags[0x503] && targets.stack_invalidations && targets.indirect);
    else CHECK(flags[0x503]==DP_SEGA_STATIC && targets.returns_resolved==1);
}
static void stack_pointer(unsigned system,unsigned pair) {
    uint16_t callee=system==3?0x4600:0x600; uint8_t code[16]; unsigned at=0;
    if(pair) code[at++]=pair==1?0xdd:0xfd;
    code[at++]=0x21; code[at++]=0; code[at++]=0xd0;
    if(pair) code[at++]=pair==1?0xdd:0xfd;
    code[at++]=0xf9; code[at++]=0xcd; code[at++]=(uint8_t)callee; code[at++]=(uint8_t)(callee>>8); code[at++]=0x76;
    fixture(system,code,at); if(backend(system)->close) backend(system)->close(); rom[0x600]=0xc9;
    CHECK(backend(system)->init(rom,sizeof(rom),NULL,system==3?DP_MSX_PLAIN:system==1));
    backend(system)->cpu()->pc=system==3?0x4500:0x500;
    DpSegaClosureStats base,targets; run(system,&base,&targets);
    CHECK(flags[0x500+at-1]==DP_SEGA_STATIC && targets.returns_resolved==1);
}
static void mapper(unsigned system) {
    const uint8_t code[]={0x21,0xfd,0xff,0x77,0x76}; fixture(system,code,sizeof(code));
    DpSegaClosureStats base,targets; run(system,&base,&targets);
    CHECK(targets.memory_resolved==1);
    if(system==1 || system==2) CHECK(!flags[0x504] && targets.map_barriers && targets.ambiguous);
    else CHECK(flags[0x504]==DP_SEGA_STATIC);
}
static void msx_wrapped_store(void) {
    const uint8_t entry[]={0xcd,0,0x46,0x76}; fixture(3,entry,sizeof(entry)); dp_msx_close();
    const uint8_t callee[]={0xfd,0x21,0xff,0xff,0xfd,0x77,1,0xc9};
    memcpy(rom+0x600,callee,sizeof(callee)); CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_PLAIN));
    dp_msx_out(0xa8,0xf7); dp_msx_cpu()->sp=2; // CALL saves its return at $0000/$0001 RAM.
    DpSegaClosureStats base,targets; run(3,&base,&targets);
    CHECK(!flags[0x503] && targets.memory_resolved==1 && targets.stack_invalidations && targets.indirect);
}
static void msx_banks(void) {
    static const uint16_t reg[]={0,0x6800,0x7000,0x6000,0x9000};
    static const uint16_t jump[]={0,0x6100,0x8100,0x6100,0x8100};
    for(int type=DP_MSX_ASCII8;type<=DP_MSX_KONAMI_SCC;type++) {
        const uint8_t code[]={0x21,(uint8_t)reg[type],(uint8_t)(reg[type]>>8),0x77,0xc3,(uint8_t)jump[type],(uint8_t)(jump[type]>>8)};
        fixture(3,code,sizeof(code)); dp_msx_close();
        CHECK(dp_msx_init(rom,sizeof(rom),type)); dp_msx_out(0xa8,0xd4);
        int64_t index=dp_msx_rom_index(jump[type]); CHECK(index>=0); if(index<0) continue;
        DpSegaClosureStats base,targets; run(3,&base,&targets);
        CHECK(flags[0x504]==DP_SEGA_STATIC && !flags[index]);
        CHECK(targets.memory_resolved==1 && targets.map_barriers && targets.ambiguous);
    }
    const uint8_t hidden[]={0x21,0,0x68,0x77,0x76}; fixture(3,hidden,sizeof(hidden)); dp_msx_close();
    // Page 1 is RAM, page 2 cartridge. An ASCII8 decoder at $6800 is hidden.
    rom[2]=0; rom[3]=0x85; CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_ASCII8)); dp_msx_out(0xa8,0xdc);
    DpSegaClosureStats base,targets; run(3,&base,&targets);
    CHECK(flags[0x504]==DP_SEGA_STATIC && targets.memory_resolved==1 && !targets.map_barriers);
}
int main(void) {
    for(unsigned system=0;system<4;system++) {
        for(unsigned variant=0;variant<19;variant++) stores(system,variant);
        for(unsigned variant=0;variant<5;variant++) stack(system,variant);
        for(unsigned pair=0;pair<3;pair++) stack_pointer(system,pair);
        mapper(system);
    }
    msx_banks(); msx_wrapped_store(); TEST_DONE("z80_stores");
}
