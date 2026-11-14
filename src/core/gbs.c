/* gbs.c: GBS rip parsing and verification (see gbs.h).  The verifier compares every
 * rip bank (the data block loaded linearly at `load`, usually $4000) against every ROM
 * bank at the same window offset: rips are patched (VBlank sync replaced by a timer,
 * banks trimmed) so the match is scored by equal bytes and must clear min_ratio;
 * best and second-best scores are kept so a bad match is never silent.  When no rip
 * bank is byte-identical (rips are often a mid-bank contiguous extract of the engine
 * with ripper chunks in it), exact 16-byte fragments relocate the block: the dominant
 * rom-offset delta says where it lives, and init/play are mapped by a local vote of
 * the fragments surrounding their data offset. */
#define _GNU_SOURCE   // memmem
#include "gbs.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }

int dp_gbs_parse(const uint8_t *buf, size_t n, DpGbs *g, char *err, size_t errsz) {
    memset(g, 0, sizeof *g);
    if (n < 113 || memcmp(buf, "GBS", 3)) { snprintf(err, errsz, "not a GBS file"); return -1; }
    g->version = buf[3]; g->songs = buf[4]; g->first = buf[5];
    g->load = (uint16_t)le16(buf + 6); g->init = (uint16_t)le16(buf + 8); g->play = (uint16_t)le16(buf + 10);
    g->sp = buf[12]; g->tma_tac = buf[13];
    for (int s = 0; s < 3; s++) {   // title/author/copyright: 32 bytes each, force-terminated
        char *dst = s == 0 ? g->title : s == 1 ? g->author : g->copyright;
        memcpy(dst, buf + 14 + s * 32, 32); dst[32] = 0;
    }
    g->data = buf + 112; g->data_size = n - 112;
    if (!g->data_size) { snprintf(err, errsz, "empty data block"); return -1; }
    if (g->load >= 0x8000) { snprintf(err, errsz, "load address $%04x is not ROM-bank mapped", g->load); return -1; }
    return 0;
}

/* linear data offset of a rip address, SIZE_MAX when the address is outside the block */
static size_t data_offset(const DpGbs *g, uint16_t pc) {
    unsigned vb = pc >> 14, vo = pc & 0x3FFF;
    if (vb < g->load >> 14) return SIZE_MAX;   // below the load point: player stub, not rip data
    size_t d = (size_t)(vb - (g->load >> 14)) * 0x4000 + vo - (g->load & 0x3FFF);
    return d < g->data_size ? d : SIZE_MAX;
}

static void map_pc(const DpGbs *g, const DpGbsMatch *m, uint16_t pc, int *bank, uint16_t *addr) {
    *bank = -1;
    if (pc >= 0x8000) return;   // WRAM/HRAM address: player stub, not ROM data
    size_t d = data_offset(g, pc);
    if (d == SIZE_MAX) return;
    unsigned r = (pc >> 14) - (g->load >> 14);
    if (r >= m->rip_banks || !m->bank[r].matched) return;
    *bank = (int)m->bank[r].rom_bank;
    *addr = pc;   // the CPU address in the $4000-$7FFF window is the seed address itself
}

