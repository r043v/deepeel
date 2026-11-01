#include "../src/platform/sega/explore.h"
#include "../src/platform/sega/visits.h"
#include "test_util.h"
#include <string.h>

static uint8_t rom[0x10000];
static DpSegaResults result;
static void table(void) {
    DpSegaVisits v={0};
    // ROM index zero is real, not the empty-bucket sentinel.
    CHECK(dp_sega_visits_enter(&v,0,0,1)==1);
    CHECK(dp_sega_visits_enter(&v,0,0,1)==0);
    CHECK(dp_sega_visits_enter(&v,0x4000,0,1)==1); // New bank, same CPU address.
    CHECK(dp_sega_visits_enter(&v,0,0x4000,1)==1); // Same byte, new CPU window.
    CHECK(dp_sega_visits_enter(&v,0x10000,0,1)==1); // Outside-ROM identity.
    CHECK(v.used==4 && v.peak==4);
    for(unsigned i=0;i<65536;i++) CHECK(dp_sega_visits_enter(&v,0x20000+i,(uint16_t)i,2)==1);
    CHECK(v.used==65540 && v.peak==65540 && v.growths>1);
    for(unsigned i=0;i<65536;i++) {
        CHECK(dp_sega_visits_enter(&v,0x20000+i,(uint16_t)i,2)==1);
        CHECK(dp_sega_visits_enter(&v,0x20000+i,(uint16_t)i,2)==0);
    }
    size_t capacity=v.capacity,peak=v.peak; uint64_t growths=v.growths;
    dp_sega_visits_reset(&v); CHECK(!v.used && v.peak==peak);
    CHECK(dp_sega_visits_enter(&v,0,0,1)==1);
    CHECK(v.capacity==capacity && v.growths==growths);
    CHECK(dp_sega_visits_enter(&v,UINT32_MAX,0,1)==-1);
    dp_sega_visits_free(&v); CHECK(!v.slots && !v.capacity && !v.used);
}
static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options(); o.max_steps=24; o.max_total=96;
    o.input_total=0; o.input_masks=0; o.closure.nodes=0; o.seed_vectors=0;
    o.fork_cap=0; o.loop_cap=1; return o;
}
static void run(const char *system,size_t size,DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system(system),rom,size,&o,&result));
    CHECK(result.loop_context_peak && result.loop_context_peak<=o.max_steps);
    CHECK(result.loop_context_capacity>=2*result.loop_context_peak);
    CHECK(result.loop_context_growths && result.loop_cuts>0);
    CHECK(result.steps<=o.max_total);
}
static void bank_switch(const char *system) {
    memset(rom,0xff,sizeof(rom));
    const uint8_t boot[]={0xc3,0,0x40};
    const uint8_t bank1[]={0x3e,2,0x32,0xfe,0xff};
    const uint8_t bank2[]={0x21,0xcd,0xab,0x76,0,0xc3,0,0x40};
    memcpy(rom,boot,sizeof(boot)); memcpy(rom+0x4000,bank1,sizeof(bank1));
    memcpy(rom+0x8000,bank2,sizeof(bank2));
    run(system,sizeof(rom),options());
    CHECK(result.flags[0x4000]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x8000]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x8002]&DP_SEGA_NATURAL);
    CHECK(result.bank_changes==1 && result.paths==1);
}
static void aliases(void) {
    memset(rom,0xff,sizeof(rom));
    const uint8_t boot[]={0x21,0,0x40,0xc3,0,1};
    const uint8_t body[]={0x24,0xe9}; // INC H; JP (HL), at $0100 and its $4100 alias.
    const uint8_t target[]={0x01,0xa5,0x5a,0x76};
    memcpy(rom,boot,sizeof(boot)); memcpy(rom+0x100,body,sizeof(body));
    memcpy(rom+0x200,target,sizeof(target));
    run("sg",0x4000,options());
    CHECK(result.flags[0x100]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x200]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x202]&DP_SEGA_NATURAL);
}
static void paths(void) {
    memset(rom,0xff,sizeof(rom));
    const uint8_t boot[]={0xc3,0,1};
    const uint8_t branch[]={0xc2,0,2,0x21,0xcd,0xab,0x76};
    memcpy(rom,boot,sizeof(boot)); memcpy(rom+0x100,branch,sizeof(branch)); rom[0x200]=0x76;
    DpSegaOptions o=options(); o.fork_cap=1; run("sms",sizeof(rom),o);
    CHECK(result.paths==2 && result.forks==1 && result.loop_cuts==2);
    CHECK(result.flags[0x200]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x103]&DP_SEGA_FORCED);
}
int main(void) {
    table(); bank_switch("sms"); bank_switch("gg"); aliases(); paths();
    dp_sega_results_free(&result); TEST_DONE("sega_visits");
}
