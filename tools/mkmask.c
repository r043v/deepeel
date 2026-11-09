/* mkmask: dump the bench's per-frame input masks (tas.h layout) from a TAS
 * movie, using the very same loader as the bench (dp_sega_tas_load).  Any
 * ad-hoc re-parse of the movie log drifts on the LogKey column mapping
 * (system keys | P1 | P2 cells) — always extract through this tool. */
#include "tas.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mkmask movie.bk2 out.bin [frames]\n");
        return 2;
    }
    DpSegaTas t;
    char err[256];
    if (!dp_sega_tas_load(argv[1], &t, err, sizeof err)) {
        fprintf(stderr, "mkmask: %s\n", err);
        return 1;
    }
    long n = t.frames;
    if (argc > 3) {
        long cap = atol(argv[3]);
        if (cap < n) n = cap;
    }
    FILE *f = fopen(argv[2], "wb");
    if (!f) {
        fprintf(stderr, "mkmask: cannot write %s\n", argv[2]);
        dp_sega_tas_free(&t);
        return 1;
    }
    fwrite(t.masks, 1, (size_t)n, f);
    fclose(f);
    fprintf(stderr, "mkmask: %ld frames -> %s (source %s)\n", n, argv[2], t.source);
    dp_sega_tas_free(&t);
    return 0;
}
