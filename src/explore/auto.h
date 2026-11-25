// auto.h: the autonomous coverage campaign (`deepeel ROM [OVERFLOW_DIR]`)
#ifndef DP_AUTO_H
#define DP_AUTO_H
#include <stddef.h>
// exe: this executable (children are `exe [platform] explore ...`); platform: gb, nes, pce, md, sms, gg, sg, msx, ngp or ws; overflow: optional disk for the bulk data; outdir: small outputs (PNG, reports)
int dp_auto_campaign(const char *exe, const char *platform, const char *rom, const char *overflow, size_t rom_size, const char *outdir, const char *name);
#endif
