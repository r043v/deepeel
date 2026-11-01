#include "../src/platform/sega/backend.h"
#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
static unsigned step(const DpSegaBackend *b) { unsigned cycles=z80_step(b->cpu()); b->advance(cycles); return cycles; }
static void test_machine(const DpSegaBackend *b,int sms,int sg) {
    uint8_t *rom=calloc(1,0x10000), *cart=calloc(1,0x8000);
    void *snapshot=malloc(b->state_size);
    CHECK(rom&&cart&&snapshot); if(!rom||!cart||!snapshot) exit(1);
    // Four distinct physical banks; no commercial ROM is needed.
    for(unsigned i=0x4000;i<0x10000;i++) rom[i]=(uint8_t)(i>>14);
    const uint8_t code[]={0x31,0x00,0xd0,0x3e,0x5a,0x32,0x23,0xc1,0x76};
    memcpy(rom,code,sizeof(code));
    b->init(rom,sg?0xc000:0x10000,sg?NULL:cart,sms);
    CHECK(b->cpu()->pc==0);
    for(unsigned i=0;i<4;i++) CHECK(step(b)>0);
    CHECK(b->read(0xc123)==0x5a); CHECK(b->read(0xe123)==0x5a);
    CHECK(b->cpu()->halted);
    if(sg) CHECK(b->read(0xc523)==0x5a);
    CHECK(b->rom_index(0xc123)<0);
    CHECK(b->rom_index(0x8001)==0x8001); // Sega ROM continues past GB's $8000 boundary.
    if(!sg) {
        b->write(0xffff,3); CHECK(b->read(0x8001)==3); CHECK(b->rom_index(0x8001)==0xc001);
        b->write(0xfffd,2); CHECK(b->read(0x0000)==0x31); CHECK(b->rom_index(0x0000)==0);
        CHECK(b->read(0x0400)==2); CHECK(b->rom_index(0x0400)==0x8400);
        b->write(0xfffc,8); b->write(0x8001,0x77); CHECK(b->read(0x8001)==0x77); CHECK(b->rom_index(0x8001)<0);
        b->write(0xfffc,0); CHECK(b->read(0x8001)==3);
    }
    // VRAM addressing, read-ahead and auto-increment use the actual VDP bus.
    b->out(0xbf,0x34); b->out(0xbf,0x52); b->out(0xbe,0x6a); b->out(0xbe,0x7b);
    b->out(0xbf,0x34); b->out(0xbf,0x12);
    CHECK(b->in(0xbe)==0x6a); CHECK(b->in(0xbe)==0x7b);
    b->cpu()->ix=0xabcd; b->cpu()->bc2=0x6543; b->cpu()->iff2=1;
    b->save(snapshot);
    b->write(0xc123,0x99); b->cpu()->ix=0; b->cpu()->bc2=0;
    if(!sg) b->write(0xffff,1);
    b->load(snapshot);
    CHECK(b->read(0xc123)==0x5a); CHECK(b->cpu()->ix==0xabcd); CHECK(b->cpu()->bc2==0x6543);
    CHECK(b->cpu()->iff2);
    if(!sg) CHECK(b->rom_index(0x8001)==0xc001);
    // A VBlank appears from elapsed machine cycles; status acknowledges INT.
    b->out(0xbf,0x20); b->out(0xbf,0x81);
    b->advance(228*194);
    CHECK(b->cpu()->irq); CHECK(b->in(0xbf)&0x80); CHECK(!b->cpu()->irq);
    free(snapshot); free(cart); free(rom);
}
static void exploration(void) {
    uint8_t *rom=calloc(1,0x10000); CHECK(rom); if(!rom) exit(1);
    // The natural side copies ROM to RAM; the forced side copies ROM to VDP.
    const uint8_t start[]={0xc3,0x00,0x01}; memcpy(rom,start,sizeof(start));
    const uint8_t body[]={0xaf,0x20,0x10,0x21,0x00,0x40,0x11,0x00,0xc0,
        0x01,0x04,0x00,0xed,0xb0,0xc3,0x0e,0x01,0,0,
        0x21,0x00,0x40,0x01,0xbe,0x04,0xed,0xb3,0xc3,0x1b,0x01};
    memcpy(rom+0x100,body,sizeof(body));
    for(unsigned i=0;i<4;i++) rom[0x4000+i]=(uint8_t)(0x50+i);
    DpSegaOptions o=dp_sega_options(); o.seed_cap=0; o.max_total=1000; o.max_steps=100; o.loop_cap=12; o.seed_vectors=0; o.input_masks=0;
    DpSegaResults r;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.forks>=3 && r.paths>=4); CHECK(r.loop_cuts>=2);
    CHECK(r.flags[0x103]&DP_SEGA_NATURAL); CHECK(r.flags[0x113]&DP_SEGA_FORCED);
    CHECK(!(r.flags[0x113]&DP_SEGA_NATURAL));
    CHECK(r.ram_copies>=4 && r.vdp_copies>=4);
    for(unsigned i=0;i<4;i++) CHECK((r.flags[0x4000+i]&(DP_SEGA_DATA|DP_SEGA_COPIED))==(DP_SEGA_DATA|DP_SEGA_COPIED));
    CHECK(r.io_write[0xbe]>=4);
    dp_sega_results_free(&r);
    o.stagnation=3;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.stagnation_cuts>0 && r.loop_cuts==1);
    CHECK(r.flags[0x103]&DP_SEGA_NATURAL);
    dp_sega_results_free(&r); o.stagnation=32768;
    o.max_paths=1;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.paths==1 && r.path_cuts && !r.forks); dp_sega_results_free(&r);
    o.max_paths=4096; o.max_total=2;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.steps==2 && r.global_cuts==1); dp_sega_results_free(&r);
    // Executed RAM contributes no physical ROM flags.
    memset(rom,0,0x10000);
    const uint8_t ramcode[]={0x3e,0x76,0x32,0x00,0xc0,0xc3,0x00,0xc0};
    memcpy(rom,ramcode,sizeof(ramcode)); o.max_total=50; o.loop_cap=4;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.exec_outside==1); CHECK(r.flags[0xc000]==0); dp_sega_results_free(&r);
    // A mapped $8000 read uses the compact physical bank index, not CPU PC.
    memset(rom,0,0x10000);
    const uint8_t bankcode[]={0x3e,3,0x32,0xff,0xff,0x3a,0x00,0x80,0x76};
    memcpy(rom,bankcode,sizeof(bankcode)); rom[0xc000]=0x42;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.flags[0xc000]&DP_SEGA_DATA); CHECK(!r.flags[0x8000]);
    CHECK(r.bank_changes==1 && r.bank_count==1 && r.banks[0].addr==0xffff);
    dp_sega_results_free(&r);
    // Queue saturation is visible while several different branch sites fork.
    // Default: the frontier grows (GB adaptive queue) instead of dropping;
    // a bounded --queue-max keeps the counted refusals visible.
    memset(rom,0,0x10000);
    const uint8_t branches[]={0xaf,0x20,1,0,0x20,1,0,0x76};
    memcpy(rom,branches,sizeof(branches)); o.queue_cap=1; o.loop_cap=12; o.max_total=200;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.queue_growths>0 && r.forks>0 && r.queue_cuts==0); dp_sega_results_free(&r);
    o.queue_max=1;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.queue_cuts>0 && r.forks>0 && r.queue_growths==0); dp_sega_results_free(&r);
    o.queue_max=4096;
    // VDP-driven INT is dispatched naturally; the alternative delay is forked.
    memset(rom,0,0x10000); memcpy(rom,start,sizeof(start));
    const uint8_t irqcode[]={0x31,0x00,0xd0,0xed,0x56,0x3e,0x20,0xd3,0xbf,
        0x3e,0x81,0xd3,0xbf,0xfb,0x76,0x18,0xfd};
    const uint8_t handler[]={0xdb,0xbf,0xfb,0xed,0x4d};
    memcpy(rom+0x100,irqcode,sizeof(irqcode)); memcpy(rom+0x38,handler,sizeof(handler));
    o.queue_cap=8; o.loop_cap=20000; o.max_steps=15000; o.max_total=30000; o.fork_cap=1;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.irq_forks>0); CHECK(r.flags[0x38]&DP_SEGA_NATURAL); CHECK(r.io_read[0xbf]>0);
    dp_sega_results_free(&r);
    free(rom);
}
int main(void) {
    test_machine(dp_sega_backend_sg(),0,1);
    test_machine(dp_sega_backend_gg(),0,0);
    test_machine(dp_sega_backend_gg(),1,0);
    exploration();
    TEST_DONE("sega");
}
