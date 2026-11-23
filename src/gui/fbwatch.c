#include "../explore/stop.h"
#include <time.h>
#include <unistd.h>
#include "fbwatch.h"
#include "../explore/md_ref_feed.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t buf[DP_FB_MAX_W * DP_FB_MAX_H];
static int bw, bh;
static uint64_t bframe;
/* caption state (the GB watch tile information, mutualized): a fixed part set
 * by the CLI and a progress line the explorer updates per published frame;
 * read back by the SDL window thread through dp_fb_caption_read */
static char cap1[160], cap2[160];
static int cap_done;

void dp_fbwatch_caption(const char *base) {
    pthread_mutex_lock(&mu);
    snprintf(cap1, sizeof cap1, "%s", base ? base : "");
    pthread_mutex_unlock(&mu);
}

void dp_fbwatch_progress(uint32_t f, uint32_t total, int done) {
    pthread_mutex_lock(&mu);
    cap_done = done;
    if (total) snprintf(cap2, sizeof cap2, "%u/%u%s", (unsigned)f, (unsigned)total, done ? "  done" : "");
    else snprintf(cap2, sizeof cap2, "%u frames%s", (unsigned)f, done ? "  done" : "");
    pthread_mutex_unlock(&mu);
}

int dp_fb_caption_read(char *c1, size_t n1, char *c2, size_t n2) {
    int done = 0;
    pthread_mutex_lock(&mu);
    snprintf(c1, n1, "%s", cap1);
    snprintf(c2, n2, "%s", cap2);
    done = cap_done;
    pthread_mutex_unlock(&mu);
    return done;
}

#define DP_NREF 3
static DpMdRefFeed *ref_feed[DP_NREF];
static int ref_feed_tried[DP_NREF];
static const char *const ref_env[DP_NREF] = { "DEEPEEL_MD_REF_FEED", "DEEPEEL_MD_REF2_FEED", "DEEPEEL_MD_REF3_FEED" };
static const char *const ref_name_env[DP_NREF] = { "DEEPEEL_MD_REF_NAME", "DEEPEEL_MD_REF2_NAME", "DEEPEEL_MD_REF3_NAME" };
static const char *const ref_name_def[DP_NREF] = { "ClownMDEmu", "BizHawk GPGX", "Genesis Plus GX" };
/* platform-neutral names first (DEEPEEL_REF_FEED, DEEPEEL_REF2_FEED, ..._NAME), the historical DEEPEEL_MD_* ones as aliases */
static const char *const ref_env_g[DP_NREF] = { "DEEPEEL_REF_FEED", "DEEPEEL_REF2_FEED", "DEEPEEL_REF3_FEED" };
static const char *const ref_name_env_g[DP_NREF] = { "DEEPEEL_REF_NAME", "DEEPEEL_REF2_NAME", "DEEPEEL_REF3_NAME" };
static const char *env2(const char *a, const char *b) { const char *v = getenv(a); return v && *v ? v : getenv(b); }

int dp_fb_ref_active(int i) {
    if (i < 0 || i >= DP_NREF) return 0;
    if (!ref_feed[i] && ++ref_feed_tried[i] % 30 == 1)   // the harness may start after the window: retry now and then
        ref_feed[i] = dp_md_ref_feed_open(env2(ref_env_g[i], ref_env[i]), 0);
    return ref_feed[i] && ref_feed[i]->magic == DP_MDRF_MAGIC;
}

const char *dp_fb_ref_name(int i) {
    const char *n = i >= 0 && i < DP_NREF ? env2(ref_name_env_g[i], ref_name_env[i]) : NULL;
    return n && *n ? n : i >= 0 && i < DP_NREF ? ref_name_def[i] : "";
}

int dp_fb_ref_read(int i, uint32_t *out, int *w, int *h, uint32_t *vframe, uint32_t *total, int *done) {
    if (!dp_fb_ref_active(i)) return 0;
    return dp_md_ref_read(ref_feed[i], out, w, h, vframe, total, done);
}

