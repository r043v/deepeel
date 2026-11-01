#include "../src/platform/sega/closure.h"
#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

static void scenario(int overwrite,int mapper,int bounded) {
    uint8_t *rom=malloc(0x10000),*flags=calloc(0x10000,1),*cart=calloc(0x8000,1);
    CHECK(rom&&flags&&cart); if(!rom||!flags||!cart) exit(1);
    memset(rom,0xff,0x10000);
    const uint8_t entry[]={0x31,0x00,0xd0,0xcd,0x00,0x06,0x76};
    memcpy(rom+0x500,entry,sizeof(entry));
    const uint8_t safe[]={0x0e,0xbe,0xed,0x79,0xc9};
    const uint8_t corrupt[]={0x32,0xfe,0xcf,0xc9};
    const uint8_t bank[]={0x32,0xff,0xff,0xc3,0x00,0x80};
    const uint8_t loop[]={0xcd,0x00,0x06,0xc9};
    memcpy(rom+0x600,bounded?loop:mapper?bank:overwrite?corrupt:safe,
        bounded?sizeof(loop):mapper?sizeof(bank):overwrite?sizeof(corrupt):sizeof(safe));
    rom[0x8000]=0x76;
    const DpSegaBackend *b=dp_sega_backend_gg(); b->init(rom,0x10000,cart,1);
    b->cpu()->pc=0x500;
    DpSegaClosureStats s;
    DpSegaClosureOptions o={128,8,4,64};
    DpSegaClosure *c=dp_sega_closure_create(b,dp_sega_system("sms"),rom,0x10000,flags,o,&s);
    CHECK(c); dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(b->cpu()->pc==0x500); CHECK(!b->cpu()->halted);
    CHECK(flags[0x500]==DP_SEGA_STATIC); CHECK(s.processed==s.nodes && s.nodes<=128);
    if(bounded) CHECK(s.stack_cuts || s.node_cuts || s.context_cuts);
    else if(mapper) { CHECK(!flags[0x8000]); CHECK(s.ambiguous && s.map_barriers); }
    else if(overwrite) { CHECK(!flags[0x506]); CHECK(s.stack_invalidations && s.indirect); }
    else { CHECK(flags[0x506]==DP_SEGA_STATIC); CHECK(s.returns_resolved==1); }
    uint64_t count=s.processed; dp_sega_closure_run(c); CHECK(s.processed==count);
    dp_sega_closure_free(c); free(cart); free(flags); free(rom);
}
static void branches_and_limits(void) {
    uint8_t *rom=malloc(0x10000),*flags=calloc(0x10000,1),*cart=calloc(0x8000,1);
    CHECK(rom&&flags&&cart); if(!rom||!flags||!cart) exit(1);
    memset(rom,0xff,0x10000);
    const uint8_t code[]={0x20,0x02,0x76,0xff,0x76};
    memcpy(rom+0x500,code,sizeof(code));
    const DpSegaBackend *b=dp_sega_backend_gg();
    for(unsigned variant=0;variant<4;variant++) {
        memset(flags,0,0x10000); b->init(rom,0x10000,cart,1); b->cpu()->pc=0x500;
        if(variant==1) flags[0x504]=DP_SEGA_COPIED;
        DpSegaClosureOptions o={variant==2?1:32,8,1,8};
        DpSegaClosureStats stats;
        DpSegaClosure *c=dp_sega_closure_create(b,dp_sega_system("sms"),rom,0x10000,flags,o,&stats);
        CHECK(c); dp_sega_closure_observe(c);
        if(variant==3) {
            b->write(0xffff,3); dp_sega_closure_invalidate(c); dp_sega_closure_observe(c);
            CHECK(stats.view_skips==1); CHECK(stats.views==1);
        }
        dp_sega_closure_run(c);
        if(variant==2) CHECK(stats.node_cuts && stats.nodes==1);
        else {
            CHECK(flags[0x502]==DP_SEGA_STATIC);
            if(variant==1) { CHECK(flags[0x504]==DP_SEGA_COPIED); CHECK(stats.data==1); }
            else CHECK(flags[0x504]==DP_SEGA_STATIC);
        }
        dp_sega_closure_free(c);
    }
    free(cart); free(flags); free(rom);
}
static void exploration_cutoff(void) {
    uint8_t *rom=malloc(0x4000); CHECK(rom); if(!rom) exit(1);
    memset(rom,0xff,0x4000); rom[0]=0xc3; rom[1]=0; rom[2]=5; rom[0x500]=0x76;
    DpSegaOptions o=dp_sega_options(); o.max_total=1; o.input_total=0; o.seed_vectors=0;
    DpSegaResults r;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x4000,&o,&r));
    CHECK(r.steps==1); CHECK(r.flags[0]&DP_SEGA_NATURAL);
    CHECK(r.flags[0x500]==DP_SEGA_STATIC); CHECK(r.closure.new_bytes==1);
    CHECK(!r.ram_copies && !r.vdp_copies);
    dp_sega_results_free(&r); free(rom);
}
int main(void) {
    scenario(0,0,0); scenario(1,0,0); scenario(0,1,0); scenario(0,0,1);
    branches_and_limits(); exploration_cutoff();
    TEST_DONE("sega_closure");
}
