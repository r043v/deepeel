// memscan.c: command line front-end.
//
//   memscan explore rom.gb [options] [--json f] [--text f] [--cov f] [--gui]
//   memscan map rom.gb [--json f] [--text f]        static map only
//   memscan regs gb                                 dump the GB register registry
//   memscan dis rom.gb bank:addr [n]                disassemble n instructions
#include "core/cart.h"
#include "core/machine.h"
#include "explore/explorer.h"
#include "report/report.h"
#include "gui/gui.h"
#include "cpu/sm83/sm83.h"

#include <time.h>

static void usage(void) {
    fprintf(stderr,
            "memscan — ROM memory-map explorer (game boy first)\n\n"
            "  memscan explore rom.gb [options]\n"
            "    --max-steps N        per-path step budget (default 100000)\n"
            "    --max-paths N        path cap (default 200000)\n"
            "    --max-total N        global step wall (default 20000000)\n"
            "    --queue N            pending-state cap (default 2048)\n"
            "    --loop-cap N         iterations of one backward edge before cut (default 65536)\n"
            "    --seed-cap N         pointer-scan seed cap (default 4096)\n"
            "    --entry bank:addr    extra seed (repeatable)\n"
            "    --seed-banks         also seed every bank at $4000\n"
            "    --no-pointer-scan    disable pointer-table seeding\n"
            "    --no-context-evict   disable same-context eviction\n"
            "    --no-irq-fork        disable IRQ forking\n"
            "    --gui                SDL2 live viewer\n"
            "    --json f / --text f / --cov f   outputs\n"
            "  memscan map rom.gb     static map only (pointer tables, padding)\n"
            "  memscan regs gb        dump the register registry\n"
            "  memscan dis rom.gb bank:addr [n]\n");
}

static int parse_entry(const char *s, uint8_t *bank, uint16_t *addr) {
    char *end;
    long b = strtol(s, &end, 0);
    if (end == s || *end != ':' || b < 0 || b > 65535) return 0;
    long a = strtol(end + 1, &end, 16); // addresses are hex ($0180 style)
    if (end == end + 1 || a < 0 || a > 0xFFFF) return 0;
    *bank = (uint8_t)b;
    *addr = (uint16_t)a;
    return 1;
}

static int write_file(const char *path, const MsBuf *b) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "memscan: cannot write %s\n", path);
        return 0;
    }
    fwrite(b->p, 1, b->len, f);
    fclose(f);
    return 1;
}

static int cmd_explore(int argc, char **argv) {
    const char *json = NULL, *text = NULL, *cov = NULL, *path = NULL;
    int gui = 0;
    MsConfig cfg;
    ms_config_defaults(&cfg);
    uint8_t eb[32];
    uint16_t ea[32];
    int entries = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(a, "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "--cov") && i + 1 < argc) cov = argv[++i];
        else if (!strcmp(a, "--gui")) gui = 1;
        else if (!strcmp(a, "--max-steps") && i + 1 < argc) cfg.max_steps_path = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-paths") && i + 1 < argc) cfg.max_paths = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--max-total") && i + 1 < argc) cfg.max_steps_total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--queue") && i + 1 < argc) cfg.queue_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--loop-cap") && i + 1 < argc) cfg.loop_back_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-cap") && i + 1 < argc) cfg.seed_cap = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--seed-banks")) cfg.seed_banks = 1;
        else if (!strcmp(a, "--no-pointer-scan")) cfg.no_pointer_scan = 1;
        else if (!strcmp(a, "--no-context-evict")) cfg.no_context_evict = 1;
        else if (!strcmp(a, "--no-irq-fork")) cfg.no_irq_fork = 1;
        else if (!strcmp(a, "--entry") && i + 1 < argc && entries < 32) {
            if (!parse_entry(argv[++i], &eb[entries], &ea[entries])) {
                fprintf(stderr, "memscan: bad --entry '%s' (bank:addr)\n", argv[i]);
                return 2;
            }
            entries++;
        } else if (!path) path = a;
        else { fprintf(stderr, "memscan: unknown option %s\n", a); return 2; }
    }
    if (!path) {
        usage();
        return 2;
    }
    char err[256];
    MsCart *cart = ms_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "memscan: %s\n", err);
        return 1;
    }
    const MsPlatform *plat = ms_platform_gb();
    MsResults res;
    ms_results_init(&res, cart, plat);
    ms_static_scan(&res);
    MsExplorer *ex = ms_explorer_new(&cfg, &res);
    ms_seed_gb(ex);
    for (int i = 0; i < entries; i++) {
        MsMachine m;
        ms_machine_init(&m, cart, plat);
        if (ea[i] >= 0x4000) {
            m.rom_lo = eb[i] ? eb[i] : 1;
            ms_mapper_update_windows(&m);
        }
        m.pc = ea[i];
        ms_explorer_seed(ex, &m, MS_SEED_POINTER, 1);
    }
    int rc = 0;
    if (gui && ms_gui_available()) {
        rc = ms_gui_run(&res, &cfg, ex);
    } else {
        clock_t t0 = clock();
        ms_explore(ex);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        fwrite(out.p, 1, out.len, stdout);
        ms_buf_free(&out);
        fprintf(stderr, "memscan: done in %.2fs\n", secs);
    }
    if (json) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_json(&res, &cfg, &out);
        if (!write_file(json, &out)) rc = 1;
        ms_buf_free(&out);
    }
    if (text) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        if (!write_file(text, &out)) rc = 1;
        ms_buf_free(&out);
    }
    if (cov) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_cov(&res, &out);
        if (!write_file(cov, &out)) rc = 1;
        ms_buf_free(&out);
    }
    ms_explorer_free(ex);
    ms_results_free(&res);
    ms_cart_free(cart);
    return rc;
}

