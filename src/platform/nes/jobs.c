// jobs.c: NES --jobs 2.  The NES machine and the explorer are process globals, so the
// second instance is a complete namespaced copy of the NES stack (dp_nes2_ prefix, built
// by objcopy like the PCE clone).  The work is split by phase: the secondary takes the
// TAS / play phases and drains the checkpoints they leave, the primary drains the
// power-on fork tree and runs the finalization (sections, static closure, tables,
// inference) on the union of both maps.
#include "explore.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int dp_nes2_dp_nes_explore(const uint8_t *data, size_t size, int mapper, const DpNesOptions *options,
                           DpNesResults *results);
void dp_nes2_dp_nes_results_free(DpNesResults *results);

typedef struct {
    const uint8_t *data;
    size_t size;
    int mapper;
    DpNesOptions opt;
    DpNesResults *res;
    pthread_t th;
    int ok;
} Job;

static void *job_main(void *p) {
    Job *j = p;
    j->ok = dp_nes2_dp_nes_explore(j->data, j->size, j->mapper, &j->opt, j->res);
    return NULL;
}

#define ADD(f) a->f += b->f
static void fold(DpNesResults *a, void *user) {
    Job *j = user;
    pthread_join(j->th, NULL);
    if (!j->ok) {
        fprintf(stderr, "deepeel: nes --jobs 2: the second instance failed; its findings are lost\n");
        return;
    }
    const DpNesResults *b = j->res;
    size_t n = a->prg_size < b->prg_size ? a->prg_size : b->prg_size;
    for (size_t i = 0; i < n; i++) {
        a->flags[i] |= b->flags[i];
        uint64_t e = (uint64_t)a->exec_hits[i] + b->exec_hits[i], d = (uint64_t)a->data_hits[i] + b->data_hits[i];
        a->exec_hits[i] = e > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e;
        a->data_hits[i] = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
    }
    if (a->chr_flags && b->chr_flags) {
        size_t m = a->chr_size < b->chr_size ? a->chr_size : b->chr_size;
        for (size_t i = 0; i < m; i++) a->chr_flags[i] |= b->chr_flags[i];
        a->chr_tiles_bg = a->chr_tiles_spr = 0;   // distinct tiles: recounted on the union
        for (size_t t = 0; t + 16 <= a->chr_size; t += 16) {
            if (a->chr_flags[t] & DP_NES_CHR_BG) a->chr_tiles_bg++;
            if (a->chr_flags[t] & DP_NES_CHR_SPR) a->chr_tiles_spr++;
        }
    }
    dp_ramarch_merge(&a->ra, &b->ra);
    ADD(s6_decoders); ADD(s6_tables); ADD(s6_candidates); ADD(s6_runs); ADD(s6_sat); ADD(steps); ADD(paths); ADD(forks); ADD(irq_forks); ADD(exec_outside); ADD(data_reads); ADD(oam_copies);
    ADD(dmc_starts); ADD(forced_paths); ADD(step_cuts); ADD(loop_cuts); ADD(fork_cuts); ADD(queue_cuts);
    ADD(global_cuts); ADD(path_cuts); ADD(faults); ADD(decode_cuts); ADD(ctx_evicted); ADD(pad_probes);
    ADD(pad_probe_sites); ADD(pad_probe_refused); ADD(evicted); ADD(parked); ADD(parked_resumed);
    ADD(parked_lost); ADD(pending_dropped); ADD(ram_writes); ADD(smc_writes); ADD(io_writes); ADD(io_reads); ADD(chr_reads);
    ADD(play_frames_run); ADD(play_sessions_run); ADD(play_steps_accounted); ADD(seeds_play);
    ADD(tas_same); ADD(tas_different); ADD(tas_unrecorded); ADD(cpu_copy_bytes);
    ADD(novelty_trials); ADD(novelty_hits); ADD(novelty_archived); ADD(novelty_replaced);
    ADD(novelty_frames_run); ADD(novelty_deaths);
    if (b->novelty_depth > a->novelty_depth) a->novelty_depth = b->novelty_depth;
    ADD(seeds_irq); ADD(seeds_refused_queue);
    for (int i = 0; i < 40; i++) { a->io_reg_writes[i] += b->io_reg_writes[i]; a->io_reg_reads[i] += b->io_reg_reads[i]; }
    for (int i = 0; i < 2; i++) a->irq_counts[i] += b->irq_counts[i];
    // logs: a record already present (same site) is merged, the rest appended; overflow is counted
    for (unsigned i = 0; i < b->copy_count; i++) {
        unsigned k;
        for (k = 0; k < a->copy_count; k++)
            if (a->copies[k].source == b->copies[i].source && a->copies[k].pc == b->copies[i].pc &&
                a->copies[k].channel == b->copies[i].channel) {
                a->copies[k].occurrences += b->copies[i].occurrences;
                break;
            }
        if (k < a->copy_count) continue;
        if (a->copy_count < DP_NES_COPY_CAP) a->copies[a->copy_count++] = b->copies[i];
        else a->copy_overflow++;
    }
    a->copy_overflow += b->copy_overflow;
    for (unsigned i = 0; i < b->bank_count; i++) {
        unsigned k;
        for (k = 0; k < a->bank_count; k++)
            if (a->banks[k].pc == b->banks[i].pc && a->banks[k].window == b->banks[i].window &&
                a->banks[k].old_bank == b->banks[i].old_bank && a->banks[k].new_bank == b->banks[i].new_bank) break;
        if (k < a->bank_count) continue;
        if (a->bank_count < DP_NES_BANK_LOG_CAP) a->banks[a->bank_count++] = b->banks[i];
        else a->bank_overflow++;
    }
    a->bank_overflow += b->bank_overflow;
    for (unsigned i = 0; i < b->interrupt_count; i++) {
        unsigned k;
        for (k = 0; k < a->interrupt_count; k++)
            if (a->interrupts[k].pc == b->interrupts[i].pc && a->interrupts[k].kind == b->interrupts[i].kind) break;
        if (k < a->interrupt_count) continue;
        if (a->interrupt_count < DP_NES_IRQ_LOG_CAP) a->interrupts[a->interrupt_count++] = b->interrupts[i];
        else a->interrupt_overflow++;
    }
    a->interrupt_overflow += b->interrupt_overflow;
    for (unsigned i = 0; i < b->register_count; i++) {
        unsigned k;
        for (k = 0; k < a->register_count; k++)
            if (a->register_writes[k].reg == b->register_writes[i].reg &&
                a->register_writes[k].value == b->register_writes[i].value) break;
        if (k < a->register_count) continue;
        if (a->register_count < 256) a->register_writes[a->register_count++] = b->register_writes[i];
        else a->register_overflow++;
    }
    a->register_overflow += b->register_overflow;
    a->jobs_folded++;
}

