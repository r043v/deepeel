#include "../src/platform/sega/explore.h"
#include "../src/platform/sega/tas.h"
#include "../src/core/sha1.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// GB tas.c parity: a BizHawk .bk2 (a stored zip here: Header + Input Log) or
// a plain '|'-frame log loads to one sega-layout mask byte per frame; the
// replay runs the movie first, from reset, and its natural coverage carries
// PLAYED for sections the fork explorer cannot reach (the SMS Pause NMI
// handler). No real movie ships in the repository: the .bk2 is built here.
static void wr16(uint8_t *p,size_t o,unsigned v) { p[o]=(uint8_t)v; p[o+1]=(uint8_t)(v>>8); }
static void wr32(uint8_t *p,size_t o,uint32_t v) {
    wr16(p,o,(uint16_t)v); wr16(p,o+2,(uint16_t)(v>>16));
}
// stored-entry zip with up to two entries: local headers + data, then one
// central directory record per entry and the EOCD
static size_t zip_write2(const char *name0,const uint8_t *d0,size_t n0,
                         const char *name1,const uint8_t *d1,size_t n1,uint8_t *out) {
    size_t p=0;
    uint32_t lho[2],cd;
    const char *name[2]={name0,name1};
    const uint8_t *data[2]={d0,d1};
    size_t n[2]={n0,n1};
    for(int e=0;e<2;e++) {
        if(!name[e]) continue;
        size_t nl=strlen(name[e]);
        lho[e]=(uint32_t)p;
        memcpy(out+p,"PK\x03\x04",4);
        wr16(out,p+4,20); wr16(out,p+6,0); wr16(out,p+8,0);
        wr16(out,p+10,0); wr16(out,p+12,0);
        wr32(out,p+14,0); wr32(out,p+18,(uint32_t)n[e]); wr32(out,p+22,(uint32_t)n[e]);
        wr16(out,p+26,(unsigned)nl); wr16(out,p+28,0);
        p+=30; memcpy(out+p,name[e],nl); p+=nl;
        memcpy(out+p,data[e],n[e]); p+=n[e];
    }
    cd=(uint32_t)p;
    int entries=0;
    for(int e=0;e<2;e++) {
        if(!name[e]) continue;
        size_t nl=strlen(name[e]);
        entries++;
        memcpy(out+p,"PK\x01\x02",4);
        wr16(out,p+4,20); wr16(out,p+6,20); wr16(out,p+8,0); wr16(out,p+10,0);
        wr16(out,p+12,0); wr16(out,p+14,0);
        wr32(out,p+16,0); wr32(out,p+20,(uint32_t)n[e]); wr32(out,p+24,(uint32_t)n[e]);
        wr16(out,p+28,(unsigned)nl); wr16(out,p+30,0); wr16(out,p+32,0);
        wr16(out,p+34,0); wr16(out,p+36,0); wr32(out,p+38,0); wr32(out,p+42,lho[e]);
        p+=46; memcpy(out+p,name[e],nl); p+=nl;
    }
    memcpy(out+p,"PK\x05\x06",4);
    wr16(out,p+4,0); wr16(out,p+6,0); wr16(out,p+8,(unsigned)entries); wr16(out,p+10,(unsigned)entries);
    wr32(out,p+12,(uint32_t)p-cd); wr32(out,p+16,cd); wr16(out,p+20,0);
    return p+22;
}
static void loader_plain_log(void) {
    static const char log[]="LogKey: Up|Down|Left|Right|B|C|Start|Reset\n"
        "|U.......\n|.....C..\n|......S.\n|.......R\n";
    char path[]="/tmp/dp_tas_plain_XXXXXX";
    int fd=mkstemp(path); CHECK(fd>=0);
    CHECK(write(fd,log,sizeof(log)-1)==(ssize_t)(sizeof(log)-1)); close(fd);
    DpSegaTas t; char err[256];
    CHECK(dp_sega_tas_load(path,&t,err,sizeof err));
    CHECK(t.frames==4 && !t.anchored);
    static const uint8_t want[]={1,32,64,128}; // up, C (button 2), Start, Reset
    for(unsigned i=0;i<4;i++) CHECK(t.masks[i]==want[i]);
    dp_sega_tas_free(&t);
    unlink(path);
}
static void loader_bk2_zip(void) {
    static const char hdr[]="StartsFromSavestate True\n";
    static const char log[]="LogKey: Up|Down|Left|Right|Button1|Button2|Start|Reset\n"
        "|....B...\n|....B...\n";
    uint8_t zip[512];
    size_t n=zip_write2("Header.txt",(const uint8_t *)hdr,sizeof(hdr)-1,
                        "Input Log.txt",(const uint8_t *)log,sizeof(log)-1,zip);
    char path[]="/tmp/dp_tas_bk2_XXXXXX";
    int fd=mkstemp(path); CHECK(fd>=0);
    CHECK(write(fd,zip,n)==(ssize_t)n); close(fd);
    DpSegaTas t; char err[256];
    int loaded=dp_sega_tas_load(path,&t,err,sizeof err);
    if(!loaded) fprintf(stderr,"bk2 loader: %s\n",err);
    CHECK(loaded);
    CHECK(t.frames==2 && t.anchored);
    CHECK(t.masks[0]==16 && t.masks[1]==16); // Button 1 held
    dp_sega_tas_free(&t);
    unlink(path);
}
// header SHA1 line -> ROM identity verdict (1 same, 0 different, -1 none)
static void rom_identity(void) {
    uint8_t rom[64]; memset(rom,0x5a,sizeof rom);
    uint8_t h[20]; dp_sha1(rom,sizeof rom,h);
    char good[128],bad[128]; int g=snprintf(good,sizeof good,"SHA1 "),b=snprintf(bad,sizeof bad,"SHA1 ");
    for(int k=0;k<20;k++) { g+=snprintf(good+g,4,"%02x",h[k]); b+=snprintf(bad+b,4,"%02x",(uint8_t)(h[k]^1)); }
    snprintf(good+g,8,"\n"); snprintf(bad+b,8,"\n");
    static const char log[]="|....B...\n";
    const char *hdrs[3]={good,bad,"Author x\n"}; int want[3]={1,0,-1};
    for(int i=0;i<3;i++) {
        uint8_t zip[1024];
        size_t n=zip_write2("Header.txt",(const uint8_t *)hdrs[i],strlen(hdrs[i]),
                            "Input Log.txt",(const uint8_t *)log,sizeof(log)-1,zip);
        char path[]="/tmp/dp_tas_id_XXXXXX";
        int fd=mkstemp(path); CHECK(fd>=0);
        CHECK(write(fd,zip,n)==(ssize_t)n); close(fd);
        DpSegaTas t; char err[256],why[256];
        CHECK(dp_sega_tas_load(path,&t,err,sizeof err));
        CHECK(dp_sega_tas_rom_match(&t,rom,sizeof rom,why,sizeof why)==want[i]);
        dp_sega_tas_free(&t);
        unlink(path);
    }
}
static void loader_rejects_garbage(void) {
    char path[]="/tmp/dp_tas_bad_XXXXXX";
    int fd=mkstemp(path); CHECK(fd>=0);
    CHECK(write(fd,"not a movie",11)==11); close(fd);
    DpSegaTas t; char err[256];
    CHECK(!dp_sega_tas_load(path,&t,err,sizeof err) && err[0]);
    CHECK(!t.frames); dp_sega_tas_free(&t);
    unlink(path);
}
// replay fixture: same shape as the play-session one — the Pause key raises
// NMI and the handler is unreachable without the system key held
static uint8_t rom[0x10000];
static DpSegaResults result;
static void fixture(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01;
    rom[0x100]=0x31; rom[0x101]=0x00; rom[0x102]=0xf0; // ld sp,$F000
    rom[0x103]=0x18; rom[0x104]=0xfd;                  // jr $100: idle loop
    rom[0x66]=0xc3; rom[0x67]=0x50; rom[0x68]=0x01;    // jp $0150
    rom[0x150]=0x06; rom[0x151]=0x77;                  // ld b,$77
    rom[0x152]=0xed; rom[0x153]=0x45;                  // retn
}
static void write_movie(const char *path,const char *frames) {
    char log[256];
    int n=snprintf(log,sizeof log,"LogKey: Up|Down|Left|Right|B|C|Start|Pause|Reset\n%s",frames);
    FILE *f=fopen(path,"w"); CHECK(f);
    CHECK(fwrite(log,1,(size_t)n,f)==(size_t)n); fclose(f);
}
static DpSegaSection *section_at(unsigned physical) {
    for(uint32_t i=0;i<result.section_count;i++) {
        DpSegaSection *s=&result.sections[i];
        if(s->bank==(uint8_t)(physical>>14) && (physical&0x3fff)>=s->lo && (physical&0x3fff)<=s->hi) return s;
    }
    return NULL;
}
static void replay_reaches_movie_code(void) {
    fixture();
    char path[]="/tmp/dp_tas_replay_XXXXXX";
    int fd=mkstemp(path); CHECK(fd>=0); close(fd);
    write_movie(path,"|.......P.\n|.......P.\n|.........\n");
    DpSegaOptions o=dp_sega_options();
    o.max_total=20000; o.max_steps=256; o.max_paths=64; o.queue_cap=16;
    o.seed_vectors=0; o.seed_cap=0; o.input_masks=0; o.closure.nodes=0;
    o.no_context_evict=1; o.tas_path=path;
    o.tas_total=20000; o.tas_frame_steps=2000;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&result));
    CHECK(result.tas_frames==3 && result.tas_frames_refused==0 && result.tas_steps>0);
    // handler coverage is movie-only: PLAYED, and the idle loop is boot
    CHECK((result.flags[0x150]&DP_SEGA_NATURAL) && (result.flags[0x152]&DP_SEGA_NATURAL));
    DpSegaSection *nmi=section_at(0x150);
    CHECK(nmi && nmi->played_hits>0 && !nmi->forced_hits && !nmi->boot_hits);
    CHECK(nmi->verdict==DP_SEGA_VERDICT_PLAYED);
    DpSegaSection *idle=section_at(0x100);
    CHECK(idle && idle->boot_hits>0 && idle->verdict==DP_SEGA_VERDICT_REQUIRED);
    dp_sega_results_free(&result);
    // the movie is replayed in full, never cut (0.251.14): a tight tas_total
    // no longer drops frames (refused counts only frames the replay could not
    // advance, e.g. a starved tas_frame_steps)
    DpSegaOptions tight=o; tight.tas_total=1;
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&tight,&result));
    CHECK(result.tas_frames==3 && result.tas_frames_refused==0);
    dp_sega_results_free(&result);
    unlink(path);
}
int main(void) {
    loader_plain_log();
    loader_bk2_zip();
    rom_identity();
    loader_rejects_garbage();
    replay_reaches_movie_code();
    TEST_DONE("sega_tas");
}
