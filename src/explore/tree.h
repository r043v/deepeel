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
#ifndef MS_TREE_H
#define MS_TREE_H

#include <stdint.h>
#include <stdio.h>

enum {
    MS_ISSUE_OPEN = 0,
    MS_ISSUE_DONE,
    MS_ISSUE_MERGED,
    MS_ISSUE_DEAD,
    MS_ISSUE_RESET,
    MS_ISSUE_BUDGET,
    MS_ISSUE_CAPPED,
    MS_ISSUE_N,
};

typedef struct MsChoice {
    uint32_t id, parent;        // parent 0 = the root choice
    uint16_t depth;
    uint8_t kind;               // seed kind or MS_PATH_FORK
    uint8_t issue;              // MS_ISSUE_*
    uint32_t site;              // rom key of the branch site (0 = none)
    uint16_t bank, pc;          // where this choice's path starts
} MsChoice;

typedef struct MsTree {
    MsChoice *v;
    uint32_t n, cap;
    uint32_t max_depth;
    uint32_t closed[MS_ISSUE_N];
    uint32_t open;
    uint32_t next_id;
} MsTree;

void ms_tree_init(MsTree *t);
void ms_tree_free(MsTree *t);
// add a choice (open); parent 0 = root; returns its id
uint32_t ms_tree_add(MsTree *t, uint32_t parent, int kind, uint32_t site, int bank, int pc);
void ms_tree_close(MsTree *t, uint32_t id, int issue);
// tree.bin persistence (choices only; the frontier images ride in pool-style
// files of their own).  Returns 1 on success.
int ms_tree_save(const MsTree *t, const char *path);
int ms_tree_load(MsTree *t, const char *path);
const char *ms_issue_name(int issue);
// one line of stats for the report
void ms_tree_stats(const MsTree *t, char *out, size_t outsz);

#endif
