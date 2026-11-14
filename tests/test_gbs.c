// test_gbs: GBS rip parsing and verification (src/core/gbs.c): header fields, per-bank
// best match of the data block against the fixture ROM (a patched rip must still locate
// its bank and leave the zero-filled bank far behind), init/play mapping to
// {bank, address}, player stubs left unmapped, and the gbs.joshw.info index helpers.
#include "../src/core/gbs.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void read_file(const char *dir, const char *name, uint8_t **buf, size_t *n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    *buf = malloc((size_t)l);
    CHECK(*buf);
    CHECK(fread(*buf, 1, (size_t)l, f) == (size_t)l);
    fclose(f);
    *n = (size_t)l;
}

int main(int argc, char **argv) {
    CHECK(argc > 1);
    uint8_t *rom, *gbsbuf;
    size_t rn, gn;
    read_file(argv[1], "fix_gbs.gb", &rom, &rn);
    read_file(argv[1], "fix_gbs.gbs", &gbsbuf, &gn);
    DpGbs g;
    char err[256];
    CHECK(dp_gbs_parse(gbsbuf, gn, &g, err, sizeof err) == 0);
    CHECK(g.version == 1 && g.songs == 2 && g.first == 1);
    CHECK(g.load == 0x4000 && g.init == 0x4100 && g.play == 0x4200);
    CHECK(g.data_size == 0x4000);
    CHECK(!strcmp(g.title, "FIXT ENGINE"));

    DpGbsMatch m;
    unsigned nm = dp_gbs_verify(&g, rom, rn, 0.9, &m);
    CHECK(nm == 1);
    CHECK(m.rip_banks == 1);
    CHECK(m.bank[0].matched && m.bank[0].rom_bank == 1);
    CHECK(m.bank[0].best == m.bank[0].bytes - 16);   // the rip's patched bytes are the only mismatch
    CHECK(m.bank[0].second < m.bank[0].bytes / 2);   // the pattern-fill bank is far ahead of anything else
    CHECK(m.init_bank == 1 && m.init_addr == 0x4100);
    CHECK(m.play_bank == 1 && m.play_addr == 0x4200);

    // a ROM without bank 1 matches nothing
    DpGbsMatch m2;
    CHECK(dp_gbs_verify(&g, rom, 0x4000, 0.9, &m2) == 0);
    CHECK(!m2.bank[0].matched);

    // a player stub address below the load point stays unmapped, init still maps
    DpGbs g2 = g; g2.play = 0x0100;
    DpGbsMatch m3;
    CHECK(dp_gbs_verify(&g2, rom, rn, 0.9, &m3) == 1);
    CHECK(m3.play_bank == -1 && m3.init_bank == 1);

    // fragment relocation: the engine bytes moved to another load point (no aligned bank
    // can match) are relocated by the vote of the surrounding fragments
    static uint8_t rip3[112 + 0x3000];
    DpGbs g3;
    memcpy(rip3, gbsbuf, 112);
    rip3[6] = 0x10; rip3[7] = 0x60;              // load $6010
    rip3[8] = 0x20; rip3[9] = 0x60;              // init $6020: data offset $10, rom $4100
    rip3[10] = 0x20; rip3[11] = 0x61;            // play $6120: data offset $110, rom $4200
    memcpy(rip3 + 112, rom + 0x40F0, 0x3000);
    CHECK(dp_gbs_parse(rip3, sizeof rip3, &g3, err, sizeof err) == 0);
    DpGbsMatch m4;
    CHECK(dp_gbs_verify(&g3, rom, rn, 0.9, &m4) == 0);   // the extract is $F0-shifted: no aligned match
    CHECK(m4.frag_scanned && m4.frag_dom_votes >= 3 && m4.frag_dom_delta == 0x40F0);
    CHECK(m4.init_bank == 1 && m4.init_addr == 0x4100);
    CHECK(m4.play_bank == 1 && m4.play_addr == 0x4200);

    // bad magic and truncated headers are refused
    CHECK(dp_gbs_parse(gbsbuf, 40, &g, err, sizeof err) != 0);
    CHECK(dp_gbs_parse(gbsbuf + 1, gn - 1, &g, err, sizeof err) != 0);

    // index picking: exact normalized match wins, containment picks the shortest
    static const char html[] =
        "<a href=\"Super%20Mario%20Land%202%20-%206%20Golden%20Coins%20%281992-10-21%29%28Nintendo%29%5BGB%5D.7z\">sml2</a>\n"
        "<a href=\"Super%20Mario%20Land%20%281989-04-21%29%28Nintendo%20RD1%29%28Nintendo%29%5BGB%5D.7z\">sml</a>\n"
        "<a href=\"Solar%20Striker%20%281990%29%28Nintendo%29%5BGB%5D.7z\">solar</a>\n";
    char out[1024];
    CHECK(dp_gbs_listing_pick(html, "Super Mario Land", out, sizeof out));
    CHECK(strstr(out, "Land%20%281989"));
    CHECK(dp_gbs_listing_pick(html, "Super Mario Land 2", out, sizeof out));
    CHECK(strstr(out, "Golden%20Coins"));
    CHECK(!dp_gbs_listing_pick(html, "Tetris", out, sizeof out));
    char dec[1024];
    dp_gbs_url_decode("Super%20Mario%20Land%20%281989%29", 33, dec, sizeof dec);
    CHECK(!strcmp(dec, "Super Mario Land (1989)"));
    char nn[64];
    dp_gbs_norm_name("Super Mario Land (World)!", nn, sizeof nn);
    CHECK(!strcmp(nn, "supermariolandworld"));
    TEST_DONE("gbs");
}
