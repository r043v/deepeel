/* test_music_index.c: DPMDX002 reader (src/core/music_index.c) over a synthetic index
 * written to the build dir: layout walk, CRC32/title/game binary searches, entry
 * decode, header-title normalization.  The layout mirrors tools/music_index.py
 * write_binary — the real index is validated on real ROMs by the CLI. */
#include "core/music_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // unlink

static uint8_t out[1 << 16];
static size_t outn;
static void w32(uint32_t v) { out[outn++] = v; out[outn++] = v >> 8; out[outn++] = v >> 16; out[outn++] = v >> 24; }
static void w16(uint16_t v) { out[outn++] = v; out[outn++] = v >> 8; }
static void w8(uint8_t v) { out[outn++] = v; }
static void wbuf(const void *p, size_t n) { memcpy(out + outn, p, n); outn += n; }

static const char *POOL[] = {
    "bab_gbs2", "GB", "gbs2",            // 0, 1, 2
    "joshw_gbs", "gbs",                  // 3, 4
    "bab_nsf", "NES", "nsf",             // 5, 6, 7
    "local_rips",                        // 8
    "supermarioland", "tetris",          // 9, 10
    "raw0", "raw1", "raw2", "raw3",      // 11..14
    "cols", "origin",                    // 15, 16 (shared)
};
#define NP (int)(sizeof POOL / sizeof *POOL)
static uint32_t poff[NP];

/* the four entries: sorted by crc then id; entry 3 has no crc and trails */
static void entry(uint32_t gk, int src, int plat, int fmt,
                  uint32_t init_rom, uint32_t play_rom, int raw) {
    w32(gk);
    w32(poff[src]); w32(poff[plat]); w32(poff[fmt]);
    w16(0x65E4); w16(0x65E4); w16(0x6662); w16(0xFFFE);   // load/init/play/stack
    w8(0x07); w8(0x04); w16(18);                          // tma/tac/songs
    w32(init_rom); w32(play_rom); w32(0xFFFFFFFF);        // init_rom/play_rom/s_addr
    w32(0); w32(0);                                       // chunks
    w32(poff[raw]); w32(poff[15]); w32(poff[16]);
}

