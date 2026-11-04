// jobs.c: PC Engine --jobs 2.  The runtime and the explorer are process
// globals, so the second instance is a complete namespaced copy of the
// PCE stack (dp_pce2_ prefix, built by objcopy like the Sega/MSX clones).
// The work is split by phase, not by queue: the secondary takes the
// TAS / play / novelty phases and drains the checkpoints they leave, the
// primary drains the power-on fork tree and runs the finalization (static
// closure, tables, sections, inference) on the union of both maps.
#include "explore.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int dp_pce2_dp_pce_explore(const uint8_t *data, size_t size, const DpPceOptions *options,
                           DpPceResults *results);
void dp_pce2_dp_pce_results_free(DpPceResults *results);

typedef struct {
    const uint8_t *data;
    size_t size;
    DpPceOptions opt;
    DpPceResults *res;
    pthread_t th;
    int ok;
} Job;

static void *job_main(void *p) {
    Job *j = p;
    j->ok = dp_pce2_dp_pce_explore(j->data, j->size, &j->opt, j->res);
    return NULL;
}

#define ADD(f) a->f += b->f
static void merge_counts(DpPceResults *a, const DpPceResults *b) {
    dp_ramarch_merge(&a->ra, &b->ra);
    ADD(s6_decoders); ADD(s6_tables); ADD(s6_candidates); ADD(s6_runs); ADD(s6_sat);
    ADD(steps); ADD(paths); ADD(forks); ADD(irq_forks); ADD(pad_forks); ADD(exec_outside);
    ADD(data_reads); ADD(forced_paths); ADD(evicted); ADD(faults); ADD(illegal_cuts);
    ADD(step_cuts); ADD(loop_cuts); ADD(fork_cuts); ADD(pad_fork_cuts); ADD(irq_fork_cuts);
    ADD(queue_cuts); ADD(global_cuts); ADD(parked); ADD(parked_resumed); ADD(parked_lost);
    ADD(pending_dropped); ADD(demo_frames_run); ADD(play_frames_run); ADD(play_sessions_run); ADD(pool_saved); ADD(pool_starts); ADD(tas_frames_run);
    ADD(novelty_trials); ADD(novelty_hits); ADD(novelty_archived); ADD(novelty_replaced);
    ADD(novelty_frames_run); ADD(novelty_deaths); ADD(io_writes); ADD(io_reads); for (int k_ = 0; k_ < DP_PCE_IO_SLOTS; k_++) { a->io_reg_reads[k_] += b->io_reg_reads[k_]; a->io_reg_writes[k_] += b->io_reg_writes[k_]; } ADD(smc_writes); ADD(vdc_data_writes);
    ADD(copied_bytes); ADD(vce_data_writes);
    for (int i = 0; i < 3; i++) a->irq_counts[i] += b->irq_counts[i];
    if (b->novelty_depth > a->novelty_depth) a->novelty_depth = b->novelty_depth;
    if (b->state_bytes_peak > a->state_bytes_peak) a->state_bytes_peak = b->state_bytes_peak;
}

// logs: a record already present (same site) is merged, the rest appended;
// whatever does not fit is counted in the overflow, never dropped silently
static void merge_logs(DpPceResults *a, const DpPceResults *b) {
    for (unsigned i = 0; i < b->copy_count; i++) {
        const DpPceCopy *c = &b->copies[i];
        unsigned k;
        for (k = 0; k < a->copy_count; k++)
            if (a->copies[k].source == c->source && a->copies[k].pc == c->pc &&
                a->copies[k].channel == c->channel) {
                a->copies[k].occurrences += c->occurrences;
                if (c->length > a->copies[k].length) a->copies[k].length = c->length;
                break;
            }
        if (k < a->copy_count) continue;
        if (a->copy_count < DP_PCE_COPY_CAP) a->copies[a->copy_count++] = *c;
        else a->copy_overflow++;
    }
    a->copy_overflow += b->copy_overflow;
    for (unsigned i = 0; i < b->bank_count; i++) {
        const DpPceBankEvent *e = &b->banks[i];
        unsigned k;
        for (k = 0; k < a->bank_count; k++)
            if (a->banks[k].pc == e->pc && a->banks[k].window == e->window &&
                a->banks[k].old_page == e->old_page && a->banks[k].new_page == e->new_page) break;
        if (k < a->bank_count) continue;
        if (a->bank_count < DP_PCE_BANK_LOG_CAP) {
            a->banks[a->bank_count] = *e;
            a->banks[a->bank_count].seq = a->bank_count + 1;
            a->bank_count++;
        } else a->bank_overflow++;
    }
    a->bank_overflow += b->bank_overflow;
    for (unsigned i = 0; i < b->interrupt_count; i++) {
        const DpPceInterrupt *e = &b->interrupts[i];
        unsigned k;
        for (k = 0; k < a->interrupt_count; k++)
            if (a->interrupts[k].pc == e->pc && a->interrupts[k].kind == e->kind) break;
        if (k < a->interrupt_count) continue;
        if (a->interrupt_count < DP_PCE_IRQ_LOG_CAP) a->interrupts[a->interrupt_count++] = *e;
        else a->interrupt_overflow++;
    }
    a->interrupt_overflow += b->interrupt_overflow;
    for (unsigned i = 0; i < b->register_count; i++) {
        const DpPceRegisterWrite *e = &b->register_writes[i];
        unsigned k;
        for (k = 0; k < a->register_count; k++)
            if (a->register_writes[k].reg == e->reg && a->register_writes[k].value == e->value) break;
        if (k < a->register_count) continue;
        if (a->register_count < DP_PCE_REG_LOG_CAP) a->register_writes[a->register_count++] = *e;
        else a->register_overflow++;
    }
    a->register_overflow += b->register_overflow;
}

