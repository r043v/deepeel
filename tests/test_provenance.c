#include "../src/cpu/z80/provenance.h"
#include "test_util.h"
#include <string.h>

static uint8_t memory[65536],original[65536];
static DpZ80Origin labels[65536];
static DpZ80Provenance provenance;
static unsigned writes,outputs;
static DpZ80Origin read_origin(void *u,uint16_t a) { (void)u; return labels[a]; }
static uint8_t read_byte(void *u,uint16_t a) { (void)u; return memory[a]; }
static void check_origin(DpZ80Origin origin,uint8_t value) {
    CHECK(origin<=65536);
    if(origin && origin<=65536) CHECK(original[origin-1]==value);
}
static void write_byte(void *u,uint16_t a,uint8_t v) {
    (void)u; writes++;
    DpZ80Origin origin=dp_z80_provenance_write(&provenance,a);
    check_origin(origin,v); labels[a]=origin; memory[a]=v;
}
static void output_byte(void *u,uint16_t a,uint8_t v) {
    (void)u; (void)a; outputs++; check_origin(provenance.output,v);
}
static void values(const z80_t *c,uint8_t out[DP_Z80_TAG_COUNT]) {
    const uint8_t v[]={c->b,c->c,c->d,c->e,c->h,c->l,0,c->a,c->f,
        c->ix>>8,c->ix,c->iy>>8,c->iy,c->sp>>8,c->sp,c->i,
        c->bc2>>8,c->bc2,c->de2>>8,c->de2,c->hl2>>8,c->hl2,c->af2>>8,c->af2};
    memcpy(out,v,sizeof(v));
}
static z80_t setup(const uint8_t *bytes,unsigned n) {
    memset(&provenance,0,sizeof(provenance)); writes=outputs=0;
    for(unsigned a=0;a<65536;a++) { memory[a]=(uint8_t)(a*37+(a>>8)*11); labels[a]=a+1; }
    memcpy(memory+0x100,bytes,n); memcpy(original,memory,sizeof(original));
    z80_t c={0}; c.read=c.in=read_byte; c.write=write_byte; c.out=output_byte; z80_reset(&c);
    c.pc=0x100; c.sp=0xf000; c.hl=0xc010; c.ix=0xc020; c.iy=0xc030; c.bc=0xc040; c.de=0xc050;
    c.a=0x80; c.f=0x93; c.i=0x63; c.bc2=0x1234; c.de2=0x5678; c.hl2=0x9abc; c.af2=0xdef0;
    uint8_t v[DP_Z80_TAG_COUNT]; values(&c,v);
    for(unsigned r=0;r<DP_Z80_TAG_COUNT;r++) if(r!=DP_Z80_TAG_UNUSED) {
        provenance.reg[r]=0x8000+r+1; original[0x8000+r]=v[r];
    }
    return c;
}
static void execute(z80_t *c) {
    z80_decoded_t d; CHECK(z80_decode(c,&d));
    dp_z80_provenance_begin(&provenance,c,&d,read_origin,NULL);
    CHECK(z80_execute_decoded(c,&d)>0);
    uint8_t v[DP_Z80_TAG_COUNT]; values(c,v);
    for(unsigned r=0;r<DP_Z80_TAG_COUNT;r++) check_origin(provenance.reg[r],v[r]);
    CHECK(provenance.write_at==provenance.write_count);
    CHECK(writes==provenance.write_count);
    CHECK(outputs<=1);
    if(!outputs) CHECK(!provenance.output);
}
static void opcode_spaces(void) {
    for(unsigned group=0;group<7;group++) for(unsigned op=0;op<256;op++) {
        uint8_t bytes[16]={0}; unsigned n=0;
        if(group==1) bytes[n++]=0xcb;
        if(group==2) bytes[n++]=0xed;
        if(group==3||group==5) bytes[n++]=0xdd;
        if(group==4||group==6) bytes[n++]=0xfd;
        if(group>=5) { bytes[n++]=0xcb; bytes[n++]=0x10; }
        bytes[n++]=(uint8_t)op;
        bytes[n++]=0x10; bytes[n++]=0xc0;
        z80_t c=setup(bytes,sizeof(bytes)); execute(&c);
    }
}
static void unchanged_arithmetic_is_not_a_copy(void) {
    const uint8_t bytes[][3]={{0xee,0,0},{0xf6,0,0},{0xe6,0xff,0},{0xed,0x44,0},{0xcb,0x87,0},{0xcb,0xff,0}};
    for(unsigned i=0;i<sizeof(bytes)/sizeof(bytes[0]);i++) {
        z80_t c=setup(bytes[i],3); c.a=i==5?0x80:0;
        original[provenance.reg[DP_Z80_TAG_A]-1]=c.a;
        execute(&c); CHECK(!provenance.reg[DP_Z80_TAG_A]);
    }
    const uint8_t cp[]={0xfe,0x80}; z80_t c=setup(cp,sizeof(cp));
    DpZ80Origin a=provenance.reg[DP_Z80_TAG_A]; execute(&c);
    CHECK(provenance.reg[DP_Z80_TAG_A]==a); CHECK(!provenance.reg[DP_Z80_TAG_F]);
    const uint8_t ld[]={0x3e,0x80}; c=setup(ld,sizeof(ld)); execute(&c);
    CHECK(provenance.reg[DP_Z80_TAG_A]==0x102);
}
int main(void) { opcode_spaces(); unchanged_arithmetic_is_not_a_copy(); TEST_DONE("provenance"); }
