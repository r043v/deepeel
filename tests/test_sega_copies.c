#include "../src/platform/sega/explore.h"
#include "../src/platform/sega/backend.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
static DpSegaResults result;
static uint8_t rom[0x10000];
static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options(); o.seed_cap=0;
    o.max_total=4096; o.max_steps=256; o.loop_cap=4; o.max_paths=1; o.fork_cap=0; o.seed_vectors=0; o.input_masks=0;
    return o;
}
static void program(const uint8_t *bytes,size_t size) {
    memset(rom,0,sizeof(rom)); memcpy(rom,bytes,size);
    rom[0x4000]=0x25; rom[0x4001]=0x12; rom[0x4002]=0x91; rom[0x4003]=0x43;
}
static void run(const char *system,DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system(system),rom,!strcmp(system,"sg")?0xc000:sizeof(rom),&o,&result));
    CHECK(!result.provenance_mismatches);
    uint64_t recorded=result.copy_log_overflow;
    for(unsigned i=0;i<result.copy_count;i++) {
        const DpSegaCopy *c=&result.copies[i];
        CHECK(c->length && c->occurrences && c->first_step<=c->last_step);
        recorded+=(uint64_t)c->length*c->occurrences;
    }
    CHECK(recorded==result.ram_copies+result.vdp_copies);
}
static uint64_t bytes_from(unsigned source,unsigned space) {
    uint64_t count=0;
    for(unsigned i=0;i<result.copy_count;i++) {
        const DpSegaCopy *c=&result.copies[i];
        if(c->space!=space) continue;
        for(unsigned j=0;j<c->length;j++) if((int64_t)c->source+(int64_t)c->source_stride*j==source) count+=c->occurrences;
    }
    return count;
}
static void cpu_loop(void) {
    const uint8_t code[]={0x21,0x00,0x40,0x06,4,0x0e,0xbe,0x7e,0x23,0xed,0x79,0x10,0xfa,0x76};
    program(code,sizeof(code));
    const char *systems[]={"sg","sms","gg"};
    for(unsigned s=0;s<3;s++) {
        run(systems[s],options()); CHECK(result.vram_copies==4 && !result.cram_copies);
        CHECK(result.copy_count==1 && !result.copy_log_overflow);
        CHECK(result.copies[0].source==0x4000 && result.copies[0].destination==0);
        CHECK(result.copies[0].length==4 && result.copies[0].source_stride==1 && result.copies[0].destination_stride==1 && result.copies[0].verbatim);
        for(unsigned i=0;i<4;i++) CHECK(result.flags[0x4000+i]&DP_SEGA_COPIED);
    }
    DpSegaOptions o=options(); o.copy_cap=0; run("sms",o);
    CHECK(!result.copy_count && result.copy_log_overflow==4 && result.vram_copies==4);
    CHECK(result.flags[0x4000]&DP_SEGA_COPIED);
    const uint8_t backwards[]={0x21,3,0x40,0x11,3,0xc0,0x01,4,0,0xed,0xb8,0x76};
    program(backwards,sizeof(backwards)); run("sms",options());
    CHECK(result.ram_copies==4 && result.copy_count==1);
    CHECK(result.copies[0].source==0x4003 && result.copies[0].destination==3);
    CHECK(result.copies[0].length==4 && result.copies[0].source_stride==-1 && result.copies[0].destination_stride==-1);
    const uint8_t otdr[]={0x21,3,0x40,0x01,0xbe,4,0xed,0xbb,0x76};
    program(otdr,sizeof(otdr)); run("sms",options());
    CHECK(result.copy_count==1 && result.copies[0].length==4);
    CHECK(result.copies[0].source_stride==-1 && result.copies[0].destination_stride==1);
    const uint8_t fill[]={0x21,0,0xc0,0x36,0,0x11,1,0xc0,0x01,16,0,0xed,0xb0,0x76};
    program(fill,sizeof(fill)); o=options(); o.loop_cap=32; run("sms",o);
    CHECK(result.copy_count==2 && result.ram_copies==17);
    CHECK(result.copies[1].source==4 && result.copies[1].length==16);
    CHECK(result.copies[1].source_stride==0 && result.copies[1].destination_stride==1);
}
static void ram_and_stack(void) {
    const uint8_t ram[]={0x3a,0,0x40,0x32,0x10,0xc0,0x3e,0,0x3a,0x10,0xe0,0xd3,0xbe,0x76};
    program(ram,sizeof(ram)); run("sms",options());
    CHECK(bytes_from(0x4000,DP_SEGA_WRAM)==1 && bytes_from(0x4000,DP_SEGA_VRAM)==1);
    DpSegaOptions cap=options(); cap.copy_cap=1; run("sms",cap);
    CHECK(result.copy_count==1 && result.copy_log_overflow==1 && result.vram_copies==1);
    const uint8_t repeated[]={0x06,2,0x3a,0,0x40,0x32,0x10,0xc0,0xd3,0xbe,0x10,0xf9,0x76};
    program(repeated,sizeof(repeated)); run("sms",cap);
    CHECK(result.copy_count==1 && result.copy_log_overflow==2);
    CHECK(result.copies[0].occurrences==2 && result.ram_copies==2 && result.vram_copies==2);
    const uint8_t overwritten[]={0x3a,0,0x40,0x32,0x10,0xc0,0xee,0,0x32,0x10,0xe0,0x3a,0x10,0xc0,0xd3,0xbe,0x76};
    program(overwritten,sizeof(overwritten)); run("sms",options());
    CHECK(result.ram_copies==1 && !result.vdp_copies);
    const uint8_t stack[]={0x31,0,0xd0,0x3a,0,0x40,0x57,0x1e,0,0xd5,0xaf,0x16,0,0xd1,0x7a,0xd3,0xbe,0x76};
    program(stack,sizeof(stack)); run("sms",options());
    CHECK(bytes_from(0x4000,DP_SEGA_WRAM)==1 && bytes_from(0x4000,DP_SEGA_VRAM)==1);
    const uint8_t exchange[]={0x31,0,0xd0,0x2a,0,0x40,0xe5,0x21,0,0,0xe3,0x7d,0xd3,0xbe,0x7c,0xd3,0xbe,0x76};
    program(exchange,sizeof(exchange)); run("sms",options());
    CHECK(bytes_from(0x4000,DP_SEGA_VRAM)==1 && bytes_from(0x4001,DP_SEGA_VRAM)==1);
    const uint8_t exx[]={0x3a,0,0x40,0x57,0xd9,0x16,0,0xd9,0x7a,0xd3,0xbe,0x76};
    program(exx,sizeof(exx)); run("sms",options()); CHECK(bytes_from(0x4000,DP_SEGA_VRAM)==1);
    const uint8_t af[]={0x3a,0,0x40,0x08,0xaf,0x08,0xd3,0xbe,0x76};
    program(af,sizeof(af)); run("sms",options()); CHECK(bytes_from(0x4000,DP_SEGA_VRAM)==1);
}
static void counterexamples(void) {
    const uint8_t changed[]={0x3a,0,0x40,0xee,0,0xd3,0xbe,0x76};
    program(changed,sizeof(changed)); run("sms",options());
    CHECK(!result.vdp_copies && !(result.flags[0x4000]&DP_SEGA_COPIED));
    // Equal immediate replaces the origin; it does not retain the old ROM data tag.
    const uint8_t immediate[]={0x3a,0,0x40,0x3e,0x25,0xd3,0xbe,0x76};
    program(immediate,sizeof(immediate)); run("sms",options());
    CHECK(!bytes_from(0x4000,DP_SEGA_VRAM)); CHECK(bytes_from(4,DP_SEGA_VRAM)==1);
    CHECK(!(result.flags[0x4000]&DP_SEGA_COPIED));
    // ED OUT (C),0 cannot inherit a coincidentally equal previous data read.
    const uint8_t zero[]={0x3a,0,0x40,0x0e,0xbe,0xed,0x71,0x76};
    program(zero,sizeof(zero)); rom[0x4000]=0; run("sms",options());
    CHECK(!result.vdp_copies && !(result.flags[0x4000]&DP_SEGA_COPIED));
    // An untracked input overwrites A even if the external byte happens to match.
    const uint8_t input[]={0x3a,0,0x40,0xdb,0x10,0xd3,0xbe,0x76};
    program(input,sizeof(input)); rom[0x4000]=0xff; run("sms",options());
    CHECK(!result.vdp_copies);
}
static void banks_and_forks(void) {
    const uint8_t bank[]={0x3e,3,0x32,0xff,0xff,0x3a,0,0x80,0x57,0x3e,2,0x32,0xff,0xff,0x7a,0xd3,0xbe,0x76};
    program(bank,sizeof(bank)); rom[0xc000]=0x57; run("sms",options());
    CHECK(bytes_from(0xc000,DP_SEGA_VRAM)==1 && !bytes_from(0x8000,DP_SEGA_VRAM));
    const uint8_t sram[]={0x3e,8,0x32,0xfc,0xff,0x3a,0,0x40,0x32,0,0x80,
        0x3e,12,0x32,0xfc,0xff,0x3a,0,0x80,0xd3,0xbe,
        0x3e,8,0x32,0xfc,0xff,0x3a,0,0x80,0xd3,0xbe,0x76};
    program(sram,sizeof(sram)); rom[0x4000]=0; run("sms",options());
    CHECK(bytes_from(0x4000,DP_SEGA_SRAM)==1 && bytes_from(0x4000,DP_SEGA_VRAM)==1);
    const uint8_t branch[]={0x3a,0,0x40,0xfe,0,0x28,5,0xd3,0xbe,0xaf,0x18,4,0,0,0xd3,0xbe,0x76};
    program(branch,sizeof(branch)); DpSegaOptions o=options(); o.max_paths=3; o.fork_cap=1;
    run("sms",o); CHECK(result.paths==2 && result.vram_copies==2);
    CHECK(bytes_from(0x4000,DP_SEGA_VRAM)==2);
    const uint8_t rambranch[]={0x3a,0,0x40,0x32,0x10,0xc0,0xfe,0,0x28,7,
        0x3e,0,0x32,0x10,0xe0,0x18,5,0x3a,0x10,0xc0,0xd3,0xbe,0x76};
    program(rambranch,sizeof(rambranch)); run("sms",o);
    CHECK(result.paths==2 && bytes_from(0x4000,DP_SEGA_VRAM)==1);
    // A forced flag must not carry a false source through PUSH AF's F byte.
    const uint8_t forcedflag[]={0x31,0,0xd0,0x21,0,0x40,0xe5,0xf1,0x28,0,0xf5,0xc1,0xed,0x49,0x76};
    program(forcedflag,sizeof(forcedflag)); rom[0x4000]=0xbe;
    run("sms",o); CHECK(!result.provenance_mismatches);
}
static void colour_latch(void) {
    const uint8_t code[]={0x3e,0,0xd3,0xbf,0x3e,0xc0,0xd3,0xbf,
        0x3a,0,0x40,0xd3,0xbe,0xfe,0,0x28,0,
        0x3a,1,0x40,0xd3,0xbe,0x3a,2,0x40,0xd3,0xbe,0x76};
    program(code,sizeof(code)); rom[0x4001]=0x0b; rom[0x4002]=rom[0x4000];
    DpSegaOptions o=options(); o.max_total=6;
    run("gg",o); CHECK(!result.cram_copies && !result.copy_count);
    CHECK(!(result.flags[0x4000]&DP_SEGA_COPIED)); // even write is still only latched
    o=options(); o.max_paths=3; o.fork_cap=1;
    run("gg",o); CHECK(result.cram_copies==4 && !result.vram_copies);
    CHECK(bytes_from(0x4000,DP_SEGA_CRAM)==2 && bytes_from(0x4001,DP_SEGA_CRAM)==2);
    CHECK(!bytes_from(0x4002,DP_SEGA_CRAM) && !(result.flags[0x4002]&DP_SEGA_COPIED));
    const uint8_t masked[]={0x3e,0,0xd3,0xbf,0x3e,0xc0,0xd3,0xbf,0x3a,0,0x40,0xd3,0xbe,0x3a,1,0x40,0xd3,0xbe,0x76};
    program(masked,sizeof(masked)); rom[0x4001]=0xab;
    run("gg",options()); CHECK(result.cram_copies==2 && result.copy_transformed==1);
    CHECK(result.flags[0x4000]&DP_SEGA_COPIED); CHECK(!(result.flags[0x4001]&DP_SEGA_COPIED));
    CHECK(result.copy_count==2 && result.copies[0].verbatim && !result.copies[1].verbatim);
    program(masked,sizeof(masked)); rom[0x4000]=0xc5;
    run("sms",options()); CHECK(result.cram_copies==2 && result.copy_transformed==1);
    CHECK(!(result.flags[0x4000]&DP_SEGA_COPIED)); CHECK(result.flags[0x4001]&DP_SEGA_COPIED);
    // An odd address reuses the previous even latch, even in another colour.
    const uint8_t reused[]={0x3e,0,0xd3,0xbf,0x3e,0xc0,0xd3,0xbf,0x3a,0,0x40,0xd3,0xbe,
        0x3a,1,0x40,0xd3,0xbe,0x3e,3,0xd3,0xbf,0x3e,0xc0,0xd3,0xbf,0x3a,1,0x40,0xd3,0xbe,0x76};
    program(reused,sizeof(reused)); rom[0x4001]=0x0b;
    run("gg",options()); CHECK(result.cram_copies==4 && bytes_from(0x4000,DP_SEGA_CRAM)==2);
    CHECK(result.copies[result.copy_count-1].destination==2 && result.copies[result.copy_count-1].length==2);
}
int main(void) {
    cpu_loop(); ram_and_stack(); counterexamples(); banks_and_forks(); colour_latch();
    dp_sega_results_free(&result); TEST_DONE("sega_copies");
}
