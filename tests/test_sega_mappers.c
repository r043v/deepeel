#include "../src/platform/sega/backend.h"
#include "../src/platform/sega/explore.h"
#include "../src/platform/msx/explore.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

static uint8_t rom[0x10000],cart[0x8000];
static DpSegaResults result;
static void fixture(void) {
    memset(rom,0xff,sizeof(rom));
    for(unsigned i=0;i<sizeof(rom);i++) rom[i]=(uint8_t)(i>>13);
}
static void backend(int sms,unsigned mapper) {
    fixture();
    const DpSegaBackend *b=dp_sega_backend_gg();
    CHECK(b->init(rom,sizeof(rom),cart,sms));
    CHECK(b->select_mapper && b->select_mapper(mapper)); CHECK(b->mapper()==mapper);
    if(mapper==DP_SEGA_MAPPER_SEGA) {
        b->write(0xfffd,2); CHECK(b->rom_index(0)==0 && b->read(0)==0);
        CHECK(b->rom_index(0x400)==0x8400 && b->read(0x400)==4);
    } else if(mapper==DP_SEGA_MAPPER_CODEMASTERS) {
        b->write(0,2); CHECK(b->rom_index(0)==0x8000 && b->read(0)==4);
        CHECK(b->rom_index(0x38)==0x8038); // This board also banks the vectors.
        b->write(0xffff,1); CHECK(b->rom_index(0)==0x8000);
    } else {
        CHECK(b->rom_index(0x4000)==0);
        b->write(0xfff3,1); CHECK(b->rom_index(0)==0x4000 && b->read(0)==2);
        CHECK(b->rom_index(0x2000)==0x6000 && b->rom_index(0x4000)==0x4000);
        b->write(0xfffc,0x20); CHECK(b->rom_index(0x4000)==0x4000);
    }
    void *state=malloc(b->state_size); CHECK(state); if(!state) exit(1);
    int64_t before=b->rom_index(0x4000); b->save(state);
    b->write(0x4000,3); b->write(0xfffe,3); b->write(0xfff3,2);
    b->load(state); CHECK(b->mapper()==mapper && b->rom_index(0x4000)==before);
    CHECK(!b->select_mapper(0) && !b->select_mapper(4));
    CHECK(b->mapper()==mapper && b->rom_index(0x4000)==before);
    free(state);
}
static void run(const char *system,int selected) {
    DpSegaOptions o=dp_sega_options(); o.sega_mapper=selected;
    o.max_steps=24; o.max_total=48; o.input_total=0; o.input_masks=0;
    o.seed_vectors=0; o.fork_cap=0; o.closure.nodes=128; o.closure.seeds=32;
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system(system),rom,sizeof(rom),&o,&result));
}
static void dynamic(const char *system,unsigned mapper) {
    fixture();
    const uint8_t entry[]={0xc3,0,1}; memcpy(rom,entry,sizeof(entry));
    const uint8_t write[]={0x3e,2,0x32,0,0,0x21,0x11,0x11,0x76};
    const uint8_t target[]={0x21,0xcd,0xab,0x76};
    memcpy(rom+0x100,write,sizeof(write));
    // Deliberately looks like a Codemasters checksum, even for the Sega test.
    rom[0x7fe6]=1; rom[0x7fe7]=0; rom[0x7fe8]=rom[0x7fe9]=0xff;
    if(mapper==DP_SEGA_MAPPER_KOREAN150) {
        rom[0x101]=1; rom[0x103]=0xf3; rom[0x104]=0xff;
        memcpy(rom+0x4105,target,sizeof(target));
    } else memcpy(rom+0x8105,target,sizeof(target));
    run(system,(int)mapper);
    CHECK(result.sega_mapper==mapper && result.sega_mapper_explicit);
    if(mapper==DP_SEGA_MAPPER_SEGA) {
        CHECK(result.flags[0x105]&DP_SEGA_NATURAL);
        CHECK(!(result.flags[0x8105]&DP_SEGA_NATURAL));
        CHECK(!result.bank_changes);
    } else {
        unsigned offset=mapper==DP_SEGA_MAPPER_CODEMASTERS?0x8105:0x4105;
        CHECK(result.flags[offset]&DP_SEGA_NATURAL);
        CHECK(!(result.flags[0x105]&DP_SEGA_NATURAL)); CHECK(result.bank_changes==1);
    }
    if(mapper!=DP_SEGA_MAPPER_KOREAN150) {
        run(system,0); CHECK(result.sega_mapper==DP_SEGA_MAPPER_CODEMASTERS);
        CHECK(!result.sega_mapper_explicit && (result.flags[0x8105]&DP_SEGA_NATURAL));
    }
}
static void invalid(void) {
    fixture(); DpSegaOptions o=dp_sega_options(); DpSegaResults unused;
    o.sega_mapper=-1; CHECK(!dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&unused));
    o.sega_mapper=4; CHECK(!dp_sega_explore(dp_sega_system("gg"),rom,sizeof(rom),&o,&unused));
    o.sega_mapper=1; CHECK(!dp_sega_explore(dp_sega_system("sg"),rom,0xc000,&o,&unused));
    CHECK(!dp_sega_explore(dp_msx_system(),rom,sizeof(rom),&o,&unused));
    // Korean150's runtime bank index is eight bits in 8 KiB units.
    size_t size=0x204000; uint8_t *large=calloc(size,1);
    CHECK(large); if(!large) exit(1);
    const DpSegaBackend *b=dp_sega_backend_gg();
    CHECK(b->init(large,size,cart,1)); CHECK(b->select_mapper(DP_SEGA_MAPPER_SEGA));
    CHECK(!b->select_mapper(DP_SEGA_MAPPER_KOREAN150));
    CHECK(b->mapper()==DP_SEGA_MAPPER_SEGA && b->rom_index(0)==0);
    o.sega_mapper=DP_SEGA_MAPPER_KOREAN150;
    CHECK(!dp_sega_explore(dp_sega_system("sms"),large,size,&o,&unused));
    free(large);
}
int main(void) {
    for(unsigned mapper=DP_SEGA_MAPPER_SEGA;mapper<=DP_SEGA_MAPPER_KOREAN150;mapper++) {
        backend(0,mapper); backend(1,mapper); dynamic("sms",mapper); dynamic("gg",mapper);
    }
    invalid(); dp_sega_results_free(&result); TEST_DONE("sega_mappers");
}
