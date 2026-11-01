#include "../src/platform/msx/msx.h"
#include "../src/platform/msx/explore.h"
#include "test_util.h"
#include <string.h>
static uint8_t rom[0x8000],flags[0x8000];
static void scenario(unsigned system,unsigned variant) {
    const DpSegaBackend *b=system==0?dp_sega_backend_sg():system==3?dp_msx_backend():dp_sega_backend_gg();
    const DpSegaSystem *s=system==3?dp_msx_system():dp_sega_system(system==0?"sg":system==1?"sms":"gg");
    uint16_t base=system==3?0x4000:0,target=(uint16_t)(base+0x600),table=(uint16_t)(base+0x700);
    uint8_t lo=(uint8_t)target,hi=(uint8_t)(target>>8),tl=(uint8_t)table,th=(uint8_t)(table>>8);
    memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags));
    rom[0]='A'; rom[1]='B'; rom[2]=0; rom[3]=0x45; memset(rom+4,0,12);
    uint8_t code[32]; size_t length=0; int resolved=1;
#define CODE(...) do { const uint8_t bytes[]={__VA_ARGS__}; memcpy(code,bytes,sizeof(bytes)); length=sizeof(bytes); } while(0)
    switch(variant) {
    case 0: CODE(0x21,lo,hi,0xe9); break;
    case 1: CODE(0xdd,0x21,lo,hi,0xdd,0xe9); break;
    case 2: CODE(0xfd,0x21,lo,hi,0xfd,0xe9); break;
    case 3: CODE(0x26,hi,0x2e,lo,0xe9); break;
    case 4: CODE(0xdd,0x26,hi,0xdd,0x2e,lo,0xdd,0xe9); break;
    case 5: CODE(0xfd,0x26,hi,0xfd,0x2e,lo,0xfd,0xe9); break;
    case 6: CODE(0x2a,tl,th,0xe9); break;
    case 7: CODE(0xdd,0x2a,tl,th,0xdd,0xe9); break;
    case 8: CODE(0xfd,0x2a,tl,th,0xfd,0xe9); break;
    case 9: CODE(0xed,0x6b,tl,th,0xe9); break;
    case 10: CODE(0x21,(uint8_t)(target-1),(uint8_t)((target-1)>>8),0x23,0xe9); break; // Exact INC HL
    case 11: CODE(0xdd,0x21,lo,hi,0xdd,0x24,0xdd,0xe9); resolved=0; break;
    case 12: CODE(0xfd,0x21,lo,hi,0xfd,0x2d,0xfd,0xe9); resolved=0; break;
    case 13: CODE(0x21,lo,hi,0xd9,0xe9); resolved=0; break; // EXX
    case 14: CODE(0x21,lo,hi,0xeb,0xe9); resolved=0; break; // EX DE,HL
    case 15: CODE(0x21,lo,hi,0x60,0xe9); resolved=0; break; // LD H,B
    case 16: CODE(0x21,lo,hi,0xdd,0x60,0xdd,0x66,0,0xe9); resolved=0; break; // Unknown IX before LD H,(IX+d)
    case 17: CODE(0x21,lo,hi,0xe1,0xe9); resolved=0; break; // POP HL from RAM
    case 18: CODE(0x2a,0,0xc0,0xe9); resolved=0; break; // RAM word is not static ROM
    case 19: CODE(0xdd,0x21,lo,hi,0xdd,0xeb,0xdd,0xe9); break; // DD EX uses plain HL
    case 20: CODE(0x21,lo,hi,0x64,0x6d,0xe9); break; // LD H,H / LD L,L
    case 21: CODE(0x21,lo,hi,0xcb,0x44,0xe9); break; // BIT does not transform H
    case 22: CODE(0x21,lo,hi,0xcb,0x84,0xe9); resolved=0; break; // RES H invalidates even unchanged value
    case 23: CODE(0xe9); break; // Concrete observed constant is a valid seed.
    case 24: CODE(0xdd,0xed,0x6b,tl,th,0xe9); break; // DD is ignored before ED
    case 26: CODE(0xdd,0x21,(uint8_t)(target-1),(uint8_t)((target-1)>>8),0xdd,0x23,0xdd,0xe9); break;
    case 27: CODE(0xfd,0x21,(uint8_t)(target+1),(uint8_t)((target+1)>>8),0xfd,0x2b,0xfd,0xe9); break;
    case 28: CODE(0x21,lo,hi,0x60,0x23,0xe9); resolved=0; break; // Partial pair cannot be incremented exactly.
    default: CODE(0x2a,tl,th,0xe9); resolved=0; break; // Truncated ROM word
    }
