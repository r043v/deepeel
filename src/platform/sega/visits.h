#ifndef DP_SEGA_VISITS_H
#define DP_SEGA_VISITS_H
#include <stddef.h>
#include <stdint.h>
// Per-path loop counts. Keep CPU address as well as the compact physical ROM
// index: bank switches split counters, while aliases remain separate addresses.
typedef struct { uint32_t key,count; uint16_t pc; } DpSegaVisit;
typedef struct {
    DpSegaVisit *slots;
    size_t capacity,used,peak;
    uint64_t growths;
} DpSegaVisits;
void dp_sega_visits_reset(DpSegaVisits *visits);
void dp_sega_visits_free(DpSegaVisits *visits);
// key is physical ROM index, or ROM size + CPU address outside ROM.
// Return 1 when admitted, 0 at the visit cap, -1 on allocation failure.
int dp_sega_visits_enter(DpSegaVisits *visits,uint32_t key,uint16_t pc,uint32_t cap);
#endif
