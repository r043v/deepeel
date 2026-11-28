// pages.h: a content-addressed store of 1 KiB pages shared by every GP32
// machine instance.  A machine snapshot holds page ids instead of the 8 MiB of
// SDRAM; identical pages are stored once.  Id 0 is the zero page.  With a path
// the pages live in a file (the spooled and checkpointed states stay loadable
// on resume), else in anonymous memory.  Pages are never freed.
#ifndef DP_GP32_PAGES_H
#define DP_GP32_PAGES_H

#include <stddef.h>
#include <stdint.h>

#define DP_GP32_PAGE 1024u

typedef struct DpPageStore DpPageStore;

DpPageStore *dp_pages_open(const char *path);   // NULL path: memory only; NULL return: message printed
uint32_t dp_pages_intern(DpPageStore *s, const uint8_t *page, uint64_t hash);   // exits when the store cannot grow
const uint8_t *dp_pages_get(const DpPageStore *s, uint32_t id);
uint64_t dp_pages_hash(const uint8_t *page);
uint32_t dp_pages_count(const DpPageStore *s);
void dp_pages_close(DpPageStore *s);

#endif
