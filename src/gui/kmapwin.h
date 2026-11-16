// kmapwin.h: the live ROM map window (--mapwin), every platform: one pixel per ROM byte, one 16 KiB
// tile per bank/window, coloured from the live DpCoreResult flags (natural / forced / static / data / unknown).
#ifndef DP_KMAPWIN_H
#define DP_KMAPWIN_H
struct DpCoreResult;
int dp_kmapwin_start(const struct DpCoreResult *live, const char *title, int scale);   // 0 on success
void dp_kmapwin_stop(void);
#endif
