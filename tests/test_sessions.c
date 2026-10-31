// test_sessions.c: the play sessions — input-driven coverage the forking
// explorer cannot reach, verbatim copy proofs, snapshot PNGs (screen +
// machine in one file) and the persistence round-trip.
#include "../src/core/cart.h"
#include "../src/core/snapshot_png.h"
#include "../src/explore/explorer.h"
#include "../src/explore/sessions.h"
#include "../src/report/report.h"
#include "mach_util.h"
#include "test_util.h"

#include <sys/stat.h>

static char dir[512];

static DpCart *load(const char *name) {
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/%s.gb", dir, name);
    DpCart *c = dp_cart_load(path, err, sizeof err);
    if (!c) fprintf(stderr, "cannot load %s: %s\n", path, err);
    return c;
}

static void results_for(DpResults *res, DpCart *cart) {
    dp_results_init(res, cart, dp_platform_gb());
    dp_static_scan(res);
}

// session engine tuned for the fixtures: tiny epochs, fixed pads, no fork
// drain (the fork explorer would make the maps time-dependent)
static DpSessionCfg test_cfg(const char *state_dir) {
    DpSessionCfg c;
    dp_session_cfg_defaults(&c);
    c.sessions = 2;
    c.epoch_frames = 8;
    c.segment_min = 480; // segments never end inside a test epoch
    c.segment_span = 1;
    c.pool_cap = 8;
    c.seed = 42;
    c.fork_paths_per_epoch = 0;
    c.state_dir = state_dir;
    return c;
}

static int exec_at(const DpResults *res, int bank, int addr) {
    return res->exec_flags[(size_t)bank * 0x4000 + addr] & (DP_XF_NATURAL | DP_XF_FORCED);
}

static int find_copy(const DpResults *res, int verbatim_wanted) {
    for (int i = 0; i < res->copy_count; i++)
        if ((int)res->copies[i].verbatim == verbatim_wanted) return i;
    return -1;
}

