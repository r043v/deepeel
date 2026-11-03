#include "../src/platform/sega/explore.h"
#include "../src/platform/msx/explore.h"
#include "../src/platform/msx/msx.h"
#include "test_util.h"
#include <string.h>

// Parallel drain parity: --jobs 2 splits the seed list round-robin across two
// prefixed runtime clones; after the join the merged evidence must match the
// single-core run byte for byte on disjoint-seed fixtures, seeds routed to the
// other lane are counted, a request above two instances is capped (counted),
// and MSX (no clone) falls back single core without dropping coverage.
static uint8_t rom[0x10000];
static DpSegaResults one,two,three;

// dispatch fixture: reset reaches nothing but the boot HALT; banks 2 and 3
// only run through dispatch-table seeds (disjoint subtrees per seed)
static void fixture(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01;
    rom[0x100]=0x76; // halt
    rom[0x1fe]=0x15; rom[0x1ff]=0x80; // table head $8015
    rom[0x200]=0x05; rom[0x201]=0x80;
    rom[0x202]=0x0a; rom[0x203]=0x80;
    rom[0x204]=0x0f; rom[0x205]=0x80;
    rom[0x8005]=0x21; rom[0x8006]=0x34; rom[0x8007]=0x12; rom[0x8008]=0x76;
    rom[0xc005]=0x11; rom[0xc006]=0x78; rom[0xc007]=0x56; rom[0xc008]=0x76;
}
static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options();
    o.max_total=20000; o.max_steps=256; o.max_paths=64; o.queue_cap=64;
    o.seed_vectors=0; o.input_masks=0; o.closure.nodes=0;
    return o;
}
static size_t known(const DpSegaResults *r) {
    size_t n=0;
    for(size_t i=0;i<r->size;i++) if(r->flags[i]) n++;
    return n;
}
static void same_flags(const DpSegaResults *a,const DpSegaResults *b) {
    CHECK(a->size==b->size);
    for(size_t i=0;i<a->size;i++) CHECK(a->flags[i]==b->flags[i]);
}
int main(void) {
    fixture();
    DpSegaOptions o=options();
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&one));
    CHECK((one.flags[0x8005]&DP_SEGA_FORCED) && (one.flags[0xc005]&DP_SEGA_FORCED));
    // two jobs: same evidence, the off-lane seeds are counted, nothing refused
    o.jobs=2;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&two));
    same_flags(&one,&two);
    CHECK(two.seeds_offlane>0 && !two.seeds_refused_view && !two.jobs_capped);
    // every worker generates the same spec list, so generated-seed counters
    // sum across lanes; admissions stay disjoint and identical to one core
    CHECK(two.seeds_pointer==2*one.seeds_pointer);
    CHECK(two.seeds_admitted==one.seeds_admitted);
    // three requested, two instances: the excess is capped, counted, and the
    // merged evidence still agrees with the single-core run
    o.jobs=3;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&three));
    same_flags(&one,&three);
    CHECK(three.jobs_capped==1);
    // MSX runs on its prefixed second instance: two workers, merged evidence
    // equal to the single-core run, nothing capped
    memset(rom,0xff,sizeof(rom));
    rom[0]='A'; rom[1]='B'; rom[2]=0x10; rom[3]=0x40;
    rom[0x4010]=0x31; rom[0x4011]=0x00; rom[0x4012]=0xf0; rom[0x4013]=0x76;
    DpSegaOptions mo=options(); mo.msx_mapper=DP_MSX_ASCII8; mo.jobs=2; mo.seed_cap=0; mo.seed_banks=1;
    dp_sega_results_free(&two); dp_sega_results_free(&three);
    CHECK(dp_sega_explore(dp_msx_system(),rom,sizeof(rom),&mo,&two));
    DpSegaOptions m1=mo; m1.jobs=1;
    CHECK(dp_sega_explore(dp_msx_system(),rom,sizeof(rom),&m1,&three));
    CHECK(two.jobs_capped==0 && two.seeds_admitted>=1);
    CHECK(known(&two)==known(&three));
    same_flags(&three,&two);
    dp_sega_results_free(&one); dp_sega_results_free(&two); dp_sega_results_free(&three);
    TEST_DONE("sega_jobs");
}
