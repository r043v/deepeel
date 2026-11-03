/* tas.c: TAS movie loading for the z80 systems (GB tas.c parity).  Every
 * format reduces to one mask byte per frame in the sega layout (tas.h);
 * BizHawk key names of each console map onto it at load time. */
#define _GNU_SOURCE
#include "tas.h"
#include "../../../src/core/zip.h"
#include "../../../src/core/sha1.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DP_TAS_MAX_FRAMES (16u * 1024 * 1024) // 16 M frames: 16 MiB of masks

static int emit(DpSegaTas *out,uint8_t mask,char *err,size_t errsz) {
    if(out->frames>=DP_TAS_MAX_FRAMES) { snprintf(err,errsz,"movie too long"); return 0; }
    if(out->frames%65536==0) {
        uint8_t *p=realloc(out->masks,out->frames+65536);
        if(!p) { snprintf(err,errsz,"out of memory"); return 0; }
        out->masks=p;
    }
    out->masks[out->frames++]=mask;
    return 1;
}
// BizHawk key names across the three consoles: SMS/GG pads are B and C,
// MSX uses full words and Return for Enter
static uint8_t btn_mask(const char *n) {
    if(!strcmp(n,"up") || !strcmp(n,"u")) return 1;
    if(!strcmp(n,"down") || !strcmp(n,"d")) return 2;
    if(!strcmp(n,"left") || !strcmp(n,"l")) return 4;
    if(!strcmp(n,"right") || !strcmp(n,"r")) return 8;
    if(!strcmp(n,"button1") || !strcmp(n,"b1") || !strcmp(n,"b")) return 16;
    if(!strcmp(n,"button2") || !strcmp(n,"b2") || !strcmp(n,"c")) return 32;
    if(!strcmp(n,"start") || !strcmp(n,"st") || !strcmp(n,"pause") || !strcmp(n,"return")) return 64;
    if(!strcmp(n,"reset")) return 128;
    return 0; // Power, hard/soft keys, motion: ignored
}
// frames are '|'-separated segments; within a segment one character per
// LogKey name in order, '.' = released, anything else = pressed
static int log_parse(const uint8_t *d,size_t n,DpSegaTas *out,char *err,size_t errsz) {
    const char *p=(const char *)d,*end=(const char *)d+n;
    uint8_t key_mask[64];
    int nkey=0,any_frame=0;
    char line[4096];
    while(p<end) {
        size_t len=0;
        while(p+len<end && p[len]!='\n' && len<sizeof line-1) len++;
        memcpy(line,p,len); line[len]=0;
        p+=len+(p+len<end?1:0);
        size_t ll=strlen(line);
        if(ll && line[ll-1]=='\r') line[ll-1]=0;
        if(!strncmp(line,"LogKey:",7)) {
            nkey=0;
            char *tok=strtok(line+7,"| \t#");
            while(tok && nkey<64) {
                for(char *c=tok;*c;c++) *c=(char)tolower((unsigned char)*c);
                key_mask[nkey++]=btn_mask(tok);
                tok=strtok(NULL,"| \t#");
            }
            continue;
        }
        if(line[0]!='|') continue;
        char fl[4096];
        snprintf(fl,sizeof fl,"%s",line+1);
        uint8_t mask=0;
        int idx=0,seen=0;
        for(char *c=fl;*c;c++) {
            if(*c=='|') { idx=0; continue; } // next controller: restart the LogKey
            if(nkey && idx<nkey) {
                if(*c!='.') mask|=key_mask[idx];
                idx++;
            } else {
                // no LogKey: positional right,left,up,down,b1,b2,system,reset
                static const uint8_t pos[8]={8,4,1,2,16,32,64,128};
                if(*c!='.') mask|=idx<8?pos[idx]:0;
                idx++;
            }
            seen=1;
        }
        if(!seen) continue;
        if(!emit(out,mask,err,errsz)) return 0;
        any_frame=1;
    }
    if(!any_frame) { snprintf(err,errsz,"no input frames found"); return 0; }
    return 1;
}
static int tas_load_buf(uint8_t *d,size_t n,DpSegaTas *out,char *err,size_t errsz,int depth) {
    if(depth>3) { snprintf(err,errsz,"too many nested containers"); return 0; }
    if(dp_zip_is(d,n)) {
        size_t hn=0;
        uint8_t *hdr=dp_zip_read(d,n,"header",&hn,NULL);
        if(hdr) {
            if(strcasestr((const char *)hdr,"startsfromsavestate true")) out->anchored=1;
            for(size_t i=0;i+45<=hn;i++) { // line "SHA1 <40 hex>"
                if((i==0 || hdr[i-1]=='\n') && !strncmp((const char *)hdr+i,"SHA1 ",5)) {
                    uint8_t h[20]; int ok=1;
                    for(int k=0;k<20 && ok;k++) {
                        char b[3]={(char)hdr[i+5+2*k],(char)hdr[i+6+2*k],0},*e;
                        h[k]=(uint8_t)strtoul(b,&e,16); if(*e) ok=0;
                    }
                    if(ok) { memcpy(out->sha1,h,20); out->has_sha1=1; }
                    break;
                }
            }
            free(hdr);
        }
        size_t ln=0;
        uint8_t *log=dp_zip_read(d,n,"input log",&ln,NULL);
        if(!log) log=dp_zip_read(d,n,"log",&ln,NULL);
        if(!log) log=dp_zip_read(d,n,".txt",&ln,NULL);
        if(log) {
            int ok=tas_load_buf(log,ln,out,err,errsz,depth+1);
            free(log);
            return ok;
        }
        size_t an=0;
        char name[128];
        uint8_t *any=dp_zip_read(d,n,NULL,&an,name);
        if(!any) { snprintf(err,errsz,"zip: no readable entry"); return 0; }
        int ok=tas_load_buf(any,an,out,err,errsz,depth+1);
        free(any);
        return ok;
    }
    if(memchr(d,'|',n<4096?n:4096)) return log_parse(d,n,out,err,errsz);
    snprintf(err,errsz,"unknown movie format");
    return 0;
}
int dp_sega_tas_load(const char *path,DpSegaTas *out,char *err,size_t errsz) {
    memset(out,0,sizeof *out);
    err[0]=0;
    FILE *f=fopen(path,"rb");
    if(!f) { snprintf(err,errsz,"cannot read %s",path); return 0; }
    fseek(f,0,SEEK_END);
    long sz=ftell(f);
    fseek(f,0,SEEK_SET);
    if(sz<=0 || sz>(1<<30)) { fclose(f); snprintf(err,errsz,"%s: empty or too large",path); return 0; }
    uint8_t *d=malloc((size_t)sz);
    if(!d || fread(d,1,(size_t)sz,f)!=(size_t)sz) {
        free(d); fclose(f);
        snprintf(err,errsz,"cannot read %s",path);
        return 0;
    }
    fclose(f);
    snprintf(out->source,sizeof out->source,"%s",path);
    int ok=tas_load_buf(d,(size_t)sz,out,err,errsz,0);
    free(d);
    if(!ok) dp_sega_tas_free(out);
    return ok;
}
void dp_sega_tas_free(DpSegaTas *t) {
    if(!t) return;
    free(t->masks);
    memset(t,0,sizeof *t);
}
int dp_sega_tas_rom_match(const DpSegaTas *t,const uint8_t *rom,size_t size,char *why,size_t whysz) {
    if(!t->has_sha1) { snprintf(why,whysz,"movie records no ROM identity"); return -1; }
    uint8_t h[20]; dp_sha1(rom,size,h);
    char a[41],b[41];
    for(int k=0;k<20;k++) { snprintf(a+2*k,3,"%02x",t->sha1[k]); snprintf(b+2*k,3,"%02x",h[k]); }
    snprintf(why,whysz,"movie SHA-1 %s, ROM SHA-1 %s",a,b);
    return !memcmp(h,t->sha1,20);
}
