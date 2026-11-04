// S6 on the Z80 family: a decoder called with HL from a pointer table; the stream the
// ROM never calls is read through the replay.
#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <string.h>

int main(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0xFF, sizeof rom);
    memset(rom, 0, 0x500);
    const uint8_t main_code[] = {0x21, 0x00, 0x03, 0xCD, 0x00, 0x02, 0x21, 0x10, 0x03, 0xCD, 0x00, 0x02, 0x76};
    memcpy(rom, main_code, sizeof main_code);
    const uint8_t dec[] = {0x7E, 0x23, 0xB7, 0x20, 0xFB, 0xC9};   // ld a,(hl); inc hl; or a; jr nz; ret
    memcpy(rom + 0x200, dec, sizeof dec);
    const uint8_t s0[] = {1, 2, 3, 0}, s1[] = {5, 0}, s2[] = {7, 8, 0};
    memcpy(rom + 0x300, s0, sizeof s0); memcpy(rom + 0x310, s1, sizeof s1); memcpy(rom + 0x330, s2, sizeof s2);
    const uint8_t table[] = {0x00, 0x03, 0x10, 0x03, 0x30, 0x03};
    memcpy(rom + 0x400, table, sizeof table);
    DpSegaOptions o = dp_sega_options();
    DpSegaResults r;
    memset(&r, 0, sizeof r);
    CHECK(dp_sega_explore(dp_sega_system("sms"), rom, sizeof rom, &o, &r));
    CHECK(r.s6_decoders >= 1);
    CHECK(r.s6_tables >= 1);
    CHECK(r.s6_runs >= 1);
    CHECK(r.flags[0x330] & DP_SEGA_DATA);
    dp_sega_results_free(&r);
    TEST_DONE("sega_streams");
}
