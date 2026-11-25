// tasshot.c: the TAS replay summary picture. The kernel hands over the key screens of the unshifted replay
// (any machine: the screen is just a w*h ARGB buffer); this file composes them on a DpCanvas with the movie's
// identity and a verdict, then writes one PNG of at most 1920x1080 that can be judged at a glance.
#include "tasshot.h"
#include "../gui/canvas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MAXSHOT = 12 };
typedef struct { uint64_t frame; char tag[16]; int w, h; uint32_t *px; } Shot;
static Shot shots[MAXSHOT]; static int nshots;

void dp_tasshot_capture(uint64_t frame, const char *tag, const uint32_t *px, int w, int h) {
    if (nshots >= MAXSHOT || w < 1 || h < 1) return;
    Shot *s = &shots[nshots]; s->px = malloc((size_t)w * h * 4); if (!s->px) return;
    memcpy(s->px, px, (size_t)w * h * 4); s->w = w; s->h = h; s->frame = frame; snprintf(s->tag, sizeof s->tag, "%s", tag); nshots++;
}

static int distinct(const uint32_t *p, int n) {   // number of different colours among the first 4096 samples, capped at 8
    uint32_t seen[8]; int k = 0;
    for (int i = 0; i < n && i < 4096 * 4 && k < 8; i += 4) { int f = 0; for (int j = 0; j < k; j++) if (seen[j] == p[i]) { f = 1; break; } if (!f) seen[k++] = p[i]; }
    return k;
}

