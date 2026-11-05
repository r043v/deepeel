/* tree.c: the common exploration tree.  See tree.h. */
#include "tree.h"

#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#define CHB DP_TREE_CHUNK_BITS
#define CHN (1u << CHB)
#define CHM (CHN - 1u)
#define CHBYTES ((size_t)CHN * sizeof(DpChoice))

static int tree_grow(DpTree *t) {
    if (t->nch == t->chcap) {
        uint32_t nc = t->chcap ? t->chcap * 2 : 16;
        DpChoice **a = realloc(t->ch, sizeof *a * nc);
        if (!a) return 0;
        t->ch = a;
        uint32_t *o = realloc(t->chopen, sizeof *o * nc);
        if (!o) return 0;
        t->chopen = o;
        uint8_t *e = realloc(t->chev, nc);
        if (!e) return 0;
        t->chev = e;
        t->chcap = nc;
    }
    DpChoice *blk = malloc(CHBYTES);
    if (!blk) return 0;
    t->ch[t->nch] = blk;
    t->chopen[t->nch] = 0;
    t->chev[t->nch] = 0;
    t->nch++;
    return 1;
}

// write chunk c (full, not the growing last one) to the spill file and free it; on any failure it stays in RAM
static void tree_evict(DpTree *t, uint32_t c) {
    if (t->spill_fd < 0 || !t->ch[c] || t->chopen[c] || c + 1 >= t->nch) return;
    const uint8_t *p = (const uint8_t *)t->ch[c];
    size_t left = CHBYTES, done = 0;
    while (left) {
        ssize_t w = pwrite(t->spill_fd, p + done, left, (off_t)(c * CHBYTES + done));
        if (w <= 0) return;
        done += (size_t)w;
        left -= (size_t)w;
    }
    free(t->ch[c]);
    t->ch[c] = NULL;
    t->chev[c] = 1;
}

static int tree_read(const DpTree *t, uint32_t id, DpChoice *out) {
    uint32_t c = id >> CHB;
    if (id >= t->n || c >= t->nch) return 0;
    if (t->ch[c]) { *out = t->ch[c][id & CHM]; return 1; }
    return t->spill_fd >= 0 && pread(t->spill_fd, out, sizeof *out, (off_t)id * (off_t)sizeof *out) == (ssize_t)sizeof *out;
}

static void tree_boot(DpTree *t) {
    // id 0 is the reserved root: the reset lineage every seed hangs from
    if (!tree_grow(t)) return;
    DpChoice root = {0, 0, 0, 0, DP_ISSUE_OPEN, 0, 0, 0x100};
    t->ch[0][0] = root;
    t->chopen[0] = 1;
    t->n = 1;
    t->next_id = 1;
}

static void tree_drop(DpTree *t) {
    for (uint32_t c = 0; c < t->nch; c++) free(t->ch[c]);
    free(t->ch);
    free(t->chopen);
    free(t->chev);
    t->ch = NULL;
    t->chopen = NULL;
    t->chev = NULL;
    t->nch = t->chcap = t->n = t->open = t->next_id = t->max_depth = 0;
    memset(t->closed, 0, sizeof t->closed);
}

void dp_tree_init(DpTree *t) {
    memset(t, 0, sizeof *t);
    t->spill_fd = -1;
    pthread_mutex_init(&t->mtx, NULL);
    tree_boot(t);
}

void dp_tree_set_spill(DpTree *t, const char *path) {
    if (t->spill_fd >= 0 || !path) return;
    snprintf(t->spill_path, sizeof t->spill_path, "%s", path);
    t->spill_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
}

void dp_tree_free(DpTree *t) {
    tree_drop(t);
    if (t->spill_fd >= 0) {
        close(t->spill_fd);
        unlink(t->spill_path);
    }
    pthread_mutex_destroy(&t->mtx);
    memset(t, 0, sizeof *t);
    t->spill_fd = -1;
}

uint32_t dp_tree_add(DpTree *t, uint32_t parent, int kind, uint32_t site, int bank, int pc) {
    if (!t->ch) return 0;
    pthread_mutex_lock(&t->mtx);
    if ((t->n >> CHB) == t->nch) {
        if (!tree_grow(t)) { pthread_mutex_unlock(&t->mtx); return 0; }
        for (uint32_t c = 0; c + 1 < t->nch; c++) tree_evict(t, c);   // closed chunks leave RAM
    }
    DpChoice pc_;
    uint16_t depth = (uint16_t)(tree_read(t, parent, &pc_) ? pc_.depth + 1 : 1);
    uint32_t c = t->n >> CHB;
    DpChoice *e = &t->ch[c][t->n & CHM];
    e->id = t->next_id++;
    e->parent = parent;
    e->depth = depth;
    e->kind = (uint8_t)kind;
    e->issue = DP_ISSUE_OPEN;
    e->site = site;
    e->bank = (uint16_t)bank;
    e->pc = (uint16_t)pc;
    t->open++;
    t->chopen[c]++;
    if (depth > t->max_depth) t->max_depth = depth;
    t->n++;
    uint32_t id = e->id;
    pthread_mutex_unlock(&t->mtx);
    return id;
}

