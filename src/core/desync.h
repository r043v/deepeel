// desync.h: generic per-frame machine-state exchange ("DSY1") and the desync finder that compares two streams
// (deepeel/m0boy vs BizHawk or any other emulator) while keeping the full state of the last N frames.
#ifndef DP_DESYNC_H
#define DP_DESYNC_H
#include <stdint.h>
#include <stdio.h>

// File layout: "DSY1", then per frame: u32 frame, u16 nblocks, per block: u8 namelen, name, u32 len, bytes.
// Blocks are named memory areas or register images ("wram", "vram", "regs", "oam", ...): a producer emits the same
// names on both sides; a name present on one side only is reported, not compared.
typedef struct DpDsyWriter { FILE *f; } DpDsyWriter;
int dp_dsy_open(DpDsyWriter *w, const char *path);
int dp_dsy_frame(DpDsyWriter *w, uint32_t frame, unsigned n, const char *const *names, const uint8_t *const *data, const uint32_t *len);
void dp_dsy_close(DpDsyWriter *w);

typedef struct DpDsyIgnore { char block[24]; uint32_t lo, hi; } DpDsyIgnore;   // byte range left out of the comparison
typedef struct DpDsyOptions {
    unsigned window;            // frames of full state kept on both sides (default 8)
    int offset_a, offset_b;     // frames skipped at the start of each stream (alignment)
    unsigned show;              // differing bytes listed per block (default 32)
    const DpDsyIgnore *ignore; unsigned nignore;
    const char *ring_dir;       // when set, the window's frames of both sides are written there at the divergence
} DpDsyOptions;
// returns 0 identical (frames compared in *compared), 1 diverged (report printed to `out`), -1 error
int dp_dsy_compare(const char *path_a, const char *path_b, const DpDsyOptions *o, FILE *out, uint32_t *compared);
#endif