#undef CODE
    memcpy(rom+0x500,code,length); rom[0x600]=0x76; rom[0x700]=lo; rom[0x701]=hi;
    size_t size=variant==25?0x701:sizeof(rom);
    CHECK(b->init(rom,size,NULL,system==3?DP_MSX_PLAIN:system==1));
    b->cpu()->pc=(uint16_t)(base+0x500); b->cpu()->sp=0xd000;
    b->cpu()->hl=variant==23?target:0x1234;
    DpSegaClosureOptions options={128,8,4,16}; DpSegaClosureStats stats,baseline;
    DpSegaClosure *c=dp_sega_closure_create(b,s,rom,size,flags,options,&baseline);
    CHECK(c); CHECK(dp_sega_closure_targets(c,&stats)); z80_t cpu=*b->cpu();
    dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(!memcmp(&cpu,b->cpu(),sizeof(cpu)) && stats.nodes==stats.processed);
    if(resolved) { CHECK(flags[0x600]==DP_SEGA_STATIC); CHECK(stats.indirect_resolved==1 && !stats.indirect); }
    else { CHECK(!flags[0x600]); CHECK(!stats.indirect_resolved && stats.indirect==1); }
    dp_sega_closure_free(c); if(b->close) b->close();
}
static void mapping_barrier(void) {
    for(unsigned system=1;system<4;system++) {
        int msx=system==3;
        const DpSegaBackend *b=msx?dp_msx_backend():dp_sega_backend_gg();
        const DpSegaSystem *s=msx?dp_msx_system():dp_sega_system(system==1?"sms":"gg");
        uint16_t target=msx?0x6100:0x8100,address=msx?0x6800:0xffff;
        memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags));
        rom[0]='A'; rom[1]='B'; rom[2]=0; rom[3]=0x45; memset(rom+4,0,12);
        const uint8_t code[]={0x21,(uint8_t)target,(uint8_t)(target>>8),0x32,(uint8_t)address,(uint8_t)(address>>8),0xe9};
        memcpy(rom+0x500,code,sizeof(code)); rom[0x100]=0x76;
        CHECK(b->init(rom,sizeof(rom),NULL,msx?DP_MSX_ASCII8:system==1));
        b->cpu()->pc=msx?0x4500:0x500;
        DpSegaClosureStats stats,baseline; DpSegaClosureOptions options={64,8,4,8};
        DpSegaClosure *c=dp_sega_closure_create(b,s,rom,sizeof(rom),flags,options,&baseline);
        CHECK(c); CHECK(dp_sega_closure_targets(c,&stats)); dp_sega_closure_observe(c); dp_sega_closure_run(c);
        CHECK(!flags[0x100] && stats.indirect_resolved==1 && stats.ambiguous && stats.map_barriers);
        dp_sega_closure_free(c); if(b->close) b->close();
    }
}
static void independent_frontier(void) {
    for(unsigned system=0;system<4;system++) {
        int msx=system==3; uint16_t target=msx?0x4600:0x600;
        const DpSegaSystem *s=msx?dp_msx_system():dp_sega_system(system==0?"sg":system==1?"sms":"gg");
        memset(rom,0xff,sizeof(rom));
        if(msx) { rom[0]='A'; rom[1]='B'; rom[2]=0; rom[3]=0x45; memset(rom+4,0,12); }
        else { rom[0]=0xc3; rom[1]=0; rom[2]=5; }
        const uint8_t code[]={0x21,(uint8_t)target,(uint8_t)(target>>8),0xe9};
        memcpy(rom+0x500,code,sizeof(code)); rom[0x600]=0x76;
        DpSegaOptions options=msx?dp_msx_options():dp_sega_options();
        options.max_total=1; options.input_total=0; options.seed_vectors=0;
        options.closure=(DpSegaClosureOptions){64,8,4,8};
        DpSegaResults on,off;
        CHECK(dp_sega_explore(s,rom,sizeof(rom),&options,&on));
        options.target_closure=0; CHECK(dp_sega_explore(s,rom,sizeof(rom),&options,&off));
        CHECK(on.steps==off.steps && on.steps==1 && on.paths==off.paths);
        CHECK(!memcmp(&on.closure,&off.closure,sizeof(on.closure)));
        for(unsigned i=0;i<sizeof(rom);i++) CHECK((on.flags[i]&off.flags[i])==off.flags[i]);
        CHECK(on.flags[0x600]==DP_SEGA_STATIC && !off.flags[0x600]);
        CHECK(on.target_closure.new_bytes==1 && on.target_closure.indirect_resolved);
        CHECK(on.target_closure.nodes<=options.closure.nodes && !off.target_closure.nodes);
        dp_sega_results_free(&on); dp_sega_results_free(&off);
    }
    const DpSegaBackend *b=dp_sega_backend_gg(); b->init(rom,sizeof(rom),NULL,1); b->cpu()->pc=0x500;
    DpSegaClosureOptions options={32,4,2,2}; DpSegaClosureStats base,targets;
    DpSegaClosure *c=dp_sega_closure_create(b,dp_sega_system("sms"),rom,sizeof(rom),flags,options,&base);
    CHECK(c); CHECK(!dp_sega_closure_targets(c,&base));
    CHECK(dp_sega_closure_targets(c,&targets)); CHECK(!dp_sega_closure_targets(c,&targets));
    dp_sega_closure_observe(c); dp_sega_closure_run(c); dp_sega_closure_free(c);
}
static void byte_tables(unsigned system,unsigned variant) {
    const DpSegaBackend *b=system==0?dp_sega_backend_sg():system==3?dp_msx_backend():dp_sega_backend_gg();
    const DpSegaSystem *s=system==3?dp_msx_system():dp_sega_system(system==0?"sg":system==1?"sms":"gg");
    uint16_t base=system==3?0x4000:0,target=(uint16_t)(base+0x600);
    uint16_t table=(uint16_t)(base+(variant==10?0x7ff:0x700));
    if(variant==8) table=0xc000;
    uint8_t tl=(uint8_t)table,th=(uint8_t)(table>>8),hi=(uint8_t)(target>>8);
    uint8_t code[32]; size_t length=0; int resolved=1;
#define CODE(...) do { const uint8_t bytes[]={__VA_ARGS__}; memcpy(code,bytes,sizeof(bytes)); length=sizeof(bytes); } while(0)
    switch(variant) {
    case 0: case 8: case 9: case 10: CODE(0x21,tl,th,0x7e,0x23,0x66,0x6f,0xe9); break;
    case 1: CODE(0x21,(uint8_t)(table+1),(uint8_t)((table+1)>>8),0x7e,0x2b,0x6e,0x67,0xe9); break;
    case 2: CODE(0xdd,0x21,tl,th,0xdd,0x7e,0,0xdd,0x66,1,0x6f,0xe9); break;
    case 3: CODE(0xfd,0x21,(uint8_t)(table+1),(uint8_t)((table+1)>>8),0xfd,0x7e,0xff,0xfd,0x66,0,0x6f,0xe9); break;
    case 4: CODE(0x3a,tl,th,0x26,hi,0x6f,0xe9); break;
    case 5: CODE(0x21,tl,th,0x7e,0x23,0x66,0xaf,0x6f,0xe9); resolved=0; break;
    case 6: CODE(0x60,0x7e,0x26,hi,0x6f,0xe9); resolved=0; break;
    case 7: CODE(0xdd,0x60,0xdd,0x7e,0,0x26,hi,0x6f,0xe9); resolved=0; break;
    default: CODE(0x21,tl,th,0x7e,0xfe,0,0x23,0x66,0x6f,0xe9); break; // CP preserves A.
    }
#undef CODE
    if(variant==8 || variant==9) resolved=0;
    memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags));
    rom[0]='A'; rom[1]='B'; rom[2]=0; rom[3]=0x45; memset(rom+4,0,12);
    memcpy(rom+0x500,code,length); rom[0x600]=0x76;
    unsigned offset=variant==10?0x7ff:0x700; rom[offset]=0; rom[offset+1]=hi;
    size_t size=variant==9?0x701:sizeof(rom);
    CHECK(b->init(rom,size,NULL,system==3?DP_MSX_PLAIN:system==1));
    b->cpu()->pc=(uint16_t)(base+0x500); b->cpu()->hl=0x1234; b->cpu()->ix=0x1234;
    DpSegaClosureOptions options={256,8,4,16}; DpSegaClosureStats baseline,targets;
    DpSegaClosure *c=dp_sega_closure_create(b,s,rom,size,flags,options,&baseline);
    CHECK(c); CHECK(dp_sega_closure_targets(c,&targets));
    z80_t cpu=*b->cpu(); dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(!memcmp(&cpu,b->cpu(),sizeof(cpu)) && targets.processed==targets.nodes);
    if(resolved) CHECK(flags[0x600]==DP_SEGA_STATIC && targets.indirect_resolved==1);
    else CHECK(!flags[0x600] && targets.indirect_resolved==0 && targets.indirect);
    dp_sega_closure_free(c); if(b->close) b->close();
}
static void mapped_byte_table(void) {
    for(unsigned system=1;system<4;system++) {
        int msx=system==3;
        const DpSegaBackend *b=msx?dp_msx_backend():dp_sega_backend_gg();
        const DpSegaSystem *s=msx?dp_msx_system():dp_sega_system(system==1?"sms":"gg");
        uint16_t reg=msx?0x6800:0xffff,table=msx?0x6100:0x8100;
        uint8_t high=msx?0x46:6;
        const uint8_t code[]={0xdd,0x21,(uint8_t)reg,(uint8_t)(reg>>8),0xdd,0x77,0,
            0x21,(uint8_t)table,(uint8_t)(table>>8),0x7e,0x23,0x66,0x6f,0xe9};
        memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags));
        rom[0]='A'; rom[1]='B'; rom[2]=0; rom[3]=0x45; memset(rom+4,0,12);
        for(unsigned bank=0;bank<4;bank++) { rom[bank*0x2000+0x100]=0; rom[bank*0x2000+0x101]=high; }
        memcpy(rom+0x500,code,sizeof(code)); rom[0x600]=0x76;
        CHECK(b->init(rom,sizeof(rom),NULL,msx?DP_MSX_ASCII8:system==1)); b->cpu()->pc=msx?0x4500:0x500;
        DpSegaClosureOptions options={128,8,4,16}; DpSegaClosureStats baseline,targets;
        DpSegaClosure *c=dp_sega_closure_create(b,s,rom,sizeof(rom),flags,options,&baseline);
        CHECK(c); CHECK(dp_sega_closure_targets(c,&targets)); dp_sega_closure_observe(c); dp_sega_closure_run(c);
        CHECK(!flags[0x600] && !targets.indirect_resolved && targets.map_barriers && targets.indirect);
        dp_sega_closure_free(c); if(b->close) b->close();
    }
}
static void pointer_arithmetic(void) {
    const DpSegaBackend *b=dp_sega_backend_gg();
    const uint8_t code[]={0x21,0xff,0xff,0x23,0x2b,0xe9};
    memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags)); memcpy(rom+0x500,code,sizeof(code));
    CHECK(b->init(rom,sizeof(rom),NULL,1)); b->cpu()->pc=0x500;
    DpSegaClosureStats baseline,targets; DpSegaClosureOptions options={64,8,4,8};
    DpSegaClosure *c=dp_sega_closure_create(b,dp_sega_system("sms"),rom,sizeof(rom),flags,options,&baseline);
    CHECK(c); CHECK(dp_sega_closure_targets(c,&targets)); dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(targets.indirect_resolved==1 && targets.unmapped && !targets.indirect);
    dp_sega_closure_free(c);
}
int main(void) {
    for(unsigned system=0;system<4;system++) for(unsigned variant=0;variant<29;variant++) scenario(system,variant);
    for(unsigned system=0;system<4;system++) for(unsigned variant=0;variant<12;variant++) byte_tables(system,variant);
    mapping_barrier(); independent_frontier(); mapped_byte_table(); pointer_arithmetic(); TEST_DONE("z80_targets");
}