int main(int argc, char **argv) {
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : ".");

    // --- both JOYP branches through play sessions -----------------------------
    {
        DpCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            DpResults res;
            results_for(&res, cart);
            DpConfig cfg;
            dp_config_defaults(&cfg);
            DpMach *mach = mach_for(cart);
            DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
            dp_seed_gb(ex);
            DpSessionCfg sc = test_cfg(NULL);
            sc.fixed[0] = 0x01; // session 0 holds A
            sc.fixed[1] = 0x08; // session 1 holds START
            sc.fixed_n = 2;
            sc.max_frames = 32; // 4 epochs
            DpPlay *p = dp_play_new(&sc, &res, ex);
            CHECK(p != NULL);
            dp_play_run(p);
            dp_explore_finalize(ex);
            // the forking explorer has no keys: only the sessions took them
            CHECK(exec_at(&res, 0, 0x0180)); // path A
            CHECK(exec_at(&res, 0, 0x01A0)); // path B
            CHECK(res.exec_flags[0x40] & DP_XF_IRQ);   // vblank handler via natural IRQ
            CHECK(res.exec_flags[0x40] & DP_XF_NATURAL);
            int p1 = -1;
            for (int i = 0; i < res.io_count; i++)
                if (!strcmp(res.plat->io_regs[i].name, "P1")) p1 = i;
            CHECK(p1 >= 0 && res.io[p1].rd > 0);
            CHECK(res.steps_total > 0);
            dp_play_free(p);
            dp_explorer_free(ex);
            dp_results_free(&res);
            dp_cart_free(cart);
        }
    }

    // --- persistence: split run == continuous run, byte for byte ---------------
    {
        DpCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            mkdir("test_state_a", 0755);
            DpResults ra, rb, rc;
            // A: two epochs, saved on stop
            results_for(&ra, cart);
            DpConfig cfg;
            dp_config_defaults(&cfg);
            DpMach *macha = mach_for(cart);
            DpExplorer *exa = dp_explorer_new(&cfg, &ra, macha);
            dp_seed_gb(exa);
            DpSessionCfg sa = test_cfg("test_state_a");
            sa.save_epochs = 1;
            sa.max_frames = 16;
            sa.fixed[0] = 0x01;
            sa.fixed[1] = 0x08;
            sa.fixed_n = 2;
            DpPlay *pa = dp_play_new(&sa, &ra, exa);
            dp_play_run(pa);
            CHECK(dp_play_frames(pa) == 16);
            dp_play_free(pa);
            dp_explorer_free(exa);
            // B: resume from the saved pool, two more epochs
            results_for(&rb, cart);
            DpMach *machb = mach_for(cart);
            DpExplorer *exb = dp_explorer_new(&cfg, &rb, machb);
            dp_seed_gb(exb);
            DpSessionCfg sb = test_cfg("test_state_a");
            sb.resume = 1;
            sb.max_frames = 32; // absolute: the pool file carries the frame count
            sb.fixed[0] = 0x01;
            sb.fixed[1] = 0x08;
            sb.fixed_n = 2;
            DpPlay *pb = dp_play_new(&sb, &rb, exb);
            dp_play_run(pb);
            CHECK(dp_play_frames(pb) == 32); // the pool file restored the frame count
            dp_play_free(pb);
            dp_explorer_free(exb);
            // C: the same four epochs without interruption
            results_for(&rc, cart);
            DpMach *machc = mach_for(cart);
            DpExplorer *exc = dp_explorer_new(&cfg, &rc, machc);
            dp_seed_gb(exc);
            DpSessionCfg scc = test_cfg(NULL);
            scc.max_frames = 32;
            scc.fixed[0] = 0x01;
            scc.fixed[1] = 0x08;
            scc.fixed_n = 2;
            DpPlay *pc = dp_play_new(&scc, &rc, exc);
            dp_play_run(pc);
            dp_play_free(pc);
            dp_explorer_free(exc);
            size_t n = (size_t)cart->rom_banks * 0x4000;
            CHECK(memcmp(ra.exec_flags, rc.exec_flags, n) == 0); // A's marks are a subset
            CHECK(memcmp(rb.exec_flags, rc.exec_flags, n) == 0); // resume == continuous
            CHECK(memcmp(rb.data_flags, rc.data_flags, n) == 0);
            dp_results_free(&ra);
            dp_results_free(&rb);
            dp_results_free(&rc);
            dp_cart_free(cart);
        }
    }

    // --- snapshot PNG: screen + machine in one file, loadable -------------------
    {
        DpCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            DpResults res;
            results_for(&res, cart);
            DpConfig cfg;
            dp_config_defaults(&cfg);
            DpMach *mach = mach_for(cart);
            DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
            dp_seed_gb(ex);
            DpSessionCfg sc = test_cfg(NULL);
            DpPlay *p = dp_play_new(&sc, &res, ex);
            dp_play_human_begin(p, 0); // power-on slot
            for (int f = 0; f < 12; f++) dp_play_human_frame(p, f < 6 ? 0x01 : 0x08);
            CHECK(dp_play_human_active(p));
            CHECK(dp_play_human_screen(p) != NULL);
            CHECK(dp_play_human_snapshot(p)); // writes a snap PNG and joins the pool
            const char *snap = dp_play_last_snapshot(p);
            CHECK(snap != NULL);
            uint16_t pc_saved = dp_play_slot_pc(p, dp_play_pool_count(p) - 1);
            CHECK(pc_saved != 0x100); // the game left the reset entry
            // p owns machine instances: free it before the second engine opens
            // the same ones (the instance registry hands out one DpMach per slot)
            char snap_copy[600];
            snprintf(snap_copy, sizeof snap_copy, "%s", snap ? snap : "");
            dp_play_free(p);
            dp_explorer_free(ex);
            dp_results_free(&res);
            dp_cart_free(cart);
            // load it back into a fresh engine and check the state follows
            cart = load("fix_vblank");
            CHECK(cart != NULL);
            DpResults res2;
            results_for(&res2, cart);
            DpMach *mach2 = mach_for(cart);
            DpExplorer *ex2 = dp_explorer_new(&cfg, &res2, mach2);
            dp_seed_gb(ex2);
            DpPlay *p2 = dp_play_new(&sc, &res2, ex2);
            int loaded = -1;
            if (snap_copy[0]) loaded = dp_play_load_png(p2, snap_copy);
            CHECK(loaded >= 0);
            if (loaded >= 0) {
                CHECK(dp_play_slot_pc(p2, loaded) == pc_saved);
                // and the machine can keep playing from there
                dp_play_human_begin(p2, loaded);
                for (int f = 0; f < 4; f++) dp_play_human_frame(p2, 0);
                CHECK(dp_play_human_screen(p2) != NULL);
            }
            dp_play_free(p2);
            dp_explorer_free(ex2);
            dp_results_free(&res2);
            dp_cart_free(cart);
        }
    }

    // --- PNG writer/reader round trip ------------------------------------------
    {
        uint8_t rgb[16 * 3];
        for (int i = 0; i < 16; i++) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (uint8_t)(i * 16);
        uint8_t payload[] = { 1, 2, 3, 4, 250, 0, 255 };
        CHECK(dp_png_save("test_state_a/rt.png", 4, 4, rgb, payload, sizeof payload));
        int w, h;
        uint8_t *orgb, *opay;
        size_t on;
        CHECK(dp_png_load("test_state_a/rt.png", &w, &h, &orgb, &opay, &on));
        CHECK(w == 4 && h == 4);
        CHECK(memcmp(orgb, rgb, 16 * 3) == 0);
        CHECK(on == sizeof payload && memcmp(opay, payload, sizeof payload) == 0);
        free(orgb);
        free(opay);
    }

    // --- TAS priority: the movie replays in full, past every frame budget ------
    {
        // a 700-frame movie holding START; max_frames = 400 would cut the play
        // mid-movie if TAS replay were not exempt from the frame budgets
        char tpath[512];
        snprintf(tpath, sizeof tpath, "%s/tas_long.log", dir);
        FILE *f = fopen(tpath, "w");
        CHECK(f != NULL);
        if (f) {
            for (int i = 0; i < 700; i++) fputs("|.......S|\n", f);
            fclose(f);
        }
        DpCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            DpResults res;
            results_for(&res, cart);
            DpConfig cfg;
            dp_config_defaults(&cfg);
            DpMach *mach = mach_for(cart);
            DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
            dp_seed_gb(ex);
            DpSessionCfg sc = test_cfg(NULL);
            sc.sessions = 2; // worker 0 attract, worker 1 TAS
            sc.max_frames = 400;
            sc.tas[0] = tpath;
            sc.tas_n = 1;
            DpPlay *p = dp_play_new(&sc, &res, ex);
            CHECK(p != NULL);
            dp_play_run(p);
            // the TAS worker replayed its whole movie before the max_frames
            // wall could fire: total = both workers, 700 frames each or more
            CHECK(dp_play_frames(p) >= 2 * 700);
            dp_play_free(p);
            dp_explorer_free(ex);
            dp_results_free(&res);
            dp_cart_free(cart);
        }
    }

    // --- TAS checkpoint snapshots: saved during the replay, seeded after -----
    {
        // the same 700-frame movie as above; with the auto interval
        // min(700/ckpt_max, secs*60) the captures land a few frames apart,
        // the PNGs land in the state dir, and the post-play seeding hands
        // them to the fork explorer
        char tpath[512];
        snprintf(tpath, sizeof tpath, "%s/tas_ckpt.log", dir);
        FILE *f = fopen(tpath, "w");
        CHECK(f != NULL);
        if (f) {
            for (int i = 0; i < 700; i++) fputs("|.......S|\n", f);
            fclose(f);
        }
        DpCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            DpResults res;
            results_for(&res, cart);
            DpConfig cfg;
            dp_config_defaults(&cfg);
            DpMach *mach = mach_for(cart);
            DpExplorer *ex = dp_explorer_new(&cfg, &res, mach);
            dp_seed_gb(ex);
            DpSessionCfg sc = test_cfg("test_state_tas");
            mkdir(sc.state_dir, 0755);
            sc.sessions = 2; // worker 0 attract, worker 1 TAS
            sc.tas[0] = tpath;
            sc.tas_n = 1;
            sc.max_frames = 400; // the TAS priority lets the movie finish first
            sc.tas_ckpt_secs = 1;  // 60 frames of game time between captures
            sc.tas_ckpt_max = 1024;
            DpPlay *p = dp_play_new(&sc, &res, ex);
            CHECK(p != NULL);
            dp_play_run(p);
            // 700 frames / 60 = 11 captures, the movie fully replayed
            CHECK(dp_play_tas_snap_files(p) >= 10);
            CHECK(dp_play_frames(p) >= 2 * 700);
            // the fork explorer received nothing during the play (the seeds
            // only enter at the post-play phase); q0 = the initial seeds
            uint32_t q0 = ex->q_len + ex->pq_len;
            int seeded = dp_play_seed_tas_snaps(p);
            CHECK(seeded == (int)dp_play_tas_snap_files(p));
            // snapshot seeds take the priority inbox: queued means q + pq
            CHECK(ex->q_len + ex->pq_len == q0 + (uint32_t)seeded);
            dp_play_free(p);
            dp_explorer_free(ex);
            dp_results_free(&res);
            dp_cart_free(cart);
        }
    }

    TEST_DONE("test_sessions");
}
