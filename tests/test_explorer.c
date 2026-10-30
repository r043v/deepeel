// test_explorer.c: end-to-end assertions on the synthetic fixtures.
#include "../src/core/cart.h"
#include "../src/explore/explorer.h"
#include "../src/report/report.h"
#include "test_util.h"

static MsCart *g_cart;
static MsResults res;

// runs the full pipeline on one fixture; the results stay in `res` until cleanup
static void run(const char *dir, const char *name) {
    char path[512];
    char err[256];
    snprintf(path, sizeof path, "%s/fix_%s.gb", dir, name);
    if (g_cart) ms_cart_free(g_cart);
    if (res.cart) ms_results_free(&res);
    memset(&res, 0, sizeof res);
    g_cart = ms_cart_load(path, err, sizeof err);
    if (!g_cart) {
        fprintf(stderr, "cannot load %s: %s\n", path, err);
        exit(2);
    }
    ms_results_init(&res, g_cart, ms_platform_gb());
    ms_static_scan(&res);
    MsConfig cfg;
    ms_config_defaults(&cfg);
    MsExplorer *ex = ms_explorer_new(&cfg, &res);
    ms_seed_gb(ex);
    ms_explore(ex);
    ms_explorer_free(ex);
}

static const MsSection *section_at(int bank, uint16_t addr) {
    for (int i = 0; i < res.section_count; i++) {
        const MsSection *s = &res.sections[i];
        if (s->bank == bank && s->lo <= addr && addr <= s->hi) return s;
    }
    return NULL;
}

static const MsCopyRec *copy_of(uint8_t channel, uint32_t sbank, uint16_t slo, uint16_t shi, uint16_t dlo,
                                uint16_t dhi) {
    for (int i = 0; i < res.copy_count; i++) {
        const MsCopyRec *r = &res.copies[i];
        if (r->channel == channel && r->src_bank == sbank && r->src_lo <= slo && slo <= r->src_hi &&
            r->src_lo <= shi && shi <= r->src_hi && r->dst_lo <= dlo && dlo <= r->dst_hi && r->dst_lo <= dhi &&
            dhi <= r->dst_hi)
            return r;
    }
    return NULL;
}

