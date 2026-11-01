#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <string.h>

// GB dp_seed_indirect parity: an indirect jump whose target the static
// closure cannot prove (closure disabled here) is resolved by observation —
// the executed jp (hl) records its target, and a second pass seeds every
// target that still carries no execution evidence. The phase's budget
// refuses the remainder, counted; 0 disables the pass entirely.
static uint8_t rom[0x10000];
static DpSegaResults result;

static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options();
    // max_steps=6: the jp (hl) is the last instruction the base pass runs,
    // so its target carries no execution evidence — exactly the case the
    // indirect pass exists for (GB dp_seed_indirect parity)
    o.max_total=100000; o.max_steps=6; o.max_paths=64; o.queue_cap=16;
    o.seed_vectors=0; o.seed_cap=0; o.input_masks=0; o.closure.nodes=0;
    o.no_context_evict=1;
    return o;
}
static void fixture(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01;
    rom[0x100]=0x31; rom[0x101]=0x00; rom[0x102]=0xf0; // ld sp,$F000
    rom[0x103]=0x21; rom[0x104]=0x20; rom[0x105]=0x01; // ld hl,$0120
    rom[0x106]=0x6e;                                   // ld a,(hl)   -> $30
    rom[0x107]=0x26; rom[0x108]=0x01;                  // ld h,$01    -> hl=$0130
    rom[0x109]=0xe9;                                   // jp (hl)
    rom[0x120]=0x30;                                   // dispatch byte
    rom[0x130]=0x0e; rom[0x131]=0x07; rom[0x132]=0x76; // ld c,7; halt
}
static void run(DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&result));
}
static void observed_and_seeded(void) {
    fixture();
    run(options());
    // the jump site was observed with exactly one distinct target
    CHECK(result.indirect_sites==1 && result.indirect_targets==1);
    CHECK(!result.indirect_site_overflow && !result.indirect_target_overflow);
    // the static closure is off, so only the indirect pass reached $0130
    CHECK(result.indirect_seeds==1 && !result.indirect_refused_budget && !result.indirect_pending);
    CHECK(result.phase_limits[2]>0);
    CHECK(result.flags[0x130]&DP_SEGA_FORCED);
    CHECK(result.phase_steps[2]>0);
    CHECK(result.phase_known[2]>=3);
}
static void pass_disabled(void) {
    fixture();
    DpSegaOptions o=options(); o.indirect_total=0;
    run(o);
    // the observation still happens, but nothing is seeded
    CHECK(result.indirect_sites==1 && result.indirect_targets==1);
    CHECK(result.indirect_seeds==0 && result.phase_limits[2]==0);
    CHECK(!result.flags[0x130]);
}
static void budget_bounds(void) {
    fixture();
    DpSegaOptions o=options(); o.indirect_total=1; // one step of the pass only
    run(o);
    CHECK(result.indirect_seeds==1);
    CHECK(result.phase_steps[2]<=1);
    CHECK(result.phase_pending[2]>=1); // the bounded pass left work, counted
    CHECK(result.flags[0x130]&DP_SEGA_FORCED);
}
int main(void) {
    observed_and_seeded();
    pass_disabled();
    budget_bounds();
    dp_sega_results_free(&result);
    TEST_DONE("sega_indirect");
}
