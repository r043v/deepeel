// test_export.c: the playable export — a .STA written by dp_sta_write must
// read back the way m0boy's m0_state.cpp reads its slots (magic, format tag,
// FNV checksum over exactly the data bytes, size, title, data from sector 1).
#include "../src/core/sta.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t fnv(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

int main(void) {
    mkdir("test_state_a", 0755);
    mkdir("test_state_a/proj", 0755);

    // a generated pc project's gbl_config.h, only the defines the exporter
    // reads (a real one carries far more)
    FILE *f = fopen("test_state_a/proj/gbl_config.h", "w");
    CHECK(f != NULL);
    fprintf(f,
            "#define GBL_MBC 5\n"
            "#define GBL_ERAM_SIZE 8192\n"
            "#define GBL_SD_NAME \"TESTGAME\"\n"
            "#define GBL_STATE_NAME \"TESTGAME.STA\"\n"
            "#define GBL_STATE_SLOTS 10\n"
            "#define GBL_STATE_SLOT_SECTORS 53\n"
            "#define GBL_SAVE_NAME \"TESTGAME.SAV\"\n"
            "#define GBL_TITLE \"TEST GAME\"\n"
            "#define GBL_GEN_VERSION \"0.1-test\"\n");
    fclose(f);

    // the machine image: 18432 + 8192 bytes, like the variant's state
    enum { BYTES = 18432 + 8192 };
    static uint8_t img[BYTES];
    for (int i = 0; i < BYTES; i++) img[i] = (uint8_t)(i * 7 + i / 251);

    DpSta sta;
    char err[256];
    CHECK(dp_sta_from_project(&sta, "test_state_a/proj", BYTES, err, sizeof err));
    CHECK(!strcmp(sta.sta_path, "test_state_a/proj/TESTGAME.STA"));
    CHECK(!strcmp(sta.sav_path, "test_state_a/proj/TESTGAME.SAV"));
    CHECK(sta.slots == 10 && sta.sectors == 53);
    // the format tag, m0_state.cpp's recipe: FNV-1a over the version string,
    // then the little-endian state size
    uint32_t fmt = fnv(2166136261u, (const uint8_t *)"0.1-test", 8);
    uint8_t le[4] = { BYTES & 255, (BYTES >> 8) & 255, (BYTES >> 16) & 255, (BYTES >> 24) & 255 };
    fmt = fnv(fmt, le, sizeof le);
    CHECK(sta.format == fmt);

    CHECK(dp_sta_write(&sta, 0, img, BYTES, 12345, err, sizeof err));

    // read it back exactly like m0_state_load does
    static uint8_t slot[53 * 512];
    f = fopen(sta.sta_path, "rb");
    CHECK(f != NULL);
    CHECK(fread(slot, 1, sizeof slot, f) == sizeof slot);
    fclose(f);
    CHECK(!memcmp(slot, "GBLSTATE", 8));
    uint32_t hdr_fmt, hdr_bytes, hdr_ck, hdr_frame;
    memcpy(&hdr_fmt, slot + 8, 4);
    memcpy(&hdr_bytes, slot + 12, 4);
    memcpy(&hdr_ck, slot + 16, 4);
    memcpy(&hdr_frame, slot + 20, 4);
    CHECK(hdr_fmt == fmt);
    CHECK(hdr_bytes == BYTES);
    CHECK(hdr_frame == 12345);
    CHECK(!memcmp(slot + 24, "TEST GAME", 10) && slot[24 + 10] == 0);
    uint32_t ck = fnv(2166136261u, slot + 512, BYTES);
    CHECK(hdr_ck == ck);
    for (uint32_t i = 0; i < BYTES; i++) CHECK_MSG(slot[512 + i] == img[i], "data mismatch");
    // the data ends exactly on a sector boundary: nothing after it was touched
    // beyond the used area (53 sectors = 1 header + 52 data)

    // a second write lands in the same file, slot 0 overwritten
    img[0] ^= 0xFF;
    CHECK(dp_sta_write(&sta, 0, img, BYTES, 5, err, sizeof err));
    f = fopen(sta.sta_path, "rb");
    CHECK(f != NULL);
    CHECK(fread(slot, 1, sizeof slot, f) == sizeof slot);
    fclose(f);
    CHECK(slot[512] == img[0]);

    // a project whose slots cannot hold the image: refused
    mkdir("test_state_a/proj2", 0755);
    f = fopen("test_state_a/proj2/gbl_config.h", "w");
    fprintf(f,
            "#define GBL_STATE_NAME \"SMALL.STA\"\n"
            "#define GBL_STATE_SLOT_SECTORS 10\n"
            "#define GBL_GEN_VERSION \"0.1-test\"\n");
    fclose(f);
    DpSta bad;
    CHECK(!dp_sta_from_project(&bad, "test_state_a/proj2", BYTES, err, sizeof err));
    CHECK(!dp_sta_from_project(&bad, "test_state_a/proj_nonexistent", BYTES, err, sizeof err));
    CHECK(!dp_sta_from_project(&bad, "test_state_a", BYTES, err, sizeof err)); // no gbl_config.h

    TEST_DONE("test_export");
}
