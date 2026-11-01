// report.c: human text report, machine-readable JSON map, and a .cov export
// compatible with m0boy's coverage format (flags 1=exec 2=entry 4=speculative).
#include "report.h"
#include "../explore/tree.h"

// block classes: '.' untouched, 'C' code natural, 'c' code forced-only,
// 'D' data read, 'V' copied to VRAM/OAM, 'P' pointer table, 'p' padding
char dp_block_class(const DpResults *res, int bank, int block) {
    size_t base = (size_t)bank * 0x4000 + (size_t)block * 64;
    size_t end = base + 64;
    int natural = 0, forced = 0, copied = 0, data = 0, ptr = 0, pad = 0;
    for (size_t i = base; i < end && i < (size_t)res->banks * 0x4000; i++) {
        if (res->exec_flags[i] & DP_XF_NATURAL) natural++;
        else if (res->exec_flags[i] & DP_XF_FORCED) forced++;
        if (res->data_flags[i] & DP_DF_COPIED) copied++;
        else if (res->data_flags[i] & DP_DF_READ) data++;
        if (res->static_flags[i] & DP_SF_POINTER) ptr++;
        if (res->static_flags[i] & DP_SF_PAD) pad++;
    }
    if (natural) return 'C';
    if (forced) return 'c';
    if (copied) return 'V';
    if (data) return 'D';
    if (ptr) return 'P';
    if (pad) return 'p';
    return '.';
}

// run-length encode the block classes of one bank: "C12.D3..." style
static void bank_blocks_rle(const DpResults *res, int bank, DpBuf *out) {
    char prev = -1;
    int count = 0;
    for (int b = 0; b < 256; b++) {
        char cls = dp_block_class(res, bank, b);
        if (cls == prev) {
            count++;
        } else {
            if (prev >= 0) dp_buf_fmt(out, "%c%d.", prev, count);
            prev = cls;
            count = 1;
        }
    }
    if (prev >= 0) dp_buf_fmt(out, "%c%d.", prev, count);
}

static const char *verdict_name(uint8_t v) {
    switch (v) {
    case DP_V_REQUIRED: return "required";
    case DP_V_PLAYED: return "played";
    case DP_V_FORCED: return "forced";
    default: return "speculative";
    }
}

static const char *chan_name(uint8_t c) {
    switch (c) {
    case 1: return "oam_dma";
    case 2: return "hdma";
    default: return "cpu_loop";
    }
}