static double sync_waited;
static double mono_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
double dp_fb_sync_waited(void) { return sync_waited; }
uint32_t dp_fb_ref_unsync_x10(int i) { return dp_fb_ref_active(i) ? ref_feed[i]->unsync_x10 : 0; }
/* real-time pacing of the replay (DP_WATCH_FPS, default 0 = flat out; 60 gives exactly one frame per window image): the window presents ~60 images a second, so a core running
 * at 130 frames/s would be shown as 2,2,3,2... frames per image (judder); one frame per image is the smooth way.  Called once per
 * replayed frame by every platform, so the pace lives here and nowhere else. */
static void pace_frame(void) {
    static double fps = -1, next;
    if (fps < 0) { const char *e = getenv("DP_WATCH_FPS"); fps = e && *e ? atof(e) : 0.0; next = 0; }
    if (fps <= 0) return;
    double now = mono_now();
    if (next == 0 || now - next > 0.25) next = now;   /* first frame, or far behind: no catching up */
    next += 1.0 / fps;
    while (!dp_stop) {
        double left = next - mono_now();
        if (left <= 0) break;
        usleep(left > 0.002 ? (useconds_t)((left - 0.001) * 1e6) : 100);
    }
}
void dp_fb_sync_gate(uint64_t frame) {
    pace_frame();
    static double last_prog[2], stale_since[2];
    static uint32_t last_v[2];
    /* DEEPEEL_REF_WAIT_S: how long a reference may take to start (feed file, first frame) or to move before the
     * core runs on without it; every configured reference is a participant of one shared frame counter */
    static double wait_s = -1, first_call;
    if (wait_s < 0) { const char *e = getenv("DEEPEEL_REF_WAIT_S"); wait_s = e && *e ? atof(e) : 2.0; first_call = mono_now(); }
    int any = 0, configured = 0;
    for (int k = 0; k < DP_NREF; k++) {
        if (env2(ref_env_g[k], ref_env[k])) configured = 1;
        if (dp_fb_ref_active(k)) { ref_feed[k]->leader = (uint32_t)frame; any = 1; }
    }
    if (!any && !configured) return;
    double t0 = mono_now();
    for (;;) {
        int wait = 0;
        double now = mono_now();
        for (int k = 0; k < DP_NREF; k++) {
            if (!dp_fb_ref_active(k)) {   /* configured but not started yet (the harness loads its ROM first) */
                if (env2(ref_env_g[k], ref_env[k]) && now - first_call < wait_s) wait = 1;
                continue;
            }
            ref_feed[k]->leader = (uint32_t)frame;
            if (ref_feed[k]->done) continue;
            uint32_t v = ref_feed[k]->vframe;
            if (v != last_v[k] || !last_prog[k]) { last_v[k] = v; last_prog[k] = now; }
            if (now - last_prog[k] > wait_s && v == 0) continue;   /* the harness never started: do not stall on it */
            if (now - last_prog[k] > wait_s + 1.0) continue;               /* stalled harness */
            if (frame > (uint64_t)v + 2) wait = 1;
        }
        if (!wait || dp_stop) break;
        usleep(100);
    }
    sync_waited += mono_now() - t0;
    (void)stale_since;
}

void dp_fb_publish(const uint32_t *argb, int w, int h, uint64_t frame) {
    if (w <= 0 || h <= 0 || w > DP_FB_MAX_W || h > DP_FB_MAX_H) return;
    pthread_mutex_lock(&mu);
    memcpy(buf, argb, (size_t)w * h * sizeof *buf);
    bw = w; bh = h; bframe = frame;
    pthread_mutex_unlock(&mu);
}

