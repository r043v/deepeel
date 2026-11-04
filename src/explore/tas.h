/* tas.h: tool-assisted movie files as session inputs.  Three formats:
 *  - VBM (.vmv, VBA/VBA-M): binary "VBM\x1A" header and the older text
 *    "VBM_MOVIE v1" header, the bulk of the classic GB TASes;
 *  - BK2 (.bk2, BizHawk): zip (sometimes nested) with an "Input Log.txt";
 *  - plain input log (a .txt whose lines start with '|', e.g. an extracted
 *    bk2 log or a .bkm log).
 * The loaded movie is one mask byte per frame in the deepeel joypad layout
 * (a=1 b=2 select=4 start=8 right=16 left=32 up=64 down=128). */
#ifndef DP_TAS_H
#define DP_TAS_H

#include <stddef.h>
#include <stdint.h>

typedef struct DpTas {
    uint8_t *masks;   // one byte per frame
    uint32_t frames;
    int anchored;     // 1 = the movie starts from a savestate (replay from
                      // power-on is best-effort, not frame-exact)
    char source[512]; // file path
    // the ROM the movie was made on, when the format records it: BizHawk
    // writes the SHA-1 (Header.txt), VBA the cartridge header checksum
    // ($014D) and global checksum ($014E-$014F)
    int has_sha1, has_cksum;
    uint8_t sha1[20];
    uint8_t hdr_ck;
    uint16_t global_ck;
    char game[96];       // the movie's game name (BizHawk GameName), if any
    // frame clock of the recording emulator: 1 = every frame lasts exactly
    // 70224 cycles from power-on (BizHawk Gambatte "EqualLengthFrames"),
    // 0 = a frame ends at vblank (VBA; m0boy's own frame clock)
    int equal_frames;
    // the recording emulator, when the format tells it: replaying a movie
    // needs that emulator's timing quirks (sessions.c sync profile)
    int recorder;
    // VBM header minor version ($30): 0 = VBA 1.7.2 based recorders (up to
    // VBA-rr v23.0), 1 = later (VBA-rr v23.1+, v24 "V8" core)
    int recorder_rev;
    // movie frames the DMG boot ROM ran before the cartridge's first
    // instruction (BizHawk Gambatte with the original boot ROM; the masks of
    // those frames are dropped at load, frame 0 is the first with the game)
    int boot_frames;
    // BizHawk header "Core Gambatte": the sync profile calibrated on libgambatte applies
    int gambatte;
} DpTas;

// Gambatte, original DMG boot ROM: $0100 is reached 4496 cycles into movie
// frame 334 (observed on libgambatte, both Mario Land 2 movies)
#define DP_TAS_GAMBATTE_DMG_BOOT_FRAMES 334

enum { DP_TAS_REC_UNKNOWN = 0, DP_TAS_REC_VBA, DP_TAS_REC_BIZHAWK };

// the movie's recorded ROM identity against this ROM: 1 match, 0 mismatch,
// -1 the movie records no identity.  `why` gets a human description.
int dp_tas_rom_match(const DpTas *t, const uint8_t *rom, size_t rom_size, char *why, size_t whysz);

// sniff and load any supported format; returns 1 on success, 0 with a human
// message in `err` otherwise
int dp_tas_load(const char *path, DpTas *out, char *err, size_t errsz);
void dp_tas_free(DpTas *t);

#endif
