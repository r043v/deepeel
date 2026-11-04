// A BizHawk 2.x GB input log names its keys "P1 Up|P1 Down|...": the controller prefix must not shift
// the characters of a frame (regression: the prefix used to become a key of its own).
#include "explore/tas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const char *key, const char *tag) {
    char path[256], err[256] = "";
    snprintf(path, sizeof path, "%s/tas_logkey_%d.txt", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)(size_t)tag[0]);
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    // frames: Up, Right+A, Start, Select+B, Down+Left
    fprintf(f, "[Input]\nLogKey:%s\n|U........|\n|...R...A.|\n|....S....|\n|.....sB..|\n|.DL......|\n[/Input]\n", key);
    fclose(f);
    DpTas t;
    memset(&t, 0, sizeof t);
    if (!dp_tas_load(path, &t, err, sizeof err) || t.frames != 5) { printf("%s: load failed (%s)\n", tag, err); remove(path); return 1; }
    const uint8_t want[5] = { 0x40, 0x11, 0x08, 0x06, 0xA0 };
    int bad = 0;
    for (int i = 0; i < 5; i++)
        if (t.masks[i] != want[i]) { printf("%s: frame %d mask %02x, want %02x\n", tag, i, t.masks[i], want[i]); bad = 1; }
    dp_tas_free(&t);
    remove(path);
    return bad;
}

int main(void) {
    int bad = 0;
    bad |= check("#Up|Down|Left|Right|Start|Select|B|A|Power|", "plain");
    bad |= check("#P1 Up|P1 Down|P1 Left|P1 Right|P1 Start|P1 Select|P1 B|P1 A|P1 Power|", "prefixed");
    return bad;
}