// --- text -----------------------------------------------------------------------
void dp_report_text(const DpResults *res, const DpConfig *cfg, DpBuf *out) {
    const DpCart *c = res->cart;
    char line[2048];
    snprintf(line, sizeof line, "deepeel — %s (%s, %d banks, %u KiB, RAM %u KiB)\n",
             c->title, dp_mbc_name(c->mbc), c->rom_banks, c->rom_size / 1024, c->ram_size / 1024);
    dp_buf_str(out, line);
    snprintf(line, sizeof line,
             "exploration: %llu steps in %llu paths (%llu forks: %u branch, %u irq, %u key), %u sections\n"
             "evicted: %u exact states, %u same-context entries; cuts: %u cycles, %u step-budget, %u padding\n"
             "nonsense evictions: %u echo/OAM, %u verbatim data, %u incoherent stack\n"
             "budgets: %u paths cap hit, %u step-wall hit, %u queue overflows (%u growths), %u forks capped, %u outside-rom exec\n"
             "seeds: %u natural, %u speculative\n"
             "data: %llu bytes proven verbatim (cov flag 8), %u exec∩data contradictions\n",
             (unsigned long long)res->steps_total, (unsigned long long)res->paths_run,
             (unsigned long long)(res->forks_branch + res->forks_irq + res->forks_key),
             res->forks_branch, res->forks_irq, res->forks_key,
             res->section_count, res->evicted_exact, res->evicted_ctx, res->cycle_cuts,
             res->step_cuts, res->pad_cuts, res->nonsense_echo, res->nonsense_data,
             res->nonsense_stack, res->budget_paths, res->budget_steps, res->queue_overflows,
             res->queue_growths, res->forks_capped, res->exec_outside,
             res->seeds_natural, res->seeds_spec,
             (unsigned long long)res->data_verbatim_bytes, res->data_contradictions);
    dp_buf_str(out, line);
    {
        uint64_t stat_only = 0, ind_sites = 0, ind_over = 0;
        size_t nb = (size_t)res->banks * 0x4000;
        for (size_t k = 0; k < nb; k++)
            if ((res->exec_flags[k] & DP_XF_STATIC) && !(res->exec_flags[k] & (DP_XF_NATURAL | DP_XF_FORCED)))
                stat_only++;
        for (size_t i = 0; i < res->indirect.cap; i++) {
            if (!res->indirect.used[i]) continue;
            ind_sites++;
            if (((const DpIndirect *)(res->indirect.vals + i * res->indirect.stride))->overflow) ind_over++;
        }
        snprintf(line, sizeof line,
                 "static closure: %u rounds, %llu bytes (%u instructions) reached by direct flow only, "
                 "%u seeds (%u dropped), %u stopped by data/padding, %u by undefined opcodes\n"
                 "  unresolved: %u bank-0 edges into $4000+ with no bank evidence, "
                 "%llu JP HL sites observed (%llu with more than 8 targets)\n"
                 "forks pruned (both sides covered): %u\n",
                 res->closure_rounds, (unsigned long long)stat_only, res->closure_starts,
                 res->closure_seeds, res->closure_seeds_dropped, res->closure_stopped_data,
                 res->closure_stopped_illegal, res->closure_unresolved_bank,
                 (unsigned long long)ind_sites, (unsigned long long)ind_over, res->forks_covered_pruned);
        dp_buf_str(out, line);
        snprintf(line, sizeof line,
                 "  seeding: %u bank candidates on the ambiguous edges, %u JP HL targets seeded, "
                 "%u seed refusals retried\n",
                 res->closure_bank_seeds, res->indirect_seeds, res->closure_seeds_refused);
        dp_buf_str(out, line);
        {
            DpMapSummary rsum;
            uint8_t *rmap = dp_map_build(res, &rsum);
            if (rmap) {
                dp_map_residual_text(res, rmap, out);
                free(rmap);
            }
        }
    }

    // per-bank coverage
    dp_buf_str(out, "\nper-bank executed bytes:\n");
    for (int bank = 0; bank < res->banks; bank++) {
        int nat = 0, frc = 0, dat = 0;
        for (int i = 0; i < 0x4000; i++) {
            size_t k = (size_t)bank * 0x4000 + i;
            if (res->exec_flags[k] & DP_XF_NATURAL) nat++;
            else if (res->exec_flags[k] & DP_XF_FORCED) frc++;
            if (res->data_flags[k] & DP_DF_READ) dat++;
        }
        if (!nat && !frc && !dat) continue;
        snprintf(line, sizeof line, "  bank %3d: %5d code (%5d natural) %5d data read\n", bank, nat + frc, nat, dat);
        dp_buf_str(out, line);
    }

    // sections: required first, then forced, top hits
    dp_buf_str(out, "\nsections (top 60 by hits):\n");
    dp_buf_str(out, "  bank:lo    hi     hits  nat/for  ctx  cond-both  io  copies  verdict\n");
    // simple selection: iterate sections, keep index list sorted by hits desc, cap 60
    uint32_t idx[60];
    int n = 0;
    for (int i = 0; i < res->section_count; i++) {
        if (n < 60) {
            idx[n++] = (uint32_t)i;
        } else {
            int worst = 0;
            for (int j = 1; j < 60; j++)
                if (res->sections[idx[j]].hits < res->sections[idx[worst]].hits) worst = j;
            if (res->sections[i].hits > res->sections[idx[worst]].hits) idx[worst] = (uint32_t)i;
        }
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++)
            if (res->sections[idx[j]].hits > res->sections[idx[i]].hits) {
                uint32_t t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
    }
    for (int i = 0; i < n; i++) {
        const DpSection *s = &res->sections[idx[i]];
        snprintf(line, sizeof line, "  %3d:%04x-%04x %7u %4u/%-4u %4u %5u/%-3u %3u %6u  %s\n",
                 s->bank, s->lo, s->hi, s->hits, s->natural_hits, s->forced_hits, s->ctx_count,
                 s->cond_both, s->cond_sites, s->io_used_n, s->copies, verdict_name(s->verdict));
        dp_buf_str(out, line);
    }

    // io usage
    dp_buf_str(out, "\nregister usage:\n");
    for (int i = 0; i < res->io_count; i++) {
        const DpIoStat *st = &res->io[i];
        const DpIoReg *r = &res->plat->io_regs[i];
        if (!st->rd && !st->wr) continue;
        snprintf(line, sizeof line, "  %-6s %s rd=%-7u wr=%-7u", r->name, r->category, st->rd, st->wr);
        dp_buf_str(out, line);
        if (st->nvalues) {
            dp_buf_str(out, " values=");
            for (int v = 0; v < st->nvalues; v++) dp_buf_fmt(out, "%02x,", st->values[v]);
        }
        if (st->bank_events) dp_buf_fmt(out, " banks=%u", st->bank_events);
        if (st->copies) dp_buf_fmt(out, " copies=%u", st->copies);
        dp_buf_ch(out, '\n');
    }

    // copies
    if (res->copy_count) {
        dp_buf_fmt(out, "\ncopies (%d):\n", res->copy_count);
        int show = res->copy_count > 40 ? 40 : res->copy_count;
        for (int i = 0; i < show; i++) {
            const DpCopyRec *r = &res->copies[i];
            snprintf(line, sizeof line, "  %-8s rom b%u:%04x-%04x -> %s b%u:%04x-%04x (%u bytes)%s at %04x\n",
                     chan_name(r->channel), r->src_bank, r->src_lo, r->src_hi,
                     r->dst_space == DP_SPACE_VRAM ? "vram" : "oam", r->dst_bank, r->dst_lo, r->dst_hi,
                     r->bytes, r->verbatim ? " verbatim" : "", r->pc);
            dp_buf_str(out, line);
        }
        if (res->copy_count > show) dp_buf_fmt(out, "  ... %d more\n", res->copy_count - show);
    }

    // bank switches
    if (res->bank_count) {
        dp_buf_fmt(out, "\nbank switches (%d", res->bank_count);
        int show = res->bank_count > 30 ? 30 : res->bank_count;
        dp_buf_str(out, "):\n");
        for (int i = 0; i < show; i++) {
            const DpBankEvent *e = &res->bank_events[i];
            static const char *wn[3] = { "rom0", "romN", "ram" };
            snprintf(line, sizeof line, "  at %04x: %s %u -> %u\n", e->pc, wn[e->window], e->old_bank, e->new_bank);
            dp_buf_str(out, line);
        }
        if (res->bank_count > show) dp_buf_fmt(out, "  ... %d more\n", res->bank_count - show);
    }

    if (res->bad_op_count) {
        dp_buf_fmt(out, "\nundefined opcodes: %d (first sites:", res->bad_op_count);
        int show = res->bad_op_count > 8 ? 8 : res->bad_op_count;
        for (int i = 0; i < show; i++)
            dp_buf_fmt(out, " %06x=%02x", res->bad_ops[i].key, res->bad_ops[i].op);
        dp_buf_str(out, ")\n");
    }
    if (res->disc_n) {
        // discovery log: who found how much, and when (wall clock, tool observation)
        int show = res->disc_n > 40 ? 40 : res->disc_n;
        dp_buf_fmt(out, "\ndiscovery log (%d entries%s): time(s) source credit known-bytes\n", res->disc_n,
                   show < res->disc_n ? ", last 40 shown; full log in --html" : "");
        for (int i = res->disc_n - show; i < res->disc_n; i++) {
            const DpDiscEntry *e = &res->disc[i];
            dp_buf_fmt(out, "  %8.2f  %-28s %10llu  %u\n", e->t, e->source, (unsigned long long)e->credit, e->known);
        }
    }
    (void)cfg;
}

