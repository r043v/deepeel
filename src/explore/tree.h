/* tree.h: the common exploration tree.  Every path the fork explorer runs —
 * reset seed, vector seed, pointer-scan seed, session deposit, branch fork,
 * IRQ fork — is one choice linked to the choice whose path created it.  The
 * frontier (open choices) IS the explorer's queue: each queued state image
 * carries its choice id.  When a path ends, its choice closes with an issue:
 *  - done     ran to a natural stop with fresh discovery on the way
 *  - merged   ended in a known loop without new discovery (the gameover case:
 *             execution fell back onto already-mapped ground, no way out)
 *  - dead     an undefined opcode executed
 *  - reset    execution returned to $0100 (the game reset itself)
 *  - budget   the per-path step wall or the global budget closed it
 *  - capped   a loop the back-edge cap closed, or a site cap on forks
 * The global stop is "total mapping knowledge": empty frontier, every choice
 * closed, no unexplained byte left.
 */
#ifndef DP_TREE_H
#define DP_TREE_H

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

enum {
    DP_ISSUE_OPEN = 0,
    DP_ISSUE_DONE,
    DP_ISSUE_MERGED,
    DP_ISSUE_DEAD,
    DP_ISSUE_RESET,
    DP_ISSUE_DEFERRED,
    DP_ISSUE_CAPPED,
    DP_ISSUE_NONSENSE,
    DP_ISSUE_SPOOLED,
    DP_ISSUE_N,
};

typedef struct DpChoice {
    uint32_t id, parent;        // parent 0 = the root choice
    uint16_t depth;
    uint8_t kind;               // seed kind or DP_PATH_FORK
    uint8_t issue;              // DP_ISSUE_*
    uint32_t site;              // rom key of the branch site (0 = none)
    uint16_t bank, pc;          // where this choice's path starts
} DpChoice;

// Choices live in fixed chunks of 1<<DP_TREE_CHUNK_BITS entries. Every full chunk but the first is written to
// the spill file (when one is set) and its RAM freed; its still-open choices stay in the `pin` table. RAM holds
// the open frontier, never the whole tree. Ids are indexes (id == position); the tree.bin format is unchanged.
#define DP_TREE_CHUNK_BITS 16
typedef struct DpTree {
    DpChoice **ch;              // chunk pointers; NULL = evicted to the spill file
    DpChoice *pin;              // open choices of evicted chunks (hash by id, id 0 = empty slot)
    uint32_t pin_cap, pin_n;
    uint8_t *chev;              // 1 = evicted
    uint32_t nch, chcap;
    uint32_t n;
    uint32_t max_depth;
    uint32_t closed[DP_ISSUE_N];
    uint32_t open;
    uint32_t next_id;
    uint32_t spill_skip;        // chunks below this index have no slot in the spill file (a v2 load leaves them out)
    int spill_fd;
    char spill_path[560];
    pthread_mutex_t mtx;        // leaf lock: add/close may come from any worker
} DpTree;

void dp_tree_init(DpTree *t);
void dp_tree_free(DpTree *t);
// optional: closed chunks spill to this file (call right after init, before load); NULL = all in RAM
void dp_tree_set_spill(DpTree *t, const char *path);
// add a choice (open); parent 0 = root; returns its id
uint32_t dp_tree_add(DpTree *t, uint32_t parent, int kind, uint32_t site, int bank, int pc);
void dp_tree_close(DpTree *t, uint32_t id, int issue);
// tree.bin v2 keeps the open choices only (the closed ones are never read again: the counters ride in the
// header); v1 (every choice) still loads.  tree.bin persistence (choices only; the frontier images ride in pool-style
// files of their own).  Returns 1 on success.
int dp_tree_save(const DpTree *t, const char *path);
int dp_tree_load(DpTree *t, const char *path);
const char *dp_issue_name(int issue);
// one line of stats for the report
void dp_tree_stats(const DpTree *t, char *out, size_t outsz);

#endif
