#include "explore.h"
#include <stdlib.h>
#include <string.h>
#include "../../cpu/m68k/m68k.h"
#include "../../mach/mdmach.h"
#include "md_registry.h"

typedef struct {
    void *state;
    int forced;
} Item;

typedef struct {
    const DpMdCart *c;
    const DpMdOptions *o;
    DpMdResults *r;
    DpMdMach *m;
    Item *q;
    uint32_t nq;
    uint32_t *gen;       /* per even rom address: path generation */
    uint8_t *vis;        /* per even rom address: visits in that generation */
    uint8_t *site;       /* per even rom address: forks made from that site */
    uint32_t cur_gen;
    int forced, halted;
    uint32_t pc;         /* instruction being run */
    int irq6_done, irq4_done;
} X;

static size_t ssz;

static void mark(X *x, uint32_t pc, unsigned len) {
    uint8_t f = x->forced ? DP_MDF_FORCED : DP_MDF_NATURAL;
    for (unsigned i = 0; i < len && pc + i < x->r->rom_size; i++) x->r->flags[pc + i] |= f;
}

static int push(X *x, uint32_t pc, int vint, int hint) {
    if (x->nq >= x->o->queue_cap) { x->r->sat_queue++; return 0; }
    void *s = malloc(ssz);
    if (!s) { x->r->sat_queue++; return 0; }
    dp_mdmach_save(x->m, s);
    dp_mdmach_state_set_pc(s, pc);
    dp_mdmach_state_set_irq(s, vint, hint);
    x->q[x->nq++] = (Item){s, 1};
    x->r->flags[pc] |= DP_MDF_QUEUED;
    return 1;
}

static void on_step(void *u, uint32_t pc, uint16_t op) {
    X *x = u;
    (void)op;
    x->pc = pc & 0xFFFFFFu;
    if (x->pc < x->r->rom_size) {
        size_t i = x->pc >> 1;
        if (x->gen[i] != x->cur_gen) { x->gen[i] = x->cur_gen; x->vis[i] = 0; }
        if (x->vis[i] >= x->o->visit_cap) {
            x->r->sat_visit++;
            x->halted = 1;
            dp_mdmach_halt(x->m);
            return;
        }
        x->vis[i]++;
    }
}

static void irq_forks(X *x, uint32_t next) {
    const DpMdCart *c = x->c;
    if (!x->irq6_done && (dp_mdmach_vdp_reg(x->m, 1) & 0x20)) {
        uint32_t h = c->vec[30] & 0xFFFFFFu;
        x->irq6_done = 1;
        if (h < x->r->rom_size && !(h & 1) && !(x->r->flags[h] & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_QUEUED)))
            if (push(x, next & 0xFFFFFFu, 1, 0)) x->r->irq_forks++;
    }
    if (!x->irq4_done && (dp_mdmach_vdp_reg(x->m, 0) & 0x10)) {
        uint32_t h = c->vec[28] & 0xFFFFFFu;
        x->irq4_done = 1;
        if (h < x->r->rom_size && !(h & 1) && !(x->r->flags[h] & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_QUEUED)))
            if (push(x, next & 0xFFFFFFu, 0, 1)) x->r->irq_forks++;
    }
}

static void on_end(void *u, uint32_t pc, uint32_t next, unsigned len) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    pc &= 0xFFFFFFu;
    if (pc >= r->rom_size) {
        r->exec_outside++;
        if (pc >= 0xE00000) r->ram_exec++;
        return;
    }
    mark(x, pc, len);
    irq_forks(x, next);
    DpM68kInsn in;
    size_t avail = r->rom_size - pc;
    if (!dp_m68k_decode(x->c->rom + pc, avail, pc, &in)) return;
    if ((in.cls != DP_M68K_BCC && in.cls != DP_M68K_DBCC) || !in.has_target) return;
    uint32_t fall = (pc + in.len) & 0xFFFFFFu;
    uint32_t alt = ((next & 0xFFFFFFu) == fall) ? in.target : fall;
    if (alt == fall && in.target == fall) return;
    if (alt >= r->rom_size || (alt & 1)) { r->fork_off_rom++; return; }
    if (r->flags[alt] & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_QUEUED)) { r->fork_seen++; return; }
    size_t si = pc >> 1;
    if (x->site[si] >= x->o->fork_cap) { r->sat_site++; return; }
    if (push(x, alt, 0, 0)) {
        x->site[si]++;
        r->forks++;
        if (in.cls == DP_M68K_DBCC) /* make the counter agree with the forced side */
            dp_mdmach_state_set_dn_word(x->q[x->nq - 1].state, x->c->rom[pc + 1] & 7, alt == fall ? 0xFFFF : 1);
    }
}

static void on_read(void *u, uint32_t a, unsigned size) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    a &= 0xFFFFFFu;
    if (a < r->rom_size) {
        for (unsigned i = 0; i < size && a + i < r->rom_size; i++) r->flags[a + i] |= DP_MDF_DATA;
    } else if (a < 0x400000) {
        r->unmapped_r++;
    } else if (a < 0xE00000) {
        int k = dp_md_reg_find(a);
        if (k < 0) r->unmapped_r++;
        else if (k < 64) r->reg_r[k]++;
    }
}

