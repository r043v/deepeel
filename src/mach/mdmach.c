#include "mdmach.h"
#include <stdlib.h>
#include <string.h>
#include "md_full.h"
#include "md_trace.h"

void dp_mdrom_set(const uint8_t *rom, size_t size);
uint32_t dp_mdrom_peek32(uint32_t a);

struct DpMdMach {
    MdFull *m;
    DpMdHooks h;
    uint64_t steps, budget;
};

static DpMdMach *cur;

static void t_step(void *u, MdFull *m, uint32_t pc, uint16_t op) {
    DpMdMach *d = u;
    if (d->steps >= d->budget) {
        m->fault = DP_MD_FAULT_BUDGET;
        m->fault_pc = pc;
        m->slice_limit = 0;
        return;
    }
    ++d->steps;
    if (d->h.on_step) d->h.on_step(d->h.user, pc, op);
}

static void t_end(void *u, MdFull *m, uint32_t pc, uint32_t next) {
    DpMdMach *d = u;
    (void)m;
    if (!d->h.on_end) return;
    unsigned len = (next > pc && next - pc <= 12u) ? (unsigned)(next - pc) : 2u;
    d->h.on_end(d->h.user, pc, next, len);
}

static void t_rd(void *u, MdFull *m, uint32_t a, unsigned sz) {
    DpMdMach *d = u;
    (void)m;
    if (d->h.on_read) d->h.on_read(d->h.user, a, sz);
}

static void t_wr(void *u, MdFull *m, uint32_t a, unsigned sz, uint32_t v) {
    DpMdMach *d = u;
    (void)m;
    if (d->h.on_write) d->h.on_write(d->h.user, a, sz, v);
}

DpMdMach *dp_mdmach_open(const uint8_t *rom, size_t size) {
    if (!rom || !size || size > DP_MD_ROM_MAX) return NULL;
    DpMdMach *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->m = calloc(1, sizeof *d->m);
    if (!d->m) { free(d); return NULL; }
    dp_mdrom_set(rom, size);
    return d;
}

void dp_mdmach_close(DpMdMach *d) {
    if (!d) return;
    if (cur == d) { memset(&md_trace, 0, sizeof md_trace); cur = NULL; }
    free(d->m);
    free(d);
}

void dp_mdmach_set_hooks(DpMdMach *d, const DpMdHooks *h) { d->h = *h; }

void dp_mdmach_reset(DpMdMach *d) {
    md_full_reset(d->m);
    d->steps = 0;
}

uint64_t dp_mdmach_run(DpMdMach *d, uint64_t max_steps, int *cut, int *fault) {
    memset(&md_trace, 0, sizeof md_trace);
    md_trace.user = d;
    md_trace.step = t_step;
    md_trace.end = t_end;
    md_trace.rd = t_rd;
    md_trace.wr = t_wr;
    cur = d;
    d->steps = 0;
    d->budget = max_steps;
    d->m->fault = 0;
    while (!d->m->fault && d->steps < d->budget)
        md_full_frame(d->m, 0);
    if (cut) *cut = d->m->fault == DP_MD_FAULT_BUDGET || (!d->m->fault && d->steps >= d->budget);
    if (fault) *fault = d->m->fault == DP_MD_FAULT_BUDGET ? 0 : d->m->fault;
    return d->steps;
}

uint32_t dp_mdmach_pc(const DpMdMach *d) { return d->m->pc; }

uint32_t dp_mdmach_peek32(const DpMdMach *d, uint32_t a) {
    (void)d;
    return dp_mdrom_peek32(a);
}
