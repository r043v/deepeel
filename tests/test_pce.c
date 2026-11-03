// test_pce.c: the PC Engine bridge (m0boy's HuC6280 machine) and explorer
// contract.  The HuCard is built in-code (no fixture file): a reset flow
// mapping hardware, WRAM and a data page through TAM, a conditional on WRAM
// (one side natural, the other forced), data reads from the mapped page, a
// copy into the VDC data port and an idle loop that must be cut and counted.
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "../src/platform/pce/pce.h"
#include "../src/platform/pce/explore.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { ROM_SIZE = 0x4000 };

static void build(uint8_t *rom) {
    memset(rom, 0, ROM_SIZE);
    static const uint8_t code[] = {
        0x78,                   // 00 SEI
        0xD4,                   // 01 CSH
        0xA9, 0xFF, 0x53, 0x01, // 02 MPR0 = $FF (hardware)
        0xA9, 0xF8, 0x53, 0x02, // 06 MPR1 = $F8 (WRAM)
        0xA9, 0x01, 0x53, 0x04, // 0A MPR2 = page 1
        0xAD, 0x00, 0x20,       // 0E LDA $2000 (WRAM, zero)
        0xD0, 0x05,             // 11 BNE $18 (the forced side)
        0xAD, 0x00, 0x40,       // 13 LDA $4000 (cell $2000)
        0x80, 0x03,             // 16 BRA $1B
        0xAD, 0x08, 0x40,       // 18 LDA $4008 (cell $2008)
        0x03, 0x02,             // 1B ST0 #2 (VRAM write register)
        0xAD, 0x10, 0x40,       // 1D LDA $4010 (cell $2010)
        0x8D, 0x02, 0x00,       // 20 STA $0002 (VDC data)
        0x80, 0xFE,             // 23 BRA $23 (idle)
    };
    memcpy(rom, code, sizeof code);
    for (unsigned i = 0; i < 0x20; i++) rom[0x2000 + i] = (uint8_t)(0x40 + i);
    rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0; // reset -> $E000 (MPR7 = page 0)
}

static void test_decoder(void) {
    CHECK(dp_huc_length(0x53) == 2, "TAM length");
    CHECK(dp_huc_length(0x03) == 2, "ST0 length");
    CHECK(dp_huc_length(0x73) == 7, "TII length");
    CHECK(dp_huc_length(0xAD) == 3, "LDA abs length");
    CHECK(dp_huc_defined(0xEA), "NOP defined");
}

static void test_bridge(const uint8_t *rom) {
    // a 512-byte copier header is stripped
    uint8_t *h = calloc(1, ROM_SIZE + 512);
    memcpy(h + 512, rom, ROM_SIZE);
    CHECK(dp_pce_init(h, ROM_SIZE + 512), "init with header");
    CHECK(dp_pce_rom_size() == ROM_SIZE, "header stripped: %zu", dp_pce_rom_size());
    free(h);
    CHECK(dp_pce_cpu().pc == 0xE000, "reset vector: %04x", dp_pce_cpu().pc);
    for (int i = 0; i < 40; i++) dp_pce_step(NULL);
    CHECK(dp_pce_mpr(2) == 0x01, "TAM mapped page 1");
    CHECK(dp_pce_rom_index(0x4010) == 0x2010, "rom index through MPR2");
    size_t n = dp_pce_state_size();
    void *s = malloc(n);
    dp_pce_save(s);
    uint64_t a = dp_pce_state_hash();
    for (int i = 0; i < 1000; i++) dp_pce_step(NULL);
    dp_pce_load(s);
    CHECK(dp_pce_state_hash() == a, "snapshot round trip");
    free(s);
    dp_pce_close();
}