static int bank_events_of(uint32_t old_bank, uint32_t new_bank) {
    int n = 0;
    for (int i = 0; i < res.bank_count; i++)
        if (res.bank_events[i].old_bank == old_bank && res.bank_events[i].new_bank == new_bank) n++;
    return n;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    // --- branch: both sides, verdicts, copy record ------------------------------
    run(dir, "branch");
    const MsSection *s154 = section_at(0, 0x154);
    CHECK(s154 && s154->cond_sites >= 1 && s154->cond_both >= 1); // both outcomes ran
    // the not-taken side bytes: only ever reached through the fork
    CHECK((res.exec_flags[0x156] & MS_XF_FORCED) && !(res.exec_flags[0x156] & MS_XF_NATURAL));
    const MsSection *s180 = section_at(0, 0x180);
    CHECK(s180 && s180->verdict == MS_V_REQUIRED && s180->copies >= 1);
    CHECK(copy_of(0, 0, 0x300, 0x30F, 0x8000, 0x800F) != NULL);
    CHECK(res.data_flags[0x300] & MS_DF_COPIED);
    CHECK(res.exec_flags[0x150] & MS_XF_ENTRY); // jump target flagged as entry
    CHECK(res.exec_hits[0x156] > 0);
    // the rom data never runs on the natural flow (forked wandering may touch it)
    CHECK(!(res.exec_flags[0x300] & MS_XF_NATURAL));

    // --- mbc5: bank timeline + cross-bank code -----------------------------------
    run(dir, "mbc5");
    CHECK(bank_events_of(1, 2) >= 1 && bank_events_of(2, 3) >= 1);
    CHECK(res.exec_flags[ms_rom_index(2, 0x4000)] & MS_XF_NATURAL);  // bank 2 code ran
    CHECK(res.exec_flags[ms_rom_index(3, 0x8000)] & MS_XF_NATURAL);  // bank 3 code ran (offset 0)
    CHECK(res.exec_hits[ms_rom_index(2, 0x4002)] > 0);
    CHECK(section_at(2, 0x4000) && section_at(2, 0x4000)->verdict == MS_V_REQUIRED);
    // mapper registers in the io stats
    int romb0 = -1;
    for (int i = 0; i < res.io_count; i++)
        if (!strcmp(res.plat->io_regs[i].name, "ROMB0")) romb0 = i;
    CHECK(romb0 >= 0 && res.io[romb0].wr >= 2 && res.io[romb0].bank_events >= 2);

    // --- dma: OAM DMA record -------------------------------------------------------
    run(dir, "dma");
    CHECK(copy_of(1, 0, 0x3000, 0x309F, 0xFE00, 0xFE9F) != NULL);
    CHECK(res.data_flags[0x3000] & MS_DF_COPIED);
    int dmareg = -1;
    for (int i = 0; i < res.io_count; i++)
        if (!strcmp(res.plat->io_regs[i].name, "DMA")) dmareg = i;
    CHECK(dmareg >= 0 && res.io[dmareg].copies >= 1);

    // --- irq: fork dispatch + handler classification --------------------------------
    run(dir, "irq");
    CHECK(res.forks_irq >= 1);
    const MsSection *s40 = section_at(0, 0x40);
    CHECK(s40 && (s40->kind_bits & MS_XF_IRQ) && s40->verdict == MS_V_REQUIRED);
    // the handler's VRAM write happened in the forked dispatch
    CHECK(res.vram_write_pages[0] & 1);

    // --- jumptable: JP HL reaches both handlers -------------------------------------
    run(dir, "jumptable");
    CHECK(section_at(0, 0x220) != NULL && section_at(0, 0x210) != NULL);
    CHECK(res.exec_hits[0x215] > 0 && res.exec_hits[0x225] > 0); // both handlers halted

    // --- loop: infinite loop contained ----------------------------------------------
    run(dir, "loop");
    CHECK(res.cycle_cuts >= 1);
    CHECK(res.steps_total <= 10 * 100000ull); // bounded, no runaway

    // --- unreach: data blob stays data, unreachable code stays unexplored -----------
    run(dir, "unreach");
    for (int i = 0x200; i < 0x300; i++)
        if (res.exec_flags[i] & MS_XF_NATURAL) {
            CHECK_MSG(0, "data blob on the natural flow");
            break;
        }
    for (int i = 0x200; i < 0x300; i++)
        if (res.static_flags[i] & MS_SF_PAD) {
            CHECK_MSG(0, "data blob misread as padding");
            break;
        }
    CHECK(!(res.exec_flags[0x400] & MS_XF_NATURAL)); // never on the natural flow

    // --- bankdata: reads through the banked window -----------------------------------
    run(dir, "bankdata");
    CHECK(res.data_flags[ms_rom_index(2, 0x4000)] & MS_DF_READ);
    CHECK(res.data_flags[ms_rom_index(2, 0x4007)] & MS_DF_READ);
    CHECK(copy_of(0, 2, 0x4000, 0x4007, 0x8000, 0x8007) != NULL);
    CHECK(bank_events_of(1, 2) >= 1);

    // --- reports produce output -------------------------------------------------------
    {
        MsConfig cfg;
        ms_config_defaults(&cfg);
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        CHECK(out.len > 100);
        ms_buf_free(&out);
        ms_buf_init(&out);
        ms_report_json(&res, &cfg, &out);
        CHECK(out.len > 100);
        CHECK(out.p[0] == '{');
        ms_buf_free(&out);
        ms_buf_init(&out);
        ms_report_cov(&res, &out);
        CHECK(out.len > 20);
        ms_buf_free(&out);
    }

    if (res.cart) ms_results_free(&res);
    if (g_cart) ms_cart_free(g_cart);
    TEST_DONE("test_explorer");
}
