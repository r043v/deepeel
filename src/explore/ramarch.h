#ifndef DP_RAMARCH_H
#define DP_RAMARCH_H
// Platform-neutral archive of the code executed from RAM: one entry per distinct
// (pc, bytes) variant, so code generated at run time is kept even when no ROM
// source can be attached; the same pc with other bytes counts as polymorphic.
#include <stdint.h>
#include <stdio.h>
typedef struct { uint64_t key; uint32_t hits; int32_t src, prod; uint16_t pc; uint8_t len, b[10]; } DpRamEntry;
typedef struct { DpRamEntry *e; uint32_t cap, n, poly, attached, derived; } DpRamArch;
// src: ROM index the bytes were copied from, -1 when unknown
void dp_ramarch_add(DpRamArch *a, uint16_t pc, const uint8_t *b, int len, int32_t src);
// prod: approximate producer (last ROM cell read before the byte was stored), -1 when none; only
// meaningful when src is -1 (generated code)
void dp_ramarch_add_p(DpRamArch *a, uint16_t pc, const uint8_t *b, int len, int32_t src, int32_t prod);
void dp_ramarch_merge(DpRamArch *dst, const DpRamArch *src);
void dp_ramarch_write(const DpRamArch *a, FILE *out, const char *platform);
void dp_ramarch_free(DpRamArch *a);
#endif
