/* music_index_seeds.c: the seed extraction shared by every platform CLI
 * (see music_index_seeds.h).  The tier walk and the entry-point projection
 * live here once so the platforms cannot drift apart; each caller keeps its
 * own acceptance (mapper view, range) on top. */
#include "music_index_seeds.h"
#include "rip_verify.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>   // access(): the default index is optional
#include <zlib.h>     // crc32: the index is keyed by the dump's CRC32

int dp_music_seeds(const DpMusicIndex *ix, const char *const *plats, size_t nplat,
                   uint32_t crc, const char *norm_title, size_t rom_size,
                   DpMusicSeed **out, size_t *nout, DpMusicSeedStats *st) {
    return dp_music_seeds_full(ix, plats, nplat, crc, norm_title, rom_size, NULL, out, nout, st);
}

/* runtime fallback: verify one rip file against the dump, append its seeds,
 * cache the mapping in the rip's .verified sidecar */
static int autoverify_one(const char *rip_path, uint32_t crc, const DpMusicSeedCtx *ctx,
                          uint32_t entry, DpMusicSeed **out, size_t *nout, size_t *cap,
                          DpMusicSeedStats *st) {
    FILE *f = fopen(rip_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= 16 || fseek(f, 0, SEEK_SET)) { fclose(f); return 0; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return 0; }
    fclose(f);
    DpRipMap map;
    if (dp_rip_verify(buf, (size_t)n, ctx->raw, ctx->raw_size, &map)) {
        free(buf);
        return 0;                          // the block is not in this dump
    }
    int added = 0;
    struct { uint32_t rom; uint8_t kind, a; } pts[2] = {
        { map.init_rom, 1, 0 }, { map.play_rom, 0, 0xFF },
    };
    for (int p = 0; p < 2; p++) {
        if (pts[p].rom == DP_RIP_NONE || pts[p].rom >= ctx->raw_size) continue;
        uint16_t bk = (uint16_t)(pts[p].rom >> 14);
        uint16_t pc = bk ? (uint16_t)(0x4000 + (pts[p].rom & 0x3FFF)) : (uint16_t)pts[p].rom;
        int dup = 0;
        for (size_t i = 0; i < *nout; i++) dup |= (*out)[i].bank == bk && (*out)[i].pc == pc;
        if (dup) continue;
        if (*nout == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            DpMusicSeed *no = realloc(*out, *cap * sizeof **out);
            if (!no) break;
            *out = no;
        }
        (*out)[*nout] = (DpMusicSeed){ pts[p].rom, pc, bk, 0xFFFF, pts[p].a, pts[p].kind, 1, entry };
        (*nout)++;
        added++;
        if (pts[p].kind == 1) st->init_seeds++;
    }
    free(buf);
    if (!added) return 0;
    st->auto_seeds += added;
    st->seeds += added;
    /* cache the mapping: the next music_index.py build ingests this sidecar */
    char vpath[1200];
    snprintf(vpath, sizeof vpath, "%s.verified", rip_path);
    char needle[32];
    snprintf(needle, sizeof needle, "crc=%08x ", crc);
    int seen = 0;
    FILE *rf = fopen(vpath, "r");
    if (rf) { char ln[600]; while (fgets(ln, sizeof ln, rf)) if (strstr(ln, needle)) seen = 1; fclose(rf); }
    if (!seen) {
        FILE *vf = fopen(vpath, "a");
        if (vf) {
            fprintf(vf, "dump crc=%08x data_size=%ld", crc, n);
            if (map.load != DP_RIP_NONE) fprintf(vf, " load=%u", map.load);
            if (map.init_rom != DP_RIP_NONE) fprintf(vf, " init_rom=%u", map.init_rom);
            if (map.play_rom != DP_RIP_NONE) fprintf(vf, " play_rom=%u", map.play_rom);
            fprintf(vf, " rom=%s\n", ctx->rom_name ? ctx->rom_name : "runtime");
            fclose(vf);
        }
    }
    return added;
}

/* last resort: walk the rip library and let the byte vote decide — the name
 * only bounds the work, the evidence is the relocated block.  Depth-capped
 * directory walk; stops at the first rip that seeds this dump. */
