#include "../src/cpu/z80/z80.h"
#include "../src/platform/sega/sega.h"
#include "test_util.h"
#include <string.h>
#include <stdlib.h>

static uint8_t memory[65536];
static unsigned input_reads;
static uint16_t input_port;
static uint8_t capture_input(void *u,uint16_t port) { (void)u; input_reads++; input_port=port; return 0xff; }
static uint8_t rd(void *u,uint16_t a) { (void)u; return memory[a]; }
static void wr(void *u,uint16_t a,uint8_t v) { (void)u; memory[a]=v; }
static z80_t machine(void) {
    z80_t c={0}; c.read=rd; c.write=wr; c.in=rd; c.out=wr; z80_reset(&c); return c;
}
static void vector(const uint8_t *bytes,size_t n,unsigned length,const char *text) {
    DpZ80Instruction in; char out[128];
    CHECK(dp_z80_decode(bytes,n,0x100,&in));
    CHECK(in.raw.length==length);
    dp_z80_format(&in,out,sizeof(out));
    CHECK_MSG(!strcmp(out,text),out);
    for(size_t short_size=0;short_size<length;short_size++)
        CHECK(!dp_z80_decode(bytes,short_size,0x100,&in));
}
#define V(text,...) do { const uint8_t bytes[]={__VA_ARGS__}; vector(bytes,sizeof(bytes),sizeof(bytes),text); } while(0)
static void decode_vectors(void) {
    V("LD IX,$1234",0xdd,0x21,0x34,0x12);
    V("LD (IY-2),$42",0xfd,0x36,0xfe,0x42);
    V("LD H,(IX+5)",0xdd,0x66,5);
    V("LD (IY-1),L",0xfd,0x75,0xff);
    V("LD IXH,B",0xdd,0x60);
    V("EX DE,HL",0xdd,0xeb);
    V("LD HL,($1234)",0xdd,0xed,0x6b,0x34,0x12);
    V("BIT 0,(IX-128)",0xdd,0xcb,0x80,0x40);
    V("RLC (IY+1),B",0xfd,0xcb,1,0);
    V("SET 7,(IX+0),A",0xdd,0xcb,0,0xff);
    V("SLL A",0xcb,0x37);
    V("NOP [ED $00]",0xed,0x00);
    V("RETI",0xed,0x4d);
    V("RETN",0xed,0x55);
    V("IM 2",0xed,0x7e);
    V("LDIR",0xed,0xb0);
    V("INIR",0xed,0xb2);
    V("OTDR",0xed,0xbb);
    V("LD IY,$1234",0xdd,0xfd,0xdd,0xfd,0x21,0x34,0x12);
    V("JR NZ,$00FE",0x20,0xfc);
    V("DJNZ $0100",0x10,0xfe);
    V("HALT",0xdd,0x76);
    uint8_t *prefixes=malloc(65536); CHECK(prefixes!=NULL);
    if(prefixes) {
        memset(prefixes,0xdd,65536); DpZ80Instruction in;
        CHECK(!dp_z80_decode(prefixes,65536,0,&in));
        prefixes[65534]=0;
        CHECK(dp_z80_decode(prefixes,65535,1,&in)); CHECK(in.raw.length==65535);
        free(prefixes);
    }
    const uint8_t wrap[]={0x18,0xff}; DpZ80Instruction in;
    CHECK(dp_z80_decode(wrap,2,0xffff,&in)); CHECK(in.target==0);
}
static void conditions(void) {
    for(int cc=0;cc<9;cc++) for(int taken=0;taken<2;taken++) for(int f=0;f<256;f++) {
        z80_t c=machine(); c.f=(uint8_t)f; c.b=0x79;
        CHECK(dp_z80_force(cc,&c,taken));
        CHECK(dp_z80_condition(cc,&c)==taken);
        unsigned flag=cc<8?(unsigned[]){Z80_Z,Z80_C,Z80_PV,Z80_S}[cc>>1]:0;
        CHECK(((c.f^f)&~flag)==0);
        if(cc!=8) CHECK(c.b==0x79);
    }
    for(unsigned cc=0;cc<8;cc++) for(int taken=0;taken<2;taken++) {
        memset(memory,0,sizeof(memory)); memory[0x100]=(uint8_t)(0xc2+8*cc);
        memory[0x101]=0x34; memory[0x102]=0x12;
        DpZ80Instruction in; CHECK(dp_z80_decode(memory+0x100,3,0x100,&in));
        CHECK(in.condition==(int)cc); CHECK(in.flow==DP_Z80_JUMP);
        z80_t c=machine(); c.pc=0x100; dp_z80_force(in.condition,&c,taken);
        z80_step(&c); CHECK(c.pc==(taken?0x1234:0x103));
    }
    for(int taken=0;taken<2;taken++) {
        memory[0x100]=0x10; memory[0x101]=0x7e;
        z80_t c=machine(); c.pc=0x100; dp_z80_force(8,&c,taken);
        z80_step(&c); CHECK(c.pc==(taken?0x180:0x102));
    }
}
static void interrupts(void) {
    memset(memory,0,sizeof(memory));
    z80_t c=machine(); c.pc=0x1234; c.sp=0xd000; c.iff1=c.iff2=1; c.im=1; c.irq=1;
    CHECK(z80_service(&c)==13); CHECK(c.pc==0x38); CHECK(c.sp==0xcffe);
    CHECK(memory[0xcffe]==0x34 && memory[0xcfff]==0x12); CHECK(!c.iff1&&!c.iff2);
    c=machine(); c.pc=0x1234; c.sp=0xd000; c.iff1=c.iff2=1; c.im=2; c.i=0x80; c.irq_vector=0xfe; c.irq=1;
    memory[0x80fe]=0x78; memory[0x80ff]=0x56;
    CHECK(z80_service(&c)==19); CHECK(c.pc==0x5678);
    c=machine(); c.im=0; c.irq=1; c.iff1=1; c.irq_vector=0xcf;
    CHECK(z80_service(&c)==13); CHECK(c.pc==8);
    c=machine(); c.nmi=c.irq=c.iff1=1; c.halted=1; c.pc=0x2222; c.sp=0xd000;
    CHECK(z80_service(&c)==11); CHECK(c.pc==0x66); CHECK(!c.iff1&&c.iff2&&!c.halted);
    c=machine(); c.pc=0x100; c.im=1; c.irq=1;
    memory[0x100]=0xfb; memory[0x101]=0; memory[0x102]=0x76;
    z80_step(&c); CHECK(c.pc==0x101); CHECK(c.ei_delay);
    z80_step(&c); CHECK(c.pc==0x102);
    z80_step(&c); CHECK(c.pc==0x38);
}
static void repeat_conditions(void) {
    const uint8_t ops[]={0xb0,0xb1,0xb2,0xb3,0xb8,0xb9,0xba,0xbb};
    for(unsigned i=0;i<sizeof(ops);i++) for(int taken=0;taken<2;taken++) {
        memset(memory,0,sizeof(memory)); memory[0x100]=0xed; memory[0x101]=ops[i]; memory[0x200]=0x42;
        DpZ80Instruction in; CHECK(dp_z80_decode(memory+0x100,2,0x100,&in));
        z80_t c=machine(); c.pc=0x100; c.hl=0x200; c.de=0xc000; c.bc=1; c.a=0x42;
        CHECK(dp_z80_force_repeat(&in,&c,0x42,taken));
        CHECK(dp_z80_repeat_taken(&in,&c,0x42)==taken);
        z80_step(&c); CHECK(c.pc==(taken?0x100:0x102));
    }
    const uint8_t prefixed[]={0xdd,0xfd,0xed,0xb0}; DpZ80Instruction in;
    CHECK(dp_z80_decode(prefixed,sizeof(prefixed),0x100,&in));
    CHECK(in.flow==DP_Z80_REPEAT && in.target==0x102);
}
static void opcode_spaces(void) {
    // Every terminal opcode in every prefix family must decode and format.
    for(unsigned group=0;group<7;group++) for(unsigned op=0;op<256;op++) {
        uint8_t bytes[16]={0}; unsigned at=0;
        if(group==1) bytes[at++]=0xcb;
        if(group==2) bytes[at++]=0xed;
        if(group==3||group==5) bytes[at++]=0xdd;
        if(group==4||group==6) bytes[at++]=0xfd;
        if(group>=5) { bytes[at++]=0xcb; bytes[at++]=0x80; }
        bytes[at++]=(uint8_t)op;
        DpZ80Instruction in; char out[128];
        CHECK(dp_z80_decode(bytes,sizeof(bytes),0,&in));
        CHECK(in.raw.length>=at && in.raw.length<=sizeof(bytes));
        dp_z80_format(&in,out,sizeof(out)); CHECK(out[0]);
        char small[2]={'?', '?'}; dp_z80_format(&in,small,1); CHECK(small[0]==0 && small[1]=='?');
        dp_z80_format(&in,NULL,0);
        // Compare input-port classification with the executor's actual bus
        // call, across ignored prefixes and undocumented ED IN (C).
        z80_t c=machine(); c.a=0x42; c.bc=0x1234; c.hl=0xc000; c.sp=0xd000; c.in=capture_input;
        uint16_t expected=0; int reads=dp_z80_input_port(&in.raw,&c,&expected);
        input_reads=0;
        z80_execute_decoded(&c,&in.raw);
        CHECK(input_reads==(unsigned)reads);
        if(reads) CHECK(input_port==expected);
    }
    for(unsigned op=0;op<256;op++) {
        uint8_t bytes[]={0xed,(uint8_t)op,0,0}; DpZ80Instruction in;
        CHECK(dp_z80_decode(bytes,sizeof(bytes),0x100,&in));
        CHECK((in.flow==DP_Z80_REPEAT)==((op>=0xb0&&op<=0xb3)||(op>=0xb8&&op<=0xbb)));
        if(in.flow==DP_Z80_REPEAT) CHECK(in.target==0x100);
    }
}
static void hardware(void) {
    const DpSegaSystem *sg=dp_sega_system("sg"), *sms=dp_sega_system("sms"), *gg=dp_sega_system("gg");
    CHECK(sg&&sms&&gg); CHECK(dp_sega_system("sg1000")==sg); CHECK(!dp_sega_system("gb"));
    CHECK(sg->ram_bytes==1024 && sms->ram_bytes==8192 && gg->cram_bytes==64);
    CHECK(!strcmp(dp_sega_port(sg,0xabbe,0)->name,"VDP_DATA"));
    CHECK(!strcmp(dp_sega_port(sg,0x7fbf,0)->name,"VDP_STATUS"));
    CHECK(!strcmp(dp_sega_port(sms,0x7e,0)->name,"V_COUNTER"));
    CHECK(!strcmp(dp_sega_port(sms,0x7f,1)->name,"SN76489"));
    CHECK(!strcmp(dp_sega_port(sms,0xf2,1)->name,"AUDIO_CONTROL"));
    CHECK(!strcmp(dp_sega_port(sms,0xf2,0)->name,"AUDIO_CONTROL"));
    CHECK(!strcmp(dp_sega_port(gg,0,0)->name,"GG_START"));
    CHECK(!strcmp(dp_sega_port(gg,6,1)->name,"GG_STEREO"));
    CHECK(!dp_sega_port(gg,4,1));
    CHECK(!dp_sega_port(sg,0x20,0));
}
int main(void) { decode_vectors(); conditions(); interrupts(); repeat_conditions(); opcode_spaces(); hardware(); TEST_DONE("z80"); }