int dp_nes_explore_jobs(const uint8_t *data, size_t size, int mapper, const DpNesOptions *options,
                        DpNesResults *results, int jobs) {
    DpNesOptions o = options ? *options : dp_nes_options();
    int phases = o.tas_count || o.play_frames || o.novelty_steps;
    if (jobs < 2 || !phases) {
        if (jobs >= 2)
            fprintf(stderr, "deepeel: nes --jobs 2 needs --tas, --play or --novelty to split the work; "
                            "running on one instance\n");
        return dp_nes_explore(data, size, mapper, &o, results);
    }
    Job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->res = calloc(1, sizeof *j->res);
    if (!j->res) { free(j); return 0; }
    j->data = data;
    j->size = size;
    j->mapper = mapper;
    j->opt = o;
    j->opt.skip_root = 1;
    j->opt.no_finalize = 1;
    j->opt.lot_path = NULL;
    j->opt.lot_drain = 0;
    j->opt.tree_path = NULL;
    j->opt.second_pass = 0;
    j->opt.pre_finalize = NULL;
    o.play_frames = 0;
    o.play_sessions = 0;
    o.tas_count = 0;
    o.novelty_steps = 0;
    o.watch_every = 0;
    o.pre_finalize = fold;
    o.pre_user = j;
    if (pthread_create(&j->th, NULL, job_main, j)) {
        free(j->res); free(j);
        return dp_nes_explore(data, size, mapper, options, results);
    }
    int ok = dp_nes_explore(data, size, mapper, &o, results);
    if (!ok) pthread_join(j->th, NULL);   // a primary that failed before the fold must still join
    dp_nes2_dp_nes_results_free(j->res);
    free(j->res);
    free(j);
    results->options = options ? *options : dp_nes_options();
    return ok;
}
