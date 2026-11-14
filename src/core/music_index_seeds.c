/* music_index_seeds.c: the seed extraction shared by every platform CLI
 * (see music_index_seeds.h).  The tier walk and the entry-point projection
 * live here once so the platforms cannot drift apart; each caller keeps its
 * own acceptance (mapper view, range) on top. */
#include "music_index_seeds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // access(): the default index is optional
#include <zlib.h>     // crc32: the index is keyed by the dump's CRC32

int dp_music_seeds(const DpMusicIndex *ix, const char *const *plats, size_t nplat,
                   uint32_t crc, const char *norm_title, size_t rom_size,
                   DpMusicSeed **out, size_t *nout, DpMusicSeedStats *st) {
    memset(st, 0, sizeof *st);
    st->crc = crc;
    *out = NULL; *nout = 0;
    if (!ix->n_entry) return 0;
    size_t cap = ix->n_entry;
    uint8_t *seen = calloc(cap, 1);
    uint32_t *hits = malloc(cap * sizeof *hits);
    uint8_t *tiers = malloc(cap);
    DpMusicSeed *seeds = malloc(cap * 2 * sizeof *seeds);
    if (!seen || !hits || !tiers || !seeds) { free(seen); free(hits); free(tiers); free(seeds); return -1; }
    if (norm_title) snprintf(st->title, sizeof st->title, "%s", norm_title);
    int nh = 0, ns = 0;
    uint32_t first, count;
    /* tier 1: this exact dump's CRC is in the databases */
    if (!dp_midx_by_crc(ix, crc, &first, &count))
        for (uint32_t k = 0; k < count; k++)
            if (!seen[first + k]) { seen[first + k] = 1; hits[nh] = first + k; tiers[nh++] = 1; }
    /* tier 2: normalized title; the range indexes the refs array */
    if (st->title[0] && !dp_midx_by_title(ix, st->title, &first, &count))
        for (uint32_t k = 0; k < count; k++) {
            uint32_t e = dp_midx_title_ref(ix, first + k);
            if (!seen[e]) { seen[e] = 1; hits[nh] = e; tiers[nh++] = 2; }
        }
    /* tier 3: identity siblings — every hit's game_key pulls the other
     * dumps/regions/formats of the same game */
    for (int h = 0; h < nh; h++) {
        DpMusicRec hr; dp_midx_rec(ix, hits[h], &hr);
        if (hr.game_key == 0xFFFFFFFFu || dp_midx_by_game(ix, hr.game_key, &first, &count)) continue;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t e = dp_midx_game_ref(ix, first + k);
            if (!seen[e]) { seen[e] = 1; hits[nh] = e; tiers[nh++] = 3; }
        }
    }
    if (getenv("DEEPEEL_MIDX_DEBUG"))   // every hit with its tier, position and game key
        for (int h = 0; h < nh; h++) {
            DpMusicRec d; dp_midx_rec(ix, hits[h], &d);
            fprintf(stderr, "midx-dbg: h=%d tier=%u pos=%u gk=%u %s/%s irom=%u prom=%u\n",
                    h, tiers[h], hits[h], d.game_key, d.source, d.format, d.init_rom, d.play_rom);
        }
    for (int h = 0; h < nh; h++) {
        DpMusicRec r; dp_midx_rec(ix, hits[h], &r);
        int plat_ok = 0;
        for (size_t p = 0; p < nplat; p++) if (!strcmp(r.platform, plats[p])) plat_ok = 1;
        if (!plat_ok) { st->other_platform++; continue; }
        if (st->tier < tiers[h]) st->tier = tiers[h];
        if (r.init_rom == 0xFFFFFFFFu && r.play_rom == 0xFFFFFFFFu) { st->no_entry_point++; continue; }
        st->entries++;
        uint32_t roms[2] = { r.init_rom, r.play_rom };
        int any = 0, bad = 0;
        for (int s = 0; s < 2; s++) {
            if (roms[s] == 0xFFFFFFFFu) continue;
            if (roms[s] >= rom_size) { bad = 1; continue; }   // another dump's banking
            /* dp_rom_index -> bank + window address; bank 0 maps $0000-$3FFF directly */
            uint16_t bk = (uint16_t)(roms[s] >> 14);
            uint16_t pc = bk ? (uint16_t)(0x4000 + (roms[s] & 0x3FFF)) : (uint16_t)roms[s];
            int dup = 0;
            for (int i = 0; i < ns; i++) if (seeds[i].bank == bk && seeds[i].pc == pc) dup = 1;
            if (dup) continue;
            seeds[ns].rom = roms[s]; seeds[ns].pc = pc; seeds[ns].bank = bk;
            seeds[ns].kind = (uint8_t)(s == 0);        // s 0 is init (roms[0]): kind 1 init, 0 play
            seeds[ns].a = s == 0 ? 0 : 0xFF;           // init: song 0 in A; play: no register
            seeds[ns].sp = s == 0 && r.stack != 0xFFFF ? r.stack : 0xFFFF;   // init starts on the rip's stack
            seeds[ns].tier = tiers[h]; seeds[ns].entry = hits[h];
            ns++; any++;
            if (s == 0) st->init_seeds++;
        }
        if (bad && !any) st->out_of_range++;
        else st->seeds += any;
    }
    free(seen); free(hits); free(tiers);
    if (!ns) { free(seeds); return 0; }
    *out = seeds; *nout = (size_t)ns;
    return 0;
}

/* name of an identity tier, for the reports (0: nothing matched) */
const char *dp_music_seeds_tier_name(int tier) {
    return tier == 1 ? "crc" : tier == 2 ? "title" : tier == 3 ? "game" : "none";
}

int dp_music_seeds_cli(const char *midx_file, const char *const *plats, size_t nplat,
                       const uint8_t *raw, size_t raw_bytes, size_t rom_size,
                       const char *norm_title, DpMusicSeed **out, size_t *nout,
                       DpMusicSeedStats *st, DpMusicIndex *keep_ix) {
    const char *mp = (midx_file && midx_file[0]) ? midx_file : "output/music_index/music_index.bin";
    *out = NULL; *nout = 0;
    if (!midx_file && access(mp, F_OK) != 0) return DP_MUSIC_SEEDS_NO_FILE;
    DpMusicIndex ix; char merr[256];
    if (dp_midx_load(mp, &ix, merr, sizeof merr)) {
        fprintf(stderr, "deepeel: --music-index %s: %s\n", mp, merr);
        return DP_MUSIC_SEEDS_LOAD_ERR;
    }
    uint32_t crc = (uint32_t)crc32(0, raw, (uInt)raw_bytes);
    int rc = dp_music_seeds(&ix, plats, nplat, crc, norm_title, rom_size, out, nout, st);
    if (keep_ix) *keep_ix = ix; else dp_midx_free(&ix);
    if (rc) {
        fprintf(stderr, "deepeel: cannot allocate music-index seeds\n");
        return DP_MUSIC_SEEDS_ALLOC;
    }
    printf("music-index: crc %08X", st->crc);
    if (st->title[0]) printf(" title \"%s\"", st->title);
    printf(": %d entries, %zu seeds (%d init, song 0 in A), %d other platform, %d without entry point, %d out of range (tier %s)\n",
           st->entries, *nout, st->init_seeds, st->other_platform, st->no_entry_point, st->out_of_range,
           dp_music_seeds_tier_name(st->tier));
    return (int)*nout;
}
