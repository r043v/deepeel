// heat.c: real usage of the ROM, per zone.  A zone is one run of the memory
// map (consecutive bytes of one class inside one bank, the .map lines); its
// heat is what the played sessions did there — instructions executed (on
// their opcode bytes) and data bytes read — never the fork explorer's forced
// paths (DpResults.play_*_hits).  Zones are ranked by heat; the hot set is
// the smallest prefix of that ranking carrying HOT_SHARE of all usage: the
// parts a recompiler should keep native, resident, close.
#include "report.h"

#include <stdlib.h>
#include <string.h>

#define HOT_SHARE 0.90

static int cmp_heat(const void *a, const void *b) {
    const DpZone *x = *(const DpZone *const *)a, *y = *(const DpZone *const *)b;
    uint64_t hx = x->exec + x->reads, hy = y->exec + y->reads;
    if (hx != hy) return hx < hy ? 1 : -1;
    // equal usage: the denser zone first, then ROM order (deterministic)
    if (x->bytes != y->bytes) return x->bytes < y->bytes ? -1 : 1;
    if (x->bank != y->bank) return x->bank < y->bank ? -1 : 1;
    return x->lo < y->lo ? -1 : 1;
}

int dp_heat_build(const DpResults *res, const uint8_t *map, DpHeat *h) {
    memset(h, 0, sizeof *h);
    size_t n = (size_t)res->banks * 0x4000;
    int cap = 1024;
    h->z = malloc(sizeof(DpZone) * (size_t)cap);
    if (!h->z) return 0;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        for (size_t i = bs; i < bs + 0x4000;) {
            size_t j = i;
            uint64_t ex = 0, rd = 0;
            while (j < bs + 0x4000 && map[j] == map[i]) {
                ex += __atomic_load_n(&res->play_exec_hits[j], __ATOMIC_RELAXED);
                rd += __atomic_load_n(&res->play_data_hits[j], __ATOMIC_RELAXED);
                j++;
            }
            if (h->n == cap) {
                cap *= 2;
                DpZone *g = realloc(h->z, sizeof(DpZone) * (size_t)cap);
                if (!g) { dp_heat_free(h); return 0; }
                h->z = g;
            }
            DpZone *z = &h->z[h->n++];
            z->bank = (uint16_t)(bs / 0x4000);
            z->lo = (uint16_t)(i - bs);
            z->hi = (uint16_t)(j - 1 - bs);
            z->cls = map[i];
            z->bytes = (uint32_t)(j - i);
            z->exec = ex;
            z->reads = rd;
            z->rank = 0;
            z->hot = 0;
            h->exec_total += ex;
            h->read_total += rd;
            i = j;
        }
    }
    // rank by usage; the hot set is the prefix carrying HOT_SHARE of it
    DpZone **order = malloc(sizeof(DpZone *) * (size_t)(h->n ? h->n : 1));
    if (!order) { dp_heat_free(h); return 0; }
    for (int i = 0; i < h->n; i++) order[i] = &h->z[i];
    qsort(order, (size_t)h->n, sizeof *order, cmp_heat);
    uint64_t total = h->exec_total + h->read_total, acc = 0;
    for (int i = 0; i < h->n; i++) {
        DpZone *z = order[i];
        uint64_t u = z->exec + z->reads;
        z->rank = (uint32_t)(i + 1);
        if (u && (double)acc < HOT_SHARE * (double)total) {
            z->hot = 1;
            h->hot_n++;
            h->hot_bytes += z->bytes;
        }
        acc += u;
        if (u) h->used_n++;
    }
    free(order);
    return 1;
}

void dp_heat_free(DpHeat *h) {
    free(h->z);
    memset(h, 0, sizeof *h);
}

// "deepeel_heat" chunk: one line per zone, ROM order, every zone (a cold
// zone says so too).  Addresses as the CPU sees them (bank N at 4000-7FFF).
void dp_heat_text(const DpHeat *h, DpBuf *o) {
    dp_buf_fmt(o, "# deepeel heat v1: played usage per memory-map zone (sessions only)\n");
    dp_buf_fmt(o, "# exec %llu instructions, reads %llu bytes, %d zones used, hot set %d zones "
                  "%u bytes (%.0f%% of usage)\n",
               (unsigned long long)h->exec_total, (unsigned long long)h->read_total, h->used_n,
               h->hot_n, h->hot_bytes, HOT_SHARE * 100.0);
    dp_buf_str(o, "# columns: bank start end(inclusive) class bytes exec reads rank hot\n");
    for (int i = 0; i < h->n; i++) {
        const DpZone *z = &h->z[i];
        unsigned win = z->bank ? 0x4000u : 0u;
        dp_buf_fmt(o, "%u %04x %04x %s %u %llu %llu %u %d\n", z->bank, win + z->lo, win + z->hi,
                   dp_map_class_name(z->cls), z->bytes, (unsigned long long)z->exec,
                   (unsigned long long)z->reads, z->rank, z->hot);
    }
}

// the `top` hottest zones, for the text report
void dp_heat_top_text(const DpHeat *h, int top, DpBuf *o) {
    uint64_t total = h->exec_total + h->read_total;
    dp_buf_fmt(o, "heat (played sessions): %llu instructions executed, %llu ROM bytes read; "
                  "hot set %d of %d used zones, %u bytes (%.0f%% of usage)\n",
               (unsigned long long)h->exec_total, (unsigned long long)h->read_total, h->hot_n,
               h->used_n, h->hot_bytes, HOT_SHARE * 100.0);
    if (!total) return;
    for (int r = 1; r <= top; r++) {
        for (int i = 0; i < h->n; i++) {
            const DpZone *z = &h->z[i];
            if (z->rank != (uint32_t)r) continue;
            uint64_t u = z->exec + z->reads;
            if (!u) return;
            unsigned win = z->bank ? 0x4000u : 0u;
            dp_buf_fmt(o, "  %3d. %u:%04x-%04x %-16s %6u B  exec %llu  reads %llu  %.2f%%\n", r,
                       z->bank, win + z->lo, win + z->hi, dp_map_class_name(z->cls), z->bytes,
                       (unsigned long long)z->exec, (unsigned long long)z->reads,
                       100.0 * (double)u / (double)total);
            break;
        }
    }
}

// per-byte heat for the map image: played executions of the instruction a
// code byte belongs to (operands inherit their opcode's count), played reads
// of a data byte; malloc'd, banks*0x4000 entries
uint32_t *dp_heat_bytes(const DpResults *res) {
    size_t n = (size_t)res->banks * 0x4000;
    uint32_t *b = calloc(n, sizeof(uint32_t));
    if (!b) return NULL;
    for (size_t bs = 0; bs < n; bs += 0x4000) {
        uint32_t carry = 0;
        for (size_t k = bs; k < bs + 0x4000; k++) {
            uint8_t ef = res->exec_flags[k];
            uint32_t e = __atomic_load_n(&res->play_exec_hits[k], __ATOMIC_RELAXED);
            uint32_t r = __atomic_load_n(&res->play_data_hits[k], __ATOMIC_RELAXED);
            if (ef & DP_XF_START) carry = e;
            else if (!(ef & (DP_XF_NATURAL | DP_XF_FORCED))) carry = 0;
            uint32_t v = (ef & (DP_XF_NATURAL | DP_XF_FORCED)) ? carry : 0;
            b[k] = v > r ? v : r;
        }
    }
    return b;
}
