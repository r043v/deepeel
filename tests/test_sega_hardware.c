#include "../src/platform/sega/explore.h"
#include "../src/platform/sega/backend.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

static uint8_t rom[0x10000];
static DpSegaResults result;
static size_t cursor;
static void byte(uint8_t v) { rom[cursor++]=v; }
static void out(uint8_t v) { byte(0x3e); byte(v); byte(0xd3); byte(0xbf); }
static void reg(unsigned r,uint8_t value) { out(value); out((uint8_t)(0x80|r)); }
static void reset(void) {
    memset(rom,0xff,sizeof(rom)); rom[0]=0xc3; rom[1]=0; rom[2]=1; cursor=0x100;
}
static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options(); o.seed_cap=0;
    o.seed_vectors=0; o.input_total=0; o.input_masks=0; o.closure.nodes=0;
    o.max_steps=18000; o.max_total=36000; o.loop_cap=20000; o.fork_cap=1;
    return o;
}
static void run(const char *system,DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system(system),rom,!strcmp(system,"sg")?0xc000:sizeof(rom),&o,&result));
}
static void irq(unsigned mode,int line,const char *system) {
    reset(); byte(0x31); byte(0); byte(0xd0);
    byte(0xed); byte(mode==0?0x46:mode==1?0x56:0x5e);
    if(mode==2) {
        byte(0x3e); byte(0x40); byte(0xed); byte(0x47); // I=$40, bus vector=$FF
        byte(0x3e); byte(3); byte(0x32); byte(0xff); byte(0xff);
        rom[0x40ff]=0; rom[0x4100]=0x80; // IM2 word straddles a 256-byte page.
    }
    reg(0,line?0x16:0x06); reg(1,line?0:0x20); if(line) reg(10,0);
    byte(0xfb); byte(0x76); byte(0x18); byte(0xfd);
    const uint8_t handler[]={0xdb,0xbf,0xfb,0xed,0x4d};
    memcpy(rom+(mode==2?0xc000:0x38),handler,sizeof(handler));
    run(system,options());
    CHECK(result.interrupts[mode]>0); CHECK(result.irq_forks>0);
    CHECK(result.interrupt_suppressed>0); CHECK(result.interrupt_count>0);
    uint64_t total=0; for(unsigned j=0;j<4;j++) total+=result.interrupts[j];
    CHECK(total==result.interrupt_count+result.interrupt_log_overflow);
    const DpSegaInterrupt *e=&result.interrupt_log[0];
    CHECK(e->mode==mode && !e->forced); CHECK(e->vector==0xff);
    CHECK(e->sp==0xd000 && e->target==(mode==2?0x8000:0x38));
    CHECK(e->target_rom==(mode==2?0xc000:0x38));
    CHECK(e->sources==(line?DP_SEGA_IRQ_LINE:DP_SEGA_IRQ_FRAME));
    if(mode==2) CHECK(e->i==0x40);
    CHECK(result.vdp_register_writes[1]>0);
}
static void registers(const char *system,unsigned count) {
    reset();
    for(unsigned j=0;j<count;j++) reg(j,(uint8_t)j);
    reg(15,0x55);
    // A status read discards the first byte of an incomplete command.
    out(0x66); byte(0xdb); byte(0xbf); out(0x81); byte(0xdb); byte(0xbf);
    // A data access also discards an incomplete control command.
    out(0x77); byte(0x3e); byte(0); byte(0xd3); byte(0xbe); out(0x81);
    byte(0x76);
    DpSegaOptions o=options(); o.max_total=200; o.max_steps=200; o.fork_cap=0;
    run(system,o);
    CHECK(result.register_count==count+1);
    for(unsigned j=0;j<count;j++) {
        const DpSegaRegisterWrite *e=&result.register_log[j];
        CHECK(e->reg==j && e->value==j && !e->ignored && !e->forced);
        CHECK(e->port==((0x80|j)<<8|0xbf));
        CHECK(result.vdp_register_writes[j]==(j==7 && count==8?2:1));
    }
    const DpSegaRegisterWrite *e=&result.register_log[count];
    CHECK(e->value==0x55);
    if(count==8) { CHECK(e->reg==7 && !e->ignored); CHECK(!result.vdp_register_ignored); }
    else { CHECK(e->reg==15 && e->ignored); CHECK(result.vdp_register_ignored==1); }
}
static void saturation(void) {
    reset(); reg(7,0x12); byte(0xc3); byte(0); byte(1);
    DpSegaOptions o=options(); o.max_total=10000; o.max_steps=10000; o.fork_cap=0;
    run("sms",o);
    CHECK(result.register_count==DP_SEGA_HARDWARE_LOG_CAP && result.register_log_overflow>0);
    CHECK(result.vdp_register_writes[7]==result.register_count+result.register_log_overflow);
    reset(); byte(0x31); byte(0); byte(0xd0); byte(0xed); byte(0x56);
    reg(0,0x16); reg(1,0); reg(10,0);
    byte(0xfb); byte(0x76); byte(0x18); byte(0xfd);
    const uint8_t handler[]={0xdb,0xbf,0xfb,0xed,0x4d};
    memcpy(rom+0x38,handler,sizeof(handler));
    o.max_total=o.max_steps=o.loop_cap=200000;
    run("sms",o);
    CHECK(result.interrupt_count==DP_SEGA_HARDWARE_LOG_CAP && result.interrupt_log_overflow>0);
    CHECK(result.interrupts[1]==result.interrupt_count+result.interrupt_log_overflow);
}
static void memory_gates(int sms) {
    reset(); rom[0x8000]=2; rom[0xc000]=3;
    uint8_t cart[0x8000]={0};
    const DpSegaBackend *b=dp_sega_backend_gg();
    CHECK(b->init(rom,sizeof(rom),cart,sms));
    void *snapshot=malloc(b->state_size); CHECK(snapshot); if(!snapshot) exit(1);
    b->write(0xc123,0x55);
    b->out(0x123e,0xb8); // RAM off, cartridge on, high port byte ignored.
    CHECK(b->read(0xc123)==0xff && b->ram_index(0xc123)<0);
    b->write(0xc123,0x99);
    b->write(0xffff,3); // Mapper remains writable with internal RAM off.
    CHECK(b->rom_index(0x8000)==0xc000 && b->read(0x8000)==3);
    b->save(snapshot); b->out(0x3e,0xa8); b->load(snapshot);
    CHECK(b->ram_index(0xc123)<0 && b->read(0xc123)==0xff);
    b->out(0x3e,0xa8); CHECK(b->read(0xc123)==0x55);
    b->out(0x3e,0xe8); // SMS cartridge off; GG ignores bit 6.
    CHECK((b->rom_index(0)<0)==sms);
    CHECK((b->rom_index(0x8000)<0)==sms);
    CHECK(b->read(0x8000)==(sms?0xff:3));
    b->write(0xffff,1); CHECK(b->read(0xffff)==1);
    b->out(0x3e,0xa8); CHECK(b->read(0x8000)==(sms?3:rom[0x4000]));
    b->write(0xfffc,8); b->write(0x8000,0x66);
    b->out(0x3e,0xe8);
    CHECK((b->ram_index(0x8000)<0)==sms);
    b->write(0x8000,0x99); b->out(0x3e,0xa8);
    CHECK(b->read(0x8000)==(sms?0x66:0x99));
    b->out(0x3e,0xb8); b->write(0x8000,0x77);
    CHECK(b->read(0x8000)==0x77 && b->ram_index(0x8000)>=0);
    b->out(0x3e,0xa8); b->write(0xfffc,0);
    // Only SMS mirrors the memory-control port at low even addresses.
    b->out(0x08,0xb8); CHECK((b->ram_index(0xc000)<0)==sms);
    b->out(0x3e,0xa8);
    DpSegaInput input={{1,0},1,0,0}; b->set_input(input);
    CHECK(!(b->in(0xdc)&1));
    b->out(0x3e,0xac); CHECK((b->in(0xdc)==0xff)==sms);
    if(!sms) CHECK(!(b->in(0)&0x80)); // GG Start is independent of SMS IO gate.
    b->out(0xbf,0x20); b->out(0xbf,0x81); b->advance(228*194);
    CHECK(b->cpu()->irq); CHECK(b->in(0xbf)&0x80); CHECK(!b->cpu()->irq);
    // CPU callbacks use the same gated bus as direct backend accesses.
    b->out(0x3e,0xb8); b->cpu()->pc=0xc000;
    CHECK(b->cpu()->read(NULL,0xc000)==0xff);
    b->out(0x3e,0xa8); CHECK(!(b->in(0xdc)&1));
    const DpSegaPort *port=dp_sega_port(dp_sega_system(sms?"sms":"gg"),0x3e,1);
    CHECK(port && !strcmp(port->name,"MEMORY_CONTROL"));
    port=dp_sega_port(dp_sega_system(sms?"sms":"gg"),0x08,1);
    CHECK(sms?(port && !strcmp(port->name,"MEMORY_CONTROL")):!port);
    free(snapshot);
}
static void memory_exploration(void) {
    reset();
    const uint8_t boot[]={0x21,0,2,0x11,0,0xc0,0x01,9,0,0xed,0xb0,0xc3,0,0xc0};
    const uint8_t ram_code[]={0x3e,0xe8,0xd3,0x3e,0x3e,0xa8,0xd3,0x3e,0x76};
    memcpy(rom+0x100,boot,sizeof(boot)); memcpy(rom+0x200,ram_code,sizeof(ram_code));
    DpSegaOptions o=options(); o.max_total=o.max_steps=100; o.fork_cap=0;
    run("sms",o);
    CHECK(result.exec_outside>=5 && result.bank_count==2);
    CHECK(result.memory_writes==2 && result.memory_effect_changes==2 && result.memory_count==2);
    CHECK(result.memory_log[0].previous==0xab && result.memory_log[0].value==0xe8);
    CHECK(result.memory_log[1].previous==0xe8 && result.memory_log[1].value==0xa8);
    CHECK(result.banks[0].kind==1 && result.banks[1].kind==1);
    CHECK(result.banks[0].value==0xe8 && result.banks[1].value==0xa8);
    CHECK(result.ram_copies==9);
    CHECK(!(result.flags[0xc000]&(DP_SEGA_NATURAL|DP_SEGA_FORCED)));
    for(unsigned i=0;i<sizeof(ram_code);i++) {
        CHECK(result.flags[0x200+i]&DP_SEGA_COPIED);
        CHECK(!(result.flags[0x200+i]&(DP_SEGA_NATURAL|DP_SEGA_FORCED)));
    }
    reset();
    const uint8_t toggle[]={0x3e,0xb8,0xd3,0x3e,0x3e,0xa8,0xd3,0x3e,0xc3,0,1};
    memcpy(rom+0x100,toggle,sizeof(toggle));
    o.max_total=o.max_steps=o.loop_cap=10000;
    run("sms",o);
    CHECK(result.bank_count==1024 && result.bank_log_overflow>0);
    CHECK(result.bank_changes==result.bank_count+result.bank_log_overflow);
    CHECK(result.memory_count==DP_SEGA_HARDWARE_LOG_CAP && result.memory_log_overflow>0);
    CHECK(result.memory_writes==result.memory_count+result.memory_log_overflow);
    CHECK(result.memory_effect_changes==result.memory_writes);

}
static void memory_journal(int sms) {
    reset();
    const uint8_t values[2][5]={{0xa8,0xe8,0xac,0xbc,0xa8},{0xab,0xaa,0xae,0xae,0xaa}};
    for(unsigned i=0;i<5;i++) {
        byte(0x3e); byte(values[sms][i]); byte(0xd3); byte(0x3e);
    }
    if(!sms) { byte(0xd3); byte(8); } // A GG mirror must not produce a record.
    byte(0x76);
    DpSegaOptions o=options(); o.max_total=o.max_steps=40; o.fork_cap=0;
    run(sms?"sms":"gg",o);
    CHECK(result.memory_writes==5 && result.memory_effect_changes==2);
    CHECK(result.memory_count==5 && !result.memory_log_overflow);
    CHECK(result.bank_changes==(sms?0:2)); // IO-only changes never count as bank changes.
    for(unsigned i=0;i<5;i++) {
        const DpSegaMemoryWrite *e=&result.memory_log[i];
        CHECK(e->path==1 && !e->forced && e->pc==0x102+4*i);
        CHECK(e->previous==(i?values[sms][i-1]:sms?0xab:0xa8));
        CHECK(e->value==values[sms][i] && e->effect_mask==(sms?0x54:0x10));
        CHECK(e->port==(uint16_t)((values[sms][i]<<8)|0x3e));
        if(i) CHECK(e->step>result.memory_log[i-1].step);
    }
    FILE *f=tmpfile(); CHECK(f); if(!f) exit(1);
    dp_sega_report(f,dp_sega_system(sms?"sms":"gg"),&result); rewind(f);
    char report[16384]; size_t count=fread(report,1,sizeof(report)-1,f); report[count]=0; fclose(f);
    CHECK(strstr(report,"memory control writes 5; effective latch changes 2; records 5/1024; overflow 0"));
    CHECK(strstr(report,sms?"effect mask $54 natural":"effect mask $10 natural"));
}
static void memory_journal_forks(int sms) {
    reset();
    const uint8_t code[]={0x3e,0,0xd3,0x3e,0xaf,0x20,5,
        0x3e,0xa8,0xd3,0x3e,0x76,0x3e,0xa8,0xd3,0x3e,0x76};
    memcpy(rom+0x100,code,sizeof(code)); rom[0x101]=sms?0xac:0xb8;
    DpSegaOptions o=options(); o.max_total=100; o.max_steps=40; o.fork_cap=1;
    run(sms?"sms":"gg",o);
    CHECK(result.forks==1 && result.memory_count==3);
    CHECK(result.memory_effect_changes==3);
    CHECK(result.memory_log[0].previous==(sms?0xab:0xa8));
    for(unsigned i=1;i<3;i++) {
        CHECK(result.memory_log[i].previous==(sms?0xac:0xb8));
        CHECK(result.memory_log[i].value==0xa8);
        CHECK(result.memory_log[i].forced==(i==2));
        CHECK(result.memory_log[i].path==i);
    }
}
int main(void) {
    memory_journal_forks(1); memory_journal_forks(0);
    memory_journal(1); memory_journal(0);
    memory_gates(1); memory_gates(0); memory_exploration();
    registers("sg",8); registers("sms",11); registers("gg",11);
    irq(0,0,"sms"); irq(1,0,"sms"); irq(2,0,"sms"); irq(1,1,"sms");
    irq(1,0,"sg"); irq(1,0,"gg"); saturation();
    dp_sega_results_free(&result); TEST_DONE("sega_hardware");
}