// --- json -----------------------------------------------------------------------
static void json_esc(DpBuf *out, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') dp_buf_fmt(out, "\\%c", *p);
        else if ((unsigned char)*p < 0x20) dp_buf_fmt(out, "\\u%04x", *p);
        else dp_buf_ch(out, *p);
    }
}

void dp_report_json(const DpResults *res, const DpConfig *cfg, DpBuf *o) {
    const DpCart *c = res->cart;
    dp_buf_str(o, "{\n\"rom\": {");
    dp_buf_fmt(o, "\"title\":\"");
    json_esc(o, c->title);
    dp_buf_fmt(o, "\",\"mbc\":\"%s\",\"banks\":%d,\"size\":%u,\"ram\":%u},\n",
               dp_mbc_name(c->mbc), c->rom_banks, c->rom_size, c->ram_size);
    dp_buf_fmt(o, "\"config\": {\"max_steps_path\":%u,\"max_paths\":%u,\"max_steps_total\":%llu,"
                  "\"queue_cap\":%u,\"queue_max\":%u,\"mem_cap_mb\":%llu,\"loop_back_cap\":%u,"
                  "\"seed_cap\":%u,\"max_copies\":%u,\"closure_rounds\":%d,\"covered_fork_keep\":%u,"
                  "\"seed_banks\":%d},\n",
               cfg->max_steps_path, cfg->max_paths, (unsigned long long)cfg->max_steps_total,
               cfg->queue_cap, cfg->queue_max, (unsigned long long)cfg->mem_cap_mb,
               cfg->loop_back_cap, cfg->seed_cap, cfg->max_copies, cfg->closure_rounds,
               cfg->covered_fork_keep, cfg->seed_banks);
    dp_buf_fmt(o, "\"counters\": {\"steps\":%llu,\"paths\":%u,\"forks_branch\":%u,\"forks_irq\":%u,"
                  "\"forks_key\":%u,\"forks_capped\":%u,\"forks_covered_pruned\":%u,"
                  "\"sections\":%u,\"evicted_exact\":%u,\"evicted_ctx\":%u,\"cycle_cuts\":%u,"
                  "\"step_cuts\":%u,\"pad_cuts\":%u,\"halt_cuts\":%u,\"nonsense_echo\":%u,\"nonsense_data\":%u,\"nonsense_stack\":%u,\"budget_paths\":%u,\"budget_steps\":%u,\"queue_overflows\":%u,\"queue_growths\":%u,"
                  "\"exec_outside\":%u,\"seeds_natural\":%u,\"seeds_spec\":%u,\"seeds_skipped_data\":%u,\"bad_ops\":%u},\n",
               (unsigned long long)res->steps_total, (unsigned long long)res->paths_run, res->forks_branch, res->forks_irq,
               res->forks_key, res->forks_capped, res->forks_covered_pruned,
               (unsigned)res->section_count, res->evicted_exact, res->evicted_ctx, res->cycle_cuts,
               res->step_cuts, res->pad_cuts, res->halt_cuts, res->nonsense_echo, res->nonsense_data,
               res->nonsense_stack, res->budget_paths, res->budget_steps, res->queue_overflows,
               res->queue_growths, res->exec_outside, res->seeds_natural, res->seeds_spec,
               res->seeds_skipped_data, (unsigned)res->bad_op_count);
    dp_buf_fmt(o, "\"closure\": {\"rounds\":%u,\"starts\":%u,\"unresolved_bank\":%u,"
                  "\"stopped_data\":%u,\"stopped_illegal\":%u,\"seeds\":%u,\"seeds_dropped\":%u,"
                  "\"seeds_refused\":%u,\"bank_seeds\":%u,\"indirect_seeds\":%u},\n",
               res->closure_rounds, res->closure_starts, res->closure_unresolved_bank,
               res->closure_stopped_data, res->closure_stopped_illegal, res->closure_seeds,
               res->closure_seeds_dropped, res->closure_seeds_refused, res->closure_bank_seeds,
               res->indirect_seeds);
    dp_buf_fmt(o, "\"data\": {\"verbatim_bytes\":%llu,\"contradictions\":%u},\n",
               (unsigned long long)res->data_verbatim_bytes, res->data_contradictions);
    {   // map summary + residual ranges (the aggregation view)
        DpMapSummary s;
        uint8_t *m = dp_map_build(res, &s);
        dp_buf_str(o, "\"map\": {");
        dp_buf_fmt(o, "\"known_pct\":%.2f,\"unknown_pct\":%.2f,\"inferred_pct\":%.2f",
                   s.total ? 100.0 * (double)s.known / (double)s.total : 0.0,
                   s.total ? 100.0 * (double)s.unknown / (double)s.total : 0.0,
                   s.total ? 100.0 * (double)s.inferred / (double)s.total : 0.0);
        dp_buf_str(o, ",\"classes\":{");
        for (int cl = 0; cl < DP_MAP_COUNT; cl++)
            dp_buf_fmt(o, "%s\"%s\":%llu", cl ? "," : "", dp_map_class_name(cl),
                       (unsigned long long)s.cls[cl]);
        dp_buf_str(o, "}");
        if (m) {
            DpResidualRange rr[4096];
            int nr = dp_map_residual_list(res, m, rr, 4096);
            dp_buf_fmt(o, ",\"residual_bytes\":%llu,\"residual_ranges\":%d,\"residual\":[",
                       (unsigned long long)s.unknown, nr);
            for (int i = 0; i < nr && i < 4096; i++)
                dp_buf_fmt(o, "%s{\"bank\":%u,\"lo\":%u,\"hi\":%u,\"len\":%u}", i ? "," : "",
                           rr[i].bank, rr[i].lo, rr[i].hi, rr[i].len);
            dp_buf_str(o, "]");
            free(m);
        }
        dp_buf_str(o, "},\n");
    }
    // banks
    dp_buf_str(o, "\"banks\": [\n");
    for (int bank = 0; bank < res->banks; bank++) {
        dp_buf_fmt(o, "{\"bank\":%d,\"blocks\":\"", bank);
        bank_blocks_rle(res, bank, o);
        dp_buf_str(o, "\"}");
        if (bank + 1 < res->banks) dp_buf_ch(o, ',');
        dp_buf_ch(o, '\n');
    }
    dp_buf_str(o, "],\n");
    // sections
    dp_buf_str(o, "\"sections\": [\n");
    for (int i = 0; i < res->section_count; i++) {
        const DpSection *s = &res->sections[i];
        dp_buf_fmt(o, "{\"id\":%u,\"bank\":%d,\"lo\":%u,\"hi\":%u,\"hits\":%u,\"entries\":%u,"
                      "\"natural\":%u,\"forced\":%u,\"verdict\":\"%s\",\"ctx\":%u,"
                      "\"cond_sites\":%u,\"cond_both\":%u,\"copies\":%u,\"io_used\":[",
                   s->id, s->bank, s->lo, s->hi, s->hits, s->entries, s->natural_hits, s->forced_hits,
                   verdict_name(s->verdict), s->ctx_count, s->cond_sites, s->cond_both, s->copies);
        for (int j = 0; j < s->io_used_n; j++) {
            const DpIoReg *r = &res->plat->io_regs[s->io_used[j]];
            dp_buf_fmt(o, "%s\"%s\"", j ? "," : "", r->name);
        }
        dp_buf_str(o, "],\"banks_seen\":[");
        for (int j = 0; j < s->banks_seen_n; j++) dp_buf_fmt(o, "%s%u", j ? "," : "", s->banks_seen[j]);
        dp_buf_str(o, "],\"callers\":[");
        for (int j = 0; j < s->callers_n; j++) dp_buf_fmt(o, "%s%u", j ? "," : "", s->callers[j]);
        dp_buf_str(o, "]}");
        if (i + 1 < res->section_count) dp_buf_ch(o, ',');
        dp_buf_ch(o, '\n');
    }
    dp_buf_str(o, "],\n");
    // io
    dp_buf_str(o, "\"io\": [\n");
    int first = 1;
    for (int i = 0; i < res->io_count; i++) {
        const DpIoStat *st = &res->io[i];
        const DpIoReg *r = &res->plat->io_regs[i];
        if (!st->rd && !st->wr) continue;
        if (!first) dp_buf_str(o, ",\n");
        first = 0;
        dp_buf_fmt(o, "{\"addr\":\"%04x\",\"name\":\"%s\",\"category\":\"%s\",\"rd\":%u,\"wr\":%u,\"values\":[",
                   r->lo, r->name, r->category, st->rd, st->wr);
        for (int v = 0; v < st->nvalues; v++) dp_buf_fmt(o, "%s%u", v ? "," : "", st->values[v]);
        dp_buf_str(o, "],\"pcs\":[");
        for (int v = 0; v < st->npcs; v++) dp_buf_fmt(o, "%s%u", v ? "," : "", st->pcs[v]);
        dp_buf_fmt(o, "],\"copies\":%u,\"bank_events\":%u}", st->copies, st->bank_events);
    }
    dp_buf_str(o, "],\n");
    // copies
    dp_buf_str(o, "\"copies\": [\n");
    for (int i = 0; i < res->copy_count; i++) {
        const DpCopyRec *r = &res->copies[i];
        dp_buf_fmt(o, "{\"channel\":\"%s\",\"verbatim\":%s,\"src\":{\"space\":\"%s\",\"bank\":%u,\"lo\":%u,\"hi\":%u},"
                      "\"dst\":{\"space\":\"%s\",\"bank\":%u,\"lo\":%u,\"hi\":%u},\"bytes\":%u,\"pc\":%u}",
                   chan_name(r->channel), r->verbatim ? "true" : "false",
                   r->src_space == DP_SPACE_ROM ? "rom" : (r->src_space == DP_SPACE_WRAM ? "wram" : "vram"),
                   r->src_bank, r->src_lo, r->src_hi,
                   r->dst_space == DP_SPACE_VRAM ? "vram" : "oam", r->dst_bank, r->dst_lo, r->dst_hi,
                   r->bytes, r->pc);
        if (i + 1 < res->copy_count) dp_buf_ch(o, ',');
        dp_buf_ch(o, '\n');
    }
    dp_buf_str(o, "],\n");
    // bank events
    dp_buf_str(o, "\"bank_events\": [\n");
    for (int i = 0; i < res->bank_count; i++) {
        const DpBankEvent *e = &res->bank_events[i];
        static const char *wn[3] = { "rom0", "romN", "ram" };
        dp_buf_fmt(o, "{\"pc\":%u,\"window\":\"%s\",\"old\":%u,\"new\":%u}", e->pc, wn[e->window],
                   e->old_bank, e->new_bank);
        if (i + 1 < res->bank_count) dp_buf_ch(o, ',');
        dp_buf_ch(o, '\n');
    }
    dp_buf_str(o, "],\n");
    // bad ops
    dp_buf_str(o, "\"bad_ops\": [");
    for (int i = 0; i < res->bad_op_count && i < 64; i++)
        dp_buf_fmt(o, "%s%u", i ? "," : "", res->bad_ops[i].key);
    dp_buf_str(o, "]\n}\n");
}

