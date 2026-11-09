/* tas.h: TAS movie files as replay inputs (GB tas.c parity, sega layout).
 * Formats: BizHawk .bk2 (a zip, sometimes nested, with an "Input Log.txt")
 * and a plain input log (lines starting with '|', e.g. an extracted log).
 * The loaded movie is one mask byte per frame in the sega layout:
 * up=1 down=2 left=4 right=8 button1=16 button2=32, bit 64 is the system
 * key (GG Start, SMS/SG Pause, MSX Return) and bit 128 is Reset (SMS pad-2). */
#ifndef DP_SEGA_TAS_H
#define DP_SEGA_TAS_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint8_t *masks;   // one byte per frame, sega layout above
    uint32_t frames;
    int anchored;     // 1 = the movie starts from a savestate (replay from
                      // power-on is best-effort, not frame-exact)
    int has_sha1;     // the movie header records the SHA-1 of its ROM
    uint8_t sha1[20];
    int has_md5;      // BizHawk SMSHawk writes an MD5 under the "SHA1" label
    uint8_t md5[16];
    char source[512];
} DpSegaTas;
// 1 = same ROM, 0 = different, -1 = the movie records no identity; `why` explains
// (either digest matching counts as a match)
int dp_sega_tas_rom_match(const DpSegaTas *t,const uint8_t *rom,size_t size,char *why,size_t whysz);
// sniff and load; returns 1 on success, 0 with a human message in `err`
int dp_sega_tas_load(const char *path,DpSegaTas *out,char *err,size_t errsz);
void dp_sega_tas_free(DpSegaTas *t);
#endif
