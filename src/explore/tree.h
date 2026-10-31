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

enum {
    DP_ISSUE_OPEN = 0,
    DP_ISSUE_DONE,
    DP_ISSUE_MERGED,
    DP_ISSUE_DEAD,
    DP_ISSUE_RESET,
    DP_ISSUE_BUDGET,
    DP_ISSUE_CAPPED,
    DP_ISSUE_NONSENSE,
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

typedef struct DpTree {
    DpChoice *v;
    uint32_t n, cap;
    uint32_t max_depth;
    uint32_t closed[DP_ISSUE_N];
    uint32_t open;
    uint32_t next_id;
} DpTree;

void dp_tree_init(DpTree *t);
void dp_tree_free(DpTree *t);
// add a choice (open); parent 0 = root; returns its id
uint32_t dp_tree_add(DpTree *t, uint32_t parent, int kind, uint32_t site, int bank, int pc);
void dp_tree_close(DpTree *t, uint32_t id, int issue);
// tree.bin persistence (choices only; the frontier images ride in pool-style
// files of their own).  Returns 1 on success.
int dp_tree_save(const DpTree *t, const char *path);
int dp_tree_load(DpTree *t, const char *path);
const char *dp_issue_name(int issue);
// one line of stats for the report
void dp_tree_stats(const DpTree *t, char *out, size_t outsz);

#endif
