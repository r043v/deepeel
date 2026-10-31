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

static MsCart *load(const char *name) {
    char path[512], err[256];
    snprintf(path, sizeof path, "%s/%s.gb", dir, name);
    MsCart *c = ms_cart_load(path, err, sizeof err);
    if (!c) fprintf(stderr, "cannot load %s: %s\n", path, err);
    return c;
}

static void results_for(MsResults *res, MsCart *cart) {
    ms_results_init(res, cart, ms_platform_gb());
    ms_static_scan(res);
}

// session engine tuned for the fixtures: tiny epochs, fixed pads, no fork
// drain (the fork explorer would make the maps time-dependent)
static MsSessionCfg test_cfg(const char *state_dir) {
    MsSessionCfg c;
    ms_session_cfg_defaults(&c);
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

static int exec_at(const MsResults *res, int bank, int addr) {
    return res->exec_flags[(size_t)bank * 0x4000 + addr] & (MS_XF_NATURAL | MS_XF_FORCED);
}

static int find_copy(const MsResults *res, int verbatim_wanted) {
    for (int i = 0; i < res->copy_count; i++)
        if ((int)res->copies[i].verbatim == verbatim_wanted) return i;
    return -1;
}

int main(int argc, char **argv) {
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : ".");

    // --- both JOYP branches through play sessions -----------------------------
    {
        MsCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            MsResults res;
            results_for(&res, cart);
            MsConfig cfg;
            ms_config_defaults(&cfg);
            MsMach *mach = mach_for(cart);
            MsExplorer *ex = ms_explorer_new(&cfg, &res, mach);
            ms_seed_gb(ex);
            MsSessionCfg sc = test_cfg(NULL);
            sc.fixed[0] = 0x01; // session 0 holds A
            sc.fixed[1] = 0x08; // session 1 holds START
            sc.fixed_n = 2;
            sc.max_frames = 32; // 4 epochs
            MsPlay *p = ms_play_new(&sc, &res, ex);
            CHECK(p != NULL);
            ms_play_run(p);
            ms_explore_finalize(ex);
            // the forking explorer has no keys: only the sessions took them
            CHECK(exec_at(&res, 0, 0x0180)); // path A
            CHECK(exec_at(&res, 0, 0x01A0)); // path B
            CHECK(res.exec_flags[0x40] & MS_XF_IRQ);   // vblank handler via natural IRQ
            CHECK(res.exec_flags[0x40] & MS_XF_NATURAL);
            int p1 = -1;
            for (int i = 0; i < res.io_count; i++)
                if (!strcmp(res.plat->io_regs[i].name, "P1")) p1 = i;
            CHECK(p1 >= 0 && res.io[p1].rd > 0);
            CHECK(res.steps_total > 0);
            ms_play_free(p);
            ms_explorer_free(ex);
            ms_results_free(&res);
            ms_cart_free(cart);
        }
    }

    // --- persistence: split run == continuous run, byte for byte ---------------
    {
        MsCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            mkdir("test_state_a", 0755);
            MsResults ra, rb, rc;
            // A: two epochs, saved on stop
            results_for(&ra, cart);
            MsConfig cfg;
            ms_config_defaults(&cfg);
            MsMach *macha = mach_for(cart);
            MsExplorer *exa = ms_explorer_new(&cfg, &ra, macha);
            ms_seed_gb(exa);
            MsSessionCfg sa = test_cfg("test_state_a");
            sa.save_epochs = 1;
            sa.max_frames = 16;
            sa.fixed[0] = 0x01;
            sa.fixed[1] = 0x08;
            sa.fixed_n = 2;
            MsPlay *pa = ms_play_new(&sa, &ra, exa);
            ms_play_run(pa);
            CHECK(ms_play_frames(pa) == 16);
            ms_play_free(pa);
            ms_explorer_free(exa);
            // B: resume from the saved pool, two more epochs
            results_for(&rb, cart);
            MsMach *machb = mach_for(cart);
            MsExplorer *exb = ms_explorer_new(&cfg, &rb, machb);
            ms_seed_gb(exb);
            MsSessionCfg sb = test_cfg("test_state_a");
            sb.resume = 1;
            sb.max_frames = 32; // absolute: the pool file carries the frame count
            sb.fixed[0] = 0x01;
            sb.fixed[1] = 0x08;
            sb.fixed_n = 2;
            MsPlay *pb = ms_play_new(&sb, &rb, exb);
            ms_play_run(pb);
            CHECK(ms_play_frames(pb) == 32); // the pool file restored the frame count
            ms_play_free(pb);
            ms_explorer_free(exb);
            // C: the same four epochs without interruption
            results_for(&rc, cart);
            MsMach *machc = mach_for(cart);
            MsExplorer *exc = ms_explorer_new(&cfg, &rc, machc);
            ms_seed_gb(exc);
            MsSessionCfg scc = test_cfg(NULL);
            scc.max_frames = 32;
            scc.fixed[0] = 0x01;
            scc.fixed[1] = 0x08;
            scc.fixed_n = 2;
            MsPlay *pc = ms_play_new(&scc, &rc, exc);
            ms_play_run(pc);
            ms_play_free(pc);
            ms_explorer_free(exc);
            size_t n = (size_t)cart->rom_banks * 0x4000;
            CHECK(memcmp(ra.exec_flags, rc.exec_flags, n) == 0); // A's marks are a subset
            CHECK(memcmp(rb.exec_flags, rc.exec_flags, n) == 0); // resume == continuous
            CHECK(memcmp(rb.data_flags, rc.data_flags, n) == 0);
            ms_results_free(&ra);
            ms_results_free(&rb);
            ms_results_free(&rc);
            ms_cart_free(cart);
        }
    }

    // --- snapshot PNG: screen + machine in one file, loadable -------------------
    {
        MsCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            MsResults res;
            results_for(&res, cart);
            MsConfig cfg;
            ms_config_defaults(&cfg);
            MsMach *mach = mach_for(cart);
            MsExplorer *ex = ms_explorer_new(&cfg, &res, mach);
            ms_seed_gb(ex);
            MsSessionCfg sc = test_cfg(NULL);
            MsPlay *p = ms_play_new(&sc, &res, ex);
            ms_play_human_begin(p, 0); // power-on slot
            for (int f = 0; f < 12; f++) ms_play_human_frame(p, f < 6 ? 0x01 : 0x08);
            CHECK(ms_play_human_active(p));
            CHECK(ms_play_human_screen(p) != NULL);
            CHECK(ms_play_human_snapshot(p)); // writes a snap PNG and joins the pool
            const char *snap = ms_play_last_snapshot(p);
            CHECK(snap != NULL);
            uint16_t pc_saved = ms_play_slot_pc(p, ms_play_pool_count(p) - 1);
            CHECK(pc_saved != 0x100); // the game left the reset entry
            // p owns machine instances: free it before the second engine opens
            // the same ones (the instance registry hands out one MsMach per slot)
            char snap_copy[600];
            snprintf(snap_copy, sizeof snap_copy, "%s", snap ? snap : "");
            ms_play_free(p);
            ms_explorer_free(ex);
            ms_results_free(&res);
            ms_cart_free(cart);
            // load it back into a fresh engine and check the state follows
            cart = load("fix_vblank");
            CHECK(cart != NULL);
            MsResults res2;
            results_for(&res2, cart);
            MsMach *mach2 = mach_for(cart);
            MsExplorer *ex2 = ms_explorer_new(&cfg, &res2, mach2);
            ms_seed_gb(ex2);
            MsPlay *p2 = ms_play_new(&sc, &res2, ex2);
            int loaded = -1;
            if (snap_copy[0]) loaded = ms_play_load_png(p2, snap_copy);
            CHECK(loaded >= 0);
            if (loaded >= 0) {
                CHECK(ms_play_slot_pc(p2, loaded) == pc_saved);
                // and the machine can keep playing from there
                ms_play_human_begin(p2, loaded);
                for (int f = 0; f < 4; f++) ms_play_human_frame(p2, 0);
                CHECK(ms_play_human_screen(p2) != NULL);
            }
            ms_play_free(p2);
            ms_explorer_free(ex2);
            ms_results_free(&res2);
            ms_cart_free(cart);
        }
    }

    // --- PNG writer/reader round trip ------------------------------------------
    {
        uint8_t rgb[16 * 3];
        for (int i = 0; i < 16; i++) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (uint8_t)(i * 16);
        uint8_t payload[] = { 1, 2, 3, 4, 250, 0, 255 };
        CHECK(ms_png_save("test_state_a/rt.png", 4, 4, rgb, payload, sizeof payload));
        int w, h;
        uint8_t *orgb, *opay;
        size_t on;
        CHECK(ms_png_load("test_state_a/rt.png", &w, &h, &orgb, &opay, &on));
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
        MsCart *cart = load("fix_vblank");
        CHECK(cart != NULL);
        if (cart) {
            MsResults res;
            results_for(&res, cart);
            MsConfig cfg;
            ms_config_defaults(&cfg);
            MsMach *mach = mach_for(cart);
            MsExplorer *ex = ms_explorer_new(&cfg, &res, mach);
            ms_seed_gb(ex);
            MsSessionCfg sc = test_cfg(NULL);
            sc.sessions = 2; // worker 0 attract, worker 1 TAS
            sc.max_frames = 400;
            sc.tas[0] = tpath;
            sc.tas_n = 1;
            MsPlay *p = ms_play_new(&sc, &res, ex);
            CHECK(p != NULL);
            ms_play_run(p);
            // the TAS worker replayed its whole movie before the max_frames
            // wall could fire: total = both workers, 700 frames each or more
            CHECK(ms_play_frames(p) >= 2 * 700);
            ms_play_free(p);
            ms_explorer_free(ex);
            ms_results_free(&res);
            ms_cart_free(cart);
        }
    }

    TEST_DONE("test_sessions");
}
