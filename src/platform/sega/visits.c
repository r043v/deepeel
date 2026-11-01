#include "visits.h"
#include <stdlib.h>
#include <string.h>
static size_t bucket(uint32_t key,uint16_t pc,size_t capacity) {
    uint32_t hash=key*UINT32_C(2654435761) ^ (uint32_t)pc*UINT32_C(2246822519);
    hash^=hash>>16;
    return hash&(capacity-1);
}
static DpSegaVisit *find(DpSegaVisit *slots,size_t capacity,uint32_t key,uint16_t pc) {
    size_t at=bucket(key,pc,capacity);
    // Occupancy stays at or below one half; the finite bound is defensive.
    for(size_t n=0;n<capacity;n++,at=(at+1)&(capacity-1))
        if(!slots[at].key || (slots[at].key==key && slots[at].pc==pc)) return slots+at;
    return NULL;
}
static int grow(DpSegaVisits *v) {
    size_t capacity=v->capacity?v->capacity*2:256;
    if(capacity<v->capacity || capacity>SIZE_MAX/sizeof(*v->slots)) return 0;
    DpSegaVisit *next=calloc(capacity,sizeof(*next)); if(!next) return 0;
    for(size_t i=0;i<v->capacity;i++) if(v->slots[i].key) {
        DpSegaVisit *slot=find(next,capacity,v->slots[i].key,v->slots[i].pc);
        if(!slot) { free(next); return 0; }
        *slot=v->slots[i];
    }
    free(v->slots); v->slots=next; v->capacity=capacity; v->growths++; return 1;
}
void dp_sega_visits_reset(DpSegaVisits *v) {
    if(v->slots) memset(v->slots,0,v->capacity*sizeof(*v->slots));
    v->used=0;
}
void dp_sega_visits_free(DpSegaVisits *v) { free(v->slots); memset(v,0,sizeof(*v)); }
int dp_sega_visits_enter(DpSegaVisits *v,uint32_t key,uint16_t pc,uint32_t cap) {
    // Zero is reserved for an empty bucket. Exploration keys fit below 2^23.
    if(key==UINT32_MAX) return -1;
    key++;
    if(!v->capacity && !grow(v)) return -1;
    DpSegaVisit *slot=find(v->slots,v->capacity,key,pc); if(!slot) return -1;
    if(!slot->key) {
        if(v->used==v->capacity/2) {
            if(!grow(v)) return -1;
            slot=find(v->slots,v->capacity,key,pc); if(!slot) return -1;
        }
        slot->key=key; slot->pc=pc; v->used++;
        if(v->used>v->peak) v->peak=v->used;
    }
    if(slot->count>=cap) return 0;
    slot->count++; return 1;
}
