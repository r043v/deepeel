// stack.c: the campaign's single work stack (see stack.h)
#include "stack.h"
#include "campaign.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int tech_of(int kind) { return kind == K_RANDOM ? 0 : kind == K_SPLICE ? 1 : 2; }
static unsigned bucket(size_t score) { unsigned b = 0; while (score && b < STACK_BUCKETS - 1) { score >>= 1; b++; } return b; }
static unsigned region_of(unsigned frame) { unsigned r = frame / CAMP_REGION_FRAMES; return r < STACK_REGIONS ? r : STACK_REGIONS - 1; }

int stack_init(Stack *s, size_t max, size_t ng, const char *const *name, const uint8_t *sibling) {
    memset(s, 0, sizeof *s);
    s->tk = calloc(max, sizeof *s->tk); s->max = max; s->ng = ng; s->name = name; s->sibling = sibling; s->next_id = 1;
    s->gsnap = calloc(ng, sizeof *s->gsnap); s->region = calloc(ng * STACK_REGIONS, sizeof *s->region);
    return s->tk && s->gsnap && s->region;
}
void stack_free(Stack *s) { free(s->tk); free(s->gsnap); free(s->region); }
Task *stack_add(Stack *s, int kind, int group) {
    if (s->nt >= s->max) { if (!s->dropped++) fprintf(stderr, "deepeel auto: the work stack is full (%zu items): new items are dropped and counted\n", s->max); return NULL; }
    Task *t = &s->tk[s->nt++]; memset(t, 0, sizeof *t); t->kind = kind; t->movie = group; t->slot = -1;
    return t;
}
void stack_compact(Stack *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->nt; i++) {
        const Task *t = &s->tk[i];
        if (t->done && !t->cut && !is_film(t)) { s->played[tech_of(t->kind)]++; continue; }
        if (n != i) s->tk[n] = *t;
        n++;
    }
    s->nt = n;
}
void stack_add_snapshot(Stack *s, int group, const char *snap, unsigned frame, unsigned key, size_t score, unsigned depth, int splice, int forks) {
    static const int kinds[] = { K_RANDOM, K_SPLICE, K_FORK };
    for (int i = 0; i < 3; i++) {
        if ((kinds[i] == K_SPLICE && !splice) || (kinds[i] == K_FORK && !forks)) continue;
        Task *t = stack_add(s, kinds[i], group); if (!t) return;
        snprintf(t->snap, sizeof t->snap, "%s", snap); t->frame = frame; t->key = key; t->score = score; t->depth = depth;
    }
}
// What an item is worth: bytes per CPU second measured on items of its kind (same technique, first play or replay, same
// order of magnitude of the score), started from a guess that grows with the score; then corrected by what its
// neighbourhood in the game gave compared with what was expected of it.
static double bucket_rate(const Stack *s, const Task *t) {
    const int tech = tech_of(t->kind);
    const StackStat *st = &s->stat[tech][t->pass != 0][bucket(t->score)];
    const double score = t->score < CAMP_PRIOR_SCORE_CAP ? (double)t->score : (double)CAMP_PRIOR_SCORE_CAP;
    const double secs = tech == 2 ? CAMP_PRIOR_FORK_SECS : CAMP_PRIOR_PADS_SECS;
    const double bytes = (CAMP_PRIOR_BYTES + score / CAMP_PRIOR_SCORE_DIV) * (tech == 2 ? CAMP_PRIOR_FORK_SECS / CAMP_PRIOR_PADS_SECS / 4.0 : 1.0);
    return (st->bytes + CAMP_PRIOR_WEIGHT * bytes) / (st->secs + CAMP_PRIOR_WEIGHT * secs);
}
static double priority(const Stack *s, const Task *t) {
    const StackRegion *r = &s->region[(size_t)t->movie * STACK_REGIONS + region_of(t->frame)];
    double f = (r->got + CAMP_REGION_PRIOR) / (r->expected + CAMP_REGION_PRIOR);
    if (f < 0.25) f = 0.25;
    if (f > 4.0) f = 4.0;
    double p = bucket_rate(s, t) * f;
    if (s->sibling[t->movie]) p /= CAMP_SIBLING_DIV;   // another ROM version: its bytes only reach ours by transposition
    return p;
}
Task *stack_next(Stack *s, int forks_ok, long fork_cap, long film_cap) {
    long forks = 0, films = 0; Task *film = NULL; int other = 0;
    for (size_t i = 0; i < s->nt; i++) {
        Task *t = &s->tk[i];
        const int waiting = !t->pid && !t->done;
        if (is_film(t) && waiting && !film) film = t;
        if (!is_film(t) && waiting && (t->kind != K_FORK || forks_ok)) other = 1;
        films += is_film(t) && running_task(t);
        forks += t->kind == K_FORK && running_task(t);
    }
    if (film && (films < film_cap || !other)) return film;
    if (forks >= fork_cap) forks_ok = 0;
    // the floor: one start in CAMP_FLOOR_EVERY goes to the technique served least, on its coarsest untried snapshot
    int floor = -1;
    if (s->launches % CAMP_FLOOR_EVERY == CAMP_FLOOR_EVERY - 1) {
        int have[STACK_TECHS] = { 0 };
        for (size_t i = 0; i < s->nt; i++) { const Task *t = &s->tk[i]; if (!is_film(t) && !t->pid && !t->done) have[tech_of(t->kind)] = 1; }
        for (int k = 0; k < STACK_TECHS; k++) if (have[k] && (k != 2 || forks_ok) && (floor < 0 || s->picked[k] < s->picked[floor])) floor = k;
    }
    Task *best = NULL; double bp = 0;
    for (size_t i = 0; i < s->nt; i++) {
        Task *t = &s->tk[i];
        if (is_film(t) || t->pid || t->done || (t->kind == K_FORK && !forks_ok)) continue;
        if (floor >= 0) {
            if (tech_of(t->kind) != floor) continue;
            if (!best || t->pass < best->pass || (t->pass == best->pass && t->key < best->key)) best = t;
        } else {
            const double p = priority(s, t);
            if (!best || p > bp || (p == bp && t->key < best->key)) { best = t; bp = p; }
        }
    }
    return best;
}
void stack_launched(Stack *s, const Task *t) { if (!is_film(t)) { s->launches++; s->picked[tech_of(t->kind)]++; } }
void stack_requeue(Stack *s, const Task *o, int shrink) {
    Task *r = stack_add(s, o->kind, o->movie); if (!r) return;
    memcpy(r->snap, o->snap, sizeof r->snap); r->pass = o->pass; r->score = o->score; r->key = o->key; r->frame = o->frame; r->first = o->first; r->depth = o->depth; r->shrink = shrink;
}
void stack_result(Stack *s, Task *t, double cpu_secs, size_t boot_tasks) {
    if (is_film(t)) return;
    if (cpu_secs < 0.1) cpu_secs = 0.1;
    StackRegion *r = &s->region[(size_t)t->movie * STACK_REGIONS + region_of(t->frame)];
    r->expected += bucket_rate(s, t) * cpu_secs; r->got += (double)t->gain;
    StackStat *st = &s->stat[tech_of(t->kind)][t->pass != 0][bucket(t->score)];
    st->bytes += (double)t->gain; st->secs += cpu_secs; st->n++;
    // a snapshot that still gives is played again, other seeds (a fork exploration is deterministic: once is enough)
    if (!is_pads(t) || t->gain < CAMP_RANDOM_AGAIN || t->pass + 1 >= CAMP_RANDOM_PASSES) return;
    Task *n = stack_add(s, t->kind, t->movie); if (!n) return;
    memcpy(n->snap, t->snap, sizeof n->snap); n->key = t->key; n->frame = t->frame; n->depth = t->depth;
    n->pass = t->pass + 1; n->score = t->gain; n->first = t->first + (t->snap[0] ? 1u : (unsigned)boot_tasks) * CAMP_RANDOM_RUNS;
}
double stack_rate(const Stack *s, int tech) {
    double b = 0, c = 0;
    for (int r = 0; r < 2; r++) for (int k = 0; k < STACK_BUCKETS; k++) { b += s->stat[tech][r][k].bytes; c += s->stat[tech][r][k].secs; }
    return c > 0 ? b / c : 0;
}

