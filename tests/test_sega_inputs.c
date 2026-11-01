#include "../src/platform/sega/backend.h"
#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
static uint8_t rom[0xc000];
static DpSegaResults result;
static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options(); o.max_total=10000; o.input_total=10000; o.max_steps=128;
    o.max_paths=32; o.loop_cap=8; o.fork_cap=0; o.seed_vectors=0; o.stagnation=0;
    return o;
}
static void run(const char *system,DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system(system),rom,sizeof(rom),&o,&result));
    CHECK(!result.provenance_mismatches);
    CHECK(result.input_forks==result.input_count+result.input_log_overflow);
    CHECK(result.input_drops==result.input_queue_cuts+result.input_path_cuts);
    CHECK(result.steps==result.phase_steps[0]+result.phase_steps[1]);
    CHECK(result.paths==result.phase_paths[0]+result.phase_paths[1]);
    for(unsigned p=0;p<2;p++) {
        CHECK(result.phase_steps[p]<=result.phase_limits[p]);
        CHECK(result.phase_paths[p]<=o.max_paths);
    }
    CHECK(result.phase_known[1]>=result.phase_known[0]);
}
static void program(const uint8_t *code,size_t n) { memset(rom,0,sizeof(rom)); memcpy(rom,code,n); }
static void backend_inputs(const DpSegaBackend *b,int sms,int sg) {
    uint8_t cart[32768]={0}; memset(rom,0,sizeof(rom));
    b->init(rom,sizeof(rom),sg?NULL:cart,sms);
    void *state=malloc(b->state_size); CHECK(state); if(!state) exit(1);
    DpSegaInput v={{0x15,0x2a},1,0,0}; b->set_input(v);
    DpSegaInput got=b->input(); CHECK(got.pad[0]==0x15);
    if(sg||sms) {
        CHECK(got.pad[1]==0x2a && !got.start);
        CHECK(b->in(0xabdc)==0x6a && b->in(0x12dd)==0xf5);
    } else {
        CHECK(!got.pad[1]); CHECK(b->in(0xdc)==(uint8_t)~0x15);
        CHECK(got.start && !(b->in(0)&0x80)); CHECK(!b->cpu()->nmi);
    }
    v=(DpSegaInput){{0,0},0,0,0}; b->set_input(v); CHECK(!b->cpu()->nmi);
    if(!sg && !sms) CHECK(b->in(0)&0x80);
    b->save(state);
    v.pause=1; b->set_input(v);
    if(sg||sms) {
        CHECK(b->cpu()->nmi && b->input().pause);
        b->cpu()->sp=0xd000; b->cpu()->iff1=1;
        CHECK(z80_service(b->cpu())==11 && b->cpu()->pc==0x66);
        CHECK(!b->cpu()->iff1 && b->cpu()->iff2);
        b->set_input(v); CHECK(!b->cpu()->nmi); // held Pause has no second edge
        v.pause=0; b->set_input(v); v.pause=1; b->set_input(v); CHECK(b->cpu()->nmi);
    } else CHECK(!b->cpu()->nmi && !b->input().pause);
    b->load(state); CHECK(!b->cpu()->nmi && !b->input().pause && !b->input().start);
    b->cpu()->pc=0x1234; v=(DpSegaInput){{0,63},0,0,1}; b->set_input(v);
    CHECK(b->cpu()->pc==0x1234 && !b->cpu()->nmi);
    if(sms) { CHECK(b->input().reset && !(b->in(0xdd)&16)); }
    else CHECK(!b->input().reset);
    b->load(state); CHECK(!b->input().reset && !b->input().pad[1]);
    free(state);
}
static void pad_paths(void) {
    // The loaded input decides an indirect jump. No conditional flag fork
    // can manufacture this destination; the physical input must change.
    const uint8_t code[]={0x31,0,0xd0,0xdb,0xdc,0x2f,0xe6,0x3f,0x6f,0x26,2,0xe9};
    program(code,sizeof(code)); memset(rom+0x200,0x76,64);
    run("gg",options());
    CHECK(result.input_forks==8 && result.paths==9);
    CHECK(result.flags[0x200]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x210]&DP_SEGA_FORCED); CHECK(result.flags[0x220]&DP_SEGA_FORCED);
    CHECK(!(result.flags[0x210]&DP_SEGA_NATURAL));
    CHECK(!result.forks); // ordinary conditional forking was disabled
    int start=0,release=0;
    for(unsigned i=0;i<result.input_count;i++) {
        CHECK(!result.inputs[i].input.pause);
        start+=result.inputs[i].input.start!=0;
        release+=!result.inputs[i].input.start && !result.inputs[i].input.pad[0];
    }
    CHECK(start && release);
    DpSegaOptions o=options(); o.input_masks=0; run("gg",o);
    CHECK(result.paths==1 && !result.input_forks && !result.input_site_count);
    CHECK(!result.flags[0x210] && !result.flags[0x220]);
    // All 128 pad + Pause combinations are bounded by their explicit masks.
    o=options(); o.input_masks=128; o.max_paths=256; o.queue_cap=256; o.max_total=50000;
    run("gg",o); CHECK(result.input_forks==128 && result.paths==129);
    CHECK(result.flags[0x23f]&DP_SEGA_FORCED); CHECK(result.input_cap_cuts);
}
static void start_and_pause(void) {
    const uint8_t start[]={0x31,0,0xd0,0xdb,0,0x07,0x3e,2,0xce,0,0x67,0x2e,0,0xe9};
    program(start,sizeof(start)); rom[0x200]=rom[0x300]=0x76;
    run("gg",options());
    CHECK(result.input_forks==2 && result.paths==3 && !result.input_log_overflow);
    CHECK(result.flags[0x300]&DP_SEGA_NATURAL); CHECK(result.flags[0x200]&DP_SEGA_FORCED);
    CHECK(!result.flags[0x66]); // Start is polled; no NMI is generated on GG.
    for(unsigned i=0;i<result.input_count;i++) CHECK(!result.inputs[i].input.pause);
    const uint8_t paused[]={0x31,0,0xd0,0xdb,0xdc,0x76};
    program(paused,sizeof(paused));
    const uint8_t handler[]={0x3e,0x55,0x32,0,0xc0,0xed,0x45};
    memcpy(rom+0x66,handler,sizeof(handler));
    run("sg",options()); CHECK(result.flags[0x66]&DP_SEGA_FORCED);
    CHECK(!(result.flags[0x66]&DP_SEGA_NATURAL));
    CHECK(result.input_forks>0 && result.io_read[0xdc]>0);
    run("sms",options()); CHECK(result.flags[0x66]&DP_SEGA_FORCED);
    CHECK(!(result.flags[0x66]&DP_SEGA_NATURAL));
    // Pause must wake a CPU that never reads a controller. The return PC
    // belongs after HALT, so the data read cannot be reached by a pre-HALT fork.
    const uint8_t halted[]={0x31,0,0xd0,0x76,0x3a,0,0x40,0x76,0x18,0xfd};
    program(halted,sizeof(halted)); rom[0x66]=0xed; rom[0x67]=0x45; rom[0x4000]=0x55;
    run("sg",options()); CHECK(result.flags[4]&DP_SEGA_FORCED); CHECK(result.flags[0x4000]&DP_SEGA_DATA);
    CHECK(!(result.flags[4]&DP_SEGA_NATURAL));
    CHECK(result.input_count && result.inputs[0].halted && result.inputs[0].pc==4);
    CHECK(result.interrupts[3] && result.interrupt_count);
    CHECK(result.interrupt_log[0].mode==3 && result.interrupt_log[0].target==0x66);
    CHECK(result.interrupt_log[0].forced && !result.interrupt_log[0].sources);
    run("sms",options()); CHECK(result.flags[4]&DP_SEGA_FORCED); CHECK(result.flags[0x4000]&DP_SEGA_DATA);
    run("gg",options()); CHECK(!result.input_forks && !result.flags[0x4000]);
}
static void mirrored_and_shared_sites(void) {
    // SG pad 2 at the mirrored DD port reaches a real distinct data-derived target.
    const uint8_t p2[]={0x31,0,0xd0,0xdb,0xfd,0x2f,0xe6,15,0x6f,0x26,2,0xe9};
    program(p2,sizeof(p2)); memset(rom+0x200,0x76,16);
    run("sg",options()); CHECK(result.flags[0x204]&DP_SEGA_FORCED);
    CHECK(result.flags[0x208]&DP_SEGA_FORCED);
    int second=0; for(unsigned i=0;i<result.input_count;i++) second|=result.inputs[i].input.pad[1]!=0;
    CHECK(second);
    run("sms",options()); CHECK(result.flags[0x204]&DP_SEGA_FORCED); CHECK(result.flags[0x208]&DP_SEGA_FORCED);
    run("gg",options()); CHECK(!result.input_forks); // no second physical GG pad
    // One ED IN site polls pad then Start: these require independent masks.
    const uint8_t shared[]={0x31,0,0xd0,0x01,0xdc,0,0xcd,0x20,0,0x01,0,0,0xcd,0x20,0,0x76};
    program(shared,sizeof(shared)); rom[0x20]=0xed; rom[0x21]=0x78; rom[0x22]=0xc9;
    run("gg",options()); CHECK(result.input_site_count==2);
    int pad=0,start=0;
    for(unsigned i=0;i<result.input_count;i++) {
        CHECK(result.inputs[i].pc==0x20);
        pad|=(result.inputs[i].port&255)==0xdc; start|=(result.inputs[i].port&255)==0;
    }
    CHECK(pad&&start);
    DpSegaOptions o=options(); o.input_sites=1; run("gg",o);
    CHECK(result.input_site_count==1 && result.input_site_drops);
}
static void sms_digital_pins(void) {
    const DpSegaBackend *b=dp_sega_backend_gg(); uint8_t cart[32768]={0};
    memset(rom,0,sizeof(rom)); b->init(rom,sizeof(rom),cart,1);
    DpSegaInput v={{32,32},0,0,0}; b->set_input(v);
    CHECK(!(b->in(0xdc)&32) && !(b->in(0xdd)&8));
    b->out(0x3f,0x5a); CHECK((b->in(0xdc)&32) && (b->in(0xdd)&8));
    v.pad[0]=v.pad[1]=0; b->set_input(v); b->out(0x133f,0x0a);
    CHECK(!(b->in(0xdc)&32) && !(b->in(0xdd)&8));
    b->out(0x3f,0x05); CHECK(!(b->in(0xdd)&0xc0));
    b->out(0x3f,0xa5); CHECK((b->in(0xdd)&0xc0)==0xc0);
    // The CPU bus must use the adapter too, not the imported one-pad callback.
    b->out(0x3f,0xff); v.pad[1]=16; v.reset=1; b->set_input(v);
    rom[0]=0xdb; rom[1]=0xdd;
    CHECK(z80_step(b->cpu())>0); CHECK(b->cpu()->a==b->in(0xdd)); CHECK(!(b->cpu()->a&0x14));
    const uint8_t reset[]={0x31,0,0xd0,0xdb,0xdd,0xe6,0x10,0x6f,0x26,2,0xe9};
    program(reset,sizeof(reset)); rom[0x200]=rom[0x210]=0x76;
    run("sms",options()); CHECK(result.flags[0x210]&DP_SEGA_NATURAL); CHECK(result.flags[0x200]&DP_SEGA_FORCED);
    int reset_probe=0;
    for(unsigned i=0;i<result.input_count;i++) reset_probe|=!result.inputs[i].halted && result.inputs[i].input.reset;
    CHECK(reset_probe);
    // Pad 2 Up/Down live in DC's upper bits even when DD is never read.
    const uint8_t upper[]={0x31,0,0xd0,0xdb,0xdc,0x2f,0xe6,0xc0,0x6f,0x26,2,0xe9};
    program(upper,sizeof(upper)); rom[0x200]=rom[0x240]=rom[0x280]=rom[0x2c0]=0x76;
    run("sms",options()); CHECK(result.flags[0x240]&DP_SEGA_FORCED); CHECK(result.flags[0x280]&DP_SEGA_FORCED);
    run("sg",options()); CHECK(result.flags[0x240]&DP_SEGA_FORCED); CHECK(result.flags[0x280]&DP_SEGA_FORCED);
}
static void saturation(void) {
    const uint8_t code[]={0x31,0,0xd0,0xdb,0xdc,0x76}; program(code,sizeof(code));
    DpSegaOptions o=options(); o.queue_cap=1; run("gg",o);
    CHECK(result.input_forks && result.input_drops && result.input_queue_cuts);
    o=options(); o.max_paths=1; run("gg",o);
    CHECK(result.input_forks==1 && result.input_drops && result.input_path_cuts);
    CHECK(result.phase_paths[0]==1 && result.phase_paths[1]==1);
    o=options(); o.input_sites=0; run("gg",o);
    CHECK(!result.input_forks && !result.input_site_count && result.input_site_drops);
    const uint8_t poll[]={0xdb,0xdc,0x18,0xfc}; program(poll,sizeof(poll));
    o=options(); o.queue_cap=1; o.loop_cap=1000; o.max_steps=10000; o.input_total=1;
    run("gg",o); CHECK(result.input_retry_skips>100 && result.input_queue_cuts<=2);
}
static void baseline_is_preserved(void) {
    const uint8_t code[]={0x31,0,0xd0,0xdb,0xdc,0xfe,0xff,0x28,2,0x3c,0x3c,0xfe,0xff,0x20,1,0x3d,0x76};
    program(code,sizeof(code)); DpSegaOptions o=options(); o.fork_cap=2; o.queue_cap=1; o.input_masks=0;
    run("gg",o);
    uint8_t *flags=malloc(sizeof(rom)); CHECK(flags); if(!flags) exit(1);
    memcpy(flags,result.flags,sizeof(rom));
    uint64_t steps=result.steps,paths=result.paths,known=result.phase_known[0];
    o.input_masks=8; o.input_total=1; run("gg",o);
    CHECK(result.phase_steps[0]==steps && result.phase_paths[0]==paths && result.phase_known[0]==known);
    CHECK(result.phase_steps[1]==1 && result.phase_pending[1]>0);
    for(size_t i=0;i<sizeof(rom);i++) CHECK((result.flags[i]&flags[i])==flags[i]);
    o.input_total=0; run("gg",o);
    CHECK(!result.input_forks && !result.phase_steps[1]); CHECK(!memcmp(flags,result.flags,sizeof(rom)));
    free(flags);
}
static void block_input(void) {
    const uint8_t code[]={0x31,0,0xd0,0x01,0xdc,2,0x21,0,0xc0,0xed,0xb2,
        0x3a,0,0xc0,0x2f,0xe6,0x3f,0x6f,0x26,2,0xe9};
    program(code,sizeof(code)); memset(rom+0x200,0x76,64);
    run("gg",options());
    CHECK(result.flags[0x200]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x210]&DP_SEGA_FORCED); CHECK(result.flags[0x220]&DP_SEGA_FORCED);
    CHECK(result.io_read[0xdc]>=2 && result.input_forks && !result.ram_copies);
    CHECK(!result.forks); // repetition flags/counts were not artificially changed
}
int main(void) {
    backend_inputs(dp_sega_backend_sg(),0,1);
    backend_inputs(dp_sega_backend_gg(),1,0);
    backend_inputs(dp_sega_backend_gg(),0,0);
    pad_paths(); start_and_pause(); mirrored_and_shared_sites(); saturation(); block_input(); sms_digital_pins(); baseline_is_preserved();
    dp_sega_results_free(&result); TEST_DONE("sega_inputs");
}
