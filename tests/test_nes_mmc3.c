/* test_nes_mmc3.c: the MMC3 scanline IRQ (mapper 4) — the bridge raises the
 * mapper IRQ line through its scanline-counter approximation ($C000/$C001
 * latch, $E000/$E001 gate), the machine services it on the natural path and
 * the kernel's irq_alt fork holds the service off `delay` instructions.  The
 * handler's bytes end covered; the fork counters move. */
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
    snprintf(path, sizeof path, "%s/fix_nes_mmc3.nes", argv[1]);
    FILE *f = fopen(path, "rb");
    CHECK(f);
    static uint8_t rom[16 + 2 * 0x4000 + 0x2000];
    size_t rn = fread(rom, 1, sizeof rom, f);
    fclose(f);
    CHECK(rn == sizeof rom);
    char err[128] = "";
    DpMachine *m = dp_machine_nes(rom, rn, -1, err, sizeof err);   // auto: mapper 4 from the header
    CHECK(m);
    DpCoreConfig cfg;
    dp_core_config_default(&cfg);
    cfg.hooks = dp_machine_nes_hooks();
    cfg.max_total = 2000000;
    cfg.max_path_steps = 100000;
    cfg.no_sessions = 1;
    DpCoreResult r;
    CHECK(dp_core_explore(m, dp_machine_nes_prg_size(m), &cfg, &r));
    // the handler at CPU $8020 (rom index 0x20 under the reset view) ran: the
    // machine services the line naturally, the fork runs it held off too
    CHECK(r.irq_forks >= 1);
    CHECK(r.flags[0x20] & (DP_CORE_NATURAL | DP_CORE_FORCED));
    CHECK(r.steps > 0);
    printf("nes-mmc3: irq_forks %llu jitter %llu handler flags %02x\n",
           (unsigned long long)r.irq_forks, (unsigned long long)r.jitter_forks, r.flags[0x20]);
    dp_core_result_free(&r);
    m->close(m);
    return 0;
}
