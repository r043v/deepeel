// lr_trial.c: trial of a real libretro core (level A) — save/load/replay determinism, memory, degradations.
// Build: gcc -Isrc tools/lr_trial.c build/libdeepeel_libretro.a -ldl -lm -lz -o lr_trial ; run: lr_trial CORE.so ROM
#include "mach/libretro.h"
#include <stdio.h>
#include <stdlib.h>
int main(int c, char **v) {
    char err[256] = "";
    DpLibretro *lr = dp_lr_open(v[1], err, sizeof err);
    if (!lr) { printf("open: %s\n", err); return 1; }
    if (!dp_lr_load_rom(lr, v[2], err, sizeof err)) { printf("rom: %s\n", err); return 1; }
    size_t n = dp_lr_state_size(lr);
    void *s = malloc(n);
    dp_lr_advance(lr, 30);
    dp_lr_save(lr, s, n);
    uint16_t m[20];
    for (int i = 0; i < 20; i++) m[i] = (i % 3) ? 0x80 : 0x08;
    uint64_t h1[20], h2[20];
    dp_lr_replay(lr, m, 20, h1);
    dp_lr_load(lr, s, n);
    dp_lr_replay(lr, m, 20, h2);
    int same = 1; for (int i = 0; i < 20; i++) if (h1[i] != h2[i]) same = 0;
    printf("core %s state %zu frames %llu replay identical: %d\ndeg: %s\n", dp_lr_core_name(lr), n, dp_lr_frames(lr), same, dp_lr_degradations(lr));
    size_t rs; const uint8_t *ram = dp_lr_memory(lr, 2, &rs);
    printf("sram/ram mem %p %zu\n", (void*)ram, rs);
    dp_lr_close(lr);
    return !same;
}
