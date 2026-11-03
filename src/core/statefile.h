// statefile.h: exported machine state (--export-state): the deepest Go-Explore cell.
// Layout (little endian): "DPST", u32 version 1, char platform[8], u32 rom_crc32, u32 rom_size,
// u32 depth_frames, u32 state_bytes (uncompressed), u32 zlen, then zlen bytes of zlib data.
// It is deepeel's own machine blob (the recompiled m0boy machine of that platform and version);
// no third-party emulator reads it.
#ifndef DP_STATEFILE_H
#define DP_STATEFILE_H
#include <stddef.h>
#include <stdint.h>
int dp_state_export(const char *path, const char *platform, const uint8_t *rom, size_t rom_size,
                    uint32_t depth, const uint8_t *z, uint32_t zlen, uint32_t state_bytes);
#endif