static void fold(DpPceResults *a, void *user) {
    Job *j = user;
    pthread_join(j->th, NULL);
    if (!j->ok) {
        fprintf(stderr, "deepeel: pce --jobs 2: the second instance failed; its findings are lost\n");
        return;
    }
    const DpPceResults *b = j->res;
    for (size_t i = 0; i < a->rom_size && i < b->rom_size; i++) { a->flags[i] |= b->flags[i]; if (a->exec_kind && b->exec_kind) a->exec_kind[i] |= b->exec_kind[i];
        if (a->exec_hits && b->exec_hits) { uint64_t s = (uint64_t)a->exec_hits[i] + b->exec_hits[i]; a->exec_hits[i] = s > UINT32_MAX ? UINT32_MAX : (uint32_t)s; }
        if (a->live_io && b->live_io) a->live_io[i] |= b->live_io[i];
        if (a->data_hits && b->data_hits) { uint64_t s = (uint64_t)a->data_hits[i] + b->data_hits[i]; a->data_hits[i] = s > UINT32_MAX ? UINT32_MAX : (uint32_t)s; } }
    if (a->pal_flags && b->pal_flags) {
        a->palette_bytes = 0;
        for (size_t i = 0; i < a->rom_size; i++) {
            a->pal_flags[i] |= b->pal_flags[i];
            a->palette_bytes += a->pal_flags[i];
        }
    }
    for (unsigned i = 0; i < b->nidx; i++) {   // indexed sites: union by (site, base, cell0)
        unsigned k;
        for (k = 0; k < a->nidx; k++)
            if (a->idx[k].site == b->idx[i].site && a->idx[k].base == b->idx[i].base &&
                a->idx[k].cell0 == b->idx[i].cell0) break;
        if (k == a->nidx) {
            if (k >= DP_PCE_IDX_CAP) { a->idx_overflow++; continue; }
            a->idx[k] = b->idx[i];
            a->nidx++;
        } else {
            if (b->idx[i].idx_min < a->idx[k].idx_min) a->idx[k].idx_min = b->idx[i].idx_min;
            if (b->idx[i].idx_max > a->idx[k].idx_max) a->idx[k].idx_max = b->idx[i].idx_max;
            a->idx[k].n_obs += b->idx[i].n_obs;
        }
    }
    a->idx_overflow += b->idx_overflow;
    a->idx_cross_page += b->idx_cross_page;
    a->idx_forced_ignored += b->idx_forced_ignored;
    merge_counts(a, b);
    merge_logs(a, b);
}

int dp_pce_explore_jobs(const uint8_t *data, size_t size, const DpPceOptions *options,
                        DpPceResults *results, int jobs) {
    DpPceOptions o = options ? *options : dp_pce_options();
    int phases = o.tas_count || o.demo_frames || (o.play_frames && o.play_sessions) || o.novelty_steps;
    if (jobs < 2 || !phases) {
        if (jobs >= 2)
            fprintf(stderr, "deepeel: pce --jobs 2 needs --tas, --play/--sessions or --novelty "
                            "to split the work; running on one instance\n");
        return dp_pce_explore(data, size, &o, results);
    }
    Job *j = calloc(1, sizeof *j);
    j->res = calloc(1, sizeof *j->res);
    if (!j || !j->res) { free(j ? j->res : NULL); free(j); return 0; }
    j->data = data;
    j->size = size;
    j->opt = o;
    j->opt.skip_root = 1;
    j->opt.no_finalize = 1;
    j->opt.lot_path = NULL;
    j->opt.lot_drain = 0;
    j->opt.pre_finalize = NULL;
    o.play_frames = 0; o.demo_frames = 0;
    o.play_sessions = 0;
    o.novelty_steps = 0;
    o.watch_every = 0;
    o.tas_count = 0;
    o.pre_finalize = fold;
    o.pre_user = j;
    if (pthread_create(&j->th, NULL, job_main, j)) {
        free(j->res); free(j);
        return dp_pce_explore(data, size, options, results);
    }
    int ok = dp_pce_explore(data, size, &o, results);
    // pre_finalize joins the thread; a primary that failed before reaching it
    // must still join
    if (!ok) pthread_join(j->th, NULL);
    dp_pce2_dp_pce_results_free(j->res);
    free(j->res);
    free(j);
    results->options = options ? *options : dp_pce_options();
    return ok;
}
