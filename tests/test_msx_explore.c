#include "../src/platform/msx/msx.h"
#include "../src/platform/msx/explore.h"
#include "test_util.h"
#include <string.h>
static uint8_t rom[0x10000];
static DpSegaResults result;
static DpSegaOptions options(void) {
    DpSegaOptions o=dp_msx_options();
    o.max_total=4096; o.max_steps=256; o.max_paths=16; o.loop_cap=16;
    o.input_masks=0; o.seed_cap=0; o.msx_mapper=DP_MSX_PLAIN; return o;
}
static void fixture(const uint8_t *code,size_t size) {
    memset(rom,0,sizeof(rom)); rom[0]='A'; rom[1]='B'; rom[2]=0x10; rom[3]=0x40;
    memcpy(rom+0x10,code,size); rom[0x2000]=0x52; rom[0x2001]=0x73;
    rom[0x2002]=0x26; rom[0x2003]=0xa9; rom[0x4000]=0x3b;
}
static void run(DpSegaOptions o,size_t size) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_msx_system(),rom,size,&o,&result));
    CHECK(!result.provenance_mismatches); CHECK(!dp_msx_cpu());
    uint64_t logged=result.copy_log_overflow;
    for(unsigned i=0;i<result.copy_count;i++) logged+=(uint64_t)result.copies[i].length*result.copies[i].occurrences;
    CHECK(logged==result.ram_copies+result.vdp_copies);
}
static uint64_t copies(unsigned source,unsigned space,unsigned destination) {
    uint64_t n=0;
    for(unsigned i=0;i<result.copy_count;i++) {
        DpSegaCopy *c=&result.copies[i];
        if(c->space!=space) continue;
        for(unsigned j=0;j<c->length;j++)
            if((int64_t)c->source+(int64_t)c->source_stride*j==source &&
               (int64_t)c->destination+(int64_t)c->destination_stride*j==destination) n+=c->occurrences;
    }
    return n;
}
static void branches(void) {
    const uint8_t code[]={0x31,0,0xf0,0xaf,0x28,3,0x3e,1,0x76,0x3e,2,0x76};
    fixture(code,sizeof(code)); run(options(),0x8000);
    CHECK(result.forks && result.paths>=2);
    CHECK(result.flags[0x16]&DP_SEGA_FORCED); CHECK(result.flags[0x19]&DP_SEGA_NATURAL);
    CHECK(!result.flags[0] && !result.flags[0x38]);
}
static void bios_copies(void) {
    const uint8_t code[]={0x31,0,0xf0,0x21,0,0x60,0x11,0x34,0x12,0x01,4,0,
        0xcd,0x5c,0,0x76};
    fixture(code,sizeof(code)); run(options(),0x8000);
    CHECK(result.bios_calls[15]==1 && result.exec_outside>=2);
    CHECK(result.data_reads==4 && result.vram_copies==4);
    CHECK(result.io_write[0x98]==4 && result.io_write[0x99]==2 && !result.io_write[15]);
    CHECK(copies(0x2000,DP_SEGA_VRAM,0x1234)==1 && copies(0x2003,DP_SEGA_VRAM,0x1237)==1);
    CHECK(result.flags[0x2000]&DP_SEGA_COPIED);
    DpSegaOptions o=options(); o.copy_cap=0; run(o,0x8000);
    CHECK(result.copy_log_overflow==4 && result.vram_copies==4);
    const uint8_t fill[]={0x31,0,0xf0,0x3a,0,0x60,0x21,0,0x10,0x01,3,0,0xcd,0x56,0,0x76};
    fixture(fill,sizeof(fill)); run(options(),0x8000);
    CHECK(result.bios_calls[13]==1 && result.vram_copies==3);
    CHECK(copies(0x2000,DP_SEGA_VRAM,0x1002)==1);
}
static void physical_ram(void) {
    // The simulated boot leaves the VDP pointer at $0800; data writes use
    // the actual runtime target rather than assuming power-on address zero.
    const uint8_t code[]={0x31,0,0xf0,0x3a,0,0x60,0x32,0,0xff,0x3a,0,0xff,0xd3,0x98,
        0x3a,0,0xe0,0xd3,0x98,0x76};
    fixture(code,sizeof(code)); run(options(),0x8000);
    CHECK(result.ram_copies==1 && result.vram_copies==1);
    CHECK(copies(0x2000,DP_SEGA_WRAM,0xff00)==1);
    CHECK(copies(0x2000,DP_SEGA_VRAM,0x800)==1);
    // WRSLT writes physical hidden RAM; RDSLT reads it while cartridge ROM
    // remains visible at the same CPU address.
    const uint8_t hidden[]={0x31,0,0xf0,0x3a,0,0x60,0x5f,0x3e,3,0x21,1,0x60,
        0xcd,0x14,0,0x3e,3,0xcd,0x0c,0,0xd3,0x98,0x76};
    fixture(hidden,sizeof(hidden)); run(options(),0x8000);
    CHECK(result.bios_calls[1]==1 && result.bios_calls[2]==1);
    CHECK(copies(0x2000,DP_SEGA_WRAM,0x6001)==1 && copies(0x2000,DP_SEGA_VRAM,0x800)==1);
    CHECK(!(result.flags[0x2001]&DP_SEGA_COPIED));
    const uint8_t slot_rom[]={0x31,0,0xf0,0x3e,1,0x21,0,0x80,0xcd,0x0c,0,0xd3,0x98,0x76};
    fixture(slot_rom,sizeof(slot_rom)); run(options(),0x8000);
    CHECK(result.data_reads==1 && copies(0x4000,DP_SEGA_VRAM,0x800)==1);
    // A C helper's returned value does not inherit an unrelated input A tag.
    const uint8_t changed[]={0x31,0,0xf0,0x3a,0,0x60,0x21,0,0,0xcd,0x4a,0,0xd3,0x98,0x76};
    fixture(changed,sizeof(changed)); rom[0x2000]=0; run(options(),0x8000);
    CHECK(!result.vram_copies && !result.provenance_mismatches);
}
static void snapshot_origins(void) {
    // The natural path clears a high RAM tag. The queued opposite branch
    // must restore the earlier $FF00 tag, independently of the current RAM.
    const uint8_t code[]={0x3a,0,0x60,0x32,0,0xff,0xaf,0x28,3,
        0xc3,0x1f,0x40,0x32,0,0xff,0x3a,0,0xff,0xd3,0x98,0x76};
    fixture(code,sizeof(code)); run(options(),0x8000);
    CHECK(result.forks==1 && copies(0x2000,DP_SEGA_VRAM,0x800)==1);
    int forced=0;
    for(unsigned i=0;i<result.copy_count;i++) if(result.copies[i].space==DP_SEGA_VRAM) forced|=result.copies[i].forced;
    CHECK(forced);
}
static void hardware(void) {
    const uint8_t code[]={0x31,0,0xf0,0x3e,0xd4,0xd3,0xa8,0x3a,0,0x80,0xd3,0x98,
        0x3e,0xf4,0xcd,0x3b,1,0x06,0x20,0x0e,1,0xcd,0x47,0,
        0x3e,7,0x1e,0x3f,0xcd,0x93,0,0x3e,8,0xd3,0xa0,0x3e,15,0xd3,0xa1,0x76};
    fixture(code,sizeof(code)); run(options(),0x8000);
    CHECK(result.bank_changes==2 && result.bank_count==2);
    CHECK(result.banks[0].kind==1 && result.banks[0].addr==0xd4a8);
    CHECK(result.banks[1].kind==2 && result.banks[1].addr==34);
    CHECK(result.vdp_register_writes[1]==1 && result.register_log[0].port==0x99);
    CHECK(result.ay_writes[7]==1 && result.ay_writes[8]==1 && result.ay_count==2);
    CHECK(result.ay_log[0].port==0 && result.ay_log[1].port==0xa1);
    CHECK(!result.io_write[34] && !result.io_write[28] && !result.io_write[8]);
    CHECK(copies(0x4000,DP_SEGA_VRAM,0x800)==1);
    const uint8_t aliases[]={0x3a,0,0x60,0xd3,0xbe,0x76};
    fixture(aliases,sizeof(aliases)); run(options(),0x8000); CHECK(!result.vram_copies);
    const uint8_t mapper[]={0x3e,3,0x32,0,0x68,0x3a,0,0x60,0xd3,0x98,0x76};
    fixture(mapper,sizeof(mapper)); rom[0x6000]=0xb6;
    DpSegaOptions o=options(); o.msx_mapper=DP_MSX_ASCII8; run(o,sizeof(rom));
    CHECK(result.bank_changes==1 && result.banks[0].kind==0 && result.msx_mapper==DP_MSX_ASCII8);
    CHECK(copies(0x6000,DP_SEGA_VRAM,0x800)==1);
}
static void inputs(void) {
    const uint8_t code[]={0x31,0,0xf0,0x3e,7,0xcd,0x41,1,0x76};
    fixture(code,sizeof(code)); DpSegaOptions o=options(); o.input_masks=8; o.input_total=1024;
    run(o,0x8000); CHECK(result.input_forks && result.phase_paths[1]);
    int seen=0;
    for(unsigned i=0;i<result.input_count;i++) {
        seen|=result.inputs[i].input.start; CHECK(!result.inputs[i].input.pause && !result.inputs[i].halted);
    }
    CHECK(seen && !result.interrupts[3]);
    const uint8_t pad[]={0x3e,14,0xd3,0xa0,0xdb,0xa2,0x76};
    fixture(pad,sizeof(pad)); run(o,0x8000); CHECK(result.input_forks && !result.interrupts[3]);
    for(unsigned i=0;i<result.input_count;i++) CHECK((result.inputs[i].port&255)==0xa2);
}
static void limits(void) {
    const uint8_t loop[]={0x3e,7,0xd3,0xa0,0x3e,0x3f,0xd3,0xa1,0xc3,0x16,0x40};
    fixture(loop,sizeof(loop)); DpSegaOptions o=options();
    o.max_total=o.max_steps=o.loop_cap=5000; o.max_paths=1;
    run(o,0x8000);
    CHECK(result.ay_writes[7]>DP_SEGA_HARDWARE_LOG_CAP && result.ay_count==DP_SEGA_HARDWARE_LOG_CAP);
    CHECK(result.ay_writes[7]==result.ay_count+result.ay_log_overflow && result.step_cuts);
    o=options(); o.closure.nodes=1048577;
    CHECK(!dp_sega_explore(dp_msx_system(),rom,0x8000,&o,&result));
    // Invalid requests must leave an existing result owned by the caller.
    CHECK(result.ay_count==DP_SEGA_HARDWARE_LOG_CAP);
    o=options(); o.msx_mapper=DP_MSX_KONAMI_SCC+1;
    CHECK(!dp_sega_explore(dp_msx_system(),rom,0x8000,&o,&result));
}
static void interrupts(void) {
    const uint8_t code[]={0x31,0,0xf0,0x3e,0,0x32,0xff,0xf0,0x3e,0x42,0x32,0,0xf1,
        0x3e,0xf0,0xed,0x47,0xed,0x5e,0x3e,0x20,0xd3,0x99,0x3e,0x81,0xd3,0x99,
        0xfb,0x76,0xc3,0x2d,0x40};
    fixture(code,sizeof(code)); const uint8_t handler[]={0xdb,0x99,0xfb,0xed,0x4d};
    memcpy(rom+0x200,handler,sizeof(handler));
    DpSegaOptions o=options(); o.max_total=o.max_steps=o.loop_cap=40000; o.max_paths=1; o.fork_cap=0;
    run(o,0x8000); CHECK(result.interrupts[2] && !result.interrupts[3]);
    CHECK(result.interrupt_log[0].target==0x4200 && result.interrupt_log[0].target_rom==0x200);
    CHECK(result.interrupt_log[0].sources==DP_SEGA_IRQ_FRAME && (result.flags[0x200]&DP_SEGA_NATURAL));
}
int main(void) {
    branches(); bios_copies(); physical_ram(); snapshot_origins(); hardware(); inputs(); limits(); interrupts();
    dp_sega_results_free(&result); TEST_DONE("msx_explore");
}
