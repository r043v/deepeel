#include <unistd.h>
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
static void deferral(void) {
    uint8_t *rom=calloc(1,0x10000); CHECK(rom); if(!rom) exit(1);
    const uint8_t start[]={0xc3,0x00,0x01}; memcpy(rom,start,sizeof(start));
    memset(rom+0x100,0x40,100);              // 100 "ld b,b", then a marker and HALT (a $00 run would be a padding cut)
    const uint8_t tail[]={0x3e,0x07,0x76}; memcpy(rom+0x100+100,tail,sizeof(tail));
    DpSegaOptions o=dp_sega_options(); o.seed_cap=0; o.max_total=100000; o.max_steps=5; o.seed_vectors=0; o.input_masks=0;
    DpSegaResults r;
    // deferral off: the step budget is a final wall
    o.defer_tiers=0;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.step_cuts>0 && !(r.flags[0x164]&DP_SEGA_NATURAL) && r.steps_deferred==0);
    dp_sega_results_free(&r);
    // deferral on: the cut resumes with a 4x cap and reaches the marker
    o.defer_tiers=3;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.steps_deferred>=3 && r.defer_resumed==r.steps_deferred && r.defer_pending==0);
    CHECK(r.flags[0x164]&DP_SEGA_NATURAL);
    {   // sparse first-discovery rows: one per proven byte, each consistent
        size_t any=0; for(size_t i=0;i<r.size;i++) any+=!!r.src_flags[i];
        int bad=0; for(uint32_t i=0;i<r.first.n;i++) bad+=!(r.src_flags[r.first.e[i].key]&r.first.e[i].src);
        CHECK(r.first.n>0 && r.first.n+r.first.dropped==any && !bad);
    }
    dp_sega_results_free(&r);
    {   // a wall with a deferred cut still waiting: spooled with its tier, resumed by --resume
        char sp[]="/tmp/dp_sega_wallXXXXXX"; int fd=mkstemp(sp); CHECK(fd>=0); if(fd>=0) close(fd);
        o.defer_tiers=3; o.spool_path=sp; o.max_total=5; o.input_total=0; o.indirect_total=0;
        int ok=dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r);
        CHECK(ok);
        uint64_t pend=0;
        if(ok) { pend=r.defer_pending; CHECK(r.defer_spooled==r.defer_pending); CHECK(!(r.flags[0x164]&DP_SEGA_NATURAL)); dp_sega_results_free(&r); }
        CHECK(pend>0); // the wall left a deferral behind: the case is really exercised
        o.resume=1; o.max_total=100000;
        ok=dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r);
        CHECK(ok);
        if(ok) { CHECK(r.flags[0x164]&DP_SEGA_NATURAL); dp_sega_results_free(&r); }
        o.resume=0; o.spool_path=NULL; unlink(sp);
    }
    {   // the portable lot: a wall exports the unread spool, the next run imports it and empties the file
        char sp[]="/tmp/dp_sega_lspXXXXXX", lp[]="/tmp/dp_sega_lotXXXXXX";
        int fd=mkstemp(sp); if(fd>=0) close(fd);
        fd=mkstemp(lp); if(fd>=0) close(fd);
        unlink(lp);
        o.defer_tiers=3; o.spool_path=sp; o.lot_path=lp; o.max_total=5; o.input_total=0; o.indirect_total=0;
        int ok=dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r);
        CHECK(ok);
        uint64_t left=0;
        if(ok) { left=r.lot_pending; CHECK(r.lot_written==left); dp_sega_results_free(&r); }
        CHECK(left>0);
        CHECK(access(lp,F_OK)==0);
        o.max_total=100000;
        ok=dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r);
        CHECK(ok);
        if(ok) { CHECK(r.lot_read==left); CHECK(r.flags[0x164]&DP_SEGA_NATURAL); dp_sega_results_free(&r); }
        CHECK(access(lp,F_OK)!=0);
        o.spool_path=NULL; o.lot_path=NULL; unlink(sp); unlink(lp);
    }
    // too few tiers: the final cut is counted, never silent
    o.defer_tiers=1;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.steps_final>0 && !(r.flags[0x164]&DP_SEGA_NATURAL));
    dp_sega_results_free(&r);
    free(rom);
}
static void fork_deferral(void) {
    uint8_t *rom=calloc(1,0x10000); CHECK(rom); if(!rom) exit(1);
    const uint8_t start[]={0xc3,0x00,0x01}; memcpy(rom,start,sizeof(start));
    // xor a ; jr z,+3 ; ld a,7 (only the forced side) ; halt ; halt
    const uint8_t body[]={0xaf,0x28,0x03,0x3e,0x07,0x76,0x76}; memcpy(rom+0x100,body,sizeof(body));
    DpSegaOptions o=dp_sega_options(); o.seed_cap=0; o.max_total=100000; o.seed_vectors=0; o.input_masks=0;
    o.max_paths=1;
    DpSegaResults r;
    o.defer_tiers=0;   // no holding area: the path cap drops the fork, counted
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.path_cuts>0 && !(r.flags[0x103]&DP_SEGA_FORCED));
    dp_sega_results_free(&r);
    o.defer_tiers=3;   // the fork waits and runs later
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.fork_deferred>0 && r.defer_pending==0 && (r.flags[0x103]&DP_SEGA_FORCED));
    dp_sega_results_free(&r);
    {   // a wall with deferred forks still waiting: they are spooled, not lost
        // (the fixture usually resumes them in-run; the spooled == pending identity
        // and the resumed coverage hold either way)
        char sp[]="/tmp/dp_sega_deferXXXXXX"; int fd=mkstemp(sp); CHECK(fd>=0); if(fd>=0) close(fd);
        o.spool_path=sp; o.max_total=3;
        int ok=dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r);
        CHECK(ok);
        uint64_t spooled=0;
        if(ok) { CHECK(r.defer_spooled==r.defer_pending); spooled=r.defer_spooled; dp_sega_results_free(&r); }
        // --resume replays the spooled deferrals: the forced side is reached
        o.resume=1; o.max_total=100000;
        ok=dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r);
        CHECK(ok);
        if(ok) { CHECK(r.flags[0x103]&DP_SEGA_FORCED); dp_sega_results_free(&r); }
        o.resume=0; o.spool_path=NULL;
        unlink(sp);
    }
    free(rom);
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
// a path sliding into a $00 run is a non-sense path: cut, counted, never silent
static void padding_cut(void) {
    uint8_t *rom=calloc(1,0x10000); CHECK(rom); if(!rom) exit(1);
    const uint8_t start[]={0xc3,0x00,0x01}; memcpy(rom,start,sizeof(start));
    memset(rom+0x100,0x00,64);               // 64 NOPs: padding
    rom[0x100+64]=0x76;
    DpSegaOptions o=dp_sega_options(); DpSegaResults r;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.pad_cuts>0 && !(r.flags[0x100+64]&DP_SEGA_NATURAL));
    dp_sega_results_free(&r);
    free(rom);
}
// .cov / romdata: hit counts come from the exploration, one line per known byte
static void cov_output(void) {
    uint8_t *rom=calloc(1,0x10000); CHECK(rom); if(!rom) exit(1);
    const uint8_t start[]={0xc3,0x00,0x01}; memcpy(rom,start,sizeof(start));
    // ld hl,$2000 ; ld a,(hl) ; halt   (one data read at $2000)
    const uint8_t code[]={0x21,0x00,0x20,0x7e,0x76}; memcpy(rom+0x100,code,sizeof(code));
    rom[0x2000]=0x42;
    DpSegaOptions o=dp_sega_options(); o.seed_cap=0; o.seed_banks=0; DpSegaResults r;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.exec_hits[0x100]>=1 && r.data_hits[0x2000]>=1);
    FILE *f=tmpfile(); CHECK(f);
    if(f) {
        dp_sega_cov(f,&r); rewind(f);
        char line[128]; int exec=0,data=0;
        while(fgets(line,sizeof line,f)) {
            unsigned b,a,fl,h;
            if(sscanf(line,"%x %x %x %x",&b,&a,&fl,&h)==4) {
                if(b==0 && a==0x100 && fl==1 && h>=1) exec=1;
                if(b==0 && a==0x2000 && fl==8 && h>=1) data=1;
            }
        }
        CHECK(exec && data);
        rewind(f); fclose(f);
    }
    f=tmpfile();
    if(f) {
        dp_sega_romdata(f,&r); rewind(f);
        unsigned page; unsigned long long n; CHECK(fscanf(f,"%x %llu",&page,&n)==2 && page==0x20 && n>=1);
        fclose(f);
    }
    dp_sega_results_free(&r);
    free(rom);
}
// S5: a verbatim 16-byte ROM->VRAM copy whose source is a word of a pointer
// table; the other entries become inferred data of the same length
static void copy_siblings(void) {
    uint8_t *rom=calloc(1,0x10000); CHECK(rom); if(!rom) exit(1);
    const uint8_t code[]={0x21,0x01,0x10,0x01,0x01,0x10,0x09,0x23,0x23,0x23, // hl=$2002, never a literal
        0x23,0x5e,0x23,0x56,0xeb,                                            // de=(hl+1): entry 2
        0x3e,0x00,0xd3,0xbf,0x3e,0x40,0xd3,0xbf,                             // VDP write address 0
        0x06,0x10,0x0e,0xbe,0xed,0xb3,0x76};                                 // otir 16 bytes
    memcpy(rom,(const uint8_t[]){0xc3,0x00,0x01},3); memcpy(rom+0x100,code,sizeof code);
    const uint16_t table[3]={0x3000,0x3100,0x3200};
    for(int i=0;i<3;i++) { rom[0x2002+2*i]=(uint8_t)table[i]; rom[0x2003+2*i]=(uint8_t)(table[i]>>8); }
    for(int t=0;t<3;t++) for(int k=0;k<32;k++) rom[table[t]+k]=(uint8_t)(0x11+k);
    DpSegaOptions o=dp_sega_options(); DpSegaResults r;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.sib_tables>=1 && r.sib_ranges==2 && r.sib_bytes==32);
    CHECK((r.static_flags[0x3000]&DP_SEGA_SIBLING) && (r.static_flags[0x3100]&DP_SEGA_SIBLING));
    CHECK(!(r.static_flags[0x3200]&DP_SEGA_SIBLING)); // the observed copy source itself
    dp_sega_results_free(&r);
    free(rom);
}
// S4: two jp (hl) sites load their target from adjacent words of one table;
// provenance names the words and the table grows over executed entries
static void jump_table(void) {
    uint8_t *rom=calloc(1,0x10000); CHECK(rom); if(!rom) exit(1);
    #define SEQ(at,tbl) do { unsigned d=(tbl)-0x1001; const uint8_t q[]={0x21,0x01,0x10,0x01,(uint8_t)d,(uint8_t)(d>>8),0x09,0x5e,0x23,0x56,0xeb,0xe9}; memcpy(rom+(at),q,sizeof q); } while(0)
    memcpy(rom,(const uint8_t[]){0xc3,0x00,0x01},3);
    SEQ(0x100,0x2002);
    memcpy(rom+0x200,(const uint8_t[]){0xc3,0x20,0x01},3);  // stub 0 -> second dispatch
    SEQ(0x120,0x2004);
    rom[0x210]=0x76; rom[0x220]=0x76;                         // stubs 1 and 2: halt
    const uint16_t t[3]={0x0200,0x0210,0x0220};
    for(int i=0;i<3;i++) { rom[0x2002+2*i]=(uint8_t)t[i]; rom[0x2003+2*i]=(uint8_t)(t[i]>>8); }
    #undef SEQ
    DpSegaOptions o=dp_sega_options(); DpSegaResults r;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,0x10000,&o,&r));
    CHECK(r.jt_tables==1 && r.jt_entries>=2);
    for(size_t k=0x2002;k<0x2006;k++) CHECK(r.static_flags[k]&DP_SEGA_TABLE);
    dp_sega_results_free(&r);
    free(rom);
}
int main(void) {
    test_machine(dp_sega_backend_sg(),0,1);
    test_machine(dp_sega_backend_gg(),0,0);
    test_machine(dp_sega_backend_gg(),1,0);
    exploration(); deferral(); fork_deferral(); padding_cut(); cov_output(); copy_siblings(); jump_table();
    TEST_DONE("sega");
}
