/* Mega Drive cartridge: ROM image, header ($100-$1FF) and vector table. */
#ifndef DP_CART_MD_H
#define DP_CART_MD_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *rom;   /* flat image, deinterleaved when it was .smd */
    size_t size;
    char console[17], domestic[49], overseas[49], serial[15], region[4];
    uint16_t checksum;     /* header value */
    uint16_t sum_computed; /* sum of words from $200 to end */
    int has_sram;
    uint32_t sram_lo, sram_hi;
    uint32_t rom_end;      /* header ROM end (inclusive) */
    uint32_t vec[64];      /* $0..$FF as longs: SSP, reset, ... */
} DpMdCart;

/* 1 if the image looks like a Mega Drive ROM ("SEGA" at $100). */
int dp_md_detect(const uint8_t *buf, size_t size);
int dp_md_cart_load(DpMdCart *c, const char *path, char *err, size_t errsz);
int dp_md_cart_from_mem(DpMdCart *c, const uint8_t *buf, size_t size, char *err, size_t errsz);
void dp_md_cart_free(DpMdCart *c);
#endif