unsigned dp_gbs_verify(const DpGbs *g, const uint8_t *rom, size_t rom_size, double min_ratio, DpGbsMatch *m) {
    memset(m, 0, sizeof *m);
    m->bank_base = g->load >> 14;
    m->init_bank = m->play_bank = -1;
    unsigned nbanks = (unsigned)(rom_size / 0x4000);
    size_t off0 = g->load & 0x3FFF;
    unsigned rbs = (unsigned)(((size_t)g->load + g->data_size - 1) / 0x4000 - m->bank_base + 1);
    if (!nbanks || rbs > DP_GBS_MAX_BANKS) return 0;
    m->rip_banks = rbs;
    unsigned matched = 0;
    for (unsigned r = 0; r < rbs; r++) {
        size_t d0 = r ? (size_t)r * 0x4000 - off0 : 0;          // data range of this rip bank,
        size_t d1 = (size_t)(r + 1) * 0x4000 - off0;            // clipped to the block
        if (d1 > g->data_size) d1 = g->data_size;
        size_t voff0 = r ? 0 : off0;                            // window offset of its first byte
        size_t len = d1 > d0 ? d1 - d0 : 0;
        m->bank[r].bytes = len;
        if (!len) continue;
        unsigned best = 0, second = 0, bbank = 0;
        for (unsigned b = 0; b < nbanks; b++) {
            const uint8_t *rp = g->data + d0, *bp = rom + (size_t)b * 0x4000 + voff0;
            unsigned eq = 0;
            for (size_t i = 0; i < len; i++) eq += rp[i] == bp[i];
            if (eq > best) { second = best; best = eq; bbank = b; }
            else if (eq > second) second = eq;
        }
        m->bank[r].rom_bank = bbank; m->bank[r].best = best; m->bank[r].second = second;
        m->bank[r].matched = len && best >= (unsigned)(min_ratio * (double)len);
        if (m->bank[r].matched) matched++;
    }
    if (matched) {   // path A: byte-identical banks carry their pcs directly
        map_pc(g, m, g->init, &m->init_bank, &m->init_addr);
        map_pc(g, m, g->play, &m->play_bank, &m->play_addr);
        if (m->init_bank >= 0 && m->play_bank >= 0) return matched;
        /* some pcs fell in a rip bank that could not clear the threshold (a short
         * first slice under the window, e.g. a rip starting mid-$4000 whose head is
         * 56 bytes): the matched banks still locate the block, the fragment vote
         * below carries the unmapped pcs the rest of the way */
    }
    /* path B: fragment relocation — no rip bank is byte-identical, the block is likely
     * a mid-bank extract; exact 16-byte fragments say where it lives */
    m->frag_scanned = 1;
    size_t deltas[64]; unsigned votes[64]; int nd = 0;
    for (size_t p = 0; p + 16 <= g->data_size; p += 16) {
        m->frag_total++;
        const char *f = memmem(rom, rom_size, g->data + p, 16);
        if (!f) continue;
        m->frag_matched++;
        size_t dv = (size_t)(f - (const char *)rom) - p;
        for (int i = 0; i < nd; i++)
            if (deltas[i] == dv) { votes[i]++; goto next; }
        if (nd < 64) { deltas[nd] = dv; votes[nd] = 1; nd++; }
    next:;
    }
    for (int i = 0; i < nd; i++)
        if (votes[i] > m->frag_dom_votes) { m->frag_dom_votes = votes[i]; m->frag_dom_delta = deltas[i]; }
    /* local vote around a pc's data offset: the fragments within +-512 bytes imply the
     * ROM offset of the pc itself; >= 3 exact-agreeing votes and a majority carry it */
    struct pc { uint16_t pc; int *bank; uint16_t *addr; unsigned *v; } pcs[2] = {
        { g->init, &m->init_bank, &m->init_addr, &m->init_votes },
        { g->play, &m->play_bank, &m->play_addr, &m->play_votes },
    };
    for (int s = 0; s < 2; s++) {
        if (pcs[s].pc >= 0x8000) continue;   // WRAM/HRAM address: player stub, not ROM data
        if (*pcs[s].bank >= 0) continue;     // already carried by a matched bank
        size_t d = data_offset(g, pcs[s].pc);
        if (d == SIZE_MAX) continue;
        size_t lo = d > 512 ? d - 512 : 0, hi = d + 512 < g->data_size ? d + 512 : g->data_size;
        size_t vd[64]; unsigned vc[64]; int vn = 0, tot = 0;
        for (size_t p = lo; p + 16 <= hi; p += 16) {
            const char *f = memmem(rom, rom_size, g->data + p, 16);
            if (!f) continue;
            size_t iv = (size_t)(f - (const char *)rom) - p + d;
            tot++;
            for (int i = 0; i < vn; i++)
                if (vd[i] == iv) { vc[i]++; goto voted; }
            if (vn < 64) { vd[vn] = iv; vc[vn] = 1; vn++; }
        voted:;
        }
        int best = -1;
        for (int i = 0; i < vn; i++) if (best < 0 || vc[i] > vc[best]) best = i;
        if (best >= 0 && vc[best] >= 3 && vc[best] * 2 > tot) {
            size_t implied = vd[best];
            *pcs[s].bank = (int)(implied >> 14);
            *pcs[s].addr = (uint16_t)(implied < 0x4000 ? implied : 0x4000 + (implied & 0x3FFF));
            *pcs[s].v = vc[best];
        }
    }
    return matched;
}

void dp_gbs_norm_name(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (; *in && k + 1 < cap; in++)
        if (isalnum((unsigned char)*in)) out[k++] = (char)tolower((unsigned char)*in);
    out[k] = 0;
}

void dp_gbs_url_decode(const char *in, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        if (in[i] == '%' && i + 2 < n && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char hx[3] = { in[i + 1], in[i + 2], 0 };
            out[k++] = (char)strtol(hx, NULL, 16);
            i += 2;
        } else out[k++] = in[i];
    }
    out[k] = 0;
}

size_t dp_gbs_listing_pick(const char *html, const char *name, char *out, size_t cap) {
    char q[256]; dp_gbs_norm_name(name, q, sizeof q);
    if (!q[0]) return 0;
    size_t best_len = 0; char best_href[1024] = "";
    for (const char *p = html; (p = strstr(p, "href=\"")); p += 6) {
        const char *s = p + 6, *e = strchr(s, '"');
        if (!e || (size_t)(e - s) >= sizeof best_href) continue;
        size_t hl = (size_t)(e - s);
        if (hl < 4 || strncasecmp(s + hl - 3, ".7z", 3)) continue;
        char dec[1024]; dp_gbs_url_decode(s, hl, dec, sizeof dec);
        const char *base = strrchr(dec, '/'); base = base ? base + 1 : dec;
        char nn[256]; dp_gbs_norm_name(base, nn, sizeof nn);
        if (!nn[0]) continue;
        int exact = !strcmp(nn, q), has = strstr(nn, q) != NULL;
        if (!exact && !has) continue;
        if (exact) { best_len = hl; memcpy(best_href, s, hl); best_href[hl] = 0; break; }
        if (!best_len || hl < best_len) { best_len = hl; memcpy(best_href, s, hl); best_href[hl] = 0; }
    }
    if (best_len && cap) { snprintf(out, cap, "%s", best_href); return best_len; }
    return 0;
}
