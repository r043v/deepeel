// pages.c: the content-addressed page store (see pages.h).
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pages.h"

#define CHUNK_PAGES 4096u
#define CHUNK_BYTES ((size_t)CHUNK_PAGES * DP_GP32_PAGE)
#define MAX_CHUNKS 16384u   // 64 GiB of distinct pages

typedef struct { char magic[8]; uint32_t count; } Header;   // slot 0 of the store

struct DpPageStore {
    pthread_mutex_t mu;
    int fd;                      // -1: anonymous memory
    uint8_t *chunk[MAX_CHUNKS];  // fixed array: readers never see it move
    uint32_t count;              // slots in use, the header slot included
    uint32_t *table;             // open addressing: ids (0 = empty)
    uint64_t *hashes;            // by id
    size_t table_cap, hash_cap;
};

static const uint8_t zero_page[DP_GP32_PAGE];

uint64_t dp_pages_hash(const uint8_t *page) {
    uint64_t h = 0x9E3779B97F4A7C15ull, any = 0;
    for (unsigned i = 0; i < DP_GP32_PAGE; i += 8) {
        uint64_t w; memcpy(&w, page + i, 8);
        any |= w;
        h = (h ^ w) * 0xFF51AFD7ED558CCDull; h ^= h >> 29;
    }
    return any ? h : 0;   // 0 names the zero page
}

static uint8_t *slot(const DpPageStore *s, uint32_t id) { return s->chunk[id / CHUNK_PAGES] + (size_t)(id % CHUNK_PAGES) * DP_GP32_PAGE; }

static int map_chunk(DpPageStore *s, uint32_t c) {
    if (c >= MAX_CHUNKS) return 0;
    if (s->chunk[c]) return 1;
    void *p;
    if (s->fd < 0) p = mmap(NULL, CHUNK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    else {
        int err = posix_fallocate(s->fd, (off_t)(c * CHUNK_BYTES), (off_t)CHUNK_BYTES);   // a full disk fails here, not as a SIGBUS later
        if (err) { errno = err; return 0; }
        p = mmap(NULL, CHUNK_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, (off_t)(c * CHUNK_BYTES));
    }
    if (p == MAP_FAILED) return 0;
    s->chunk[c] = p;
    return 1;
}

static void table_put(DpPageStore *s, uint32_t id) {
    size_t h = (size_t)s->hashes[id] & (s->table_cap - 1);
    while (s->table[h]) h = (h + 1) & (s->table_cap - 1);
    s->table[h] = id;
}

static int reserve(DpPageStore *s, uint32_t id) {
    if (id >= s->hash_cap) {
        size_t nc = s->hash_cap ? s->hash_cap * 2 : 8192;
        while (id >= nc) nc *= 2;
        uint64_t *n = realloc(s->hashes, nc * sizeof *n);
        if (!n) return 0;
        s->hashes = n; s->hash_cap = nc;
    }
    if ((size_t)id * 2 >= s->table_cap) {
        size_t nc = s->table_cap ? s->table_cap * 2 : 16384;
        while ((size_t)id * 2 >= nc) nc *= 2;
        uint32_t *n = calloc(nc, sizeof *n);
        if (!n) return 0;
        free(s->table); s->table = n; s->table_cap = nc;
        for (uint32_t i = 1; i < s->count; i++) table_put(s, i);
    }
    return 1;
}

DpPageStore *dp_pages_open(const char *path) {
    DpPageStore *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    pthread_mutex_init(&s->mu, NULL);
    s->fd = -1;
    uint32_t stored = 0;
    if (path) {
        s->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (s->fd < 0) { fprintf(stderr, "deepeel: %s: %s\n", path, strerror(errno)); dp_pages_close(s); return NULL; }
        Header h;
        ssize_t got = pread(s->fd, &h, sizeof h, 0);
        if (got == (ssize_t)sizeof h && !memcmp(h.magic, "DPPG0001", 8)) stored = h.count;
        else if (got != 0) {
            struct stat st;
            if (fstat(s->fd, &st) || st.st_size) { fprintf(stderr, "deepeel: %s is not a page store\n", path); dp_pages_close(s); return NULL; }
        }
    }
    if (!map_chunk(s, 0)) { fprintf(stderr, "deepeel: page store: %s\n", strerror(errno)); dp_pages_close(s); return NULL; }
    s->count = 1;
    for (uint32_t i = 1; i < stored; i++) {   // resume: the index is rebuilt from the pages
        if (!map_chunk(s, i / CHUNK_PAGES) || !reserve(s, i)) { fprintf(stderr, "deepeel: cannot reload the page store\n"); dp_pages_close(s); return NULL; }
        s->hashes[i] = dp_pages_hash(slot(s, i));
        table_put(s, i);
        s->count = i + 1;
    }
    Header h = { .count = s->count };
    memcpy(h.magic, "DPPG0001", 8);
    memcpy(slot(s, 0), &h, sizeof h);
    return s;
}

uint32_t dp_pages_intern(DpPageStore *s, const uint8_t *page, uint64_t hash) {
    if (!hash) return 0;
    pthread_mutex_lock(&s->mu);
    if (s->table_cap)
        for (size_t h = (size_t)hash & (s->table_cap - 1); s->table[h]; h = (h + 1) & (s->table_cap - 1)) {
            uint32_t id = s->table[h];
            if (s->hashes[id] == hash && !memcmp(slot(s, id), page, DP_GP32_PAGE)) { pthread_mutex_unlock(&s->mu); return id; }
        }
    uint32_t id = s->count;
    if (!map_chunk(s, id / CHUNK_PAGES) || !reserve(s, id)) {
        fprintf(stderr, "deepeel: the GP32 page store cannot grow past %u pages: %s\n", id, strerror(errno));
        exit(1);
    }
    memcpy(slot(s, id), page, DP_GP32_PAGE);
    s->hashes[id] = hash;
    table_put(s, id);
    s->count = id + 1;
    memcpy(slot(s, 0) + offsetof(Header, count), &s->count, sizeof s->count);
    pthread_mutex_unlock(&s->mu);
    return id;
}

const uint8_t *dp_pages_get(const DpPageStore *s, uint32_t id) {
    if (!id || id / CHUNK_PAGES >= MAX_CHUNKS || !s->chunk[id / CHUNK_PAGES]) return zero_page;
    return slot(s, id);
}

uint32_t dp_pages_count(const DpPageStore *s) { return s->count - 1; }

void dp_pages_close(DpPageStore *s) {
    if (!s) return;
    for (unsigned i = 0; i < MAX_CHUNKS; i++) if (s->chunk[i]) munmap(s->chunk[i], CHUNK_BYTES);
    if (s->fd >= 0) close(s->fd);
    free(s->table); free(s->hashes);
    pthread_mutex_destroy(&s->mu);
    free(s);
}
