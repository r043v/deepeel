/* tree.c: the common exploration tree.  See tree.h. */
#include "tree.h"

#include <stdlib.h>
#include <string.h>

void dp_tree_init(DpTree *t) {
    memset(t, 0, sizeof *t);
    t->cap = 256;
    t->v = malloc(sizeof(DpChoice) * t->cap);
    if (!t->v) t->cap = 0;
    // id 0 is the reserved root: the reset lineage every seed hangs from
    DpChoice root = {0, 0, 0, 0, DP_ISSUE_OPEN, 0, 0, 0x100};
    t->v[t->n++] = root;
    t->next_id = 1;
}

void dp_tree_free(DpTree *t) {
    free(t->v);
    memset(t, 0, sizeof *t);
}

uint32_t dp_tree_add(DpTree *t, uint32_t parent, int kind, uint32_t site, int bank, int pc) {
    if (!t->v) return 0;
    if (t->n == t->cap) {
        uint32_t nc = t->cap * 2;
        DpChoice *nv = realloc(t->v, sizeof(DpChoice) * nc);
        if (!nv) return 0;
        t->v = nv;
        t->cap = nc;
    }
    uint16_t depth = (uint16_t)(parent < t->n ? t->v[parent].depth + 1 : 1);
    DpChoice *c = &t->v[t->n];
    c->id = t->next_id++;
    c->parent = parent;
    c->depth = depth;
    c->kind = (uint8_t)kind;
    c->issue = DP_ISSUE_OPEN;
    c->site = site;
    c->bank = (uint16_t)bank;
    c->pc = (uint16_t)pc;
    t->open++;
    if (depth > t->max_depth) t->max_depth = depth;
    t->n++;
    return c->id;
}

void dp_tree_close(DpTree *t, uint32_t id, int issue) {
    if (id >= t->n) return;
    if (t->v[id].issue != DP_ISSUE_OPEN) return;
    t->v[id].issue = (uint8_t)issue;
    if (t->open) t->open--;
    if (issue > DP_ISSUE_OPEN && issue < DP_ISSUE_N) t->closed[issue]++;
}

const char *dp_issue_name(int issue) {
    static const char *names[DP_ISSUE_N] = {"open", "done", "merged", "dead", "reset",
                                            "budget", "capped", "nonsense", "spooled"};
    return issue >= 0 && issue < DP_ISSUE_N ? names[issue] : "?";
}

int dp_tree_save(const DpTree *t, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const char magic[4] = {'M', 'S', 'T', 'V'};
    const uint32_t ver = 1;
    fwrite(magic, 1, 4, f);
    fwrite(&ver, sizeof ver, 1, f);
    fwrite(&t->n, sizeof t->n, 1, f);
    fwrite(&t->next_id, sizeof t->next_id, 1, f);
    fwrite(&t->max_depth, sizeof t->max_depth, 1, f);
    fwrite(t->closed, sizeof t->closed, 1, f);
    fwrite(t->v, sizeof(DpChoice), t->n, f);
    fclose(f);
    return 1;
}

int dp_tree_load(DpTree *t, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    uint32_t ver, n, next_id, max_depth, closed[DP_ISSUE_N];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MSTV", 4) ||
        fread(&ver, sizeof ver, 1, f) != 1 || ver != 1 ||
        fread(&n, sizeof n, 1, f) != 1 || fread(&next_id, sizeof next_id, 1, f) != 1 ||
        fread(&max_depth, sizeof max_depth, 1, f) != 1 ||
        fread(closed, sizeof closed, 1, f) != 1) {
        fclose(f);
        return 0;
    }
    if (n == 0 || n > (1u << 24)) {
        fclose(f);
        return 0;
    }
    DpChoice *v = malloc(sizeof(DpChoice) * n);
    if (!v || fread(v, sizeof(DpChoice), n, f) != n) {
        free(v);
        fclose(f);
        return 0;
    }
    fclose(f);
    free(t->v);
    t->v = v;
    t->n = t->cap = n;
    t->next_id = next_id;
    t->max_depth = max_depth;
    memcpy(t->closed, closed, sizeof closed);
    t->open = 0;
    for (uint32_t i = 0; i < n; i++)
        if (t->v[i].issue == DP_ISSUE_OPEN) t->open++;
    return 1;
}

void dp_tree_stats(const DpTree *t, char *out, size_t outsz) {
    size_t o = 0;
    o += (size_t)snprintf(out + o, outsz - o, "tree: %u choices, depth %u, open %u",
                          t->n ? t->n - 1 : 0, t->max_depth, t->open);
    static const char *names[DP_ISSUE_N] = {"open", "done", "merged", "dead", "reset",
                                            "budget", "capped", "nonsense", "spooled"};
    for (int i = 1; i < DP_ISSUE_N; i++)
        if (t->closed[i])
            o += (size_t)snprintf(out + o, outsz - o, ", %s %u", names[i], t->closed[i]);
    snprintf(out + o, outsz - o, "\n");
}