static void test_explore(const uint8_t *rom) {
    DpPceOptions o = dp_pce_options();
    o.max_total = 2000000;
    o.max_steps = 20000;
    DpPceResults *r = calloc(1, sizeof *r);
    CHECK(dp_pce_explore(rom, ROM_SIZE, &o, r), "explore");
    if (!r->flags) { free(r); return; }
    CHECK(r->flags[0x00] & DP_PCE_NATURAL, "reset code natural");
    CHECK(r->flags[0x13] & DP_PCE_NATURAL, "taken side natural");
    CHECK(r->flags[0x18] & DP_PCE_FORCED, "other side forced");
    CHECK(r->flags[0x2000] & DP_PCE_DATA, "natural data read");
    CHECK(r->flags[0x2008] & DP_PCE_DATA, "forced data read");
    CHECK(r->flags[0x2010] & DP_PCE_COPIED, "ROM -> VDC copy");
    CHECK(r->copy_count >= 1, "copy record");
    CHECK(r->step_cuts + r->loop_cuts + r->global_cuts > 0, "idle loop cut and counted");
    CHECK(r->exec_outside == 0, "no execution outside ROM");
    dp_pce_results_free(r);
    free(r);
    {   // a one-slot queue and two successive branches: the second fork waits
        // aside, runs later, and the coverage is unchanged (§6)
        uint8_t *rom2 = malloc(ROM_SIZE);
        memcpy(rom2, rom, ROM_SIZE);
        static const uint8_t tail[] = { 0xAD, 0x00, 0x20, 0xD0, 0x02, 0xEA, 0xEA, 0x80, 0xFE };
        memcpy(rom2 + 0x23, tail, sizeof tail);
        DpPceOptions op = o;
        op.queue_cap = 1;
        DpPceResults *rp = calloc(1, sizeof *rp);
        CHECK(dp_pce_explore(rom2, ROM_SIZE, &op, rp), "tiny queue explore");
        if (rp->flags) {
            CHECK(rp->parked >= 1 && rp->parked_resumed == rp->parked && rp->parked_lost == 0, "parked then resumed");
            CHECK(rp->flags[0x13] & DP_PCE_NATURAL, "natural side kept");
            CHECK(rp->flags[0x18] & DP_PCE_FORCED, "forced side reached despite the queue wall");
            CHECK(rp->flags[0x28] & DP_PCE_FORCED, "second fork's side reached");
        }
        dp_pce_results_free(rp);
        free(rp);
        // a wall leaves states: they go to the disk lot, the next run resumes them (§6)
        char lp[] = "/tmp/dp_pce_lotXXXXXX"; int fd = mkstemp(lp); if (fd >= 0) close(fd);
        uint64_t left = 0, found = 0;
        for (uint64_t wall = 1; wall < 600 && !left; wall++) {
            found = wall;
            DpPceOptions ow = o; ow.queue_cap = 1; ow.lot_path = lp; ow.max_total = wall;
            unlink(lp);
            DpPceResults *rw = calloc(1, sizeof *rw);
            CHECK(dp_pce_explore(rom2, ROM_SIZE, &ow, rw), "walled run");
            left = rw->lot_pending;
            if (left) CHECK(rw->pending_dropped == 0, "wall leftovers saved, none dropped");
            dp_pce_results_free(rw); free(rw);
        }
        CHECK(left > 0, "a wall leaves states in the lot");
        {   // drain: only the saved states run (no root), the lot empties
            DpPceOptions od = o; od.queue_cap = 1; od.lot_path = lp; od.lot_drain = 1;
            DpPceResults *rd = calloc(1, sizeof *rd);
            CHECK(dp_pce_explore(rom2, ROM_SIZE, &od, rd), "drain run");
            CHECK(rd->lot_read >= left && rd->lot_pending == 0, "drain empties the lot");
            CHECK(access(lp, F_OK) != 0, "the drained lot file is removed");
            dp_pce_results_free(rd); free(rd);
            DpPceOptions ow = o; ow.queue_cap = 1; ow.lot_path = lp; ow.max_total = found;
            DpPceResults *rw = calloc(1, sizeof *rw);
            CHECK(dp_pce_explore(rom2, ROM_SIZE, &ow, rw), "walled run again");
            dp_pce_results_free(rw); free(rw);
        }
        DpPceOptions orr = o; orr.queue_cap = 1; orr.lot_path = lp;
        DpPceResults *rr = calloc(1, sizeof *rr);
        CHECK(dp_pce_explore(rom2, ROM_SIZE, &orr, rr), "resumed run");
        CHECK(rr->lot_read >= left && rr->lot_pending == 0, "the lot is resumed and emptied");
        CHECK(access(lp, F_OK) != 0, "the emptied lot file is removed");
        dp_pce_results_free(rr); free(rr);
        unlink(lp);
        free(rom2);
    }
}

int main(void) {
    uint8_t *rom = malloc(ROM_SIZE);
    build(rom);
    test_decoder();
    test_bridge(rom);
    test_explore(rom);
    free(rom);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("pce: ok\n");
    return 0;
}