// --- summary ------------------------------------------------------------------
// the compact per-run profile: everything a multi-ROM campaign aggregates,
// one JSON small enough to keep for every run
void dp_report_summary(const DpResults *res, const DpConfig *cfg, const DpTree *tree,
                       const char *stop_reason, unsigned long long frames, int tas_n, DpBuf *o) {
    const DpCart *c = res->cart;
    dp_buf_str(o, "{\n\"rom\": {");
    dp_buf_fmt(o, "\"title\":\"");
    json_esc(o, c->title);
    dp_buf_fmt(o, "\",\"mbc\":\"%s\",\"banks\":%d,\"size\":%u,\"ram\":%u},\n",
               dp_mbc_name(c->mbc), c->rom_banks, c->rom_size, c->ram_size);
    dp_buf_fmt(o, "\"config\": {\"max_steps_path\":%u,\"max_paths\":%u,\"max_steps_total\":%llu,"
                  "\"queue_cap\":%u,\"queue_max\":%u,\"mem_cap_mb\":%llu,\"loop_back_cap\":%u,"
                  "\"seed_cap\":%u,\"closure_rounds\":%d,\"covered_fork_keep\":%u,\"seed_banks\":%d},\n",
               cfg->max_steps_path, cfg->max_paths, (unsigned long long)cfg->max_steps_total,
               cfg->queue_cap, cfg->queue_max, (unsigned long long)cfg->mem_cap_mb,
               cfg->loop_back_cap, cfg->seed_cap, cfg->closure_rounds, cfg->covered_fork_keep,
               cfg->seed_banks);
    dp_buf_fmt(o, "\"play\": {\"frames\":%llu,\"tas_n\":%d,\"stop_reason\":\"%s\"},\n",
               frames, tas_n, stop_reason ? stop_reason : "explore-only");
    dp_buf_str(o, "\"tree\": {");
    if (tree) {
        dp_buf_fmt(o, "\"choices\":%u,\"open\":%u,\"max_depth\":%u,\"closed\":{", tree->n ? tree->n - 1 : 0,
                   tree->open, tree->max_depth);
        int first = 1;
        for (int i = 1; i < DP_ISSUE_N; i++) {
            if (!tree->closed[i]) continue;
            dp_buf_fmt(o, "%s\"%s\":%u", first ? "" : ",", dp_issue_name(i), tree->closed[i]);
            first = 0;
        }
        dp_buf_str(o, "}");
    } else {
        dp_buf_str(o, "\"choices\":0,\"open\":0,\"max_depth\":0,\"closed\":{}");
    }
    dp_buf_str(o, "},\n");
    {   // map summary + full residual list
        DpMapSummary s;
        uint8_t *m = dp_map_build(res, &s);
        dp_buf_str(o, "\"map\": {");
        dp_buf_fmt(o, "\"known_pct\":%.3f,\"unknown_pct\":%.3f,\"inferred_pct\":%.3f,"
                      "\"code_pct\":%.3f,\"data_pct\":%.3f,\"pad_pct\":%.3f",
                   s.total ? 100.0 * (double)s.known / (double)s.total : 0.0,
                   s.total ? 100.0 * (double)s.unknown / (double)s.total : 0.0,
                   s.total ? 100.0 * (double)s.inferred / (double)s.total : 0.0,
                   s.total ? 100.0 * (double)s.code / (double)s.total : 0.0,
                   s.total ? 100.0 * (double)s.data / (double)s.total : 0.0,
                   s.total ? 100.0 * (double)s.pad / (double)s.total : 0.0);
        dp_buf_str(o, ",\"classes\":{");
        for (int cl = 0; cl < DP_MAP_COUNT; cl++)
            dp_buf_fmt(o, "%s\"%s\":%llu", cl ? "," : "", dp_map_class_name(cl),
                       (unsigned long long)s.cls[cl]);
        dp_buf_str(o, "}");
        if (m) {
            int cap = 16384;
            DpResidualRange *rr = malloc((size_t)cap * sizeof *rr);
            if (rr) {
                int nr = dp_map_residual_list(res, m, rr, cap);
                dp_buf_fmt(o, ",\"residual_bytes\":%llu,\"residual_ranges\":%d,\"residual\":[",
                           (unsigned long long)s.unknown, nr);
                for (int i = 0; i < nr && i < cap; i++)
                    dp_buf_fmt(o, "%s{\"bank\":%u,\"lo\":%u,\"hi\":%u,\"len\":%u}", i ? "," : "",
                               rr[i].bank, rr[i].lo, rr[i].hi, rr[i].len);
                dp_buf_str(o, "]");
                free(rr);
            }
            free(m);
        }
        dp_buf_str(o, "},\n");
    }
    dp_buf_fmt(o, "\"counters\": {\"steps\":%llu,\"paths\":%u,\"forks_branch\":%u,\"forks_irq\":%u,"
                  "\"forks_key\":%u,\"forks_capped\":%u,\"forks_covered_pruned\":%u,\"sections\":%u,"
                  "\"evicted_exact\":%u,\"evicted_ctx\":%u,\"cycle_cuts\":%u,\"step_cuts\":%u,"
                  "\"pad_cuts\":%u,\"halt_cuts\":%u,\"nonsense_echo\":%u,\"nonsense_data\":%u,"
                  "\"nonsense_stack\":%u,\"budget_paths\":%u,\"budget_steps\":%u,"
                  "\"queue_overflows\":%u,\"queue_growths\":%u,\"exec_outside\":%u,"
                  "\"seeds_natural\":%u,\"seeds_spec\":%u,\"seeds_skipped_data\":%u,\"bad_ops\":%u},\n",
               (unsigned long long)res->steps_total, (unsigned long long)res->paths_run,
               res->forks_branch, res->forks_irq, res->forks_key, res->forks_capped,
               res->forks_covered_pruned, (unsigned)res->section_count, res->evicted_exact,
               res->evicted_ctx, res->cycle_cuts, res->step_cuts, res->pad_cuts, res->halt_cuts,
               res->nonsense_echo, res->nonsense_data, res->nonsense_stack, res->budget_paths,
               res->budget_steps, res->queue_overflows, res->queue_growths, res->exec_outside,
               res->seeds_natural, res->seeds_spec, res->seeds_skipped_data,
               (unsigned)res->bad_op_count);
    dp_buf_fmt(o, "\"closure\": {\"rounds\":%u,\"starts\":%u,\"unresolved_bank\":%u,"
                  "\"stopped_data\":%u,\"stopped_illegal\":%u,\"seeds\":%u,\"seeds_dropped\":%u,"
                  "\"seeds_refused\":%u,\"bank_seeds\":%u,\"indirect_seeds\":%u},\n",
               res->closure_rounds, res->closure_starts, res->closure_unresolved_bank,
               res->closure_stopped_data, res->closure_stopped_illegal, res->closure_seeds,
               res->closure_seeds_dropped, res->closure_seeds_refused, res->closure_bank_seeds,
               res->indirect_seeds);
    dp_buf_fmt(o, "\"data\": {\"verbatim_bytes\":%llu,\"contradictions\":%u}\n}\n",
               (unsigned long long)res->data_verbatim_bytes, res->data_contradictions);
}

