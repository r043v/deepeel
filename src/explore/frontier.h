// frontier.h: the platform-neutral policy of the deferred frontier (spec §6):
// why a continuation waits, which tier it resumes in, when a loss is final.
// The queues and the disk lots stay with each engine; the decisions are here.
#ifndef DP_FRONTIER_H
#define DP_FRONTIER_H

#include <stdint.h>

#define DP_TIER_MAX 3        // last budget tier (4^3 x); beyond it: counted, kept off
#define DP_DEFER_CAP 4096    // deferred snapshots held per explorer

// why a continuation was deferred (docs/COUVERTURE.fr.md §6.4); the numeric
// order is the resume order, cheapest first.  COVERED is the last tier and is
// resumed only by an explicit `frontier drain --tier inf`.
enum { DP_R_NONE = 0, DP_R_PRIORITY, DP_R_DEDUP_FULL, DP_R_PATHS, DP_R_FORK_CAP, DP_R_STEPS,
       DP_R_LOOP, DP_R_WALL, DP_R_COVERED, DP_R_COUNT };

const char *dp_reason_name(int r);
int dp_reason_from_name(const char *s);        // 0 when unknown
int dp_reason_eligible(int reason, int all);   // `all`: drain --tier inf

// verdict of one deferral request
enum { DP_ROOM_OK = 0, DP_ROOM_TIER_FINAL, DP_ROOM_FULL };
// tier: tier of the running path; held: states in the holding area;
// held_reason: states held for this reason
int dp_frontier_room(int tier, uint32_t held, uint32_t held_reason, int reason);

#endif