int dp_fb_read(uint32_t *out, unsigned cap, int *w, int *h, uint64_t *frame) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    if (bw && (unsigned)(bw * bh) <= cap) {
        memcpy(out, buf, (size_t)bw * bh * sizeof *buf);
        *w = bw; *h = bh; *frame = bframe;
        ok = 1;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

static void watch_ring_flush(void);
int dp_fb_dump_ppm(const char *path) {
    watch_ring_flush();
    static uint32_t tmp[DP_FB_MAX_W * DP_FB_MAX_H];
    int w, h;
    uint64_t f;
    if (!dp_fb_read(tmp, DP_FB_MAX_W * DP_FB_MAX_H, &w, &h, &f)) return 1;
    FILE *o = fopen(path, "wb");
    if (!o) return 1;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t px[3] = { (uint8_t)(tmp[i] >> 16), (uint8_t)(tmp[i] >> 8), (uint8_t)tmp[i] };
        fwrite(px, 1, 3, o);
    }
    return fclose(o) ? 1 : 0;
}

/* Movie replay smoothing: the explorer does heavy work between some frames (closure walks, fork snapshots), so the replay
 * advances in bursts.  The frames go through a ring read by a presenter thread at a steady rate (DP_WATCH_FPS, default 60),
 * after a prefill of about a second and a half; the producer is held back when the ring is full.  DP_WATCH_SMOOTH=0 disables. */
#define RING_CAP 360
#define RING_PRE 90
typedef struct { uint32_t *px; int w, h; uint64_t frame; uint32_t mf, total; int done; int16_t *au; unsigned au_n; } RingSlot;

/* sound: the samples of the frame follow it through the ring and are queued to SDL when the presenter shows it, so the 60 fps clock paces them */
static __thread int16_t pend_au[2 * 1024]; static __thread unsigned pend_au_n;
void dp_watch_audio(const int16_t *st, unsigned frames) { if (frames > 1024) frames = 1024; memcpy(pend_au, st, frames * 2u * sizeof *st); pend_au_n = frames; }
void dp_audio_play(const int16_t *stereo, unsigned frames);   /* windows.c: queues 44.1 kHz stereo frames to the sound device */
#define audio_play dp_audio_play
static RingSlot ring[RING_CAP];
static unsigned ring_head, ring_n;
static pthread_mutex_t ring_mu = PTHREAD_MUTEX_INITIALIZER;
static double ring_last_push;

static void *presenter(void *arg) {
    (void)arg;
    const char *e = getenv("DP_WATCH_FPS"); double fps = e && *e ? atof(e) : 60.0;   /* DP_WATCH_FPS=0: no cap, the window shows the newest image */
    double next = 0; int primed = 0;
    while (!dp_stop) {
        pthread_mutex_lock(&ring_mu);
        unsigned n = ring_n; double lp = ring_last_push;
        pthread_mutex_unlock(&ring_mu);
        double now = mono_now();
        if (!primed) { if (n >= RING_PRE || (n && now - lp > 0.5)) { primed = 1; next = now; } else { usleep(2000); continue; } }
        if (!n) { if (now - lp > 0.5) primed = 0; usleep(1000); next = mono_now(); continue; }
        RingSlot sl;
        pthread_mutex_lock(&ring_mu);
        sl = ring[ring_head]; ring_head = (ring_head + 1) % RING_CAP; ring_n--;
        pthread_mutex_unlock(&ring_mu);
        dp_fbwatch_progress(sl.mf, sl.total, sl.done);
        dp_fb_publish(sl.px, sl.w, sl.h, sl.frame);
        if (sl.au) { audio_play(sl.au, sl.au_n); free(sl.au); }
        free(sl.px);
        next += 1.0 / fps;
        for (;;) { double left = next - mono_now(); if (left <= 0 || dp_stop) break; usleep(left > 0.002 ? (useconds_t)((left - 0.001) * 1e6) : 100); }
        if (mono_now() - next > 0.25) next = mono_now();
    }
    return NULL;
}

