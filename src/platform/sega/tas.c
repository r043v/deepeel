/* tas.c: TAS movie loading for the z80 systems (GB tas.c parity).  Every
 * format reduces to one mask byte per frame in the sega layout (tas.h);
 * BizHawk key names of each console map onto it at load time. */
#define _GNU_SOURCE
#include "tas.h"
#include "backend.h"
#include "../../../src/core/sha1.h"
#include "../../../src/core/md5.h"
#include "../../../src/core/zip.h"
#include "../../../src/core/bk2.h"
#include "../../../src/core/bizhawk.h"

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
static int header_line(const uint8_t *data,size_t n,const char *line) {
    size_t len=strlen(line);
    for(size_t i=0;i+len<=n;i++)
        if((i==0 || data[i-1]=='\n') && !memcmp(data+i,line,len) &&
           (i+len==n || data[i+len]=='\n' || data[i+len]=='\r')) return 1;
    return 0;
}
// BizHawk key names across the three consoles: SMS/GG pads are B and C,
// MSX uses full words and Return for Enter.  Column names carry the player
// prefix ("P1 Up", "P2 Left"), so the match is on the contained word.
static uint8_t btn_mask(const char *n) {
    if(strstr(n,"up")) return 1;
    if(strstr(n,"down")) return 2;
    if(strstr(n,"left")) return 4;
    if(strstr(n,"right")) return 8;
    size_t l=strlen(n);
    if(strstr(n,"button1") || strstr(n,"button 1") || strstr(n,"b1") || !strcmp(n,"b")
       || (l>=3 && !strcmp(n+l-2," b"))) return 16;
    if(strstr(n,"button2") || strstr(n,"button 2") || strstr(n,"b2")
       || !strcmp(n,"c") || (l>=3 && !strcmp(n+l-2," c"))) return 32;
    if(strstr(n,"start") || strstr(n,"st") || strstr(n,"pause") || strstr(n,"return")) return 64;
    if(strstr(n,"reset")) return 128;
    return 0; // Power, hard/soft keys, motion: ignored
}
// frames are '|'-separated segments; within a segment one character per
// LogKey name in order, '.' = released, anything else = pressed
static int log_parse_positional(const uint8_t *d,size_t n,DpSegaTas *out,char *err,size_t errsz) {
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
            // split on '|' only: names contain spaces ("P1 Up"), and the
            // leading '#' is the sticky-column marker, not part of the name
            char *tok=strtok(line+7,"|");
            while(tok && nkey<64) {
                while(*tok=='#' || *tok==' ' || *tok=='\t') tok++;
                for(char *c=tok;*c;c++) *c=(char)tolower((unsigned char)*c);
                key_mask[nkey++]=btn_mask(tok);
                tok=strtok(NULL,"|");
            }
            continue;
        }
        if(line[0]!='|') continue;
        char fl[4096];
        snprintf(fl,sizeof fl,"%s",line+1);
        uint8_t mask=0;
        int idx=0,seen=0;
        for(char *c=fl;*c;c++) {
            // the cells group the LogKey columns (system keys | P1 | P2): the
            // column sequence is continuous across the row, so with a LogKey
            // the index does NOT restart at each cell
            if(*c=='|') {
                if(!nkey) idx=0;   // positional grammar: 8 buttons per controller
                continue;
            }
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
// LogKey movies go through the shared BizHawk reader (src/core/bizhawk.c): every named column is mapped onto the
// sega mask by btn_mask; movies without a LogKey keep the positional grammar above
static int log_parse(const uint8_t *d,size_t n,DpSegaTas *out,char *err,size_t errsz) {
    if(!memmem(d,n,"LogKey:",7)) return log_parse_positional(d,n,out,err,errsz);
    DpBzMovie m;
    if(!dp_bz_load(d,n,&m,err,errsz)) return 0;
    uint8_t km[512]; unsigned nk=m.nkeys<512?m.nkeys:512;
    for(unsigned i=0;i<nk;i++) {
        char low[DP_BZ_NAME];
        size_t k=0;
        for(const char *c=m.keys[i].name;*c && k+1<sizeof low;c++) low[k++]=(char)tolower((unsigned char)*c);
        low[k]=0;
        km[i]=m.keys[i].axis?0:btn_mask(low);
    }
    int ok=1;
    for(uint32_t f=0;f<m.nframes && ok;f++) {
        uint8_t mask=0;
        for(unsigned i=0;i<nk;i++) if(km[i] && dp_bz_held(&m,f,(int)i)) mask|=km[i];
        ok=emit(out,mask,err,errsz);
    }
    dp_bz_free(&m);
    return ok;
}
// Meka .mmv: "MMV\0", u32 frame count at 0x08, u32 header size at 0x18, ROM MD5 at 0xE4, then one u16 per
// frame whose low byte is the player-1 pad in the sega layout (up 1, down 2, left 4, right 8, b1 16, b2 32, start 64)
static int mmv_parse(const uint8_t *d,size_t n,DpSegaTas *out,char *err,size_t errsz) {
    if(n<0x100) { snprintf(err,errsz,"mmv: truncated header"); return 0; }
    uint32_t frames=d[8]|d[9]<<8|d[10]<<16|(uint32_t)d[11]<<24, hdr=d[0x18]|d[0x19]<<8|d[0x1A]<<16|(uint32_t)d[0x1B]<<24;
    if(hdr<0xF4 || hdr>n || (size_t)frames*2>n-hdr) { snprintf(err,errsz,"mmv: inconsistent header (frames %u, header %u, size %zu)",frames,hdr,n); return 0; }
    for(uint32_t f=0;f<frames;f++) if(!emit(out,d[hdr+2*(size_t)f],err,errsz)) return 0;
    memcpy(out->md5,d+0xE4,16); out->has_md5=1;
    return frames>0 ? 1 : (snprintf(err,errsz,"mmv: no input frames"),0);
}
static int tas_load_buf(uint8_t *d,size_t n,DpSegaTas *out,char *err,size_t errsz,int depth) {
    if(depth>3) { snprintf(err,errsz,"too many nested containers"); return 0; }
    if(dp_zip_is(d,n)) {
        // the container is the shared kernel's job (src/core/bk2.c); the '|'
        // frame grammar stays here
        DpBk2 b;
        if(!dp_bk2_open(d,n,&b,err,errsz)) {
            size_t ml=0;
            uint8_t *mm=dp_zip_read(d,n,".mmv",&ml,NULL);   // a zip holding a Meka movie rather than a bk2
            if(!mm) return 0;
            err[0]=0;
            int ok=tas_load_buf(mm,ml,out,err,errsz,depth+1);
            free(mm);
            return ok;
        }
        out->anchored |= b.anchored;
        out->has_sha1 |= b.has_sha1;
        if(b.has_sha1) memcpy(out->sha1,b.sha1,20);
        out->has_md5 |= b.has_md5;
        if(b.has_md5) memcpy(out->md5,b.md5,16);
        if(b.header && header_line(b.header,b.header_n,"emuVersion Version 1.11.4") &&
           header_line(b.header,b.header_n,"Core SMSHawk")) out->smshawk_1114=1;
        int ok=tas_load_buf(b.log,b.log_n,out,err,errsz,depth+1);
        dp_bk2_free(&b);
        return ok;
    }
    if(n>4 && !memcmp(d,"MMV\0",4)) return mmv_parse(d,n,out,err,errsz);
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
    // DP_SEGA_TAS_MASKS=file: the loaded movie as one mask byte per frame (feeds the BizHawk dumper, tools/smshawk_dump)
    if(ok && getenv("DP_SEGA_TAS_MASKS")) { FILE *mf=fopen(getenv("DP_SEGA_TAS_MASKS"),"wb"); if(mf) { fwrite(out->masks,1,out->frames,mf); fclose(mf); } }
    if(!ok) dp_sega_tas_free(out);
    return ok;
}
void dp_sega_tas_free(DpSegaTas *t) {
    if(!t) return;
    free(t->masks);
    memset(t,0,sizeof *t);
}
int dp_sega_tas_smshawk_profile(const DpSegaTas *t) {
    return t->smshawk_1114 ? DP_TAS_SMSHAWK_1114 : DP_TAS_SMSHAWK_CURRENT;
}
int dp_sega_tas_rom_match(const DpSegaTas *t,const uint8_t *rom,size_t size,char *why,size_t whysz) {
    if(!t->has_sha1 && !t->has_md5) { snprintf(why,whysz,"movie records no ROM identity"); return -1; }
    char a[41],b[41];
    if(t->has_sha1) {
        uint8_t h[20]; dp_sha1(rom,size,h);
        for(int k=0;k<20;k++) { snprintf(a+2*k,3,"%02x",t->sha1[k]); snprintf(b+2*k,3,"%02x",h[k]); }
        if(!memcmp(h,t->sha1,20)) { snprintf(why,whysz,"movie SHA-1 %s matches",a); return 1; }
        if(!t->has_md5) { snprintf(why,whysz,"movie SHA-1 %s, ROM SHA-1 %s",a,b); return 0; }
    }
    if(t->has_md5) {
        uint8_t m[16]; dp_md5(rom,size,m);
        char ma[33],mb[33];
        for(int k=0;k<16;k++) { snprintf(ma+2*k,3,"%02x",t->md5[k]); snprintf(mb+2*k,3,"%02x",m[k]); }
        snprintf(why,whysz,"movie MD5 %s%s, ROM MD5 %s",ma,t->has_sha1?" (SHA1 label)":"",
                 mb);
        return !memcmp(m,t->md5,16);
    }
    return 0;
}
