// sha1.h: SHA-1 digest (FIPS 180-1), used to match a TAS movie to its ROM.
#ifndef DP_SHA1_H
#define DP_SHA1_H

#include <stddef.h>
#include <stdint.h>

void dp_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

#endif
