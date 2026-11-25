// stack.h: the campaign's single work stack.  Films come first (they make the snapshots); every other piece of work is
// a (snapshot, technique) item in one list, ranked by the new ROM bytes expected per CPU second - learned from the
// items already played - with a floor that keeps every technique and every part of the game moving.  The stack is
// written to disk: a campaign stopped and run again goes on where it was.
#ifndef DP_STACK_H
#define DP_STACK_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
    int kind;                      // K_*
    int movie;                     // the group: index in the movies array, nm for the power-on group
    unsigned frame, first;         // snapshot frame; random and splice: seed rank of the first run
    int slot, paused, collected;   // display slot while running; K_FORK: stopped under IO pressure; films: snapshots all queued
    char snap[1100];               // the snapshot file (empty: power-on)
    unsigned key;                  // coarse-to-fine rank
    pid_t pid; time_t t0; size_t rss_peak;
    char dir[1100], live[1150], log[1150], rep[1150], shot[1150], lfr[1150];
    int shrink;                    // times it was killed over the RSS cap and re-queued with half the queue
    uint64_t fps_frame; double fps_t; unsigned fps;   // live window: emulated frames per second of this child
    int done; time_t over_t;       // when the RSS ceiling was first hit (SIGINT, then SIGKILL after 15 s)
    int cut;                       // ended by the campaign's stop: saved as pending
    char ph[12]; unsigned rnd_done, rnd_total; size_t queued;   // from <lfr>.st: current phase, random runs, states queued in the child
    int stalled;                   // movies: the replay ended on stagnation
    size_t gain, mark;             // bytes this child added to its coverage; films: the gain already credited to snapshots
    unsigned pass; size_t score;   // times this item was played before; what made the snapshot worth playing (bytes found there)
    unsigned depth;                // snapshots between this one and a movie or power-on (0: taken from a film)
} Task;
enum { K_MOVIE = 1, K_FORK = 2, K_BOOT = 3, K_RANDOM = 4, K_SPLICE = 5, K_KINDS = 6 };
static inline int is_film(const Task *t) { return t->kind == K_MOVIE || t->kind == K_BOOT; }
static inline int is_pads(const Task *t) { return t->kind == K_RANDOM || t->kind == K_SPLICE; }   // pad-driven runs from a snapshot
static inline int running_task(const Task *t) { return t->pid && !t->done; }

typedef struct { double bytes, secs; unsigned n; } StackStat;
#define STACK_TECHS 3              /* random pads, movie splices, forks */
#define STACK_BUCKETS 17           /* order of magnitude of the score */
#define STACK_REGIONS 512          /* neighbourhoods of a group: CAMP_REGION_FRAMES each */
typedef struct { double got, expected; } StackRegion;

typedef struct {
    Task *tk; size_t nt, max;
    size_t ng;                     // groups: the movies, then power-on
    const char *const *name;       // group -> stable name (movie path, "power-on")
    const uint8_t *sibling;        // group -> recorded on another ROM version
    unsigned *gsnap;               // group -> last film snapshot frame queued
    StackStat stat[STACK_TECHS][2][STACK_BUCKETS];   // [technique][replayed][bucket]
    StackRegion *region;           // ng x STACK_REGIONS
    unsigned long long next_id, launches, picked[STACK_TECHS];
    unsigned long long played[STACK_TECHS], dropped;   // items finished (since the first run) and items the full stack refused
} Stack;

int stack_init(Stack *s, size_t max, size_t ng, const char *const *name, const uint8_t *sibling);
void stack_free(Stack *s);
Task *stack_add(Stack *s, int kind, int group);
// forget the finished items (films stay, as done): call when no Task pointer is held
void stack_compact(Stack *s);
// the three techniques on one snapshot (film_group: a movie exists to splice from)
void stack_add_snapshot(Stack *s, int group, const char *snap, unsigned frame, unsigned key, size_t score, unsigned depth, int splice, int forks);
// the next item to start, NULL if nothing may start now
Task *stack_next(Stack *s, int forks_ok, long fork_cap, long film_cap);
void stack_launched(Stack *s, const Task *t);
// a child ended on its own: teach the model and, if it found enough, queue it again with other seeds
void stack_result(Stack *s, Task *t, double cpu_secs, size_t boot_tasks);
void stack_requeue(Stack *s, const Task *o, int shrink);
// expected new bytes per CPU second of a technique over what was measured so far (for the log)
double stack_rate(const Stack *s, int tech);
int stack_save(const Stack *s, const char *path);        // atomic
size_t stack_load(Stack *s, const char *path);           // items restored, 0 if there is no usable file
#endif
