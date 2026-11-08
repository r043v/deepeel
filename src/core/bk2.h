/* BizHawk .bk2 container — shared kernel module (§7.4 of docs/COUVERTURE.fr.md).
 * A .bk2 is a zip holding a Header.txt and an Input Log.txt.  Every platform
 * loader (GB vbm/vmv aside: NES, SG/SMS/GG/MSX, PCE, MD) used to duplicate the
 * same opening steps: zip detection, header extraction, SHA1/MD5 identity line
 * parsing and input-log entry lookup ("input log", "log", any *.txt, any
 * entry).  The per-platform frame grammar (the '|' columns, fm2 text, GMV
 * bytes) stays in each loader: this module stops at the container. */
#ifndef DP_BK2_H
#define DP_BK2_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *header; size_t header_n;  /* Header.txt bytes, may be NULL; caller frees via dp_bk2_free */
    uint8_t *log; size_t log_n;        /* Input Log bytes (grammar is per platform) */
    int has_sha1; uint8_t sha1[20];    /* Header line "SHA1 <40 hex>" */
    int has_md5; uint8_t md5[16];      /* Header line "MD5 <32 hex>" (MD movies) */
    int anchored;                      /* Header: StartsFromSavestate true */
} DpBk2;

/* 1 on success (out->log non-NULL); 0 with err filled otherwise.  A non-zip
 * input is an error: plain-text movies are the loaders' own formats, checked
 * before calling. */
int dp_bk2_open(const uint8_t *d, size_t n, DpBk2 *out, char *err, size_t errsz);
void dp_bk2_free(DpBk2 *b);
#endif
