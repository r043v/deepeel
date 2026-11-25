/* campaign.h - every tunable of the autonomous campaign (src/explore/auto.c), in one place.
 * Frame counts are machine frames (the generic DpMachine.frame counter: 60 per second of game time on the
 * handhelds and consoles supported so far); times are wall-clock seconds; sizes are bytes.
 * Rule: nothing here is platform-specific - a platform that needs another value overrides it through
 * DpPlatform, never by a special case in the campaign code. */
#ifndef DEEPEEL_CAMPAIGN_H
#define DEEPEEL_CAMPAIGN_H

#define CAMP_STR_(x) #x
#define CAMP_STR(x) CAMP_STR_(x)

/* ---- movies (TAS replay) ---- */
#define CAMP_SNAP_EVERY        600      /* movie frames between two machine snapshots (10 s of game time) */
#define CAMP_MOVIE_STALL       36000    /* movie frames without a new ROM byte = stagnation (10 min of game time: a game may walk known parts for long) */

/* ---- random play from each snapshot ---- */
#define CAMP_RANDOM_RUNS       10       /* seeded random-pad runs per snapshot */
#define CAMP_RANDOM_MAX        7200     /* hard cap of one random run (2 min of game time) */
#define CAMP_RANDOM_STALL      3600     /* a random run ends after this many frames without a ROM byte new to the campaign (1 min) */
#define CAMP_EXPLORE_STALL_SECS 1800    /* wall-clock seconds without any new byte before a whole exploration child ends */
#define CAMP_EXPLORE_MAX_SECS  900      /* wall-clock cap of one fork exploration (0: none): forks are the long part, rerun later on demand */

/* ---- attract mode (no input) ---- */
#define CAMP_ATTRACT_MAX       216000   /* from power-on: hard cap, frames (1 h of game time) */
#define CAMP_ATTRACT_MIN       36000    /* runs at least this long before a stagnation stop (10 min) */
#define CAMP_ATTRACT_STALL     3600     /* frames without a new ROM byte ends it (1 min) */
#define CAMP_POST_ATTRACT_MAX  36000    /* attract from the end state of each movie, frames (10 min) */
#define CAMP_POST_ATTRACT_MIN  3600     /* it runs at least this long before a stagnation stop */
#define CAMP_POST_ATTRACT_STALL 3600

/* ---- queues and disk ---- */
#define CAMP_QUEUE_MOVIE       1000     /* forked states held in RAM by a movie child (the rest spills to its disk spool) */
#define CAMP_QUEUE_EXPLORE     1500
#define CAMP_QUEUE_BYTES       (384ull << 20)   /* RAM held by an exploration's queued states: the count follows the state size (GB CGB: 680 KiB) */
#define CAMP_QUEUE_MIN         64
#define CAMP_RSS_RETRIES       2        /* a child killed over its RSS cap is re-queued this many times, its queue halved each time */
#define CAMP_SNAPSHOT_TASKS    65536    /* snapshot explorations the supervisor can hold */
#define CAMP_CHILD_RSS_CAP     (1ull << 30)   /* a child above this is killed, its last live coverage is kept */
#define CAMP_LOCAL_MIN_FREE    (8ull << 30)   /* under this the bulk data goes to the overflow disk */
#define CAMP_SHED_FREE         (14ull << 30)  /* under this the child holding the biggest spool is stopped */
#define CAMP_HARD_MIN_FREE     (1ull << 30)   /* under this nothing more is written: clean stop */
#define CAMP_SPOOL_MIN         (2ull << 30)   /* smallest disk spool budget of a child */

/* ---- live window and reports ---- */
#define CAMP_LOG_SECS          15       /* progress line period on stderr */
#define CAMP_PNG_SECS          120      /* coverage PNG refresh */
#define CAMP_WINDOW_MS         33       /* live window refresh period: movie tiles follow their screens at this rate */
#define CAMP_LIVE_FRAME_MS     33       /* a movie child publishes its screen this often (in /dev/shm: memory only, no disk traffic) */
#define CAMP_BLOCK_BYTES       256      /* ROM bytes shown by one square of the live map */

#endif