int dp_tasshot_finish(const char *path, const DpTasSummary *s) {
    enum { W = 1920, H = 1080, HEAD = 122, FOOT = 40 };
    DpCanvas *c = dp_canvas_new(W, H, 0xff15151au); if (!c) return 1;
    // verdict: judged on what the replay did, never on a reference emulator (none is run here)
    const char *verdict, *why; uint32_t vc;
    int frozen = nshots >= 2 && shots[nshots - 1].w == shots[nshots - 2].w && shots[nshots - 1].h == shots[nshots - 2].h &&
                 !memcmp(shots[nshots - 1].px, shots[nshots - 2].px, (size_t)shots[nshots - 1].w * shots[nshots - 1].h * 4);
    int blank = nshots && distinct(shots[nshots - 1].px, shots[nshots - 1].w * shots[nshots - 1].h) < 3;
    if (!nshots) { verdict = "NOT JUDGED: NO SCREEN"; why = "this machine exposes no screen to capture"; vc = 0xffe0b030u; }
    else if (s->stalled) { verdict = "DESYNC / STAGNATION"; why = "no new ROM byte for the stall window: the movie is desynced or idle"; vc = 0xffe05050u; }
    else if (s->end < s->frames) { verdict = "REPLAY CUT SHORT"; why = "the replay ended before the movie's last frame"; vc = 0xffe05050u; }
    else if (blank) { verdict = "SUSPECT: BLANK FINAL SCREEN"; why = "the movie reached its end but the last screen shows almost nothing"; vc = 0xffe0b030u; }
    else if (frozen) { verdict = "SUSPECT: FROZEN SCREEN"; why = "the last two key screens are identical (movie may be idle at the end, or stuck)"; vc = 0xffe0b030u; }
    else { verdict = "OK"; why = "reached the last frame, screens change, ROM coverage kept growing; sync against the source emulator is not proven"; vc = 0xff40d060u; }
    char l[400];
    snprintf(l, sizeof l, "%s", s->name ? s->name : "movie"); dp_canvas_text(c, 12, 6, l, 2, 0xffffffffu);
    snprintf(l, sizeof l, "frames %u (replayed to %llu)  source emulator: %s  rom: %s", s->frames, (unsigned long long)s->end, s->emu && *s->emu ? s->emu : "unresolved (deepeel tasdb sync)", s->rom ? s->rom : "?");
    dp_canvas_text(c, 12, 28, l, 1 + (dp_canvas_text_w(l, 2) < 1180), 0xffc8c8d0u);
    snprintf(l, sizeof l, "deepeel %s  compatibility profile: %s", DEEPEEL_VERSION, s->profile && *s->profile ? s->profile : "default");
    dp_canvas_text(c, 12, 50, l, 1 + (dp_canvas_text_w(l, 2) < 1180), 0xff96c8ffu);
    snprintf(l, sizeof l, "VERDICT: %s", verdict); dp_canvas_text(c, 12, 72, l, 2, vc);
    dp_canvas_text(c, 12, 94, why, 1, 0xffa0a0a8u);
    // concrete ROM coverage this movie brings (its own replay from power-on, the classes of the kernel's flags)
    size_t cls[4] = {0}, tot = s->rom_size, cov = 0;
    for (size_t i = 0; s->flags && i < tot; i++) { uint8_t f = s->flags[i]; if (f & 1) cls[0]++; else if (f & 2) cls[1]++; else if (f & 32) cls[2]++; else if (f & (4 | 8 | 16)) cls[3]++; }
    cov = cls[0] + cls[1] + cls[2] + cls[3];
    size_t block = 0;
    if (s->flags && tot) block = dp_canvas_covmap(c, 1250, 4, 660, 112, s->flags, tot, 0x4000, NULL, NULL);
    if (tot) {
        snprintf(l, sizeof l, "this movie alone, classified: %zu / %zu bytes = %.1f%% (static inference not included)   code natural %zu, forced %zu, static %zu, data %zu   1 square = %zu bytes",
                 cov, tot, 100.0 * (double)cov / (double)tot, cls[0], cls[1], cls[2], cls[3], block);
        dp_canvas_text(c, 12, 108, l, 1, 0xff80e090u);
    }
    int n = nshots ? nshots : 1, cols = n <= 4 ? n : (n + 1) / 2, rows = (n + cols - 1) / cols;
    int cw = W / cols, ch = (H - HEAD - FOOT) / rows;
    for (int i = 0; i < nshots; i++) {
        int cx = (i % cols) * cw, cy = HEAD + (i / cols) * ch;
        int f = dp_canvas_tile(c, cx, cy + 2, cw, ch - 20, shots[i].px, shots[i].w, shots[i].h);
        char t[64]; snprintf(t, sizeof t, "%s frame %llu  (%s%d)", shots[i].tag, (unsigned long long)shots[i].frame, f >= 1 ? "x" : "/", f >= 1 ? f : -f);
        dp_canvas_rect(c, cx + cw - 2, HEAD, 2, rows * ch, 0xff15151au); dp_canvas_rect(c, 0, cy + ch - 2, W, 2, 0xff15151au);
        dp_canvas_text(c, cx + 8, cy + ch - 16, t, 1, strcmp(shots[i].tag, "stall") ? 0xffc8c8d0u : 0xffe05050u);
    }
    dp_canvas_rect(c, 0, H - 6, W, 6, vc);
    int rc = dp_canvas_save_png(c, path);
    dp_canvas_free(c);
    for (int i = 0; i < nshots; i++) free(shots[i].px);
    nshots = 0;
    return rc;
}

void dp_tasshot_emu(const char *rom, uint32_t frames, const char *movie, char *out, size_t n) {
    char cmd[2600], idarg[80] = ""; out[0] = 0;
    if (movie) {   // .../movies/<game>/<NNNN[M|S]>/file
        const char *e = strrchr(movie, '/');
        if (e) { const char *b = e; while (b > movie && b[-1] != '/') b--; size_t l = (size_t)(e - b), d = 0; while (d + 1 < l && b[d] >= '0' && b[d] <= '9') d++;
                 if (l >= 2 && d == l - 1 && (b[l - 1] == 'M' || b[l - 1] == 'S') && l < 20) snprintf(idarg, sizeof idarg, " --id %.*s", (int)l, b); }
    }
    char exe[1100]; ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1); if (el <= 0) return; exe[el] = 0;   // popen's shell would otherwise be "self"
    snprintf(cmd, sizeof cmd, "'%s' tasdb emu '%s' %u%s 2>/dev/null", exe, rom, frames, idarg);
    FILE *p = popen(cmd, "r"); if (!p) return;
    char line[256];
    while (fgets(line, sizeof line, p)) if (!strncmp(line, "emulator=", 9)) { char *e = strstr(line, " kind="); if (e) *e = 0; line[strcspn(line, "\n")] = 0; snprintf(out, n, "%s", line + 9); break; }
    pclose(p);
}
