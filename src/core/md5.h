// md5.h: MD5 digest, used only to match BizHawk movies (Genesis headers record an MD5).
#ifndef DP_MD5_H
#define DP_MD5_H
#include <stddef.h>
#include <stdint.h>
void dp_md5(const uint8_t *data, size_t len, uint8_t out[16]);
#endif
