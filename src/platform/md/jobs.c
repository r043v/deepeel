// jobs.c: Mega Drive --jobs 2.  The 68000/Z80 machine and the explorer are process
// globals, so the second instance is a complete namespaced copy of the MD stack
// (dp_md2_ prefix, built by objcopy like the PCE/Sega clones).  The work is split by
// explicit entry (G3): the secondary skips the power-on path and takes the odd-numbered
// entries, the primary keeps the power-on path (movie, forks, replays) and the even
// ones; the primary then folds the secondary's raw findings in before the sibling pass
// and the byte counts, so the report describes the union.  Second-pass and dispatch
// seeds are not run on the secondary (they need the union, the primary runs them).
#include "explore.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int dp_md2_dp_md_explore(const DpMdCart *c, const DpMdOptions *o, DpMdResults *r);
void dp_md2_dp_md_results_free(DpMdResults *r);

typedef struct {
    const DpMdCart *c;
    DpMdOptions opt;
    DpMdResults *res;
    pthread_t th;
    int ok;
} Job;

static void *job_main(void *p) {
    Job *j = p;
    j->ok = dp_md2_dp_md_explore(j->c, &j->opt, j->res) == 0;
    return NULL;
}

#define ADD(f) a->f += b->f
static void fold(DpMdResults *a, uint8_t *bank_seen, void *user) {
    Job *j = user;
    pthread_join(j->th, NULL);
    if (!j->ok) {
        fprintf(stderr, "deepeel: md --jobs 2: the second instance failed; its findings are lost\n");
        return;
    }
    const DpMdResults *b = j->res;
    size_t n = a->rom_size < b->rom_size ? a->rom_size : b->rom_size;
    for (size_t i = 0; i < n; i++) {
        a->flags[i] |= b->flags[i];
        a->z80_flags[i] |= b->z80_flags[i];
        if (b->nonexcluded_reads[i] > a->nonexcluded_reads[i]) a->nonexcluded_reads[i] = b->nonexcluded_reads[i];
    }
    a->jobs_folded++;
    for (unsigned i = 0; i < 128; i++) bank_seen[i] |= b->z80_bank_seen_out[i];
    ADD(s4_tables); ADD(s4_seeds); ADD(s4_refused); ADD(steps); ADD(paths); ADD(forks); ADD(irq_forks); ADD(pad_forks); ADD(sat_total); ADD(sat_path);
    ADD(sat_visit); ADD(sat_queue); ADD(queue_growths); ADD(sat_site); ADD(fork_seen); ADD(fork_off_rom);
    ADD(entries_seeded); ADD(entries_refused); ADD(parked); ADD(parked_resumed); ADD(parked_lost);
    dp_ramarch_merge(&a->ra, &b->ra);
    ADD(dropped_pending); ADD(faults); ADD(exec_outside); ADD(ram_exec); ADD(smc_writes); ADD(sram_exec); ADD(sram_reads);
    ADD(decode_length_mismatches); ADD(unmapped_r); ADD(unmapped_w); ADD(cpu_vram_words);
    ADD(z80_steps); ADD(z80_exec_ram); ADD(z80_exec_bank); ADD(z80_exec_other); ADD(z80_rom_reads);
    ADD(z80_ram_copied); ADD(z80_ym_writes); ADD(z80_psg_writes); ADD(z80_typed_sample); ADD(z80_typed_music);
    ADD(pass2_seeds); ADD(pass2_refused); ADD(pass2_capped); ADD(pad_cuts); ADD(indirect_sites);
    ADD(indirect_targets); ADD(indirect_overflow); ADD(ctx_evicted);
    for (int i = 0; i < 64; i++) { a->reg_r[i] += b->reg_r[i]; a->reg_w[i] += b->reg_w[i]; }
    // copy records: same (pc, src, len, code) merged, the rest appended; overflow is counted
    for (int i = 0; i < b->ncopy; i++) {
        int k;
        for (k = 0; k < a->ncopy; k++)
            if (a->copies[k].pc == b->copies[i].pc && a->copies[k].src == b->copies[i].src &&
                a->copies[k].len == b->copies[i].len && a->copies[k].code == b->copies[i].code) break;
        if (k < a->ncopy) continue;
        if (a->ncopy < DP_MD_COPY_CAP) a->copies[a->ncopy++] = b->copies[i];
        else a->copy_overflow++;
    }
    a->copy_overflow += b->copy_overflow;
    for (unsigned i = 0; i < b->nreaders; i++) {
        unsigned k;
        for (k = 0; k < a->nreaders; k++) if (a->readers[k].pc == b->readers[i].pc) break;
        if (k == a->nreaders) {
            if (k >= DP_MD_READER_CAP) { a->reader_overflow++; continue; }
            a->readers[a->nreaders++] = b->readers[i];
        } else {
            if (b->readers[i].lo < a->readers[k].lo) a->readers[k].lo = b->readers[i].lo;
            if (b->readers[i].hi > a->readers[k].hi) a->readers[k].hi = b->readers[i].hi;
            a->readers[k].accesses += b->readers[i].accesses;
            a->readers[k].natural_accesses += b->readers[i].natural_accesses;
        }
    }
    a->reader_overflow += b->reader_overflow;
    for (unsigned i = 0; i < b->nfault_sites; i++) {
        unsigned k;
        for (k = 0; k < a->nfault_sites; k++) if (a->fault_sites[k].pc == b->fault_sites[i].pc) break;
        if (k < a->nfault_sites) { a->fault_sites[k].count += b->fault_sites[i].count; continue; }
        if (k >= DP_MD_FAULT_SITE_CAP) { a->fault_sites_overflow++; continue; }
        a->fault_sites[a->nfault_sites++] = b->fault_sites[i];
    }
    a->fault_sites_overflow += b->fault_sites_overflow;
}

int dp_md_explore_jobs(const DpMdCart *c, const DpMdOptions *options, DpMdResults *results, int jobs) {
    DpMdOptions o = *options;
    if (jobs < 2 || o.entry_count < 2) {
        if (jobs >= 2)
            fprintf(stderr, "deepeel: md --jobs 2 needs at least 2 --entry to split the work; "
                            "running on one instance\n");
        return dp_md_explore(c, &o, results);
    }
    Job *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->res = calloc(1, sizeof *j->res);
    if (!j->res) { free(j); return -1; }
    j->c = c;
    j->opt = o;
    j->opt.skip_root = 1;
    j->opt.no_finalize = 1;
    j->opt.entry_from = 1; j->opt.entry_step = 2;
    j->opt.lot_path = NULL; j->opt.lot_drain = 0;
    j->opt.movie = NULL; j->opt.play_frames = 0; j->opt.novelty_steps = 0; j->opt.watch_every = 0; j->opt.tree_path = NULL;
    j->opt.second_pass = j->opt.bank_dispatch = 0;
    j->opt.pre_finalize = NULL;
    o.entry_from = 0; o.entry_step = 2;
    o.pre_finalize = fold;
    o.pre_user = j;
    if (pthread_create(&j->th, NULL, job_main, j)) {
        free(j->res); free(j);
        return dp_md_explore(c, options, results);
    }
    int rc = dp_md_explore(c, &o, results);
    if (rc != 0) pthread_join(j->th, NULL);   // a primary that failed before the fold must still join
    dp_md2_dp_md_results_free(j->res);
    free(j->res);
    free(j);
    return rc;
}
