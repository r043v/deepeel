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
static void fixture(const uint8_t *code,size_t length) {
    memset(rom,0xff,sizeof(rom)); memset(flags,0,sizeof(flags));
    rom[0]='A'; rom[1]='B'; rom[2]=0; rom[3]=0x45; memset(rom+4,0,12);
    memcpy(rom+0x500,code,length);
}
static void check(unsigned system,size_t size,uint16_t target,int resolved,int concrete,DpSegaClosureStats *stats) {
    const DpSegaBackend *b=backend(system); CHECK(b->init(rom,size,NULL,system==3?DP_MSX_PLAIN:system==1));
    b->cpu()->pc=system==3?0x4500:0x500; b->cpu()->sp=0xd000;
    DpSegaClosureOptions options={256,8,4,16}; DpSegaClosureStats base,targets;
    DpSegaClosure *c=dp_sega_closure_create(b,profile(system),rom,size,flags,options,&base);
    CHECK(c); CHECK(dp_sega_closure_targets(c,&targets)); CHECK(dp_sega_closure_blocks(c,stats)); z80_t cpu=*b->cpu(); uint8_t ram=b->read(0xd100);
    dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(!memcmp(&cpu,b->cpu(),sizeof(cpu)) && b->read(0xd100)==ram);
    CHECK(stats->nodes+targets.nodes<=options.nodes && stats->seeds+targets.seeds<=options.seeds && stats->views+targets.views<=options.views);
    CHECK(stats->nodes==stats->processed && stats->nodes<=options.nodes);
    unsigned index=system==3?target-0x4000:target;
    if(resolved) CHECK(flags[index]&DP_SEGA_STATIC);
    else CHECK(!(flags[index]&DP_SEGA_STATIC));
    if(concrete) {
        int reached=0;
        for(unsigned step=0;step<128 && !b->cpu()->halted && !b->cpu()->fault;step++) {
            reached|=b->cpu()->pc==target;
            unsigned cycles=z80_step(b->cpu()); b->advance(cycles);
        }
        CHECK(reached && b->cpu()->halted && !b->cpu()->fault);
    }
    dp_sega_closure_free(c); if(b->close) b->close();
}
static void transfers(unsigned system,unsigned variant) {
    uint16_t base=system==3?0x4000:0,target=(uint16_t)(base+0x600),table=(uint16_t)(base+0x700);
    uint8_t lo=(uint8_t)target,hi=(uint8_t)(target>>8),tl=(uint8_t)table,th=(uint8_t)(table>>8);
    uint8_t code[40]; size_t length=0; int resolved=1;
#define CODE(...) do { const uint8_t bytes[]={__VA_ARGS__}; memcpy(code,bytes,sizeof(bytes)); length=sizeof(bytes); } while(0)
    switch(variant) {
    case 0: CODE(0x11,lo,hi,0xeb,0xe9); break;
    case 1: CODE(0x16,hi,0x1e,lo,0xeb,0xe9); break;
    case 2: CODE(0xed,0x5b,tl,th,0xeb,0xe9); break;
    case 3: CODE(0xdd,0xed,0x5b,tl,th,0xeb,0xe9); break;
    case 4: CODE(0x01,lo,hi,0x60,0x69,0xe9); break;
    case 5: CODE(0xed,0x4b,tl,th,0x60,0x69,0xe9); break;
    case 6: CODE(0x11,tl,th,0x1a,0x13,0x47,0x1a,0x67,0x68,0xe9); break;
    case 7: CODE(0x01,tl,th,0x0a,0x03,0x6f,0x0a,0x67,0xe9); break;
    case 8: CODE(0x01,(uint8_t)(target-1),(uint8_t)((target-1)>>8),0x03,0x60,0x69,0xe9); break;
    case 9: CODE(0x11,(uint8_t)(target+1),(uint8_t)((target+1)>>8),0x1b,0xeb,0xe9); break;
    case 10: CODE(0x06,(uint8_t)(hi-1),0x0e,lo,0x04,0x60,0x69,0xe9); break;
    case 11: CODE(0x11,(uint8_t)(lo+1),hi,0x1d,0xeb,0xe9); break;
    case 12: CODE(0x01,lo,(uint8_t)(hi+1),0x10,0,0x60,0x69,0xe9); break;
    case 13: CODE(0x0e,lo,0x60,0x69,0xe9); resolved=0; break;
    case 14: CODE(0x11,lo,hi,0xd9,0xeb,0xe9); resolved=0; break;
    case 15: CODE(0x01,lo,hi,0x09,0xe9); resolved=0; break; // ADD HL,BC remains unmodelled.
    case 16: CODE(0xed,0x5b,tl,th,0xeb,0xe9); resolved=0; break; // Truncated high byte.
    case 17: CODE(0xed,0x5b,0,0xc0,0xeb,0xe9); resolved=0; break;
    case 18: CODE(0x1e,0,0x1a,0x26,hi,0x6f,0xe9); resolved=0; break; // Unknown D must not read address zero.
    default: CODE(0x3e,lo,0x5f,0x3e,hi,0x57,0xeb,0xe9); break;
    }
#undef CODE
    fixture(code,length); rom[0x600]=0x76; rom[0x700]=lo; rom[0x701]=hi;
    DpSegaClosureStats stats; check(system,variant==16?0x701:sizeof(rom),target,resolved,resolved,&stats);
    if(resolved) CHECK(stats.indirect_resolved);
    else CHECK(!stats.indirect_resolved && stats.indirect);
    if(variant==18) CHECK(!stats.static_data_reads);
}
static void block(unsigned system,unsigned variant) {
    uint16_t base=system==3?0x4000:0,target=(uint16_t)(base+0x600),source=(uint16_t)(base+0x700);
    uint8_t code[32]; unsigned length=0;
    uint16_t hl=source,de=(uint16_t)(target-1),bc=1; uint8_t opcode=0xa0;
    if(variant==1) { hl=(uint16_t)(source+1); de=(uint16_t)(target+1); opcode=0xa8; }
    if(variant==2 || variant==3) { hl=(uint16_t)(target+(variant==2?-1:1)); de=0xd100; opcode=variant==2?0xa1:0xa9; }
    if(variant>=4 && variant<=7) {
        hl=(uint16_t)(target+((variant&1)?1:-1)); de=0xd100; bc=0x0199;
        opcode=variant==4?0xa2:variant==5?0xaa:variant==6?0xa3:0xab;
    }
    if(variant==8) { // OUTI decrements B, preserving C rather than subtracting 1 from BC.
        target=(uint16_t)(base+0x699); hl=source; de=0xd100;
        bc=(uint16_t)(((target>>8)+1)*256+(target&255)); opcode=0xa3;
    }
    code[length++]=0x21; code[length++]=(uint8_t)hl; code[length++]=(uint8_t)(hl>>8);
    code[length++]=0x11; code[length++]=(uint8_t)de; code[length++]=(uint8_t)(de>>8);
    code[length++]=0x01; code[length++]=(uint8_t)bc; code[length++]=(uint8_t)(bc>>8);
    code[length++]=0xed; code[length++]=opcode;
    if(variant<2) code[length++]=0xeb;
    if(variant==8) { code[length++]=0x60; code[length++]=0x69; }
    code[length++]=0xe9;
    fixture(code,length); rom[target-base]=0x76;
    DpSegaClosureStats stats; check(system,sizeof(rom),target,1,1,&stats);
    CHECK(stats.indirect_resolved);
    if(variant<2 || variant==4 || variant==5) CHECK(stats.memory_resolved);
}
static void stack_and_limits(unsigned system,unsigned variant) {
    uint16_t base=system==3?0x4000:0,callee=(uint16_t)(base+0x600),source=(uint16_t)(base+0x700);
    const uint8_t entry[]={0xcd,(uint8_t)callee,(uint8_t)(callee>>8),0x76};
    fixture(entry,sizeof(entry));
    uint16_t destination=variant==1?0xcffe:variant==2?0xeffe:0xd100;
    const uint8_t copy[]={0x21,(uint8_t)source,(uint8_t)(source>>8),0x11,(uint8_t)destination,(uint8_t)(destination>>8),
        0x01,variant==3?0:2,0,0xed,0xb0,0xc9};
    memcpy(rom+0x600,copy,sizeof(copy));
    int corrupt=variant==1 || (variant==2 && system!=3);
    DpSegaClosureStats stats;
    check(system,sizeof(rom),(uint16_t)(base+0x503),!corrupt,variant!=3 && !corrupt,&stats);
    CHECK(stats.memory_resolved && stats.static_data_reads);
    if(corrupt) CHECK(stats.stack_invalidations);
    else CHECK(stats.returns_resolved);
    CHECK(stats.context_cuts || stats.node_cuts); // Repeated path cannot grow without a declared cap.
}
static void ports(unsigned system,int slot) {
    uint16_t target=system==3?0x4600:0x600;
    const uint8_t code[]={0x21,(uint8_t)target,(uint8_t)(target>>8),0x06,slot?0xa8:0x99,0x48,0xed,0x79,0xe9};
    fixture(code,sizeof(code)); rom[0x600]=0x76;
    DpSegaClosureStats stats; int mapped=slot && system==3;
    check(system,sizeof(rom),target,!mapped,!mapped,&stats);
    if(mapped) CHECK(stats.map_barriers);
    else CHECK(stats.indirect_resolved && !stats.map_barriers);
}
static void budgets(void) {
    const uint8_t code[]={0x11,0,6,0xeb,0xe9};
    for(unsigned variant=0;variant<4;variant++) {
        fixture(code,sizeof(code)); rom[0x600]=0x76;
        const DpSegaBackend *b=backend(1); CHECK(b->init(rom,sizeof(rom),NULL,1)); b->cpu()->pc=0x500;
        DpSegaClosureOptions o={variant==0?1:64,variant==1?1:8,variant==2?1:4,8};
        DpSegaClosureStats base,targets,blocks;
        DpSegaClosure *c=dp_sega_closure_create(b,profile(1),rom,sizeof(rom),flags,o,&base);
        CHECK(c); CHECK(!dp_sega_closure_blocks(c,&blocks)); CHECK(dp_sega_closure_targets(c,&targets));
        CHECK(!dp_sega_closure_blocks(c,&base) && !dp_sega_closure_blocks(c,&targets));
        CHECK(dp_sega_closure_blocks(c,&blocks)); CHECK(!dp_sega_closure_blocks(c,&blocks));
        dp_sega_closure_observe(c); dp_sega_closure_run(c);
        CHECK(targets.nodes+blocks.nodes<=o.nodes && targets.seeds+blocks.seeds<=o.seeds && targets.views+blocks.views<=o.views);
        if(variant<3) {
            CHECK(!blocks.processed && !flags[0x600]);
            CHECK(blocks.seed_skips || blocks.view_skips);
        } else CHECK(flags[0x600]==DP_SEGA_STATIC && blocks.indirect_resolved==1);
        uint64_t count=blocks.processed,skips=base.seed_skips;
        dp_sega_closure_run(c); CHECK(blocks.processed==count);
        dp_sega_closure_observe(c); CHECK(base.seed_skips==skips+1 && blocks.processed==count);
        dp_sega_closure_free(c);
    }
    DpSegaClosureStats base,targets; DpSegaClosureOptions o={8,4,2,4};
    DpSegaClosure *c=dp_sega_closure_create(backend(1),profile(1),rom,sizeof(rom),flags,o,&base);
    CHECK(c); dp_sega_closure_run(c); CHECK(!dp_sega_closure_targets(c,&targets)); dp_sega_closure_free(c);
}
static void preserve_searches(unsigned system) {
    uint16_t target=system==3?0x4600:0x600;
    const uint8_t code[]={0x11,(uint8_t)target,(uint8_t)(target>>8),0xeb,0xe9};
    fixture(code,sizeof(code)); rom[0x600]=0x76;
    if(system!=3) { rom[0]=0xc3; rom[1]=0; rom[2]=5; }
    DpSegaOptions o=system==3?dp_msx_options():dp_sega_options(); o.msx_mapper=DP_MSX_PLAIN;
    o.max_total=1; o.input_total=0; o.seed_vectors=0; o.closure=(DpSegaClosureOptions){256,32,8,16};
    DpSegaResults on,off;
    CHECK(dp_sega_explore(profile(system),rom,sizeof(rom),&o,&on));
    o.block_closure=0; CHECK(dp_sega_explore(profile(system),rom,sizeof(rom),&o,&off));
    CHECK(on.steps==off.steps && on.steps==1 && on.paths==off.paths);
    CHECK(!memcmp(&on.closure,&off.closure,sizeof(on.closure)) && !memcmp(&on.target_closure,&off.target_closure,sizeof(on.target_closure)));
    CHECK(on.block_closure.nodes+on.target_closure.nodes<=o.closure.nodes);
    CHECK(on.block_closure.seeds+on.target_closure.seeds<=o.closure.seeds && on.block_closure.views+on.target_closure.views<=o.closure.views);
    for(unsigned i=0;i<sizeof(rom);i++) CHECK((on.flags[i]&off.flags[i])==off.flags[i]);
    CHECK((on.flags[0x600]&DP_SEGA_STATIC) && !(off.flags[0x600]&DP_SEGA_STATIC));
    dp_sega_results_free(&on); dp_sega_results_free(&off);
}
int main(void) {
    for(unsigned system=0;system<4;system++) {
        for(unsigned variant=0;variant<20;variant++) transfers(system,variant);
        for(unsigned variant=0;variant<9;variant++) block(system,variant);
        for(unsigned variant=0;variant<4;variant++) stack_and_limits(system,variant);
        ports(system,0); ports(system,1); preserve_searches(system);
    }
    budgets(); TEST_DONE("z80_blocks");
}
