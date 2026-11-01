#include "../src/platform/msx/msx.h"
#include "../src/platform/sega/backend.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
static uint8_t rom[0x10000];
static void fixture(uint16_t init) {
    for(unsigned i=0;i<sizeof(rom);i++) rom[i]=(uint8_t)(i/0x2000);
    rom[0]='A'; rom[1]='B'; rom[2]=(uint8_t)init; rom[3]=(uint8_t)(init>>8);
    memset(rom+4,0,12);
}
static void slots(void) {
    fixture(0x4010); CHECK(dp_msx_init(rom,0x8000,DP_MSX_PLAIN));
    CHECK(dp_msx_cpu()->pc==0x4010 && dp_msx_cpu()->sp==0xf380 && dp_msx_cpu()->im==1);
    CHECK(dp_msx_rom_index(0x4000)==0 && dp_msx_rom_index(0x8000)==-1);
    CHECK(dp_msx_ram_index(0x8000)==0x8000);
    dp_msx_out(0xa8,0xd4); CHECK(dp_msx_rom_index(0xbfff)==0x7fff);
    dp_msx_out(0xa8,0xf4);
    CHECK(dp_msx_rom_index(0)==-1 && dp_msx_rom_index(0xc000)==-1);
    CHECK(dp_msx_ram_index(0xc000)==0xc000 && dp_msx_ram_index(0x4000)==-1);
    dp_msx_write(0xc123,0x5a); CHECK(dp_msx_read(0xc123)==0x5a);
    CHECK(dp_msx_in(0xa8)==0xf4); dp_msx_out(0x12a8,0xff);
    CHECK(dp_msx_rom_index(0x4000)==-1 && dp_msx_ram_index(0x4000)==0x4000);
    dp_msx_write(0x4123,0x6b); CHECK(dp_msx_read(0x4123)==0x6b);
    dp_msx_out(0xa8,0xf4); CHECK(dp_msx_read(0x4123)==rom[0x123]);
    dp_msx_out(0xa8,0xf8); CHECK(dp_msx_read(0x4123)==0xff); // empty slot 2
    CHECK(dp_msx_ram_index(0x4123)==-1 && dp_msx_rom_index(0x4123)==-1);
    fixture(0x8010); CHECK(dp_msx_init(rom,0x4000,DP_MSX_PLAIN));
    CHECK(dp_msx_info().base==0x8000 && dp_msx_cpu()->pc==0x8010);
    CHECK(dp_msx_rom_index(0x8000)==0 && dp_msx_rom_index(0x4000)==-1);
    fixture(0x8010); CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_ASCII8));
    CHECK(dp_msx_rom_index(dp_msx_cpu()->pc)==0x10 && dp_msx_info().slots==0xd4);
    fixture(0x7fff); CHECK(!dp_msx_init(rom,0x2011,DP_MSX_PLAIN));
    fixture(0x4010); CHECK(dp_msx_init(rom,0x2011,DP_MSX_PLAIN));
    CHECK(dp_msx_rom_index(0x6010)==0x2010 && dp_msx_rom_index(0x6011)==-1);
    CHECK(dp_msx_read(0x6011)==0xff); // allocated page padding is not a ROM byte
}
static void mappers(void) {
    for(int mapper=DP_MSX_ASCII8;mapper<=DP_MSX_KONAMI_SCC;mapper++) {
        fixture(0x4010); CHECK(dp_msx_init(rom,sizeof(rom),mapper));
        uint16_t reg=mapper==DP_MSX_KONAMI_SCC?0x5000:0x6000;
        dp_msx_write(reg,3);
        uint16_t window=mapper==DP_MSX_KONAMI?0x6000:0x4000;
        uint32_t physical=mapper==DP_MSX_ASCII16?0xc000:0x6000;
        CHECK(dp_msx_rom_index(window)==physical && dp_msx_read(window)==physical/0x2000);
        if(mapper==DP_MSX_ASCII16) CHECK(dp_msx_rom_index(0x6000)==physical+0x2000);
        if(mapper==DP_MSX_KONAMI) CHECK(dp_msx_rom_index(0x4000)==0);
        if(mapper==DP_MSX_KONAMI_SCC) {
            dp_msx_out(0xa8,0xd4); // expose the cartridge in page 2 before SCC access
            dp_msx_write(0x9000,0x3f); CHECK(dp_msx_info().scc_on);
            CHECK(dp_msx_rom_index(0x9800)==-1 && dp_msx_rom_index(0x9fff)==-1);
            dp_msx_write(0x9000,0); CHECK(!dp_msx_info().scc_on);
            CHECK(dp_msx_rom_index(0x9800)==0x1800);
        }
        // Writes in a hidden cartridge slot cannot modify its bank registers.
        DpMsxInfo before=dp_msx_info(); dp_msx_out(0xa8,0xff); dp_msx_write(reg,2);
        dp_msx_out(0xa8,before.slots); CHECK(dp_msx_rom_index(window)==physical);
    }
}
static void mapper_decoders(void) {
    static const struct { int mapper; uint16_t reg,window; uint8_t value; uint32_t physical; } cases[]={
        {DP_MSX_ASCII8,0x67ff,0x4000,5,0xa000},{DP_MSX_ASCII8,0x6fff,0x6000,6,0xc000},
        {DP_MSX_ASCII8,0x77ff,0x8000,7,0xe000},{DP_MSX_ASCII8,0x7fff,0xa000,2,0x4000},
        {DP_MSX_ASCII16,0x67ff,0x4000,2,0x8000},{DP_MSX_ASCII16,0x77ff,0x8000,3,0xc000},
        {DP_MSX_KONAMI,0x7fff,0x6000,5,0xa000},{DP_MSX_KONAMI,0x9fff,0x8000,6,0xc000},
        {DP_MSX_KONAMI,0xbfff,0xa000,7,0xe000},
        {DP_MSX_KONAMI_SCC,0x57ff,0x4000,4,0x8000},{DP_MSX_KONAMI_SCC,0x77ff,0x6000,5,0xa000},
        {DP_MSX_KONAMI_SCC,0x97ff,0x8000,6,0xc000},{DP_MSX_KONAMI_SCC,0xb7ff,0xa000,7,0xe000}
    };
    for(unsigned j=0;j<sizeof(cases)/sizeof(cases[0]);j++) {
        fixture(0x4010); CHECK(dp_msx_init(rom,sizeof(rom),cases[j].mapper)); dp_msx_out(0xa8,0xd4);
        dp_msx_write(cases[j].reg,cases[j].value);
        CHECK(dp_msx_rom_index(cases[j].window)==cases[j].physical);
        CHECK(dp_msx_rom_index((uint16_t)(cases[j].window+0x1fff))==cases[j].physical+0x1fff);
        if(cases[j].mapper==DP_MSX_ASCII16) CHECK(dp_msx_rom_index(cases[j].window+0x2000)==cases[j].physical+0x2000);
    }
    for(int mapper=DP_MSX_ASCII16;mapper<=DP_MSX_KONAMI_SCC;mapper++) {
        fixture(0x4010); CHECK(dp_msx_init(rom,sizeof(rom),mapper)); dp_msx_out(0xa8,0xd4);
        DpMsxInfo before=dp_msx_info();
        const uint16_t unused[]={0x4000,0x6800,0x7800,0x5800,0x9800,0xb800};
        for(unsigned j=0;j<sizeof(unused)/sizeof(unused[0]);j++) {
            uint16_t address=unused[j];
            if(mapper==DP_MSX_ASCII16 && address!=0x6800 && address!=0x7800) continue;
            if(mapper==DP_MSX_KONAMI && address!=0x4000) continue;
            if(mapper==DP_MSX_KONAMI_SCC && address!=0x5800 && address!=0x7800 && address!=0x9800 && address!=0xb800) continue;
            dp_msx_write(address,7); DpMsxInfo after=dp_msx_info();
            CHECK(!memcmp(before.banks,after.banks,sizeof(before.banks)));
        }
    }
}
static void peripherals(void) {
    fixture(0x4010); CHECK(dp_msx_init(rom,0x8000,DP_MSX_PLAIN));
    dp_msx_out(0xa0,14); dp_msx_buttons(0,17); dp_msx_buttons(1,34);
    CHECK((dp_msx_in(0xa2)&63)==(63^17));
    dp_msx_out(0xa0,15); dp_msx_out(0xa1,0x40); dp_msx_out(0xa0,14);
    CHECK((dp_msx_in(0xa2)&63)==(63^34));
    dp_msx_out(0xaa,8); CHECK((dp_msx_in(0xa9)&0x21)==0); // UP + SPACE
    dp_msx_return(1); dp_msx_out(0xaa,7); CHECK(!(dp_msx_in(0xa9)&0x80)); CHECK(!dp_msx_cpu()->nmi);
    dp_msx_return(0); CHECK(dp_msx_in(0xa9)==0xff);
    dp_msx_out(0xaa,0); dp_msx_out(0xab,3); CHECK(dp_msx_in(0xaa)==2); // BSR set bit 1
    dp_msx_out(0xab,2); CHECK(dp_msx_in(0xaa)==0);
    dp_msx_out(0x99,0x34); dp_msx_out(0x99,0x52); dp_msx_out(0x98,0x6a);
    dp_msx_out(0x99,0x34); dp_msx_out(0x99,0x12); CHECK(dp_msx_in(0x98)==0x6a);
    dp_msx_out(0x99,0x20); dp_msx_out(0x99,0x81); dp_msx_advance(228*194);
    CHECK(dp_msx_cpu()->irq); CHECK(dp_msx_in(0x99)&128); CHECK(!dp_msx_cpu()->irq);
}
static void snapshots(void) {
    fixture(0x4010); CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_ASCII8));
    size_t n=dp_msx_state_size(); void *state=malloc(n); CHECK(state); if(!state) exit(1);
    dp_msx_write(0x6000,3); dp_msx_write(0xc123,0x5a); dp_msx_cpu()->ix=0xabcd;
    dp_msx_out(0x99,0x20); CHECK(dp_msx_save(state,n)); // half a VDP command
    dp_msx_write(0x6000,2); dp_msx_write(0xc123,0); dp_msx_cpu()->ix=0;
    CHECK(!dp_msx_load(state,n-1)); CHECK(dp_msx_load(state,n));
    CHECK(dp_msx_rom_index(0x4000)==0x6000 && dp_msx_read(0xc123)==0x5a && dp_msx_cpu()->ix==0xabcd);
    dp_msx_out(0x99,0x81); CHECK(dp_msx_info().vdp[1]==0x20);
    CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_ASCII8)); CHECK(!dp_msx_load(state,n)); free(state);
}
static void bios_and_isolation(void) {
    fixture(0x4010);
    // A physical low-port OUT from cartridge code must not call a BIOS trap.
    const uint8_t code[]={0x21,0x34,0x12,0x3e,0x5a,0xcd,0x4d,0,0x3e,0x66,0xd3,0x0a,0x76};
    memcpy(rom+0x10,code,sizeof(code)); CHECK(dp_msx_init(rom,0x8000,DP_MSX_PLAIN));
    for(unsigned steps=0;steps<32 && !dp_msx_cpu()->halted;steps++) { unsigned cycles=z80_step(dp_msx_cpu()); dp_msx_advance(cycles); }
    CHECK(dp_msx_cpu()->halted && dp_msx_info().bios_calls==1);
    dp_msx_out(0x99,0x34); dp_msx_out(0x99,0x12); CHECK(dp_msx_in(0x98)==0x5a);
    dp_msx_out(0xa8,0xf7); dp_msx_cpu()->pc=0x4f; dp_msx_out(0x0a,0x77);
    CHECK(dp_msx_info().bios_calls==1); // matching PC while BIOS is hidden is insufficient
    const DpSegaBackend *sg=dp_sega_backend_sg(),*gg=dp_sega_backend_gg();
    sg->init(rom,0x8000,NULL,0); gg->init(rom,sizeof(rom),NULL,1);
    sg->cpu()->pc=0x1234; gg->cpu()->pc=0x5678;
    dp_msx_cpu()->pc=0x4010; dp_msx_write(0xc123,0x66);
    CHECK(sg->cpu()->pc==0x1234 && gg->cpu()->pc==0x5678);
    CHECK(sg->read(0xc123)==0 && gg->read(0xc123)==0);
}
int main(void) {
    CHECK(!dp_msx_init(NULL,0,DP_MSX_AUTO));
    fixture(0x4010); CHECK(!dp_msx_init(rom,15,DP_MSX_AUTO));
    CHECK(!dp_msx_init(rom,sizeof(rom),DP_MSX_PLAIN));
    slots(); mappers(); mapper_decoders(); peripherals(); snapshots(); bios_and_isolation(); dp_msx_close();
    CHECK(!dp_msx_cpu() && dp_msx_read(0)==0xff && dp_msx_rom_index(0)==-1);
    TEST_DONE("msx");
}
