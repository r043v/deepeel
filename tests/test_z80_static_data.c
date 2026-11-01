#include "../src/platform/msx/msx.h"
#include "../src/platform/msx/explore.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
static uint8_t rom[0x8000];
static DpSegaResults result;
static const DpSegaSystem *profile_info(unsigned profile) {
    return profile==3?dp_msx_system():dp_sega_system(profile==0?"sg":profile==1?"sms":"gg");
}
static void fixture(unsigned profile,const uint8_t *body,size_t size) {
    memset(rom,0xff,sizeof(rom));
    if(profile==3) {
        rom[0]='A'; rom[1]='B'; rom[2]=0x10; rom[3]=0x40; memset(rom+4,0,12);
        rom[0x10]=0xc3; rom[0x11]=0; rom[0x12]=0x45;
    } else { rom[0]=0xc3; rom[1]=0; rom[2]=5; }
    memcpy(rom+0x500,body,size); rom[0x600]=0x76;
}
static void explore(unsigned profile,size_t size,int targets) {
    DpSegaOptions o=profile==3?dp_msx_options():dp_sega_options();
    o.max_total=1; o.input_total=0; o.seed_vectors=0; o.msx_mapper=DP_MSX_PLAIN;
    o.closure=(DpSegaClosureOptions){256,16,4,16}; o.target_closure=targets;
    dp_sega_results_free(&result); CHECK(dp_sega_explore(profile_info(profile),rom,size,&o,&result));
    CHECK(result.steps==1 && !result.data_reads && !result.ram_copies && !result.vdp_copies);
    for(unsigned port=0;port<256;port++) CHECK(!result.io_read[port] && !result.io_write[port]);
}
static void byte_table(unsigned profile) {
    uint16_t table=profile==3?0x4700:0x700; uint8_t high=profile==3?0x46:6;
    const uint8_t code[]={0x21,(uint8_t)table,(uint8_t)(table>>8),0x7e,0x23,0x66,0x6f,0xe9};
    fixture(profile,code,sizeof(code)); rom[0x700]=0; rom[0x701]=high;
    explore(profile,sizeof(rom),1);
    CHECK(result.flags[0x700]==DP_SEGA_STATIC_DATA && result.flags[0x701]==DP_SEGA_STATIC_DATA);
    CHECK(result.flags[0x500]==DP_SEGA_STATIC && result.flags[0x600]==DP_SEGA_STATIC);
    CHECK(result.closure.static_data_reads==0 && result.target_closure.static_data_reads==2);
    CHECK(result.target_closure.static_data_bytes==2 && result.target_closure.indirect_resolved==1);
    CHECK(result.closure.new_bytes==sizeof(code) && result.target_closure.new_bytes==1);
    uint64_t known=0; for(unsigned i=0;i<sizeof(rom);i++) known+=!!result.flags[i]; CHECK(known==14);
    explore(profile,sizeof(rom),0); CHECK(!result.flags[0x700] && !result.flags[0x701] && !result.flags[0x600]);
    CHECK(!result.target_closure.static_data_reads && !result.closure.static_data_reads);
    // A truncated table proves only the low byte that was actually consulted.
    explore(profile,0x701,1); CHECK(result.flags[0x700]==DP_SEGA_STATIC_DATA && !result.flags[0x600]);
    CHECK(result.target_closure.static_data_reads==1 && result.target_closure.static_data_bytes==1);
}
static void word_and_dual_use(unsigned profile) {
    uint16_t table=profile==3?0x4700:0x700;
    const uint8_t code[]={0xed,0x7b,(uint8_t)table,(uint8_t)(table>>8),0x76};
    fixture(profile,code,sizeof(code)); rom[0x700]=0; rom[0x701]=0xd0;
    explore(profile,sizeof(rom),1);
    CHECK(result.closure.static_data_reads==2 && result.closure.static_data_bytes==2);
    CHECK(result.target_closure.static_data_reads==2 && result.target_closure.static_data_bytes==0);
    CHECK(result.flags[0x700]==DP_SEGA_STATIC_DATA && result.flags[0x701]==DP_SEGA_STATIC_DATA);
    uint16_t target=profile==3?0x4600:0x600;
    const uint8_t dual[]={0x3a,(uint8_t)target,(uint8_t)(target>>8),0xc3,(uint8_t)target,(uint8_t)(target>>8)};
    fixture(profile,dual,sizeof(dual)); explore(profile,sizeof(rom),1);
    CHECK(result.flags[0x600]==(DP_SEGA_STATIC|DP_SEGA_STATIC_DATA));
    CHECK(result.target_closure.static_data_bytes==1);
    CHECK(result.closure.new_bytes==sizeof(dual)+1 && !result.target_closure.new_bytes);
}
static void guards(void) {
    uint8_t flags[sizeof(rom)];
    for(unsigned variant=0;variant<3;variant++) {
        uint16_t source=variant==0?0xc000:variant==1?0x98ff:0x6100;
        const uint8_t code[]={0x3a,(uint8_t)source,(uint8_t)(source>>8),0x76};
        fixture(3,code,sizeof(code)); memset(flags,0,sizeof(flags));
        const DpSegaBackend *b=dp_msx_backend();
        CHECK(b->init(rom,sizeof(rom),NULL,variant==1?DP_MSX_KONAMI_SCC:variant==2?DP_MSX_ASCII8:DP_MSX_PLAIN));
        dp_msx_out(0xa8,0xd4); dp_msx_cpu()->pc=0x4500;
        if(variant==1) dp_msx_write(0x9000,0x3f);
        DpSegaClosureOptions o={128,8,4,16}; DpSegaClosureStats base,targets;
        DpSegaClosure *c=dp_sega_closure_create(b,dp_msx_system(),rom,sizeof(rom),flags,o,&base);
        CHECK(c); CHECK(dp_sega_closure_targets(c,&targets));
        if(variant==2) {
            // A known memory write invalidates the table window before the read.
            dp_sega_closure_free(c);
            const uint8_t banked[]={0xdd,0x21,0,0x68,0xdd,0x77,0,0x3a,0,0x61,0x76};
            dp_msx_close(); fixture(3,banked,sizeof(banked));
            CHECK(b->init(rom,sizeof(rom),NULL,DP_MSX_ASCII8)); dp_msx_out(0xa8,0xd4); dp_msx_cpu()->pc=0x4500;
            c=dp_sega_closure_create(b,dp_msx_system(),rom,sizeof(rom),flags,o,&base);
            CHECK(c); CHECK(dp_sega_closure_targets(c,&targets));
        }
        dp_sega_closure_observe(c); dp_sega_closure_run(c);
        CHECK(!base.static_data_reads && !targets.static_data_reads);
        for(unsigned i=0;i<sizeof(flags);i++) CHECK(!(flags[i]&DP_SEGA_STATIC_DATA));
        dp_sega_closure_free(c); dp_msx_close();
    }
}
static void promotion_and_caps(void) {
    // Data may be established before the same byte is reached as code.
    // Code counters and final map priority must remain independent of that order.
    const uint8_t code[]={0x3a,0,0x46,0xc3,0,0x46}; fixture(3,code,sizeof(code));
    CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_PLAIN)); dp_msx_cpu()->pc=0x4500;
    DpSegaClosureOptions o={64,8,4,8}; DpSegaClosureStats base,targets;
    uint8_t flags[sizeof(rom)]={0};
    DpSegaClosure *c=dp_sega_closure_create(dp_msx_backend(),dp_msx_system(),rom,sizeof(rom),flags,o,&base);
    CHECK(c); CHECK(dp_sega_closure_targets(c,&targets));
    // A preexisting data-only proof models an earlier bounded batch. It must
    // neither block code traversal nor suppress new code evidence.
    flags[0x600]=DP_SEGA_STATIC_DATA;
    dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(flags[0x600]==(DP_SEGA_STATIC|DP_SEGA_STATIC_DATA)); CHECK(base.new_bytes==7);
    dp_sega_closure_free(c); dp_msx_close();
    const uint8_t load[]={0x3a,0,0x47,0x76}; fixture(3,load,sizeof(load));
    CHECK(dp_msx_init(rom,sizeof(rom),DP_MSX_PLAIN)); dp_msx_cpu()->pc=0x4500; memset(flags,0,sizeof(flags)); o.nodes=1;
    c=dp_sega_closure_create(dp_msx_backend(),dp_msx_system(),rom,sizeof(rom),flags,o,&base);
    CHECK(c); CHECK(dp_sega_closure_targets(c,&targets)); dp_sega_closure_observe(c); dp_sega_closure_run(c);
    CHECK(base.node_cuts && targets.node_cuts && targets.static_data_reads==1 && targets.nodes==1);
    CHECK(flags[0x700]==DP_SEGA_STATIC_DATA && !flags[0x503]);
    dp_sega_closure_free(c); dp_msx_close();
}
static void evidence_roundtrip(const DpSegaResults *r) {
    FILE *f=tmpfile(); CHECK(f); if(!f) return;
    dp_sega_evidence(f,r); rewind(f); char line[256]; unsigned bank,lo,hi,mask; size_t seen=0;
    while(fgets(line,sizeof(line),f)) {
        if(line[0]=='#') continue;
        if(sscanf(line,"%x %x %x %x",&bank,&lo,&hi,&mask)!=4) { CHECK(0); continue; }
        size_t start=(size_t)bank*0x4000+lo,end=(size_t)bank*0x4000+hi;
        CHECK(start==seen && lo<=hi && hi<0x4000 && end<r->size && mask<=255);
        for(size_t i=start;i<=end && i<r->size;i++) CHECK(r->flags[i]==mask);
        seen=end+1;
    }
    CHECK(seen==r->size); fclose(f);
}
static void map_and_report(void) {
    uint8_t flags[]={0,DP_SEGA_STATIC_DATA,DP_SEGA_STATIC_DATA|DP_SEGA_STATIC,
        DP_SEGA_STATIC_DATA|DP_SEGA_DATA,DP_SEGA_STATIC_DATA|DP_SEGA_FORCED,
        DP_SEGA_STATIC_DATA|DP_SEGA_COPIED|DP_SEGA_FORCED,
        DP_SEGA_STATIC_DATA|DP_SEGA_NATURAL|DP_SEGA_COPIED,DP_SEGA_STATIC_DATA|DP_SEGA_STATIC|DP_SEGA_DATA};
    static DpSegaResults r; r.flags=flags; r.size=sizeof(flags);
    // GB precedence: code (natural > forced > static) beats data (copied >
    // static > read), which beats the static scan classes
    const char *expected[]={"unknown","data-static","code-static","data-static","code-forced","code-forced","code-natural","code-static"};
    FILE *f=tmpfile(); CHECK(f); if(!f) return; dp_sega_map(f,&r); rewind(f);
    char line[512],kind[32]; unsigned seen=0,bank,lo,hi;
    while(fgets(line,sizeof(line),f)) {
        if(line[0]=='#') continue;
        if(sscanf(line,"%x %x %x %31s",&bank,&lo,&hi,kind)!=4) { CHECK(0); continue; }
        CHECK(!bank && lo==seen && hi<sizeof(flags));
        for(unsigned i=lo;i<=hi && i<sizeof(flags);i++) CHECK(!strcmp(kind,expected[i]));
        seen=hi+1;
    }
    CHECK(seen==sizeof(flags)); fclose(f);
    f=tmpfile(); CHECK(f); if(!f) return; dp_sega_report(f,dp_sega_system("sms"),&r); rewind(f);
    int matched=0; while(fgets(line,sizeof(line),f))
        matched|=strstr(line,"code-natural 1; code-forced 2; code-static 2; data 0; data-static 2")!=NULL;
    CHECK(matched); fclose(f); evidence_roundtrip(&r);
    uint8_t *boundary=malloc(0x4002); CHECK(boundary); if(!boundary) return;
    memset(boundary,DP_SEGA_STATIC_DATA,0x4002); r.flags=boundary; r.size=0x4002;
    evidence_roundtrip(&r); free(boundary); r.flags=NULL; r.size=0;
}
int main(void) {
    for(unsigned profile=0;profile<4;profile++) { byte_table(profile); word_and_dual_use(profile); }
    dp_sega_results_free(&result); guards(); promotion_and_caps(); map_and_report();
    TEST_DONE("z80_static_data");
}
