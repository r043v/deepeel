#ifndef DP_MD_MOVIE_H
#define DP_MD_MOVIE_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *pads; uint32_t frames; int has_md5; uint8_t md5[16]; } DpMdMovie;
/* Loads a Gens GMV (no ROM identity) or a BizHawk BK2 (Genesis header records the ROM MD5).
 * GMV, optionally wrapped in a ZIP, , optionally wrapped in a ZIP, for a 3-button NTSC pad. */
int dp_md_movie_load(const char *path, DpMdMovie *out, char *err, size_t errsz);
void dp_md_movie_free(DpMdMovie *movie);
#endif