void dp_tree_close(DpTree *t, uint32_t id, int issue) {
    pthread_mutex_lock(&t->mtx);
    uint32_t c = id >> CHB;
    if (id < t->n && t->ch[c]) {            // an evicted chunk holds closed choices only
        DpChoice *e = &t->ch[c][id & CHM];
        if (e->issue == DP_ISSUE_OPEN) {
            e->issue = (uint8_t)issue;
            if (t->open) t->open--;
            if (t->chopen[c]) t->chopen[c]--;
            if (issue > DP_ISSUE_OPEN && issue < DP_ISSUE_N) t->closed[issue]++;
            if (!t->chopen[c]) tree_evict(t, c);
        }
    }
    pthread_mutex_unlock(&t->mtx);
}

const char *dp_issue_name(int issue) {
    static const char *names[DP_ISSUE_N] = {"open", "done", "merged", "dead", "reset",
                                            "deferred", "capped", "nonsense", "spooled"};
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
    DpChoice *tmp = NULL;
    int ok = 1;
    for (uint32_t c = 0; c < t->nch && ok; c++) {
        size_t cnt = (size_t)(t->n - (c << CHB));
        if (cnt > CHN) cnt = CHN;
        const DpChoice *src = t->ch[c];
        if (!src) {
            if (!tmp && !(tmp = malloc(CHBYTES))) { ok = 0; break; }
            if (t->spill_fd < 0 || pread(t->spill_fd, tmp, cnt * sizeof(DpChoice), (off_t)(c * CHBYTES)) != (ssize_t)(cnt * sizeof(DpChoice))) { ok = 0; break; }
            src = tmp;
        }
        if (fwrite(src, sizeof(DpChoice), cnt, f) != cnt) ok = 0;
    }
    free(tmp);
    if (fclose(f)) ok = 0;
    return ok;
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
    // without a spill file the whole tree must fit in RAM: keep the historical wall
    if (n == 0 || (t->spill_fd < 0 && n > (1u << 24))) {
        fclose(f);
        return 0;
    }
    long here = ftell(f);
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, here, SEEK_SET);
    if (here < 0 || fsz < here || (uint64_t)(fsz - here) < (uint64_t)n * sizeof(DpChoice)) {   // truncated file
        fclose(f);
        return 0;
    }
    tree_drop(t);
    int ok = 1;
    uint32_t nopen = 0;
    for (uint32_t done = 0; done < n && ok; done += CHN) {
        uint32_t cnt = n - done < CHN ? n - done : CHN;
        if (!tree_grow(t)) { ok = 0; break; }
        uint32_t c = t->nch - 1;
        if (c) tree_evict(t, c - 1);   // the previous chunk is full now: closed ones go to the spill file
        if (fread(t->ch[c], sizeof(DpChoice), cnt, f) != cnt) { ok = 0; break; }
        uint32_t o = 0;
        for (uint32_t i = 0; i < cnt; i++) o += t->ch[c][i].issue == DP_ISSUE_OPEN;
        t->chopen[c] = o;
        nopen += o;
        t->n = done + cnt;
    }
    fclose(f);
    if (!ok) {                      // keep the tree usable: start over from the root
        tree_drop(t);
        tree_boot(t);
        return 0;
    }
    t->next_id = next_id;
    t->max_depth = max_depth;
    memcpy(t->closed, closed, sizeof closed);
    t->open = nopen - (t->ch[0][0].issue == DP_ISSUE_OPEN);   // the root is never counted (as after init)
    return 1;
}

void dp_tree_stats(const DpTree *t, char *out, size_t outsz) {
    size_t o = 0;
    o += (size_t)snprintf(out + o, outsz - o, "tree: %u choices, depth %u, open %u",
                          t->n ? t->n - 1 : 0, t->max_depth, t->open);
    static const char *names[DP_ISSUE_N] = {"open", "done", "merged", "dead", "reset",
                                            "deferred", "capped", "nonsense", "spooled"};
    for (int i = 1; i < DP_ISSUE_N; i++)
        if (t->closed[i])
            o += (size_t)snprintf(out + o, outsz - o, ", %s %u", names[i], t->closed[i]);
    snprintf(out + o, outsz - o, "\n");
}