static void on_write(void *u, uint32_t a, unsigned size, uint32_t v) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    a &= 0xFFFFFFu;
    if (a < 0xA00000 || a >= 0xE00000) return;
    int k = dp_md_reg_find(a);
    if (k < 0) r->unmapped_w++;
    else if (k < 64) r->reg_w[k]++;
    if (a >= 0xC00004 && a <= 0xC00007) {
        /* the second command word of a pair carries CD5 (DMA); a long write
         * delivers both words in one access */
        int second = size == 4 || dp_mdmach_vdp_pending(x->m);
        uint16_t w = (uint16_t)v, first = size == 4 ? (uint16_t)(v >> 16) : dp_mdmach_vdp_first(x->m);
        if (!second || !(w & 0x80) || (size != 4 && (w & 0xC000) == 0x8000)) return;
        if (!(dp_mdmach_vdp_reg(x->m, 1) & 0x10) || (dp_mdmach_vdp_reg(x->m, 23) & 0x80)) return;
        DpMdCopyRec c;
        c.pc = x->pc;
        c.src = ((uint32_t)(dp_mdmach_vdp_reg(x->m, 23) & 0x7F) << 17) |
                ((uint32_t)(dp_mdmach_vdp_reg(x->m, 22) & 0xFF) << 9) |
                ((uint32_t)(dp_mdmach_vdp_reg(x->m, 21) & 0xFF) << 1);
        c.len = ((uint32_t)(dp_mdmach_vdp_reg(x->m, 20) & 0xFF) << 8) | (dp_mdmach_vdp_reg(x->m, 19) & 0xFF);
        if (!c.len) c.len = 0x10000;
        c.code = ((first >> 14) & 3) | ((w >> 2) & 0x3C);
        for (int i = 0; i < r->ncopy; i++)
            if (!memcmp(&r->copies[i], &c, sizeof c)) return;
        if (r->ncopy >= DP_MD_COPY_CAP) { r->copy_overflow++; return; }
        r->copies[r->ncopy++] = c;
    }
}

void dp_md_options_default(DpMdOptions *o) {
    o->max_total = 20000000;
    o->path_steps = 2000000;
    o->queue_cap = 256;
    o->fork_cap = 2;
    o->visit_cap = 64;
}

static void run_path(X *x, Item *it, uint64_t budget, int *cut) {
    DpMdResults *r = x->r;
    int fault = 0;
    x->halted = 0;
    x->forced = it ? it->forced : 0;
    x->cur_gen++;
    if (it) dp_mdmach_load(x->m, it->state);
    r->paths++;
    uint64_t pb = x->o->path_steps < budget ? x->o->path_steps : budget;
    uint64_t n = dp_mdmach_run(x->m, pb, cut, &fault);
    r->steps += n;
    if (*cut) {
        if (n >= budget) r->sat_total++;
        else r->sat_path++;
    }
    if (fault) r->faults++;
}

int dp_md_explore(const DpMdCart *c, const DpMdOptions *o, DpMdResults *r) {
    memset(r, 0, sizeof *r);
    r->rom_size = c->size;
    r->flags = calloc(c->size, 1);
    r->copies = calloc(DP_MD_COPY_CAP, sizeof *r->copies);
    X x = {.c = c, .o = o, .r = r};
    x.q = calloc(o->queue_cap ? o->queue_cap : 1, sizeof *x.q);
    x.gen = calloc(c->size / 2 + 1, sizeof *x.gen);
    x.vis = calloc(c->size / 2 + 1, 1);
    x.site = calloc(c->size / 2 + 1, 1);
    x.m = dp_mdmach_open(c->rom, c->size);
    if (!r->flags || !r->copies || !x.q || !x.gen || !x.vis || !x.site || !x.m) {
        dp_mdmach_close(x.m);
        free(x.q); free(x.gen); free(x.vis); free(x.site);
        dp_md_results_free(r);
        return -1;
    }
    ssz = dp_mdmach_state_size();
    DpMdHooks h = {&x, on_step, on_end, on_read, on_write};
    dp_mdmach_set_hooks(x.m, &h);
    dp_mdmach_reset(x.m);
    int cut = 0;
    run_path(&x, NULL, o->max_total, &cut);
    while (x.nq) {
        if (r->steps >= o->max_total) break;
        Item it = x.q[--x.nq];
        run_path(&x, &it, o->max_total - r->steps, &cut);
        free(it.state);
    }
    r->dropped_pending = x.nq;
    for (uint32_t i = 0; i < x.nq; i++) free(x.q[i].state);
    for (size_t i = 0; i < c->size; i++) {
        uint8_t f = r->flags[i];
        if (f & (DP_MDF_NATURAL | DP_MDF_FORCED)) r->exec_bytes++;
        if ((f & DP_MDF_FORCED) && !(f & DP_MDF_NATURAL)) r->forced_only_bytes++;
        if (f & DP_MDF_DATA) r->dread_bytes++;
    }
    dp_mdmach_close(x.m);
    free(x.q); free(x.gen); free(x.vis); free(x.site);
    return 0;
}