// --- cov -------------------------------------------------------------------------
// m0boy coverage v1: "bank addr flags count".  Flags: 1 = executed, 2 = entry,
// 4 = speculative (m0boy's static walk), 8 = proven data (this tool's
// verbatim-copy proof and its pointer tables).  A byte never exports both
// execution and the data bit: execution wins, the contradiction is counted.
// Data lines let m0boy's loader exclude those bytes even when another
// coverage source (its static walk) proposed them as code.
void dp_report_cov(const DpResults *res, DpBuf *o) {
    dp_buf_str(o, "# gbl coverage v1: bank addr flags(1=exec,2=entry,4=speculative,8=data) hits\n");
    for (int bank = 0; bank < res->banks; bank++) {
        for (int a = 0; a < 0x4000; a++) {
            size_t k = (size_t)bank * 0x4000 + a;
            uint8_t ef = res->exec_flags[k];
            uint8_t natural = (uint8_t)(ef & DP_XF_NATURAL);
            if (ef & (DP_XF_NATURAL | DP_XF_FORCED)) {
                // speculative-only execution (the fork deliberately runs data
                // as code) does not outweigh a verbatim copy proof
                if (!natural && (res->data_flags[k] & DP_DF_VERBATIM)) {
                    dp_buf_fmt(o, "%02x %04x 8 %x\n", bank, bank ? a + 0x4000 : a,
                               res->data_hits[k] > 0xFFFFu ? 0xFFFFu : (unsigned)res->data_hits[k]);
                    continue;
                }
                uint8_t flags = 1;
                if (ef & DP_XF_ENTRY) flags |= 2;
                if (ef & DP_XF_FORCED) flags |= 4;
                dp_buf_fmt(o, "%02x %04x %x %x\n", bank, bank ? a + 0x4000 : a, flags,
                           res->exec_hits[k] > 0xFFFFu ? 0xFFFFu : (unsigned)res->exec_hits[k]);
            } else if (ef & DP_XF_STATIC) {
                // direct flow only: instruction starts, speculative (4), entry (2)
                if (ef & DP_XF_START)
                    dp_buf_fmt(o, "%02x %04x %x 0\n", bank, bank ? a + 0x4000 : a,
                               (ef & DP_XF_ENTRY) ? 6u : 4u);
            } else if ((res->data_flags[k] & DP_DF_VERBATIM) || (res->static_flags[k] & DP_SF_POINTER)) {
                // proven data, never executed: m0boy must not compile it
                dp_buf_fmt(o, "%02x %04x 8 %x\n", bank, bank ? a + 0x4000 : a,
                           res->data_hits[k] > 0xFFFFu ? 0xFFFFu : (unsigned)res->data_hits[k]);
            }
        }
    }
}

// --- romdata ----------------------------------------------------------------------
// ROM data-read profile in m0boy's romdata-*.prof format ("page count", one
// line per touched 256-byte page): it weights the flash residency/streaming
// of the generated project by what the sessions actually read.
void dp_report_romdata(const DpResults *res, DpBuf *o) {
    size_t pages = ((size_t)res->cart->rom_size + 255) / 256;
    uint64_t *counts = calloc(pages, sizeof(uint64_t));
    if (!counts) return;
    for (int bank = 0; bank < res->banks; bank++) {
        for (int a = 0; a < 0x4000; a++) {
            size_t k = (size_t)bank * 0x4000 + a;
            uint32_t h = __atomic_load_n(&res->data_hits[k], __ATOMIC_RELAXED);
            if (!h) continue;
            uint32_t off = (uint32_t)bank * 0x4000 + (uint32_t)a;
            if (off / 256 < pages) counts[off / 256] += h;
        }
    }
    for (size_t p = 0; p < pages; p++)
        if (counts[p]) dp_buf_fmt(o, "%zx %llu\n", p, (unsigned long long)counts[p]);
    free(counts);
}
