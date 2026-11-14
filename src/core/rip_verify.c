/* rip_verify.c: the fragment-vote relocator (see rip_verify.h).  GBS hands over
 * to dp_gbs_parse/dp_gbs_verify (battle-tested per-bank match + fragments);
 * NSF/NSFE/KSS verify as one linear block; HES verifies per 8K bank through the
 * header's MPR page mapping.  An entry point is only mapped when its own bytes
 * relocated verbatim — heads that are the ripper's stub map nothing. */
#include "rip_verify.h"
#include "gbs.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define RV_FRAG 16
#define RV_NFRAG 12
#define RV_MIN 9          // fragments that must agree on the dominant delta
#define DP_RIP_NONE 0xFFFFFFFFu

static const uint8_t *rv_find(const uint8_t *hay, size_t hn, const uint8_t *needle, size_t nn) {
    if (!hn || nn > hn) return NULL;
    for (const uint8_t *p = hay; (size_t)(hay + hn - p) >= nn; ) {
        p = memchr(p, needle[0], (size_t)(hay + hn - p));
        if (!p || (size_t)(hay + hn - p) < nn) return NULL;
        if (!memcmp(p, needle, nn)) return p;
        p++;
    }
    return NULL;
}

/* a fragment of only 00/FF matches everywhere: it carries no evidence */
static int rv_trivial(const uint8_t *p) {
    for (int i = 0; i < RV_FRAG; i++)
        if (p[i] != 0x00 && p[i] != 0xFF) return 0;
    return 1;
}

/* dominant delta of one contiguous block inside the rom: >= RV_MIN of the
 * non-trivial sampled fragments must agree; returns 0 and sets *delta */
static int rv_vote(const uint8_t *d, size_t dn, const uint8_t *rom, size_t rn, uint32_t *delta) {
    if (dn < RV_FRAG * 4 || rn < dn) return 0;
    size_t off[RV_NFRAG];
    int nf = 0;
    for (int i = 0; i < RV_NFRAG; i++) {
        size_t o = (size_t)i * (dn - RV_FRAG) / (RV_NFRAG - 1);
        if (!rv_trivial(d + o)) off[nf++] = o;      // spread, but only real bytes vote
    }
    if (nf < 6) return 0;                           // padding is not an engine
    uint32_t cand[8];
    int nc = 0;
    for (int i = 0; i < nf && nc < 8; i++) {
        const uint8_t *p = rom;
        for (int k = 0; k < 4; k++) {          // candidate deltas from the first hits
            p = rv_find(p, (size_t)(rom + rn - p), d + off[i], RV_FRAG);
            if (!p) break;
            uint32_t c = (uint32_t)(p - rom) - (uint32_t)off[i];
            int seen = 0;
            for (int j = 0; j < nc; j++) seen |= cand[j] == c;
            if (!seen) cand[nc++] = c;
            p++;
        }
        if (nc >= 3) break;                    // enough candidates to test
    }
    int best = -1, bh = 0;
    for (int i = 0; i < nc; i++) {
        if (cand[i] > rn - dn) continue;
        int h = 0;
        for (int k = 0; k < nf; k++)
            h += !memcmp(rom + cand[i] + off[k], d + off[k], RV_FRAG);
        if (h > bh) { bh = h; best = i; }
    }
    if (best < 0 || bh * 4 < nf * 3 || bh < RV_MIN) return 0;
    *delta = cand[best];
    return 1;
}

static uint32_t rv_entry(const uint8_t *rom, uint32_t delta, const uint8_t *d, size_t dn,
                         uint32_t addr, uint32_t load) {
    if (addr == DP_RIP_NONE || addr < load || addr - load >= dn) return DP_RIP_NONE;
    uint32_t foff = addr - load, r = delta + foff;
    size_t n = dn - foff < RV_FRAG ? dn - foff : RV_FRAG;
    if (memcmp(rom + r, d + foff, n)) return DP_RIP_NONE;   // verbatim or nothing
    if (rv_trivial(rom + r)) return DP_RIP_NONE;            // an entry point is code, not filler
    return r;                                        // physical offset == dp_rom_index
}

static int rv_gbs(const uint8_t *rip, size_t n, const uint8_t *rom, size_t rn, DpRipMap *out) {
    DpGbs g; char err[96];
    if (dp_gbs_parse(rip, n, &g, err, sizeof err)) return -1;
    /* cheap screen before the heavy per-bank scan: a real relocation leaves
     * many 16-byte fragments in the dump — a rip without 3 of them is rejected
     * without the O(banks^2) byte compare (the library walk runs this on every
     * rip, it must stay cheap) */
    size_t found = 0;
    for (size_t k = 0; k < 8 && found < 3; k++) {
        size_t o = k * (g.data_size - RV_FRAG) / 7;
        if (rv_trivial(g.data + o)) continue;
        if (rv_find(rom, rn, g.data + o, RV_FRAG)) found++;
    }
    if (found < 3) return -1;
    DpGbsMatch m;
    memset(&m, 0, sizeof m);
    /* the fragment-only relocation (path B, 3 local votes) is too weak for an
     * unguided search over the whole library: require one aligned bank that
     * matched byte for byte (dp_gbs_verify's return) */
    if (!dp_gbs_verify(&g, rom, rn, 0.90, &m)) return -1;
    out->load = g.load;
    out->data_size = g.data_size;
    out->init_rom = m.init_bank >= 0 ? (uint32_t)m.init_bank * 0x4000u + (m.init_addr & 0x3FFF) : DP_RIP_NONE;
    out->play_rom = m.play_bank >= 0 ? (uint32_t)m.play_bank * 0x4000u + (m.play_addr & 0x3FFF) : DP_RIP_NONE;
    return 0;
}