// Text, one record per line, tab separated; groups by name, so that a changed movie list does not mix them up.
int stack_save(const Stack *s, const char *path) {
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return 0;
    fprintf(f, "deepeel-stack 1\nnext\t%llu\t%llu\t%llu\t%llu\t%llu\n", s->next_id, s->launches, s->picked[0], s->picked[1], s->picked[2]);
    fprintf(f, "played\t%llu\t%llu\t%llu\t%llu\n", s->played[0], s->played[1], s->played[2], s->dropped);
    for (int k = 0; k < STACK_TECHS; k++) for (int r = 0; r < 2; r++) for (int b = 0; b < STACK_BUCKETS; b++) { const StackStat *st = &s->stat[k][r][b]; if (st->n) fprintf(f, "stat\t%d\t%d\t%d\t%.1f\t%.3f\t%u\n", k, r, b, st->bytes, st->secs, st->n); }
    for (size_t g = 0; g < s->ng; g++) {
        fprintf(f, "group\t%u\t%s\n", s->gsnap[g], s->name[g]);
        for (int i = 0; i < STACK_REGIONS; i++) { const StackRegion *r = &s->region[g * STACK_REGIONS + i]; if (r->expected > 0) fprintf(f, "region\t%d\t%.2f\t%.2f\t%s\n", i, r->got, r->expected, s->name[g]); }
    }
    for (size_t i = 0; i < s->nt; i++) {
        const Task *t = &s->tk[i];
        const int done = t->done && !t->cut;
        if (done && !is_film(t)) continue;   // played items are in the coverage and the statistics
        fprintf(f, "task\t%d\t%d\t%u\t%u\t%u\t%zu\t%u\t%u\t%d\t%s\t%s\n", t->kind, done, t->frame, t->pass, t->first, t->score, t->depth, t->key, t->shrink, s->name[t->movie], t->snap);
    }
    if (fflush(f) || ferror(f)) { fclose(f); remove(tmp); return 0; }
    if (fclose(f) || rename(tmp, path)) { remove(tmp); return 0; }
    return 1;
}
static int group_of(const Stack *s, const char *name) { for (size_t g = 0; g < s->ng; g++) if (!strcmp(s->name[g], name)) return (int)g; return -1; }
static int split(char *line, char **v, int max) {   // tab-separated fields, empty ones kept
    int n = 0; line[strcspn(line, "\n")] = 0;
    while (n < max) { v[n++] = line; line = strchr(line, '\t'); if (!line) break; *line++ = 0; }
    return n;
}
size_t stack_load(Stack *s, const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char *line = NULL; size_t cap = 0, got = 0, lost = 0; char *v[16];
    if (getline(&line, &cap, f) < 0 || strcmp(line, "deepeel-stack 1\n")) { free(line); fclose(f); return 0; }
    while (getline(&line, &cap, f) > 0) {
        const int n = split(line, v, 16);
        if (!strcmp(v[0], "next") && n == 6) { s->next_id = strtoull(v[1], NULL, 10); s->launches = strtoull(v[2], NULL, 10); for (int k = 0; k < 3; k++) s->picked[k] = strtoull(v[3 + k], NULL, 10); }
        else if (!strcmp(v[0], "played") && n == 5) { for (int k = 0; k < 3; k++) s->played[k] = strtoull(v[1 + k], NULL, 10); s->dropped = strtoull(v[4], NULL, 10); }
        else if (!strcmp(v[0], "stat") && n == 7) {
            const unsigned k = (unsigned)atoi(v[1]), r = (unsigned)atoi(v[2]), b = (unsigned)atoi(v[3]);
            if (k < STACK_TECHS && r < 2 && b < STACK_BUCKETS) s->stat[k][r][b] = (StackStat){ atof(v[4]), atof(v[5]), (unsigned)atoi(v[6]) };
        } else if (!strcmp(v[0], "group") && n == 3) { const int g = group_of(s, v[2]); if (g >= 0) s->gsnap[g] = (unsigned)strtoul(v[1], NULL, 10); }
        else if (!strcmp(v[0], "region") && n == 5) { const int g = group_of(s, v[4]), i = atoi(v[1]); if (g >= 0 && i >= 0 && i < STACK_REGIONS) s->region[(size_t)g * STACK_REGIONS + i] = (StackRegion){ atof(v[2]), atof(v[3]) }; }
        else if (!strcmp(v[0], "task") && n == 12) {
            const int kind = atoi(v[1]), g = group_of(s, v[10]); struct stat sb;
            if (kind <= 0 || kind >= K_KINDS || g < 0 || (v[11][0] && stat(v[11], &sb))) { lost++; continue; }
            Task *t = stack_add(s, kind, g); if (!t) { lost++; continue; }
            t->done = atoi(v[2]); t->frame = (unsigned)strtoul(v[3], NULL, 10); t->pass = (unsigned)strtoul(v[4], NULL, 10); t->first = (unsigned)strtoul(v[5], NULL, 10);
            t->score = (size_t)strtoull(v[6], NULL, 10); t->depth = (unsigned)strtoul(v[7], NULL, 10); t->key = (unsigned)strtoul(v[8], NULL, 10); t->shrink = atoi(v[9]);
            snprintf(t->snap, sizeof t->snap, "%s", v[11]); got++;
        }
    }
    if (lost) fprintf(stderr, "deepeel auto: %zu saved items dropped (their movie or snapshot is gone)\n", lost);
    free(line); fclose(f);
    return got;
}