#define M 0xFFFFFFFFu

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_music_index DIR\n"); return 2; }
    /* pool offsets: concatenate the strings NUL-terminated */
    size_t pool_at = 0;
    for (int i = 0; i < NP; i++) { poff[i] = (uint32_t)pool_at; pool_at += strlen(POOL[i]) + 1; }

    outn = 0;
    wbuf("DPMDX002", 8);
    w32(2); w32(4); w32(0); w32(NP); w32((uint32_t)pool_at); w32(2); w32(1);
    /* crc index (numeric order): 0x11223344 -> entry 0; 0x90776841 -> entries 1,2 */
    w32(0x11223344); w32(0); w32(1);
    w32(0x90776841); w32(1); w32(2);
    entry(7, 0, 1, 2, M, 0xE760, 11);          // entry 0: no init_rom
    entry(7, 3, 1, 4, 0xE6E2, 0xE760, 12);     // entry 1: both, bank 3 $66E2/$6760
    entry(M, 5, 6, 7, 0x10, M, 13);            // entry 2: NES, unlinked
    entry(7, 8, 1, 4, M, M, 14);               // entry 3: crc-less, no entry points
    /* title index: all groups first, then all refs — "supermarioland" -> [0,1,3],
     * "tetris" -> [2] */
    w32(poff[9]); w32(0); w32(3);
    w32(poff[10]); w32(3); w32(1);
    w32(0); w32(1); w32(3); w32(2);
    /* game index: one group (key 7 -> entries [0,1,3]) then its refs */
    w32(7); w32(0); w32(3);
    w32(0); w32(1); w32(3);
    for (int i = 0; i < NP; i++) wbuf(POOL[i], strlen(POOL[i]) + 1);

    char path[1024];
    snprintf(path, sizeof path, "%s/test_music_index.bin", argv[1]);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(out, 1, outn, f) != outn) { fprintf(stderr, "cannot write %s\n", path); return 2; }
    fclose(f);

    int bad = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); bad = 1; } } while (0)

    DpMusicIndex ix; char err[256];
    CHECK(dp_midx_load(path, &ix, err, sizeof err) == 0);
    CHECK(ix.n_crc == 2 && ix.n_entry == 4 && ix.n_title == 2 && ix.n_game == 1);

    uint32_t first, count;
    CHECK(dp_midx_by_crc(&ix, 0x11223344, &first, &count) == 0 && first == 0 && count == 1);
    CHECK(dp_midx_by_crc(&ix, 0x90776841, &first, &count) == 0 && first == 1 && count == 2);
    CHECK(dp_midx_by_crc(&ix, 0xDEADBEEF, &first, &count) != 0);

    CHECK(dp_midx_by_title(&ix, "supermarioland", &first, &count) == 0 && count == 3);
    CHECK(dp_midx_by_title(&ix, "tetris", &first, &count) == 0 && count == 1);
    uint32_t tref;
    memcpy(&tref, ix.title_refs + (size_t)first * 4, 4);
    CHECK(tref == 2);
    CHECK(dp_midx_by_title(&ix, "zelda", &first, &count) != 0);

    CHECK(dp_midx_by_game(&ix, 7, &first, &count) == 0 && count == 3);
    CHECK(dp_midx_game_ref(&ix, first) == 0 && dp_midx_game_ref(&ix, first + 1) == 1 &&
          dp_midx_game_ref(&ix, first + 2) == 3);
    CHECK(dp_midx_by_game(&ix, 9, &first, &count) != 0);

    DpMusicRec r;
    dp_midx_rec(&ix, 1, &r);
    CHECK(r.game_key == 7 && !strcmp(r.platform, "GB") && !strcmp(r.source, "joshw_gbs"));
    CHECK(r.init_rom == 0xE6E2 && r.play_rom == 0xE760);
    CHECK(r.init == 0x65E4 && r.play == 0x6662 && r.songs == 18 && r.tma == 0x07 && r.tac == 0x04);
    CHECK(!strcmp(r.origin, "origin") && !strcmp(r.raw, "raw1"));
    dp_midx_rec(&ix, 2, &r);
    CHECK(r.game_key == M && !strcmp(r.platform, "NES"));
    /* the deepeel conversion: dp_rom_index -> bank + window address (entry 1) */
    dp_midx_rec(&ix, 1, &r);
    CHECK((r.init_rom >> 14) == 3 && 0x4000 + (r.init_rom & 0x3FFF) == 0x66E2);

    char norm[32];
    dp_midx_norm_title((const uint8_t *)"SUPER MARIOLAND\0X", norm, sizeof norm);
    CHECK(!strcmp(norm, "supermarioland"));
    dp_midx_norm_title((const uint8_t *)"ZELDA", norm, sizeof norm);
    CHECK(!strcmp(norm, "zelda"));
    dp_midx_norm_title((const uint8_t *)"TETRIS (J)", norm, sizeof norm);
    CHECK(!strcmp(norm, "tetrisj"));
    dp_midx_norm_title((const uint8_t *)"\x8c\x9c\x8b\xd6", norm, sizeof norm);   // Shift-JIS bytes: no ASCII alnum
    CHECK(norm[0] == 0);

    dp_midx_free(&ix);

    /* a truncated file must fail at load, never later */
    FILE *t = fopen(path, "wb");
    fwrite(out, 1, outn / 2, t);
    fclose(t);
    CHECK(dp_midx_load(path, &ix, err, sizeof err) != 0);
    unlink(path);

    printf(bad ? "test_music_index: FAILED\n" : "test_music_index: ok\n");
    return bad;
}
