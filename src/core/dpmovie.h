#ifndef DP_MOVIE_H
#define DP_MOVIE_H
#include <stddef.h>
#include <stdint.h>
/* C6/E12: one movie type for the kernel. Every platform loader (NES FM2/BK2, PCE, Mega Drive
 * GMV/BK2) yields per-frame pad bytes in its own layout; dp_movie_load converts them once to
 * the kernel's DpInput masks (core/dpmachine.h) so the explorer sees a single representation. */
typedef struct { uint32_t *masks; uint32_t frames; } DpMovie;
/* plat: "nes", "pce" or "md". Returns 1 on success; on failure err holds the reason. */
int dp_movie_load(const char *plat, const char *path, DpMovie *out, char *err, size_t errsz);
void dp_movie_free(DpMovie *m);
/* The per-platform conversions, exposed for tests. */
uint32_t dp_movie_nes_pad(uint8_t b);
uint32_t dp_movie_pce_pad(uint8_t b);
uint32_t dp_movie_md_pad(uint8_t b);
#endif
