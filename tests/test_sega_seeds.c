#include "../src/platform/sega/backend.h"
#include "../src/platform/sega/explore.h"
#include "../src/platform/msx/explore.h"
#include "../src/platform/msx/msx.h"
#include "test_util.h"
#include <string.h>

// GB dp_seed_gb parity: pointer-table scan, bank sweep and explicit entries.
// A dispatch word (flanked by two other switchable-window words) seeds every
// switchable bank's view of its target; isolated words seed the boot view;
// padding targets are skipped; every admission or refusal is counted.
static uint8_t rom[0x10000];
static DpSegaResults result;

static DpSegaOptions seeded(void) {
    DpSegaOptions o=dp_sega_options();
    o.max_total=20000; o.max_steps=256; o.max_paths=64; o.queue_cap=64;
    o.seed_vectors=0; o.input_masks=0; o.closure.nodes=0;
    return o;
}
// Bank 0 jumps to a HALT main; the dispatch table at $0200 holds three
// switchable-window words; banks 2 and 3 carry code at $8005 that the reset
// path can never reach on its own.
static void dispatch_fixture(uint16_t target) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01; // jp $0100
    rom[0x100]=0x76; // halt
    rom[0x1fe]=(uint8_t)(target+0x10); rom[0x1ff]=(uint8_t)((target+0x10)>>8); // table head
    rom[0x200]=(uint8_t)target; rom[0x201]=(uint8_t)(target>>8);
    rom[0x202]=(uint8_t)(target+5); rom[0x203]=(uint8_t)((target+5)>>8);
    rom[0x204]=(uint8_t)(target+0x0a); rom[0x205]=(uint8_t)((target+0x0a)>>8);
    rom[0x8005]=0x21; rom[0x8006]=0x34; rom[0x8007]=0x12; rom[0x8008]=0x76; // bank 2: ld hl,$1234; halt
    rom[0xc005]=0x11; rom[0xc006]=0x78; rom[0xc007]=0x56; rom[0xc008]=0x76; // bank 3: ld de,$5678; halt
}
static void run(const char *system,DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system(system),rom,sizeof(rom),&o,&result));
}
static void pointer_dispatch(void) {
    dispatch_fixture(0x8005);
    DpSegaOptions o=seeded();
    run("sms",o);
    CHECK(result.seeds_pointer>=2 && result.seeds_admitted>=2);
    CHECK(result.seeds_skipped_padding>0); // bank 0/1 views and $800A/$800F targets are padding
    CHECK(result.flags[0x8005]&DP_SEGA_FORCED);
    CHECK(result.flags[0xc005]&DP_SEGA_FORCED); // only a dispatch view reaches bank 3
    CHECK(!result.seeds_refused_view && !result.seeds_pending);
}
static void pointer_disabled(void) {
    dispatch_fixture(0x8005);
    DpSegaOptions o=seeded(); o.no_pointer_scan=1;
    run("sms",o);
    CHECK(result.seeds_pointer==0 && result.seeds_admitted==0);
    CHECK(!result.flags[0x8005] && !result.flags[0xc005]);
}
static void bank_sweep(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01; rom[0x100]=0x76;
    rom[0xc000]=0x76; // the only non-padding window-base entry, in bank 3
    DpSegaOptions o=seeded(); o.seed_cap=0; o.seed_banks=1;
    run("sms",o);
    // bank 0 re-enters its own jp; banks 1/2 bases are padding and skipped.
    CHECK(result.seeds_bank==2 && result.seeds_admitted==2);
    CHECK(result.flags[0xc000]&DP_SEGA_FORCED);
    CHECK(!result.flags[0x4000] && !result.flags[0x8000]);
}
static void explicit_entry(void) {
    dispatch_fixture(0x8005);
    DpSegaOptions o=seeded(); o.seed_cap=0; o.no_pointer_scan=1;
    o.entry[0].bank=3; o.entry[0].addr=0x8005; o.entry_count=1;
    run("sms",o);
    CHECK(result.seeds_entry==1 && result.seeds_admitted==1);
    CHECK(result.flags[0xc005]&DP_SEGA_FORCED);
    CHECK(!result.flags[0x8005]); // no scan ran: bank 2 stays unreached
}
// Korean150 switches the low 32 KiB in 8 KiB steps: mode $00 shows 16 KiB bank
// b at every even slot base, so the dispatch target $4005 reaches banks 2/3.
static void korean150_dispatch(void) {
    dispatch_fixture(0x4005);
    DpSegaOptions o=seeded(); o.sega_mapper=DP_SEGA_MAPPER_KOREAN150;
    run("sms",o);
    CHECK(result.flags[0x8005]&DP_SEGA_FORCED);
    CHECK(result.flags[0xc005]&DP_SEGA_FORCED);
    CHECK(!result.seeds_refused_view);
}
// ASCII8 banks 16 KiB windows $4000/$8000 through the $7000/$7800 selectors;
// the dispatch word $8005 seeds bank 3's view into VRAM-page window w4.
static void msx_ascii8_dispatch(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]='A'; rom[1]='B'; rom[2]=0x10; rom[3]=0x40; // INIT $4010
    rom[0x4010]=0x31; rom[0x4011]=0x00; rom[0x4012]=0xf0; rom[0x4013]=0x76; // ld sp,$F000; halt
    rom[0x1fe]=0x10; rom[0x1ff]=0x80; // table head $8010
    rom[0x200]=0x05; rom[0x201]=0x80;
    rom[0x202]=0x0a; rom[0x203]=0x80;
    rom[0x204]=0x0f; rom[0x205]=0x80;
    rom[0xc005]=0x21; rom[0xc006]=0x34; rom[0xc007]=0x12; rom[0xc008]=0x76; // bank 3: ld hl,$1234; halt
    DpSegaOptions o=seeded(); o.msx_mapper=DP_MSX_ASCII8;
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_msx_system(),rom,sizeof(rom),&o,&result));
    CHECK(result.seeds_admitted>=1);
    CHECK(result.flags[0xc005]&DP_SEGA_FORCED);
    CHECK(!result.seeds_refused_view && !result.provenance_mismatches);
}
int main(void) {
    pointer_dispatch();
    pointer_disabled();
    bank_sweep();
    explicit_entry();
    korean150_dispatch();
    msx_ascii8_dispatch();
    dp_sega_results_free(&result);
    TEST_DONE("sega_seeds");
}
