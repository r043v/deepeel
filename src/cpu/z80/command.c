#include "z80.h"
#include "../../platform/sega/sega.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>

// Explicit CPU command prevents a Sega image from entering the GB loader.
int dp_z80_command(int argc,char **argv) {
    if(argc<5 || argc>6 || strcmp(argv[2],"dis")) {
        fprintf(stderr,"deepeel z80 dis ROM hex-offset [instruction-count]\n"); return 2;
    }
    char *end; errno=0;
    unsigned long offset=strtoul(argv[4],&end,16);
    if(errno || !*argv[4] || *end) return 2;
    unsigned long count=16;
    if(argc==6) { errno=0; count=strtoul(argv[5],&end,10); if(errno || !*argv[5] || *end || count>65536) return 2; }
    FILE *f=fopen(argv[3],"rb");
    if(!f) { perror(argv[3]); return 1; }
    if(fseek(f,0,SEEK_END)) { fclose(f); return 1; }
    long length=ftell(f);
    if(length<0 || offset>=(unsigned long)length || fseek(f,(long)offset,SEEK_SET)) { fclose(f); return 2; }
    size_t size=(unsigned long)length-offset;
    // No instruction can exceed a 16-bit address space; provide lookahead.
    if(size>131072) size=131072;
    uint8_t *bytes=malloc(size);
    if(!bytes) { fclose(f); return 1; }
    if(fread(bytes,1,size,f)!=size) { free(bytes); fclose(f); return 1; }
    fclose(f);
    size_t at=0;
    for(unsigned long i=0;i<count && at<size;i++) {
        DpZ80Instruction in; char text[128];
        if(!dp_z80_decode(bytes+at,size-at,(uint16_t)(offset+at),&in)) {
            fprintf(stderr,"truncated instruction or prefix saturation at ROM offset $%lX\n",offset+(unsigned long)at);
            free(bytes); return 1;
        }
        dp_z80_format(&in,text,sizeof(text));
        printf("%08lX  %s\n",offset+(unsigned long)at,text);
        at+=in.raw.length;
    }
    free(bytes); return 0;
}