static int cmd_map(int argc, char **argv) {
    const char *json = NULL, *text = NULL, *path = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) text = argv[++i];
        else if (!path) path = argv[i];
        else return usage(), 2;
    }
    if (!path) return usage(), 2;
    char err[256];
    MsCart *cart = ms_cart_load(path, err, sizeof err);
    if (!cart) {
        fprintf(stderr, "memscan: %s\n", err);
        return 1;
    }
    MsResults res;
    ms_results_init(&res, cart, ms_platform_gb());
    ms_static_scan(&res);
    printf("memscan map — %s (%s, %d banks)\n", cart->title, ms_mbc_name(cart->mbc), cart->rom_banks);
    for (int b = 0; b < res.banks; b++) {
        int ptr = 0, pad = 0;
        for (int i = 0; i < 0x4000; i++) {
            uint8_t f = res.static_flags[(size_t)b * 0x4000 + i];
            ptr += (f & MS_SF_POINTER) != 0;
            pad += (f & MS_SF_PAD) != 0;
        }
        printf("  bank %3d: %5d pointer-table bytes, %5d padding\n", b, ptr, pad);
    }
    MsConfig cfg;
    ms_config_defaults(&cfg);
    if (json) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_json(&res, &cfg, &out);
        write_file(json, &out);
        ms_buf_free(&out);
    }
    if (text) {
        MsBuf out;
        ms_buf_init(&out);
        ms_report_text(&res, &cfg, &out);
        write_file(text, &out);
        ms_buf_free(&out);
    }
    ms_results_free(&res);
    ms_cart_free(cart);
    return 0;
}

static int cmd_regs(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[2], "gb")) {
        fprintf(stderr, "memscan regs <platform> — platforms: gb\n");
        return 2;
    }
    const MsPlatform *p = ms_platform_gb();
    printf("%s register registry (%d entries, %d irqs):\n", p->name, p->io_reg_count, p->irq_count);
    for (int i = 0; i < p->io_reg_count; i++) {
        const MsIoReg *r = &p->io_regs[i];
        printf("  %04x-%04x  %-8s %-10s %s%s%s", r->lo, r->hi, r->name, r->category,
               (r->rw & 1) ? "r" : "-", (r->rw & 2) ? "w" : "-", (r->rw & 3) ? " " : "-");
        if (r->impl) {
            printf("  impl:");
            if (r->impl & MS_IMPL_BANK_LO) printf(" bank-lo");
            if (r->impl & MS_IMPL_BANK_HI) printf(" bank-hi");
            if (r->impl & MS_IMPL_RAM_BANK) printf(" ram-bank");
            if (r->impl & MS_IMPL_RAMG) printf(" ramg");
            if (r->impl & MS_IMPL_MODE) printf(" mode");
            if (r->impl & MS_IMPL_DMA_SRC) printf(" dma");
            if (r->impl & MS_IMPL_HDMA) printf(" hdma");
            if (r->impl & MS_IMPL_IRQ) printf(" irq");
            if (r->impl & MS_IMPL_EVENT) printf(" event");
            if (r->impl & MS_IMPL_WINDOW) printf(" window");
            if (r->impl & MS_IMPL_MODELLED) printf(" modelled");
        }
        printf("\n");
    }
    for (int i = 0; i < p->irq_count; i++)
        printf("  irq %s: vector $%04x, IE bit %d\n", p->irqs[i].name, p->irqs[i].vector, p->irqs[i].bit);
    return 0;
}

static int cmd_dis(int argc, char **argv) {
    if (argc < 4) return usage(), 2;
    char err[256];
    MsCart *cart = ms_cart_load(argv[2], err, sizeof err);
    if (!cart) {
        fprintf(stderr, "memscan: %s\n", err);
        return 1;
    }
    uint8_t bank;
    uint16_t addr;
    if (!parse_entry(argv[3], &bank, &addr)) {
        fprintf(stderr, "memscan: bad address '%s' (bank:addr)\n", argv[3]);
        return 2;
    }
    int n = argc > 4 ? atoi(argv[4]) : 16;
    // disassembly walks one bank linearly through a windowed view
    const uint8_t *view = cart->rom + (size_t)bank * 0x4000;
    uint32_t view_size = cart->rom_size > (uint32_t)bank * 0x4000 ? cart->rom_size - (uint32_t)bank * 0x4000 : 0;
    uint16_t a = addr & 0x3FFF;
    uint16_t win = addr < 0x4000 ? 0 : 0x4000;
    for (int i = 0; i < n; i++) {
        if (a >= view_size) break;
        char txt[64];
        int len = sm83_disasm(view, view_size, a, txt, sizeof txt);
        printf("%2d:%04x  %s\n", bank, a + win, txt);
        if (len <= 0) break;
        a = (uint16_t)(a + len);
    }
    ms_cart_free(cart);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage(), 2;
    if (!strcmp(argv[1], "explore")) return cmd_explore(argc, argv);
    if (!strcmp(argv[1], "map")) return cmd_map(argc, argv);
    if (!strcmp(argv[1], "regs")) return cmd_regs(argc, argv);
    if (!strcmp(argv[1], "dis")) return cmd_dis(argc, argv);
    return usage(), 2;
}