void dp_md_results_free(DpMdResults *r) {
    free(r->flags);
    free(r->copies);
    memset(r, 0, sizeof *r);
}

void dp_md_explore_report(const DpMdCart *c, const DpMdOptions *o, const DpMdResults *r, FILE *out) {
    fprintf(out, "deepeel megadrive (forking exploration)\n");
    fprintf(out, "rom: %s | %s | %zu bytes | region %s\n", c->console, c->overseas, c->size, c->region);
    fprintf(out, "reset pc: %06X  ssp: %08X\n", c->vec[1], c->vec[0]);
    fprintf(out, "bounds: total %llu steps, path %llu, queue %u, forks/site %u, visits/insn %u\n",
            (unsigned long long)o->max_total, (unsigned long long)o->path_steps, o->queue_cap, o->fork_cap,
            o->visit_cap);
    fprintf(out, "steps: %llu in %llu paths, forks %llu (irq %llu)\n", (unsigned long long)r->steps,
            (unsigned long long)r->paths, (unsigned long long)r->forks, (unsigned long long)r->irq_forks);
    fprintf(out, "saturation (a non-zero line means the exploration is incomplete):\n");
    fprintf(out, "  total budget: %llu  pending forks unrun: %llu\n", (unsigned long long)r->sat_total,
            (unsigned long long)r->dropped_pending);
    fprintf(out, "  path budget: %llu  loop bound: %llu\n", (unsigned long long)r->sat_path,
            (unsigned long long)r->sat_visit);
    fprintf(out, "  queue full: %llu  per-site cap: %llu\n", (unsigned long long)r->sat_queue,
            (unsigned long long)r->sat_site);
    fprintf(out, "  copy records dropped: %llu\n", (unsigned long long)r->copy_overflow);
    fprintf(out, "not losses: alternative already seen %llu, alternative outside rom %llu\n",
            (unsigned long long)r->fork_seen, (unsigned long long)r->fork_off_rom);
    if (r->faults) fprintf(out, "paths ended by a machine fault: %llu\n", (unsigned long long)r->faults);
    fprintf(out, "executed rom bytes: %u (%.1f%%), of which only via forks: %u; data-read: %u\n", r->exec_bytes,
            100.0 * r->exec_bytes / r->rom_size, r->forced_only_bytes, r->dread_bytes);
    fprintf(out, "exec outside rom: %llu (ram: %llu)\n", (unsigned long long)r->exec_outside,
            (unsigned long long)r->ram_exec);
    fprintf(out, "registers:\n");
    for (int i = 0; i < dp_md_reg_count && i < 64; i++) {
        if (!r->reg_r[i] && !r->reg_w[i]) continue;
        if (!strcmp(dp_md_regs[i].category, "memory")) continue;
        fprintf(out, "  %06X %-12s %-6s r=%llu w=%llu\n", dp_md_regs[i].lo, dp_md_regs[i].name,
                dp_md_regs[i].category, (unsigned long long)r->reg_r[i], (unsigned long long)r->reg_w[i]);
    }
    if (r->unmapped_r || r->unmapped_w)
        fprintf(out, "  unmapped: r=%llu w=%llu\n", (unsigned long long)r->unmapped_r,
                (unsigned long long)r->unmapped_w);
    fprintf(out, "rom->vdp copies: %d\n", r->ncopy);
    for (int i = 0; i < r->ncopy; i++)
        fprintf(out, "  pc %06X src %06X len %u words code %02X\n", r->copies[i].pc, r->copies[i].src,
                r->copies[i].len, r->copies[i].code);
}

int dp_md_explore_map(const DpMdResults *r, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(r->flags, 1, r->rom_size, f);
    fclose(f);
    return n == r->rom_size ? 0 : -1;
}

int dp_md_explore_evidence(const DpMdResults *r, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# executed ranges (N=natural path, F=only via a forced fork)\n");
    size_t i = 0;
    while (i < r->rom_size) {
        uint8_t m = r->flags[i] & (DP_MDF_NATURAL | DP_MDF_FORCED);
        if (!m) { i++; continue; }
        int nat = 0;
        size_t j = i;
        while (j < r->rom_size && (r->flags[j] & (DP_MDF_NATURAL | DP_MDF_FORCED))) {
            if (r->flags[j] & DP_MDF_NATURAL) nat = 1;
            j++;
        }
        fprintf(f, "exec %06zX-%06zX %c\n", i, j - 1, nat ? 'N' : 'F');
        i = j;
    }
    for (int k = 0; k < r->ncopy; k++)
        fprintf(f, "copy pc=%06X src=%06X words=%u code=%02X\n", r->copies[k].pc, r->copies[k].src,
                r->copies[k].len, r->copies[k].code);
    fclose(f);
    return 0;
}
