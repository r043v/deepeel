/* test_nes_jmpind.c: the kernel's static closure on 6502 — a `jmp (abs)` whose
 * operand sits in the ROM window reads the word table that follows: each target
 * that starts a legal instruction becomes a static root (audit E5).  Entry 0
 * runs naturally; entry 1 is only covered by the static walk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/dpmachine.h"
#include "../src/mach/nesmach.h"
#include "../src/explore/core.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv) {
    CHECK(argc > 1);
    char path[512];
    snprintf(path, sizeof path, "%s/fix_nes_jmpind.nes", argv[1]);
    FILE *f = fopen(path, "rb");
    CHECK(f);
    static uint8_t rom[16 + 2 * 0x4000 + 0x2000];
    size_t rn = fread(rom, 1, sizeof rom, f);
    fclose(f);
    CHECK(rn == sizeof rom);
    char err[128] = "";
    DpMachine *m = dp_machine_nes(rom, rn, -1, err, sizeof err);
    CHECK(m);
    DpCoreConfig cfg;
    dp_core_config_default(&cfg);
    cfg.max_total = 200000;
    cfg.no_sessions = 1;
    DpCoreResult r;
    CHECK(dp_core_explore(m, dp_machine_nes_prg_size(m), &cfg, &r));
    // entry 0 ($8040, rom index 0x40) ran naturally; entry 1 ($8050, index 0x50)
    // is static-only: the table walk proved it without execution
    CHECK(r.flags[0x40] & DP_CORE_NATURAL);
    CHECK(r.flags[0x50] & DP_CORE_STATIC);
    CHECK(!(r.flags[0x50] & (DP_CORE_NATURAL | DP_CORE_FORCED)));
    CHECK(r.closure_jp_tables >= 1);
    printf("nes-jmpind: closure tables %llu entries %llu, entry0 %02x entry1 %02x\n",
           (unsigned long long)r.closure_jp_tables, (unsigned long long)r.closure_jp_entries,
           r.flags[0x40], r.flags[0x50]);
    dp_core_result_free(&r);
    m->close(m);
    // A10: the same table at an odd address ($8021) must be walked as well
    snprintf(path, sizeof path, "%s/fix_nes_jmpind_odd.nes", argv[1]);
    f = fopen(path, "rb");
    CHECK(f);
    rn = fread(rom, 1, sizeof rom, f);
    fclose(f);
    CHECK(rn == sizeof rom);
    m = dp_machine_nes(rom, rn, -1, err, sizeof err);
    CHECK(m);
    dp_core_config_default(&cfg);
    cfg.max_total = 200000; cfg.no_sessions = 1;
    CHECK(dp_core_explore(m, dp_machine_nes_prg_size(m), &cfg, &r));
    CHECK(r.flags[0x40] & DP_CORE_NATURAL);
    CHECK(r.flags[0x50] & DP_CORE_STATIC);
    dp_core_result_free(&r);
    m->close(m);
        // C12: the second controller has a read path: A held on port 2 shows in the first $4017 read, port 1 does not
    for (int port = 0; port < 2; port++) {
        snprintf(path, sizeof path, "%s/fix_nes_pad2.nes", argv[1]);
        f = fopen(path, "rb");
        CHECK(f);
        rn = fread(rom, 1, sizeof rom, f);
        fclose(f);
        m = dp_machine_nes(rom, rn, -1, err, sizeof err);
        CHECK(m && m->set_input);
        m->set_input(m, DP_IN_B1 << (port ? 0 : DP_IN_PORT2));
        for (int i = 0; i < 30; i++) CHECK(m->run(m));
        CHECK((m->fetch(m, 0) & 1) == (port ? 0 : 1));
        m->close(m);
    }
    // C14: arm_read makes the next read of $4017 return 1 whatever the pad says; consumed once
    {
        snprintf(path, sizeof path, "%s/fix_nes_pad2.nes", argv[1]);
        f = fopen(path, "rb");
        CHECK(f);
        rn = fread(rom, 1, sizeof rom, f);
        fclose(f);
        m = dp_machine_nes(rom, rn, -1, err, sizeof err);
        CHECK(m && m->arm_read);
        CHECK(m->arm_read(m, 0x4017, 0xFF));
        for (int i = 0; i < 30; i++) CHECK(m->run(m));
        CHECK(m->fetch(m, 0) == 0xFF);
        m->close(m);
    }
    return 0;
}