static int brute_scan(const char *dir, int depth, const char *const *exts,
                      uint32_t crc, const DpMusicSeedCtx *ctx,
                      DpMusicSeed **out, size_t *nout, size_t *cap, DpMusicSeedStats *st) {
    if (depth > 6) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int hit = 0;
    while (!hit && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char pth[1200];
        snprintf(pth, sizeof pth, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (stat(pth, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { hit = brute_scan(pth, depth + 1, exts, crc, ctx, out, nout, cap, st); continue; }
        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        int ok = 0;
        for (size_t i = 0; exts[i] && !ok; i++) ok = !strcmp(dot, exts[i]);
        if (!ok) continue;
        hit = autoverify_one(pth, crc, ctx, DP_RIP_NONE, out, nout, cap, st);
    }
    closedir(d);
    return hit;
}

int dp_music_seeds_full(const DpMusicIndex *ix, const char *const *plats, size_t nplat,
                        uint32_t crc, const char *norm_title, size_t rom_size,
                        const DpMusicSeedCtx *ctx, DpMusicSeed **out, size_t *nout,
                        DpMusicSeedStats *st) {
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
    /* tier 1: this exact dump's CRC is in the databases; the pairs map to the
     * entry array (an entry can carry several dump CRCs) */
    if (!dp_midx_by_crc(ix, crc, &first, &count))
        for (uint32_t k = 0; k < count; k++) {
            uint32_t e = dp_midx_crc_pos(ix, first + k);
            if (!seen[e]) { seen[e] = 1; hits[nh] = e; tiers[nh++] = 1; }
        }
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
    int nrip = 0;
    const char **rips = ctx ? malloc((size_t)nh * sizeof *rips) : NULL;
    uint32_t *rip_ent = ctx ? malloc((size_t)nh * sizeof *rip_ent) : NULL;
    int alloc_fail2 = ctx && (!rips || !rip_ent);
    for (int h = 0; h < nh; h++) {
        DpMusicRec r; dp_midx_rec(ix, hits[h], &r);
        int plat_ok = 0;
        for (size_t p = 0; p < nplat; p++) if (!strcmp(r.platform, plats[p])) plat_ok = 1;
        if (!plat_ok) { st->other_platform++; continue; }
        if (!st->tier || tiers[h] < st->tier) st->tier = tiers[h];   // the strongest identity that matched (1 crc beats 3 game)
        /* rip-file entries (the local library) are the runtime fallback's input */
        if (ctx && !strncmp(r.origin, "local:", 6)) { rips[nrip] = r.origin + 6; rip_ent[nrip++] = hits[h]; }
        else if (ctx && !strncmp(r.origin, "joshw:", 6)) { rips[nrip] = strchr(r.origin, '!') + 1; rip_ent[nrip++] = hits[h]; }
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
    if (ctx && !alloc_fail2 && !ns && nrip) {
        /* no seed in the index: verify the matched rip files against this dump
         * right now — ROM + rip is enough, and the sidecar caches the result */
        size_t cap = 0;
        for (int q = 0; q < nrip; q++)
            for (size_t d = 0; d < ctx->n_dirs; d++) {
                char pth[1200];
                snprintf(pth, sizeof pth, "%s/%s", ctx->rip_dirs[d], rips[q]);
                if (access(pth, R_OK)) continue;
                autoverify_one(pth, crc, ctx, rip_ent[q], out, nout, &cap, st);
                break;
            }
    }
    free(rips); free(rip_ent);
    if (ctx && !ns && !*nout) {
        /* the index knows nothing for this dump: scan the local rip library —
         * the byte vote is the evidence, the extension only bounds the walk */
        const char *exts_gb[] = { ".gbs", NULL }, *exts_nes[] = { ".nsf", ".nsfe", NULL },
                   *exts_pce[] = { ".hes", NULL }, *exts_kss[] = { ".kss", NULL };
        const char *const *exts = exts_kss;
        for (size_t p = 0; p < nplat; p++) {
            if (!strcmp(plats[p], "GB")) exts = exts_gb;
            else if (!strcmp(plats[p], "NES")) exts = exts_nes;
            else if (!strcmp(plats[p], "PCE")) exts = exts_pce;
        }
        size_t cap = 0;
        for (size_t d = 0; d < ctx->n_dirs && !*nout; d++)
            brute_scan(ctx->rip_dirs[d], 0, exts, crc, ctx, out, nout, &cap, st);
    }
    if (!ns && !*nout) { free(seeds); return 0; }
    if (ns) { *out = seeds; *nout = (size_t)ns; }
    else free(seeds);
    return 0;
}

/* name of an identity tier, for the reports (0: nothing matched) */
const char *dp_music_seeds_tier_name(int tier) {
    return tier == 1 ? "crc" : tier == 2 ? "title" : tier == 3 ? "game" : "none";
}

/* the pc of one seed under the machine's own view: the 16 KiB window whose
 * mapping shows the cell (both 8 KiB halves on the 8 KiB-slot boards), 0 when
 * no window does (the caller re-banks to win_base), -1 without rom_index */
static int64_t ks_seed_pc(const DpMachine *m, uint32_t rom) {
    if (!m->rom_index) return -1;
    unsigned off = rom & 0x3FFFu, base_off = rom & ~0x3FFFu;
    for (unsigned base = 0; base < 0x10000u; base += 0x4000u) {
        if (m->rom_index((DpMachine *)m, base + off) != (int64_t)rom) continue;
        if (m->rom_index((DpMachine *)m, base + (off ^ 0x2000u)) != (int64_t)(base_off | (off ^ 0x2000u))) continue;
        return base + off;
    }
    return 0;
}

int dp_music_seeds_kernel(DpMusicKernelSeeds *ks, const DpMusicSeed *seeds, size_t n,
                          unsigned win_base, const DpMachine *m) {
    memset(ks, 0, sizeof *ks);
    if (!n) return 0;
    ks->pcs = malloc(n * sizeof *ks->pcs);
    ks->banks = malloc(n * sizeof *ks->banks);
    ks->regs = malloc(n * sizeof *ks->regs);
    if (!ks->pcs || !ks->banks || !ks->regs) { dp_music_seeds_kernel_free(ks); return -1; }
    for (size_t i = 0; i < n; i++) {
        if (seeds[i].rom == DP_RIP_NONE) continue;   // a runtime seed the verifier could not place
        uint32_t pc; uint16_t bank;
        if (m) {
            int64_t v = ks_seed_pc(m, seeds[i].rom);
            if (v < 0) { ks->skipped++; continue; }   // no rom_index: cannot resolve
            if (v > 0) { pc = (uint32_t)v; bank = 0; }   // the dump's own view already shows it
            else { pc = win_base + (seeds[i].rom & 0x3FFFu); bank = (uint16_t)(seeds[i].rom >> 14); }
        } else {
            pc = win_base + (seeds[i].rom & 0x3FFFu);
            bank = (uint16_t)(seeds[i].rom >> 14);
        }
        ks->pcs[ks->n] = pc;
        ks->banks[ks->n] = bank;
        ks->regs[ks->n].a = seeds[i].a;
        ks->regs[ks->n].sp = seeds[i].sp;
        ks->n++;
    }
    return 0;
}

void dp_music_seeds_kernel_free(DpMusicKernelSeeds *ks) {
    free(ks->pcs); free(ks->banks); free(ks->regs);
    memset(ks, 0, sizeof *ks);
}

int dp_music_seeds_cli(const char *midx_file, const char *const *plats, size_t nplat,
                       const uint8_t *raw, size_t raw_bytes, size_t rom_size,
                       const char *norm_title, DpMusicSeed **out, size_t *nout,
                       DpMusicSeedStats *st, DpMusicIndex *keep_ix, const DpMusicSeedCtx *ctx) {
    const char *mp = (midx_file && midx_file[0]) ? midx_file : "output/music_index/music_index.bin";
    *out = NULL; *nout = 0;
    if (!midx_file && access(mp, F_OK) != 0) return DP_MUSIC_SEEDS_NO_FILE;
    DpMusicIndex ix; char merr[256];
    if (dp_midx_load(mp, &ix, merr, sizeof merr)) {
        fprintf(stderr, "deepeel: --music-index %s: %s\n", mp, merr);
        return DP_MUSIC_SEEDS_LOAD_ERR;
    }
    uint32_t crc = (uint32_t)crc32(0, raw, (uInt)raw_bytes);
    int rc = dp_music_seeds_full(&ix, plats, nplat, crc, norm_title, rom_size, ctx, out, nout, st);
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
    if (st->auto_seeds)
        printf("music-index: runtime verification of the local rips seeded %d more point(s), cached in the sidecars\n",
               st->auto_seeds);
    return (int)*nout;
}
