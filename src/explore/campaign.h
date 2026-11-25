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
#define CAMP_SNAP_EVERY        300      /* movie frames between two machine snapshots (5 s of game time) */
#define CAMP_MOVIE_STALL       36000    /* movie frames without a new ROM byte = stagnation (10 min of game time: a game may walk known parts for long) */

/* ---- random play from each snapshot and from power-on ---- */
#define CAMP_RANDOM_RUNS       12       /* seeded random-pad runs per snapshot */
#define CAMP_RANDOM_NO_START   8        /* of which this many (the first ones) never press Start; the others use every button */
#define CAMP_RANDOM_NO_SELECT  6        /* ... and among those, this many never press Select either: 6 without Start and Select, 2 without Start, 4 full */
#define CAMP_RANDOM_MIN        3600     /* a random run lasts at least this long (1 min of game time) */
#define CAMP_RANDOM_STALL      600      /* then ends after this many frames without a ROM byte new to the campaign (10 s) */
#define CAMP_RANDOM_HOLD       32       /* one press in this many is held 0.5 to 4 s; the others last 8 frames (long presses: about a third of the time) */
#define CAMP_RANDOM_PASSES     5        /* a snapshot whose random runs found new bytes is played again, other seeds, up to this many passes (64 seed ranks) */
#define CAMP_RANDOM_AGAIN      64       /* ... if the pass found at least this many bytes new to the campaign; pass n runs n times CAMP_RANDOM_MIN at least */
#define CAMP_XPOSE_SECS        10       /* coverage of the other ROM versions moved onto ours this often (seconds) */
#define CAMP_RANDOM_PARALLEL   4        /* random children of one movie at a time: the four quarters of its tile */
#define CAMP_BOOT_RANDOM_TASKS 4        /* power-on random children (each its own seeds) */

/* ---- fork exploration (both sides of every conditional) ---- */
#define CAMP_FORK_RESERVE      2        /* workers the random runs leave to the forks */
#define CAMP_FORK_SHARE        4        /* while films run, the forks hold at most 1/4 of the workers */
#define CAMP_EXPLORE_STALL_SECS 1800    /* wall-clock seconds without any new byte before a fork exploration ends */
#define CAMP_EXPLORE_MAX_SECS  900      /* wall-clock cap of one fork exploration (0: none) */

/* ---- attract mode (no input) ---- */
#define CAMP_ATTRACT_MAX       216000   /* hard cap, frames (1 h of game time) */
#define CAMP_ATTRACT_MIN       36000    /* from power-on: at least this long before a stagnation stop (10 min) */
#define CAMP_ATTRACT_STALL     3600     /* frames without a new ROM byte ends it (1 min) */
#define CAMP_POST_ATTRACT_MIN  10800    /* from the end state of each movie: at least 3 min */
#define CAMP_POST_ATTRACT_STALL 3600

/* ---- machine load ---- */
#define CAMP_RAM_RESERVE_DIV   4        /* memory left to the machine: 1/4 of the RAM ... */
#define CAMP_RAM_RESERVE_MIN   (3072ull << 20)   /* ... and this much at least; under half of it the newest child is killed and re-queued */
#define CAMP_NICE              10       /* priority of every child: the desktop stays responsive */
#define CAMP_IO_PAUSE          35       /* /proc/pressure/io "full avg10" above this: one more fork is stopped (SIGSTOP) */
#define CAMP_IO_RESUME         15       /* below this: one stopped fork resumes */
#define CAMP_LIGHT_RSS         (256ull << 20)   /* first estimate of an observation child, then its measured peak */
#define CAMP_HEAVY_RSS         (600ull << 20)   /* same, fork exploration */
#define CAMP_STOP_SECS         30       /* a stop waits this long for the children, then kills them */

/* ---- queues and disk ---- */
#define CAMP_QUEUE_EXPLORE     1500
#define CAMP_QUEUE_BYTES       (384ull << 20)   /* RAM held by an exploration's queued states: the count follows the state size (GB CGB: 680 KiB) */
#define CAMP_QUEUE_MIN         64
#define CAMP_RSS_RETRIES       2        /* a child killed over its RSS cap is re-queued this many times, its queue halved each time */
#define CAMP_SNAPSHOT_TASKS    131072   /* snapshot tasks the supervisor can hold (a random and a fork per snapshot) */
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

#define CAMP_SKIP_EMULATOR "VBA-rr"   // movies of this emulator are not replayed for now: they all desync (name from the TASVideos API)
#endif
