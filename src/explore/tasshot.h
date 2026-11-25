// tasshot.h: TAS replay summary picture (see tasshot.c). The kernel calls these through weak symbols.
#ifndef DP_TASSHOT_H
#define DP_TASSHOT_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    const char *name, *emu, *profile, *rom;   // movie file, source emulator (TASVideos API), compatibility profile applied, ROM name
    uint32_t frames; uint64_t end; int stalled; uint64_t stall_frame, new_bytes;
    const uint8_t *flags; size_t rom_size;   // this replay's own ROM coverage (kernel flag bytes, one per compact ROM index)
} DpTasSummary;
void dp_tasshot_capture(uint64_t frame, const char *tag, const uint32_t *argb, int w, int h);
int dp_tasshot_finish(const char *png, const DpTasSummary *s);
// TASVideos API lookup of the source emulator (this executable's own `tasdb emu ROM FRAMES`); out gets "" when unresolved
void dp_tasshot_emu(const char *rom_path, uint32_t frames, const char *movie_path, char *out, size_t n);   // movie_path: its directory NNNN[M|S] names the TASVideos id
#endif
