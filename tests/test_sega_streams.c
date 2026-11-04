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
    {   // typed audio: PSG and YM2413 port writes show in the report
        static uint8_t rom3[0x8000];
        memset(rom3, 0xFF, sizeof rom3);
        const uint8_t au[] = {0x3E, 0x9F, 0xD3, 0x7F, 0xD3, 0xF1, 0x76};   // ld a,$9F; out ($7F),a; out ($F1),a; halt
        memcpy(rom3, au, sizeof au);
        DpSegaResults r3;
        memset(&r3, 0, sizeof r3);
        CHECK(dp_sega_explore(dp_sega_system("sms"), rom3, sizeof rom3, &o, &r3));
        FILE *tf = tmpfile();
        dp_sega_report(tf, dp_sega_system("sms"), &r3);
        rewind(tf);
        char buf[65536];
        size_t nb = fread(buf, 1, sizeof buf - 1, tf);
        buf[nb] = 0;
        fclose(tf);
        CHECK(strstr(buf, "audio (music data") && strstr(buf, "YM2413 FM 1"));
        dp_sega_results_free(&r3);
    }
    {   // RAM-code archive: code built in RAM by immediates, then called
        static uint8_t rom2[0x8000];
        memset(rom2, 0xFF, sizeof rom2);
        const uint8_t code[] = {0x21, 0x00, 0xC1, 0x36, 0x3E, 0x23, 0x36, 0x07, 0x23, 0x36, 0xC9,   // ld hl,$C100; ld (hl),$3E; inc hl; ld (hl),7; inc hl; ld (hl),$C9
                                0xCD, 0x00, 0xC1, 0x76};
        memcpy(rom2, code, sizeof code);
        DpSegaResults r2;
        memset(&r2, 0, sizeof r2);
        CHECK(dp_sega_explore(dp_sega_system("sms"), rom2, sizeof rom2, &o, &r2));
        int found = 0;
        for (uint32_t i = 0; i < r2.ra.cap; i++)
            if (r2.ra.e[i].key && r2.ra.e[i].len == 2 && r2.ra.e[i].pc == 0xC100 && r2.ra.e[i].b[0] == 0x3E && r2.ra.e[i].b[1] == 0x07) found = 1;
        CHECK(found);
        dp_sega_results_free(&r2);
    }
    TEST_DONE("sega_streams");
}
