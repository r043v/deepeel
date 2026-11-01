#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// GB parity: the frontier grows adaptively, overflow states survive on disk
// (--resume replays them into identical coverage) and every queued path is a
// choice-tree node with a closed issue. The spool run uses a frontier capped
// at one slot: everything overflows to the file, the resume pass replays the
// records, and the union of both passes must match one unconstrained run.
static uint8_t rom[0x10000];
static DpSegaResults result;

static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options();
    o.max_total=100000; o.max_steps=256; o.max_paths=64; o.queue_cap=16;
    o.seed_vectors=0; o.seed_cap=0; o.input_masks=0; o.closure.nodes=0;
    // Same-context eviction makes coverage order-sensitive (a section's
    // liveness mask grows while paths run). The spool contract under test is
    // frontier order-independence, so the runs below switch it off.
    o.no_context_evict=1;
    return o;
}
// A branch fan: each of the three sites forks once, so several paths run.
static void fixture(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01;
    rom[0x100]=0x31; rom[0x101]=0x00; rom[0x102]=0xf0;
    rom[0x103]=0xaf;                       // xor a
    rom[0x104]=0x20; rom[0x105]=0x12;      // jr nz,$118 (never taken)
    rom[0x106]=0x20; rom[0x107]=0x14;      // jr nz,$11C (never taken)
    rom[0x108]=0x20; rom[0x109]=0x02;      // jr nz,$10A (never taken)
    rom[0x10a]=0x21; rom[0x10b]=0x00; rom[0x10c]=0x03; // ld hl,$0300
    rom[0x10d]=0x11; rom[0x10e]=0x00; rom[0x10f]=0xc0; // ld de,$C000
    rom[0x110]=0x01; rom[0x111]=0x04; rom[0x112]=0x00; // ld bc,4
    rom[0x113]=0xed; rom[0x114]=0xb0;      // ldir
    rom[0x115]=0x76;                       // halt
    rom[0x118]=0x0e; rom[0x119]=0x04; rom[0x11a]=0x76; // ld c,4; halt
    rom[0x11c]=0x0e; rom[0x11d]=0x05; rom[0x11e]=0x76; // ld c,5; halt
    rom[0x300]=0x11; rom[0x301]=0x22; rom[0x302]=0x33; rom[0x303]=0x44;
}
static void run(DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&result));
}
static size_t known(void) {
    size_t n=0;
    for(size_t i=0;i<result.size;i++) if(result.flags[i]) n++;
    return n;
}
static void growing_frontier(void) {
    fixture();
    DpSegaOptions o=options(); o.queue_cap=1; // growth ceiling is the default 4096
    run(o);
    CHECK(result.queue_growths>0 && result.queue_cuts==0);
    CHECK(result.forks>=3);
    CHECK(result.flags[0x10c]&DP_SEGA_NATURAL);
    CHECK(result.flags[0x300]&DP_SEGA_COPIED); // ldir copy proof survived
}
static void spool_resume_identical(void) {
    fixture();
    const char *spool="/tmp/dp_sega_spool_test.bin";
    // reference run: no spooling, generous frontier
    run(options());
    size_t reference=known();
    CHECK((result.flags[0x115]&DP_SEGA_NATURAL) && (result.flags[0x300]&DP_SEGA_COPIED));
    // a one-slot frontier spills to disk and feeds the states back: the same
    // coverage is reached without dropping any fork
    DpSegaOptions o=options(); o.queue_cap=1; o.queue_max=1; o.spool_path=spool;
    run(o);
    CHECK(result.spool_writes>0 && !result.spool_refused);
    CHECK(known()==reference);
    // an interrupted run keeps its unread records: a budget wall closes the
    // phase with work still on disk
    o.max_total=400;
    run(o);
    size_t partial=known();
    CHECK(partial<reference); // the taken sides of the fan stayed on disk
    // resume: replay the spooled records into the full exploration
    o.resume=1; o.max_total=100000;
    run(o);
    CHECK(result.spool_reads>0);
    CHECK(known()==reference); // spool + resume = one unconstrained run
    unlink(spool);
}
static void choice_tree(void) {
    fixture();
    const char *tree="/tmp/dp_sega_tree_test.bin";
    DpSegaOptions o=options(); o.queue_cap=1; o.queue_max=1; o.spool_path="/tmp/dp_sega_spool_tree.bin"; o.tree_path=tree;
    o.max_total=400; // a budget wall leaves flushed remainders: SPOOLED issues
    run(o);
    CHECK(result.tree_count>=4); // reset + branch forks + flushed remainders
    CHECK(result.tree_nodes[0].kind==DP_SEGA_TREE_RESET && result.tree_nodes[0].parent==0);
    unsigned closed=0,evicted=0,spooled=0;
    for(uint32_t i=0;i<result.tree_count;i++) {
        if(result.tree_nodes[i].issue==DP_SEGA_ISSUE_DONE) closed++;
        if(result.tree_nodes[i].issue==DP_SEGA_ISSUE_EVICTED) evicted++;
        if(result.tree_nodes[i].issue==DP_SEGA_ISSUE_SPOOLED) spooled++;
    }
    CHECK(closed>=1 && spooled>=1 && evicted==0);
    // the binary record round-trips
    FILE *f=fopen(tree,"rb"); CHECK(f);
    if(f) {
        uint32_t hdr[2];
        CHECK(fread(hdr,sizeof hdr,1,f)==1 && hdr[0]==0x5a545044u && hdr[1]==1);
        CHECK(hdr[1]==1 && hdr[0]==0x5a545044u);
        unsigned nodes=0;
        for(;;) {
            uint32_t rec[2]; uint16_t pc; uint8_t ki[2];
            if(fread(rec,sizeof rec,1,f)!=1 || fread(&pc,2,1,f)!=1 || fread(ki,1,2,f)!=2) break;
            CHECK(rec[0]<result.tree_count+1); // parents stay in range
            nodes++;
        }
        CHECK(nodes==result.tree_count);
        fclose(f);
    }
    unlink(tree); unlink("/tmp/dp_sega_spool_tree.bin");
}
int main(void) {
    growing_frontier();
    spool_resume_identical();
    choice_tree();
    dp_sega_results_free(&result);
    TEST_DONE("sega_spool");
}
