// frontier.c: see frontier.h.
#include <string.h>

#include "frontier.h"

const char *dp_reason_name(int r) {
    static const char *n[DP_R_COUNT] = { "none", "priority", "dedup-full", "paths", "fork-cap",
                                         "steps", "loop", "wall", "covered" };
    return r >= 0 && r < DP_R_COUNT ? n[r] : "?";
}

int dp_reason_from_name(const char *s) {
    for (int r = 1; r < DP_R_COUNT; r++) if (!strcmp(s, dp_reason_name(r))) return r;
    return 0;
}

int dp_reason_eligible(int reason, int all) { return reason < DP_R_COVERED || all; }

// Each deferral raises the tier by one (the resumed path gets a bigger budget);
// past DP_TIER_MAX, or when the holding area is full, the loss stays final and
// the caller counts it per reason — never silent.  The noisy reasons (fork-cap,
// covered) hold at most a quarter of the area each so they cannot starve the rest.
int dp_frontier_room(int tier, uint32_t held, uint32_t held_reason, int reason) {
    if (tier >= DP_TIER_MAX) return DP_ROOM_TIER_FINAL;
    uint32_t lim = (reason == DP_R_FORK_CAP || reason == DP_R_COVERED) ? DP_DEFER_CAP / 4 : DP_DEFER_CAP;
    if (held >= DP_DEFER_CAP || held_reason >= lim) return DP_ROOM_FULL;
    return DP_ROOM_OK;
}
