// xpose.h: moves the coverage observed on another version of a game onto a target ROM, by content.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { DP_XPOSE_CODE = 1, DP_XPOSE_DATA = 2 };
// For every target byte whose content is found in `src`, and unknown in `out`, writes the class (DP_XPOSE_*) that
// `src_flags` (DP_CORE_* bits, one byte per source ROM byte) observed on it.  SM83 ROMs with 16 KiB banks.
void dp_xpose_gb(const uint8_t *src, size_t src_n, const uint8_t *src_flags, const uint8_t *dst, size_t dst_n, uint8_t *out);
