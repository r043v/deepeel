// stop.h: one cooperative stop flag for every platform explorer. SIGINT and the --time-limit
// alarm set it; the explorers poll it where they poll their step budget and still write
// their report. It lives in deepeel_core (never in a namespaced --jobs clone), so the
// primary and secondary instances see the same flag.
#ifndef DP_STOP_H
#define DP_STOP_H
#include <signal.h>
extern volatile sig_atomic_t dp_stop;
#endif
