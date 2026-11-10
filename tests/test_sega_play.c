#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <string.h>
#include <stdio.h>

// GB sessions parity: an instrumented play session runs the machine from
// reset with held inputs changing every epoch; its natural coverage carries
// the PLAYED verdict for sections the fork explorer cannot reach on its own
// (here: the SMS Pause NMI handler, and a joypad poll that only proceeds
// while button 1 is held).
static uint8_t rom[0x10000];
static DpSegaResults result;

static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options();
    o.max_total=20000; o.max_steps=256; o.max_paths=64; o.queue_cap=16;
    o.seed_vectors=0; o.seed_cap=0; o.input_masks=0; o.closure.nodes=0;
    o.no_context_evict=1;
    return o;
}
static void fixture(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01;
    rom[0x100]=0x31; rom[0x101]=0x00; rom[0x102]=0xf0; // ld sp,$F000
    rom[0x103]=0xdb; rom[0x104]=0xdc;                  // in a,($DC) pad 1
    rom[0x105]=0xcb; rom[0x106]=0x67;                  // bit 4,a
    rom[0x107]=0x28; rom[0x108]=0x02;                  // jr z,$10B
    rom[0x109]=0x18; rom[0x10a]=0xf8;                  // jr $103
    rom[0x10b]=0x3e; rom[0x10c]=0x77;                  // ld a,$77
    rom[0x10d]=0x32; rom[0x10e]=0x00; rom[0x10f]=0xc1; // ld ($C100),a
    rom[0x110]=0x76;                                   // halt
    // the SMS Pause key raises NMI: the handler only runs while a session
    // holds the system key (vector seeds and input probes are off here)
    rom[0x66]=0xc3; rom[0x67]=0x50; rom[0x68]=0x01;    // jp $0150
    rom[0x150]=0x06; rom[0x151]=0x77;                  // ld b,$77
    rom[0x152]=0xed; rom[0x153]=0x45;                  // retn
}
static void run(DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&result));
}
static DpSegaSection *section_at(unsigned physical) {
    for(uint32_t i=0;i<result.section_count;i++) {
        DpSegaSection *s=&result.sections[i];
        if(s->bank==(uint8_t)(physical>>14) && (physical&0x3fff)>=s->lo && (physical&0x3fff)<=s->hi) return s;
    }
    return NULL;
}
static void play_reaches_held_input_code(void) {
    fixture();
    DpSegaOptions o=options(); o.play_total=4000;
    run(o);
    CHECK(result.play_steps>0 && result.play_epochs>=8);
    CHECK(result.play_exec_hits[0x103]>0 && (result.src_flags[0x103]&DP_SRC_SESSION));
    int attract_seen=0, scripted_seen=0;
    for(uint32_t i=0;i<result.first.n;i++) {
        attract_seen |= result.first.e[i].src==DP_SRC_SESSION && result.first.e[i].actor==DP_ACTOR_ATTRACT;
        scripted_seen |= result.first.e[i].src==DP_SRC_SESSION && result.first.e[i].actor==DP_ACTOR_SCRIPT;
    }
    CHECK(attract_seen && scripted_seen);
    // the poll's guarded code is reached both ways (the jr fork explores the
    // taken side too): REQUIRED, not PLAYED
    CHECK(result.flags[0x10b]&DP_SEGA_NATURAL);
    DpSegaSection *menu=section_at(0x10b);
    CHECK(menu && menu->played_hits>0);
    CHECK(menu->verdict==DP_SEGA_VERDICT_REQUIRED);
    // the Pause NMI handler is session-only coverage: PLAYED
    CHECK((result.flags[0x150]&DP_SEGA_NATURAL) && (result.flags[0x152]&DP_SEGA_NATURAL));
    DpSegaSection *nmi=section_at(0x150);
    CHECK(nmi && nmi->played_hits>0 && nmi->forced_hits==0 && nmi->boot_hits==0);
    CHECK(nmi->verdict==DP_SEGA_VERDICT_PLAYED);
    // the poll itself is also reached by the fork explorer's reset path: boot
    DpSegaSection *poll=section_at(0x103);
    CHECK(poll && poll->boot_hits>0 && poll->verdict==DP_SEGA_VERDICT_REQUIRED);
    // session coverage is deterministic: a second identical run agrees
    size_t first=0;
    for(size_t i=0;i<result.size;i++) if(result.flags[i]) first++;
    run(o);
    size_t second=0;
    for(size_t i=0;i<result.size;i++) if(result.flags[i]) second++;
    CHECK(first==second);
}
static void no_play_no_coverage(void) {
    fixture();
    run(options()); // play_total defaults to 0, input probes off
    CHECK(result.play_steps==0);
    CHECK(!result.flags[0x150]); // nobody pressed Pause: the NMI never fired
    DpSegaSection *nmi=section_at(0x150);
    CHECK(!nmi || nmi->verdict!=DP_SEGA_VERDICT_PLAYED);
}
// J4: the novelty search runs, archives checkpoints and reaches the Pause NMI
// handler (session-only coverage) without any game knowledge.
static void novelty_reaches_pause(void) {
    fixture();
    DpSegaOptions o=options(); o.novelty_total=400000; o.novelty_frames=40; o.novelty_frame_steps=200; o.novelty_archive=32;
    run(o);
    CHECK(result.novelty_trials>0 && result.novelty_frames_run>0 && result.novelty_archived>=1);
    CHECK((result.flags[0x150]&DP_SEGA_NATURAL));
    o.export_state="sega_novelty.dpst"; run(o);
    CHECK(result.state_exported);
    FILE *f=fopen("sega_novelty.dpst","rb"); CHECK(f); char magic[4]={0}; CHECK(f && fread(magic,1,4,f)==4 && !memcmp(magic,"DPST",4)); if(f) fclose(f); remove("sega_novelty.dpst");
}
int main(void) {
    novelty_reaches_pause();
    play_reaches_held_input_code();
    no_play_no_coverage();
    dp_sega_results_free(&result);
    TEST_DONE("sega_play");
}