static int rv_linear(const uint8_t *data, size_t dn, uint32_t load, uint32_t init, uint32_t play,
                     const uint8_t *rom, size_t rn, DpRipMap *out) {
    uint32_t delta;
    if (!rv_vote(data, dn, rom, rn, &delta)) return -1;
    out->load = load;
    out->data_size = dn;
    out->init_rom = rv_entry(rom, delta, data, dn, init, load);
    out->play_rom = rv_entry(rom, delta, data, dn, play, load);
    return 0;
}

int dp_rip_verify(const uint8_t *rip, size_t n, const uint8_t *rom, size_t rn, DpRipMap *out) {
    memset(out, 0xFF, sizeof *out);
    if (n < 16 || rn < 0x4000) return -1;
    if (!memcmp(rip, "GBS", 3))
        return rv_gbs(rip, n, rom, rn, out);
    if (!memcmp(rip, "NESM\x1a", 5) && n > 128) {          // NSF: 128-byte header
        /* the magic is 5 bytes: load 8-9, init 10-11, play 12-13 (LE) */
        uint32_t load = rip[8] | (rip[9] << 8), init = rip[10] | (rip[11] << 8),
                 play = rip[12] | (rip[13] << 8);
        return rv_linear(rip + 128, n - 128, load, init, play, rom, rn, out);
    }
    if (!memcmp(rip, "NSFE", 4)) {                         // NSFE: INFO + DATA chunks
        uint32_t load = DP_RIP_NONE, init = DP_RIP_NONE, play = DP_RIP_NONE;
        uint8_t *data = NULL;
        size_t dn = 0, cap = 0;
        for (uint32_t p = 4; p + 8 <= n; ) {
            uint32_t ln = rip[p] | (rip[p+1] << 8) | ((uint32_t)rip[p+2] << 16) | ((uint32_t)rip[p+3] << 24);
            if (!memcmp(rip + p + 4, "INFO", 4) && ln >= 8 && p + 8 + ln <= n) {
                load = rip[p+8] | (rip[p+9] << 8);
                init = rip[p+10] | (rip[p+11] << 8);
                play = rip[p+12] | (rip[p+13] << 8);
            } else if (!memcmp(rip + p + 4, "DATA", 4) && p + 8 + ln <= n) {
                if (dn + ln > cap) { cap = (dn + ln) * 2; uint8_t *nd = realloc(data, cap);
                    if (!nd) { free(data); return -1; } data = nd; }
                memcpy(data + dn, rip + p + 8, ln);
                dn += ln;
            }
            if (!memcmp(rip + p + 4, "NEND", 4)) break;
            p += 8 + ln;
        }
        int rc = -1;
        if (data && dn >= RV_FRAG * 4 && load != DP_RIP_NONE)
            rc = rv_linear(data, dn, load, init, play, rom, rn, out);
        free(data);
        return rc;
    }
    if (!memcmp(rip, "HESM", 4) && n > 0x20 + 0x2000) {    // HES: 8 banks of 8K, MPR at 0x10
        uint32_t init = rip[8] | (rip[9] << 8);            // first header vector, like the reader
        uint32_t delta[8];
        for (int b = 0; b < 8; b++) delta[b] = DP_RIP_NONE;
        int found = 0;
        for (int b = 0; b < 8; b++) {
            size_t o = 0x20 + (size_t)b * 0x2000;
            size_t bn = n - o < 0x2000 ? n - o : 0x2000;
            if (bn < RV_FRAG * 4) break;
            uint32_t d;
            if (rv_vote(rip + o, bn, rom, rn, &d)) { delta[b] = d; found = 1; }
        }
        if (!found) return -1;
        out->load = DP_RIP_NONE;
        out->data_size = n - 0x20;
        /* an HES address goes through the MPR page the header declares */
        uint32_t e = DP_RIP_NONE;
        if (init != DP_RIP_NONE) {
            uint8_t page = (uint8_t)(init >> 13);
            for (int b = 0; b < 8 && e == DP_RIP_NONE; b++) {
                if (rip[0x10 + b] != page || delta[b] == DP_RIP_NONE) continue;
                uint32_t foff = (uint32_t)b * 0x2000 + (init & 0x1FFF);
                size_t bn = n - 0x20 - foff < RV_FRAG ? n - 0x20 - foff : RV_FRAG;
                uint32_t r = delta[b] + (uint32_t)b * 0x2000 + (init & 0x1FFF);
                if (r + bn <= rn && !memcmp(rom + r, rip + 0x20 + foff, bn)) e = r;
            }
        }
        out->init_rom = e;
        out->play_rom = DP_RIP_NONE;                              // HES plays through the IRQ vector
        return 0;
    }
    if (!memcmp(rip, "KSSX", 4) && n > 0x10) {             // KSS: data at 0x10 (no extra header)
        uint32_t load = rip[6] | (rip[7] << 8), init = rip[8] | (rip[9] << 8);
        return rv_linear(rip + 0x10, n - 0x10, load, init, DP_RIP_NONE, rom, rn, out);
    }
    return -1;
}