/* the queued frames of a finished replay: the last one is the picture dumped / left in the window */
static void watch_ring_flush(void) {
    pthread_mutex_lock(&ring_mu);
    while (ring_n) {
        RingSlot sl = ring[ring_head]; ring_head = (ring_head + 1) % RING_CAP; ring_n--;
        if (!ring_n) { dp_fbwatch_progress(sl.mf, sl.total, sl.done); dp_fb_publish(sl.px, sl.w, sl.h, sl.frame); }
        free(sl.au); free(sl.px);
    }
    pthread_mutex_unlock(&ring_mu);
}

/* 1 when the frame was not queued (smoothing off or allocation failure): the caller publishes it directly */
static int watch_ring_push(const uint32_t *px, int w, int h, uint64_t frame, uint32_t mf, uint32_t total, int done) {
    static int mode = -1; static pthread_t th;
    if (mode < 0) {
        const char *e = getenv("DP_WATCH_SMOOTH");
        const char *f = getenv("DP_WATCH_FPS");
        mode = !(e && *e == '0') && !(f && *f && atof(f) <= 0);   /* DP_WATCH_FPS=0: uncapped, frames go straight to the window */
        if (mode && w > 0 && h > 0 && pthread_create(&th, NULL, presenter, NULL) == 0) pthread_detach(th); else mode = 0;
    }
    if (!mode || w <= 0 || h <= 0 || w > DP_FB_MAX_W || h > DP_FB_MAX_H) return 1;
    uint32_t *cp = malloc((size_t)w * h * sizeof *cp);
    if (!cp) return 1;
    memcpy(cp, px, (size_t)w * h * sizeof *cp);
    for (;;) {
        pthread_mutex_lock(&ring_mu);
        if (ring_n < RING_CAP) {
            RingSlot *sl = &ring[(ring_head + ring_n) % RING_CAP];
            sl->px = cp; sl->w = w; sl->h = h; sl->frame = frame; sl->mf = mf; sl->total = total; sl->done = done;
            sl->au = NULL; sl->au_n = 0;
            if (pend_au_n && (sl->au = malloc(pend_au_n * 2u * sizeof *sl->au))) { memcpy(sl->au, pend_au, pend_au_n * 2u * sizeof *sl->au); sl->au_n = pend_au_n; }
            pend_au_n = 0;
            ring_n++; ring_last_push = mono_now();
            pthread_mutex_unlock(&ring_mu);
            return 0;
        }
        pthread_mutex_unlock(&ring_mu);
        if (dp_stop) { free(cp); return 0; }
        usleep(1000);
    }
}

/* the one per-frame entry of every platform's --watch: progress caption, stagnation warning (a movie keeps the
 * picture moving; the same image for 900 frames means the replay probably desynchronised), then the publish.
 * `movie_frame`/`total`: the replayed movie's position (total 0: no movie in progress). */
void dp_watch_frame(const uint32_t *px, int w, int h, uint64_t frame, uint32_t movie_frame, uint32_t total, int done) {
    static uint64_t hsh_last, since; static int warned;
    /* shared frame counter: publish ours, then wait for every reference tile (the gate paces to the fps cap too) */
    dp_fb_sync_gate(total ? movie_frame : frame);
    if (total) {
        uint64_t hsh = 1469598103934665603ull;
        for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) hsh = (hsh ^ px[i]) * 1099511628211ull;
        if (hsh != hsh_last) { hsh_last = hsh; since = movie_frame; warned = 0; }
        else if (!warned && movie_frame - since >= 900) {
            warned = 1;
            fprintf(stderr, "tas: STAGNATION - same image since movie frame %llu, probable desync with the movie (frame %u/%u)\n", (unsigned long long)since, movie_frame, total);
        }
    }
    if (total) {
        if (!watch_ring_push(px, w, h, frame, movie_frame, total, done)) return;
        dp_fbwatch_progress(movie_frame, total, done);
    }
    dp_fb_publish(px, w, h, frame);
    if (pend_au_n) {
        audio_play(pend_au, pend_au_n);
        pend_au_n = 0;
    }
}
