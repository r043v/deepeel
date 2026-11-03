#include "md_trace.h"
#include <stdlib.h>
#include <string.h>
#include "../mach/mdmach.h"
#include "../platform/md/md_registry.h"

typedef struct {
    DpMdTrace *t;
    uint32_t pc;                /* instruction being run */
    uint8_t vdp[24];            /* VDP registers as written through the ctrl port */
    uint16_t first;             /* pending first command word */
    int have_first;
} Ctx;

static void on_step(void *u, uint32_t pc, uint16_t op) {
    Ctx *x = u;
    (void)op;
    x->pc = pc;
}

static void on_end(void *u, uint32_t pc, uint32_t next, unsigned len) {
    Ctx *x = u;
    DpMdTrace *t = x->t;
    (void)next;
    pc &= 0xFFFFFFu;
    if (pc < t->rom_size) {
        for (unsigned i = 0; i < len && pc + i < t->rom_size; i++) t->exec[pc + i] = 1;
    } else {
        t->exec_outside++;
        if (pc >= 0xE00000) t->ram_exec++;
    }
}

static void count_reg(DpMdTrace *t, uint32_t a, int write) {
    int r = dp_md_reg_find(a);
    if (r < 0) { write ? t->unmapped_w++ : t->unmapped_r++; return; }
    if (r < 64) (write ? t->reg_w : t->reg_r)[r]++;
}

static void on_read(void *u, uint32_t a, unsigned size) {
    Ctx *x = u;
    DpMdTrace *t = x->t;
    a &= 0xFFFFFFu;
    if (a < t->rom_size) {
        for (unsigned i = 0; i < size && a + i < t->rom_size; i++) t->dread[a + i] = 1;
    } else if (a < 0x400000) {
        t->unmapped_r++;
    } else if (a < 0xE00000) {
        count_reg(t, a, 0);
    }
}

static void vdp_ctrl(Ctx *x, uint16_t w) {
    DpMdTrace *t = x->t;
    if (!x->have_first && (w & 0xC000) == 0x8000) { /* register write */
        x->vdp[((w >> 8) & 0x1F) % 24] = (uint8_t)w;
        return;
    }
    if (!x->have_first) { x->first = w; x->have_first = 1; return; }
    x->have_first = 0;
    /* second word: bit 7 = CD5 (DMA); only 68k->VDP when reg23 bit 7 clear */
    if (!(w & 0x80) || !(x->vdp[1] & 0x10) || (x->vdp[23] & 0x80)) return;
    if (t->ncopy >= DP_MD_MAX_COPIES) { t->copy_overflow++; return; }
    DpMdCopy *c = &t->copies[t->ncopy++];
    c->pc = x->pc;
    c->src = ((uint32_t)(x->vdp[23] & 0x7F) << 17) | ((uint32_t)x->vdp[22] << 9) | ((uint32_t)x->vdp[21] << 1);
    c->len = ((uint32_t)x->vdp[20] << 8) | x->vdp[19];
    if (!c->len) c->len = 0x10000;
    c->code = ((x->first >> 14) & 3) | ((w >> 2) & 0x3C);
}

static void on_write(void *u, uint32_t a, unsigned size, uint32_t v) {
    Ctx *x = u;
    DpMdTrace *t = x->t;
    a &= 0xFFFFFFu;
    if (a < 0xA00000 || a >= 0xE00000) return;
    count_reg(t, a, 1);
    if (a >= 0xA130F2 && a <= 0xA130FF) {
        if (t->nbank >= DP_MD_MAX_BANKEV) { t->bank_overflow++; }
        else t->bank[t->nbank++] = (DpMdBankEv){x->pc, a, (uint8_t)v};
    }
    if (a >= 0xC00004 && a <= 0xC00007) {
        if (size == 4) { vdp_ctrl(x, (uint16_t)(v >> 16)); vdp_ctrl(x, (uint16_t)v); }
        else vdp_ctrl(x, (uint16_t)v);
    }
}

