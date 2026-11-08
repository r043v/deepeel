// test_map_infer.c: the padding-fringe inference and its pointer guard. A
// filler run beside padding is inferred — unless a plausible pointer (a word
// sitting in code or table bytes, either pointer form) aims inside it; the
// rejections are counted, the guard is the fixture's contract.
#include "test_util.h"
#include "../src/explore/explorer.h"
#include "../src/report/report.h"

#include <stdlib.h>
#include <string.h>

int main(void) {
    const int banks = 2;
    size_t n = (size_t)banks * 0x4000;
    static DpCart cart;
    static DpResults res;
    memset(&cart, 0, sizeof cart);
    memset(&res, 0, sizeof res);
    cart.rom = calloc(n, 1);
    cart.rom_size = n;
    cart.rom_banks = banks;
    res.cart = &cart;
    res.banks = banks;
    res.exec_flags = calloc(n, 1);
    res.data_flags = calloc(n, 1);
    res.static_flags = calloc(n, 1);
    // bank 0: code at $0100 (one byte flags a code class the pointer scan
    // reads), padding run at $0200-$021F, then two filler fringes beside it:
    //   $0220-$022F: 3/4 filler, no pointer aims at it -> inferred padding
    //   $0240-$024F: 3/4 filler, a banked pointer (lo,hi=bank 0) in the code
    //                word at $0100 aims at $0240 -> refused, still unknown
    uint8_t *rom = cart.rom;
    res.static_flags[0x0100] = DP_SF_POINTER; // the word sits in a table: pointers live there
    for (int i = 0x0200; i < 0x0220; i++) rom[i] = 0x00;   // padding run
    for (int i = 0x0200; i < 0x0220; i++) res.static_flags[i] = DP_SF_PAD;
    for (int i = 0x0220; i < 0x0230; i++) rom[i] = (i & 7) ? 0x00 : 0x92; // fringe: 3/4 filler
    for (int i = 0x0240; i < 0x0250; i++) rom[i] = (i & 7) ? 0x00 : 0x92; // aimed fringe
    // bound the unknown runs: the gap between the two fringes is padding,
    // the bank's tail is proven data
    for (int i = 0x0230; i < 0x0240; i++) { rom[i] = 0x00; res.static_flags[i] = DP_SF_PAD; }
    for (int i = 0x0250; i < 0x4000; i++) res.data_flags[i] = DP_DF_READ;
    rom[0x0100] = 0x40; rom[0x0101] = 0x02;   // bare word $0240: aims at bank 0's $0240
    DpMapSummary sum;
    uint8_t *m = dp_map_build(&res, &sum);
    CHECK(m != NULL);
    CHECK(m[0x0220] == DP_MAP_INF_PAD && m[0x022F] == DP_MAP_INF_PAD);   // free fringe inferred
    CHECK(m[0x0240] == DP_MAP_UNKNOWN && m[0x024F] == DP_MAP_UNKNOWN);   // aimed fringe refused
    CHECK(sum.pad_fringe_refused >= 1);                                  // the refusal is counted
    // the same word in bare in-bank form (own bank) also guards: bank 1 code
    // word $4100 aims at bank 1's $0240 through its own bank
    memset(rom + 0x0240, 0, 16);
    for (int i = 0x0240; i < 0x0250; i++) rom[i] = (i & 7) ? 0x00 : 0x92;
    for (int i = 0x4000 + 0x230; i < 0x4000 + 0x240; i++) { rom[i] = 0x00; res.static_flags[i] = DP_SF_PAD; }
    for (int i = 0x4000 + 0x250; i < 0x4000 + 0x4000; i++) res.data_flags[i] = DP_DF_READ;
    for (int i = 0x4000 + 0x240; i < 0x4000 + 0x250; i++) rom[i] = (i & 7) ? 0x00 : 0x92;
    rom[0x4100] = 0x40; rom[0x4101] = 0x02;   // the same bare word in bank 1 aims at its own $0240
    res.static_flags[0x4100] = DP_SF_POINTER;
    free(m);
    m = dp_map_build(&res, &sum);
    CHECK(m != NULL);
    CHECK(m[0x4000 + 0x240] == DP_MAP_UNKNOWN);  // the bare pointer guards its own bank
    free(m);
    free(res.exec_flags);
    free(res.data_flags);
    free(res.static_flags);
    free(cart.rom);
    TEST_DONE("test_map_infer");
}
