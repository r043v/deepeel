// report.c: human text report, machine-readable JSON map, and a .cov export
// compatible with m0boy's coverage format (flags 1=exec 2=entry 4=speculative).
#include "report.h"

// block classes: '.' untouched, 'C' code natural, 'c' code forced-only,
// 'D' data read, 'V' copied to VRAM/OAM, 'P' pointer table, 'p' padding
char ms_block_class(const MsResults *res, int bank, int block) {
    size_t base = (size_t)bank * 0x4000 + (size_t)block * 64;
    size_t end = base + 64;
    int natural = 0, forced = 0, copied = 0, data = 0, ptr = 0, pad = 0;
    for (size_t i = base; i < end && i < (size_t)res->banks * 0x4000; i++) {
        if (res->exec_flags[i] & MS_XF_NATURAL) natural++;
        else if (res->exec_flags[i] & MS_XF_FORCED) forced++;
        if (res->data_flags[i] & MS_DF_COPIED) copied++;
        else if (res->data_flags[i] & MS_DF_READ) data++;
        if (res->static_flags[i] & MS_SF_POINTER) ptr++;
        if (res->static_flags[i] & MS_SF_PAD) pad++;
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
static void bank_blocks_rle(const MsResults *res, int bank, MsBuf *out) {
    char prev = -1;
    int count = 0;
    for (int b = 0; b < 256; b++) {
        char cls = ms_block_class(res, bank, b);
        if (cls == prev) {
            count++;
        } else {
            if (prev >= 0) ms_buf_fmt(out, "%c%d.", prev, count);
            prev = cls;
            count = 1;
        }
    }
    if (prev >= 0) ms_buf_fmt(out, "%c%d.", prev, count);
}

static const char *verdict_name(uint8_t v) {
    switch (v) {
    case MS_V_REQUIRED: return "required";
    case MS_V_FORCED: return "forced";
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
void ms_report_text(const MsResults *res, const MsConfig *cfg, MsBuf *out) {
    const MsCart *c = res->cart;
    char line[256];
    snprintf(line, sizeof line, "memscan — %s (%s, %d banks, %u KiB, RAM %u KiB)\n",
             c->title, ms_mbc_name(c->mbc), c->rom_banks, c->rom_size / 1024, c->ram_size / 1024);
    ms_buf_str(out, line);
    snprintf(line, sizeof line,
             "exploration: %llu steps in %llu paths (%u forks: %u branch, %u irq), %u sections\n"
             "evicted: %u exact states, %u same-context entries; cuts: %u cycles, %u step-budget\n"
             "budgets: %u paths cap hit, %u step-wall hit, %u queue overflows\n"
             "seeds: %u natural, %u speculative\n",
             (unsigned long long)res->steps_total, (unsigned long long)res->paths_run,
             (unsigned long long)(res->forks_branch + res->forks_irq), res->forks_branch, res->forks_irq,
             res->section_count, res->evicted_exact, res->evicted_ctx, res->cycle_cuts,
             res->step_cuts, res->budget_paths, res->budget_steps, res->queue_overflows,
             res->seeds_natural, res->seeds_spec);
    ms_buf_str(out, line);

    // per-bank coverage
    ms_buf_str(out, "\nper-bank executed bytes:\n");
    for (int bank = 0; bank < res->banks; bank++) {
        int nat = 0, frc = 0, dat = 0;
        for (int i = 0; i < 0x4000; i++) {
            size_t k = (size_t)bank * 0x4000 + i;
            if (res->exec_flags[k] & MS_XF_NATURAL) nat++;
            else if (res->exec_flags[k] & MS_XF_FORCED) frc++;
            if (res->data_flags[k] & MS_DF_READ) dat++;
        }
        if (!nat && !frc && !dat) continue;
        snprintf(line, sizeof line, "  bank %3d: %5d code (%5d natural) %5d data read\n", bank, nat + frc, nat, dat);
        ms_buf_str(out, line);
    }

    // sections: required first, then forced, top hits
    ms_buf_str(out, "\nsections (top 60 by hits):\n");
    ms_buf_str(out, "  bank:lo    hi     hits  nat/for  ctx  cond-both  io  copies  verdict\n");
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
        const MsSection *s = &res->sections[idx[i]];
        snprintf(line, sizeof line, "  %3d:%04x-%04x %7u %4u/%-4u %4u %5u/%-3u %3u %6u  %s\n",
                 s->bank, s->lo, s->hi, s->hits, s->natural_hits, s->forced_hits, s->ctx_count,
                 s->cond_both, s->cond_sites, s->io_used_n, s->copies, verdict_name(s->verdict));
        ms_buf_str(out, line);
    }

    // io usage
    ms_buf_str(out, "\nregister usage:\n");
    for (int i = 0; i < res->io_count; i++) {
        const MsIoStat *st = &res->io[i];
        const MsIoReg *r = &res->plat->io_regs[i];
        if (!st->rd && !st->wr) continue;
        snprintf(line, sizeof line, "  %-6s %s rd=%-7u wr=%-7u", r->name, r->category, st->rd, st->wr);
        ms_buf_str(out, line);
        if (st->nvalues) {
            ms_buf_str(out, " values=");
            for (int v = 0; v < st->nvalues; v++) ms_buf_fmt(out, "%02x,", st->values[v]);
        }
        if (st->bank_events) ms_buf_fmt(out, " banks=%u", st->bank_events);
        if (st->copies) ms_buf_fmt(out, " copies=%u", st->copies);
        ms_buf_ch(out, '\n');
    }

    // copies
    if (res->copy_count) {
        ms_buf_fmt(out, "\ncopies (%d):\n", res->copy_count);
        int show = res->copy_count > 40 ? 40 : res->copy_count;
        for (int i = 0; i < show; i++) {
            const MsCopyRec *r = &res->copies[i];
            snprintf(line, sizeof line, "  %-8s rom b%u:%04x-%04x -> %s b%u:%04x-%04x (%u bytes) at %04x\n",
                     chan_name(r->channel), r->src_bank, r->src_lo, r->src_hi,
                     r->dst_space == MS_SPACE_VRAM ? "vram" : "oam", r->dst_bank, r->dst_lo, r->dst_hi,
                     r->bytes, r->pc);
            ms_buf_str(out, line);
        }
        if (res->copy_count > show) ms_buf_fmt(out, "  ... %d more\n", res->copy_count - show);
    }

    // bank switches
    if (res->bank_count) {
        ms_buf_fmt(out, "\nbank switches (%d", res->bank_count);
        int show = res->bank_count > 30 ? 30 : res->bank_count;
        ms_buf_str(out, "):\n");
        for (int i = 0; i < show; i++) {
            const MsBankEvent *e = &res->bank_events[i];
            static const char *wn[3] = { "rom0", "romN", "ram" };
            snprintf(line, sizeof line, "  at %04x: %s %u -> %u\n", e->pc, wn[e->window], e->old_bank, e->new_bank);
            ms_buf_str(out, line);
        }
        if (res->bank_count > show) ms_buf_fmt(out, "  ... %d more\n", res->bank_count - show);
    }

    if (res->bad_op_count) {
        ms_buf_fmt(out, "\nundefined opcodes: %d (first sites:", res->bad_op_count);
        int show = res->bad_op_count > 8 ? 8 : res->bad_op_count;
        for (int i = 0; i < show; i++)
            ms_buf_fmt(out, " %06x=%02x", res->bad_ops[i].key, res->bad_ops[i].op);
        ms_buf_str(out, ")\n");
    }
    (void)cfg;
}

// --- json -----------------------------------------------------------------------
static void json_esc(MsBuf *out, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') ms_buf_fmt(out, "\\%c", *p);
        else if ((unsigned char)*p < 0x20) ms_buf_fmt(out, "\\u%04x", *p);
        else ms_buf_ch(out, *p);
    }
}

void ms_report_json(const MsResults *res, const MsConfig *cfg, MsBuf *o) {
    const MsCart *c = res->cart;
    ms_buf_str(o, "{\n\"rom\": {");
    ms_buf_fmt(o, "\"title\":\"");
    json_esc(o, c->title);
    ms_buf_fmt(o, "\",\"mbc\":\"%s\",\"banks\":%d,\"size\":%u,\"ram\":%u},\n",
               ms_mbc_name(c->mbc), c->rom_banks, c->rom_size, c->ram_size);
    ms_buf_fmt(o, "\"config\": {\"max_steps_path\":%u,\"max_paths\":%u,\"max_steps_total\":%llu,"
                  "\"queue_cap\":%u,\"loop_back_cap\":%u,\"seed_cap\":%u,\"max_copies\":%u},\n",
               cfg->max_steps_path, cfg->max_paths, (unsigned long long)cfg->max_steps_total,
               cfg->queue_cap, cfg->loop_back_cap, cfg->seed_cap, cfg->max_copies);
    ms_buf_fmt(o, "\"counters\": {\"steps\":%llu,\"paths\":%u,\"forks_branch\":%u,\"forks_irq\":%u,"
                  "\"sections\":%u,\"evicted_exact\":%u,\"evicted_ctx\":%u,\"cycle_cuts\":%u,"
                  "\"step_cuts\":%u,\"budget_paths\":%u,\"budget_steps\":%u,\"queue_overflows\":%u,"
                  "\"seeds_natural\":%u,\"seeds_spec\":%u,\"bad_ops\":%u},\n",
               (unsigned long long)res->steps_total, (unsigned long long)res->paths_run, res->forks_branch, res->forks_irq,
               (unsigned)res->section_count, res->evicted_exact, res->evicted_ctx, res->cycle_cuts,
               res->step_cuts, res->budget_paths, res->budget_steps, res->queue_overflows,
               res->seeds_natural, res->seeds_spec, (unsigned)res->bad_op_count);
    // banks
    ms_buf_str(o, "\"banks\": [\n");
    for (int bank = 0; bank < res->banks; bank++) {
        ms_buf_fmt(o, "{\"bank\":%d,\"blocks\":\"", bank);
        bank_blocks_rle(res, bank, o);
        ms_buf_str(o, "\"}");
        if (bank + 1 < res->banks) ms_buf_ch(o, ',');
        ms_buf_ch(o, '\n');
    }
    ms_buf_str(o, "],\n");
    // sections
    ms_buf_str(o, "\"sections\": [\n");
    for (int i = 0; i < res->section_count; i++) {
        const MsSection *s = &res->sections[i];
        ms_buf_fmt(o, "{\"id\":%u,\"bank\":%d,\"lo\":%u,\"hi\":%u,\"hits\":%u,\"entries\":%u,"
                      "\"natural\":%u,\"forced\":%u,\"verdict\":\"%s\",\"ctx\":%u,"
                      "\"cond_sites\":%u,\"cond_both\":%u,\"copies\":%u,\"io_used\":[",
                   s->id, s->bank, s->lo, s->hi, s->hits, s->entries, s->natural_hits, s->forced_hits,
                   verdict_name(s->verdict), s->ctx_count, s->cond_sites, s->cond_both, s->copies);
        for (int j = 0; j < s->io_used_n; j++) {
            const MsIoReg *r = &res->plat->io_regs[s->io_used[j]];
            ms_buf_fmt(o, "%s\"%s\"", j ? "," : "", r->name);
        }
        ms_buf_str(o, "],\"banks_seen\":[");
        for (int j = 0; j < s->banks_seen_n; j++) ms_buf_fmt(o, "%s%u", j ? "," : "", s->banks_seen[j]);
        ms_buf_str(o, "],\"callers\":[");
        for (int j = 0; j < s->callers_n; j++) ms_buf_fmt(o, "%s%u", j ? "," : "", s->callers[j]);
        ms_buf_str(o, "]}");
        if (i + 1 < res->section_count) ms_buf_ch(o, ',');
        ms_buf_ch(o, '\n');
    }
    ms_buf_str(o, "],\n");
    // io
    ms_buf_str(o, "\"io\": [\n");
    int first = 1;
    for (int i = 0; i < res->io_count; i++) {
        const MsIoStat *st = &res->io[i];
        const MsIoReg *r = &res->plat->io_regs[i];
        if (!st->rd && !st->wr) continue;
        if (!first) ms_buf_str(o, ",\n");
        first = 0;
        ms_buf_fmt(o, "{\"addr\":\"%04x\",\"name\":\"%s\",\"category\":\"%s\",\"rd\":%u,\"wr\":%u,\"values\":[",
                   r->lo, r->name, r->category, st->rd, st->wr);
        for (int v = 0; v < st->nvalues; v++) ms_buf_fmt(o, "%s%u", v ? "," : "", st->values[v]);
        ms_buf_str(o, "],\"pcs\":[");
        for (int v = 0; v < st->npcs; v++) ms_buf_fmt(o, "%s%u", v ? "," : "", st->pcs[v]);
        ms_buf_fmt(o, "],\"copies\":%u,\"bank_events\":%u}", st->copies, st->bank_events);
    }
    ms_buf_str(o, "],\n");
    // copies
    ms_buf_str(o, "\"copies\": [\n");
    for (int i = 0; i < res->copy_count; i++) {
        const MsCopyRec *r = &res->copies[i];
        ms_buf_fmt(o, "{\"channel\":\"%s\",\"src\":{\"space\":\"%s\",\"bank\":%u,\"lo\":%u,\"hi\":%u},"
                      "\"dst\":{\"space\":\"%s\",\"bank\":%u,\"lo\":%u,\"hi\":%u},\"bytes\":%u,\"pc\":%u}",
                   chan_name(r->channel), r->src_space == MS_SPACE_ROM ? "rom" : (r->src_space == MS_SPACE_WRAM ? "wram" : "vram"),
                   r->src_bank, r->src_lo, r->src_hi,
                   r->dst_space == MS_SPACE_VRAM ? "vram" : "oam", r->dst_bank, r->dst_lo, r->dst_hi,
                   r->bytes, r->pc);
        if (i + 1 < res->copy_count) ms_buf_ch(o, ',');
        ms_buf_ch(o, '\n');
    }
    ms_buf_str(o, "],\n");
    // bank events
    ms_buf_str(o, "\"bank_events\": [\n");
    for (int i = 0; i < res->bank_count; i++) {
        const MsBankEvent *e = &res->bank_events[i];
        static const char *wn[3] = { "rom0", "romN", "ram" };
        ms_buf_fmt(o, "{\"pc\":%u,\"window\":\"%s\",\"old\":%u,\"new\":%u}", e->pc, wn[e->window],
                   e->old_bank, e->new_bank);
        if (i + 1 < res->bank_count) ms_buf_ch(o, ',');
        ms_buf_ch(o, '\n');
    }
    ms_buf_str(o, "],\n");
    // bad ops
    ms_buf_str(o, "\"bad_ops\": [");
    for (int i = 0; i < res->bad_op_count && i < 64; i++)
        ms_buf_fmt(o, "%s%u", i ? "," : "", res->bad_ops[i].key);
    ms_buf_str(o, "]\n}\n");
}

// --- cov -------------------------------------------------------------------------
void ms_report_cov(const MsResults *res, MsBuf *o) {
    // m0boy coverage v1 semantics: flag 1 = executed, 2 = entry, 4 = reached
    // only by forced exploration ("speculative" there)
    ms_buf_str(o, "# gbl coverage v1\n");
    for (int bank = 0; bank < res->banks; bank++) {
        for (int a = 0; a < 0x4000; a++) {
            size_t k = (size_t)bank * 0x4000 + a;
            uint8_t ef = res->exec_flags[k];
            if (!(ef & (MS_XF_NATURAL | MS_XF_FORCED))) continue;
            uint8_t flags = 1;
            if (ef & MS_XF_ENTRY) flags |= 2;
            if (ef & MS_XF_FORCED) flags |= 4;
            ms_buf_fmt(o, "%02x %04x %x %x\n", bank, bank ? a + 0x4000 : a, flags,
                       res->exec_hits[k] > 0xFFFFu ? 0xFFFFu : (unsigned)res->exec_hits[k]);
        }
    }
}