int dp_md_trace_run(const DpMdCart *c, uint64_t max_steps, DpMdTrace *t) {
    memset(t, 0, sizeof *t);
    t->rom_size = c->size;
    t->max_steps = max_steps;
    t->exec = calloc(c->size, 1);
    t->dread = calloc(c->size, 1);
    DpMdMach *m = dp_mdmach_open(c->rom, c->size);
    if (!t->exec || !t->dread || !m) { dp_mdmach_close(m); dp_md_trace_free(t); return -1; }
    Ctx x = {.t = t};
    DpMdHooks h = {&x, on_step, on_end, on_read, on_write, NULL, NULL};
    dp_mdmach_set_hooks(m, &h);
    dp_mdmach_reset(m);
    t->steps = dp_mdmach_run(m, max_steps, &t->cut, &t->fault);
    for (size_t i = 0; i < c->size; i++) { t->exec_bytes += t->exec[i]; t->dread_bytes += t->dread[i]; }
    dp_mdmach_close(m);
    return 0;
}

void dp_md_trace_free(DpMdTrace *t) {
    free(t->exec);
    free(t->dread);
    memset(t, 0, sizeof *t);
}

void dp_md_trace_report(const DpMdCart *c, const DpMdTrace *t, FILE *o) {
    fprintf(o, "deepeel megadrive (linear run, no forks)\n");
    fprintf(o, "rom: %s | %s | %zu bytes | region %s\n", c->console, c->overseas, c->size, c->region);
    fprintf(o, "checksum: header %04X computed %04X%s\n", c->checksum, c->sum_computed,
            c->checksum == c->sum_computed ? "" : " (differ)");
    fprintf(o, "reset pc: %06X  ssp: %08X\n", c->vec[1], c->vec[0]);
    fprintf(o, "steps: %llu / %llu%s\n", (unsigned long long)t->steps, (unsigned long long)t->max_steps,
            t->cut ? "  SATURATED (step budget)" : "");
    if (t->fault) fprintf(o, "fault: %d at pc %06X\n", t->fault, t->fault_pc);
    fprintf(o, "executed rom bytes: %u (%.1f%%), data-read rom bytes: %u\n", t->exec_bytes,
            100.0 * t->exec_bytes / t->rom_size, t->dread_bytes);
    fprintf(o, "exec outside rom: %llu (ram: %llu)\n", (unsigned long long)t->exec_outside,
            (unsigned long long)t->ram_exec);
    fprintf(o, "registers:\n");
    for (int i = 0; i < dp_md_reg_count && i < 64; i++) {
        if (!t->reg_r[i] && !t->reg_w[i]) continue;
        if (!strcmp(dp_md_regs[i].category, "memory")) continue;
        fprintf(o, "  %06X %-12s %-6s r=%llu w=%llu\n", dp_md_regs[i].lo, dp_md_regs[i].name,
                dp_md_regs[i].category, (unsigned long long)t->reg_r[i], (unsigned long long)t->reg_w[i]);
    }
    if (t->unmapped_r || t->unmapped_w)
        fprintf(o, "  unmapped: r=%llu w=%llu\n", (unsigned long long)t->unmapped_r, (unsigned long long)t->unmapped_w);
    fprintf(o, "rom->vdp copies: %d%s\n", t->ncopy, t->copy_overflow ? "  (overflow counted below)" : "");
    for (int i = 0; i < t->ncopy; i++)
        fprintf(o, "  pc %06X src %06X len %u words code %02X\n", t->copies[i].pc, t->copies[i].src,
                t->copies[i].len, t->copies[i].code);
    if (t->copy_overflow) fprintf(o, "  copy records dropped: %llu\n", (unsigned long long)t->copy_overflow);
    fprintf(o, "bank events: %d\n", t->nbank);
    for (int i = 0; i < t->nbank; i++)
        fprintf(o, "  pc %06X %06X <- %02X\n", t->bank[i].pc, t->bank[i].addr, t->bank[i].value);
    if (t->bank_overflow) fprintf(o, "  bank events dropped: %llu\n", (unsigned long long)t->bank_overflow);
}
