#include "explore.h"
#include "../../explore/lots.h"
#include <stdlib.h>
#include <string.h>
#include "../../cpu/m68k/m68k.h"
#include "../../mach/mdmach.h"
#include "md_registry.h"
#include "../../core/hashset.h"


typedef struct {
    void *state;
    int forced;
    uint32_t origin;
    uint32_t an_writer[8];
    uint32_t watch_pc, watch_addr, watch_value;
    uint32_t watch_d0, watch_a1, watch_a2;
    unsigned watch_size;
} Item;

typedef struct {
    const DpMdCart *c;
    const DpMdOptions *o;
    DpMdResults *r;
    DpMdMach *m;
    Item *q;
    uint32_t nq, qcap;
    Item *park; uint32_t npark, parkcap; // deferred holding area (queue full)
    void *spill_state; Item spill_item; // state waiting for its post-edits before going to the lot
    DpLotFile *lot;         // disk lot: park overflow, wall leftovers, --lot resume
    void *last_state;   // blob of the state the last push() produced (queued or parked)
    uint32_t *gen;       /* per even rom address: path generation */
    uint32_t *vis;       /* per even rom address: visits in that generation */
    uint32_t *site;      /* per even rom address: forks made from that site */
    uint8_t *pad_sites;  /* input probes already queued at each instruction */
    uint32_t cur_gen;
    int forced, halted;
    uint32_t pc;         /* instruction being run */
    uint32_t origin;     /* site that created the current path */
    uint32_t trace[8];
    unsigned trace_pos, trace_len;
    uint32_t an_before[8], an_writer[8];
    uint32_t dbcc_dn_before;
    uint16_t sr_before;
    uint32_t watch_pc, watch_addr, watch_value;
    uint32_t watch_d0, watch_a1, watch_a2;
    unsigned watch_size;
    int irq6_done, irq4_done;
    int pad_read;
    int movie_active;
    uint16_t op;
    uint32_t move_src;
    int move_src_valid;
    uint32_t previous_move_src;
    uint8_t previous_move_reg;
    int previous_move_valid, register_move_valid;
    /* stream-consumer detection (natural/movie path only) */
    struct { uint32_t entry, sp, an[7]; int open; } frames[64];
    unsigned nframes;
    struct { uint32_t entry; unsigned reg, nstarts; uint32_t starts[DP_MD_DECODER_STARTS];
             void *snap; uint32_t snap_sp; } dec[DP_MD_DECODER_CAP];
    unsigned ndec;
    int replay;          /* running a stream replay */
    uint32_t replay_sp;  /* A7 at the replayed entry: above it, the routine returned */
    /* Z80 sound CPU. zsrc: ROM address of the byte a 68000 store put in each
     * Z80 RAM cell (-1: none). It is global to the exploration, not forked
     * with the machine, so it is evidence from the last store seen. */
    int in_z80;
    int32_t zsrc[8192];
    uint32_t lr[4]; unsigned nlr;       /* ROM bytes of the last 68000 data read */
    uint32_t z80_last_read;             /* ROM address of the last Z80 data read */
    int z80_last_valid;
    uint32_t zfetch_lo, zfetch_hi;      /* ROM range of the instruction being fetched */
    uint8_t z80_bank_seen[128];         /* bitmap of 32 KiB banks (4 MiB / 32 KiB) */
    DpHashSet ind_sites, ind_pairs;   // observed indirect jmp/jsr (D12)
} X;

static size_t ssz;

static void next_generation(X *x) {
    if (++x->cur_gen == 0) {
        memset(x->gen, 0, (x->c->size / 2 + 1) * sizeof *x->gen);
        x->cur_gen = 1;
    }
}

/* Per-frame loop detection permits legitimate polling across V-blank;
 * path_steps and max_total still bound paths that run forever. */
static void on_frame(void *u) {
    X *x = u;
    next_generation(x);
    if (x->movie_active) {
        x->r->movie_frames++;
        if (x->r->movie_frames >= x->o->movie->frames) {
            x->movie_active = 0;
            dp_mdmach_halt(x->m);
        } else dp_mdmach_set_pad(x->m, x->o->movie->pads[x->r->movie_frames]);
    }
}
static void on_dma_read(void *u, uint32_t addr, unsigned size) {
    X *x = u;
    for (unsigned i = 0; i < size && addr + i < x->r->rom_size; i++)
        x->r->flags[addr + i] |= DP_MDF_DMA_READ;
}

static void mark(X *x, uint32_t pc, unsigned len) {
    uint8_t f = x->forced ? DP_MDF_FORCED : DP_MDF_NATURAL;
    for (unsigned i = 0; i < len && pc + i < x->r->rom_size; i++)
        if (dp_mdmach_rom_visible(x->m, pc + i)) x->r->flags[pc + i] |= f;
}

// one queued item to the disk lot (the state blob stays owned by the caller)
static int lot_spill(X *x, const Item *it) {
    Item m = *it; m.state = NULL;
    return x->lot && dp_lf_put(x->lot, &m, sizeof m, it->state, (uint32_t)ssz);
}

static void spill_flush(X *x) {
    if (!x->spill_state) return;
    if (!lot_spill(x, &x->spill_item)) x->r->parked_lost++;
    free(x->spill_state);
    x->spill_state = NULL;
}

static int push(X *x, uint32_t pc, int vint, int hint) {
    int full = 0;
    spill_flush(x);
    if (x->nq >= x->qcap) {
        uint32_t limit = x->o->queue_max > x->qcap ? x->o->queue_max : x->qcap;
        uint32_t ncap = x->qcap > limit / 2 ? limit : x->qcap * 2;
        if (!ncap && limit) ncap = 1;
        Item *q = ncap > x->qcap ? realloc(x->q, (size_t)ncap * sizeof *q) : NULL;
        if (!q) { // saturation counted; the state waits aside instead of dying (§6)
            x->r->sat_queue++;
            if (x->npark >= 4096) { // holding area full: the new state goes straight to the disk lot
                void *ls = x->lot ? malloc(ssz) : NULL;
                if (ls) {
                    dp_mdmach_save(x->m, ls);
                    dp_mdmach_state_set_pc(ls, pc);
                    dp_mdmach_state_set_irq(ls, vint, hint);
                    Item *li = &x->spill_item; memset(li, 0, sizeof *li);
                    li->state = ls; li->forced = 1; li->origin = x->pc;
                    memcpy(li->an_writer, x->an_writer, sizeof li->an_writer);
                    li->watch_pc = x->watch_pc; li->watch_addr = x->watch_addr;
                    li->watch_value = x->watch_value; li->watch_size = x->watch_size;
                    li->watch_d0 = x->watch_d0; li->watch_a1 = x->watch_a1; li->watch_a2 = x->watch_a2;
                    x->spill_state = ls;   // written by spill_flush once the caller's post-edits are done
                    x->last_state = ls;
                    x->r->parked++;
                    if (pc < x->r->rom_size) x->r->flags[pc] |= DP_MDF_QUEUED;
                    return 1;
                }
                free(ls);
                x->r->parked_lost++; return 0;
            }
            if (x->npark == x->parkcap) {
                uint32_t nc = x->parkcap ? x->parkcap * 2 : 64;
                Item *g = realloc(x->park, (size_t)nc * sizeof *g);
                if (!g) { x->r->parked_lost++; return 0; }
                x->park = g; x->parkcap = nc;
            }
            full = 1;
        } else { x->q = q; x->qcap = ncap; x->r->queue_growths++; }
    }
    void *s = malloc(ssz);
    if (!s) { x->r->sat_queue++; return 0; }
    dp_mdmach_save(x->m, s);
    dp_mdmach_state_set_pc(s, pc);
    dp_mdmach_state_set_irq(s, vint, hint);
    Item *item = full ? &x->park[x->npark++] : &x->q[x->nq++];
    if (full) x->r->parked++;
    x->last_state = s;
    item->state = s; item->forced = 1; item->origin = x->pc;
    memcpy(item->an_writer, x->an_writer, sizeof item->an_writer);
    item->watch_pc = x->watch_pc; item->watch_addr = x->watch_addr;
    item->watch_value = x->watch_value; item->watch_size = x->watch_size;
    item->watch_d0 = x->watch_d0; item->watch_a1 = x->watch_a1; item->watch_a2 = x->watch_a2;
    if (pc < x->r->rom_size) x->r->flags[pc] |= DP_MDF_QUEUED;
    return 1;
}

static int find_decoder(X *x, uint32_t entry, unsigned reg) {
    for (unsigned i = 0; i < x->ndec; i++)
        if (x->dec[i].entry == entry && (reg > 6 || x->dec[i].reg == reg)) return (int)i;
    return -1;
}

/* A subroutine was entered on the natural path: open a frame; a routine
 * already known as a stream consumer gets its entry snapshot (once). */
static void call_entered(X *x, uint32_t entry) {
    uint32_t sp = dp_mdmach_an(x->m, 7);
    while (x->nframes && sp >= x->frames[x->nframes - 1].sp) x->nframes--;
    if (x->nframes == sizeof x->frames / sizeof x->frames[0]) { x->r->frame_overflow++; return; }
    typeof(x->frames[0]) *f = &x->frames[x->nframes++];
    f->entry = entry; f->sp = sp; f->open = 1;
    for (unsigned i = 0; i < 7; i++) f->an[i] = dp_mdmach_an(x->m, i);
    for (unsigned i = 0; i < x->ndec; i++)
        if (x->dec[i].entry == entry && !x->dec[i].snap && (x->dec[i].snap = malloc(ssz))) {
            dp_mdmach_save(x->m, x->dec[i].snap);
            dp_mdmach_state_set_pc(x->dec[i].snap, entry);
            x->dec[i].snap_sp = sp;
        }
}

/* First ROM read of each open frame: at an address an An held on entry,
 * the routine consumes a stream from that register. */
static void stream_read(X *x, uint32_t a) {
    for (unsigned k = x->nframes; k-- > 0;) {
        typeof(x->frames[0]) *f = &x->frames[k];
        if (!f->open) break; /* outer frames already saw their first read */
        f->open = 0;
        for (unsigned n = 0; n < 7; n++) {
            if ((f->an[n] & 0xFFFFFFu) != a) continue;
            int d = find_decoder(x, f->entry, n);
            if (d < 0) {
                if (x->ndec == DP_MD_DECODER_CAP) { x->r->decoder_overflow++; break; }
                d = (int)x->ndec++;
                memset(&x->dec[d], 0, sizeof x->dec[d]);
                x->dec[d].entry = f->entry; x->dec[d].reg = n;
            }
            unsigned j;
            for (j = 0; j < x->dec[d].nstarts && x->dec[d].starts[j] != a; j++) {}
            if (j == x->dec[d].nstarts) {
                if (j < DP_MD_DECODER_STARTS) x->dec[d].starts[x->dec[d].nstarts++] = a;
                else x->r->start_overflow++;
            }
            break;
        }
    }
}

static void on_step(void *u, uint32_t pc, uint16_t op) {
    X *x = u;
    (void)op;
    x->dbcc_dn_before = dp_mdmach_dn(x->m, op & 7);
    x->sr_before = dp_mdmach_sr(x->m);
    x->pad_read = 0;
    x->op = op;
    x->register_move_valid = x->previous_move_valid && (op & 0xF038) == 0x3000 &&
                             (op & 7) == x->previous_move_reg;
    x->previous_move_valid = 0;
    x->move_src_valid = 0;
    x->pc = pc & 0xFFFFFFu;
    for (unsigned i = 0; i < 8; i++) x->an_before[i] = dp_mdmach_an(x->m, i);
    while (x->nframes && x->an_before[7] > x->frames[x->nframes - 1].sp) x->nframes--;
    x->trace[x->trace_pos++ % 8] = x->pc;
    if (x->trace_len < 8) x->trace_len++;
    if (x->replay && dp_mdmach_an(x->m, 7) > x->replay_sp) {
        x->halted = 1;
        dp_mdmach_halt(x->m);
        return;
    }
    if (x->o->pad_cut && !x->replay && op == 0 && x->pc + 16 <= x->r->rom_size && dp_mdmach_rom_visible(x->m, x->pc)) {
        // D14: ori.b #0,d0 slide through zero padding: cut once 16 bytes are all zero
        unsigned z = 0;
        while (z < 16 && x->c->rom[x->pc + z] == 0) z++;
        if (z == 16) {
            x->r->pad_cuts++;
            x->halted = 1;
            dp_mdmach_halt(x->m);
            return;
        }
    }
    if (!x->movie_active && !x->replay && x->pc < x->r->rom_size && dp_mdmach_rom_visible(x->m, x->pc)) {
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
    unsigned mask = (dp_mdmach_sr(x->m) >> 8) & 7;
    if (!x->irq6_done && mask < 6 && (dp_mdmach_vdp_reg(x->m, 1) & 0x20)) {
        uint32_t h = c->vec[30] & 0xFFFFFFu;
        if (h >= x->r->rom_size || (h & 1) || (x->r->flags[h] & (DP_MDF_NATURAL | DP_MDF_FORCED)))
            x->irq6_done = 1;
        else if (push(x, next & 0xFFFFFFu, 1, 0)) { x->irq6_done = 1; x->r->irq_forks++; }
    }
    if (!x->irq4_done && mask < 4 && (dp_mdmach_vdp_reg(x->m, 0) & 0x10)) {
        uint32_t h = c->vec[28] & 0xFFFFFFu;
        if (h >= x->r->rom_size || (h & 1) || (x->r->flags[h] & (DP_MDF_NATURAL | DP_MDF_FORCED)))
            x->irq4_done = 1;
        else if (push(x, next & 0xFFFFFFu, 0, 1)) { x->irq4_done = 1; x->r->irq_forks++; }
    }
}

static void on_end(void *u, uint32_t pc, uint32_t next, unsigned len) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    for (unsigned i = 0; i < 8; i++)
        if (dp_mdmach_an(x->m, i) != x->an_before[i]) x->an_writer[i] = pc & 0xFFFFFFu;
    pc &= 0xFFFFFFu;
    if (pc >= r->rom_size || !dp_mdmach_rom_visible(x->m, pc)) {
        r->exec_outside++;
        if (pc >= 0xE00000) r->ram_exec++;
        if (!dp_mdmach_rom_visible(x->m, pc)) r->sram_exec++;
        irq_forks(x, next);
        return;
    }
    DpM68kInsn in;
    size_t avail = r->rom_size - pc;
    unsigned decoded = dp_m68k_decode(x->c->rom + pc, avail, pc, &in);
    mark(x, pc, len);
    if (x->move_src_valid && (x->op & 0xF1C0) == 0x3000) {
        x->previous_move_src = x->move_src;
        x->previous_move_reg = (uint8_t)((x->op >> 9) & 7);
        x->previous_move_valid = 1;
    }
    if (decoded && decoded != len) r->decode_length_mismatches++;
    if (!x->forced && ((x->op & 0xFFC0) == 0x4E80 || (x->op & 0xFF00) == 0x6100))
        call_entered(x, next & 0xFFFFFFu);
    if (x->replay) return; /* a replay records reads; it does not fork */
    irq_forks(x, next);
    if (x->pad_read) {
        static const uint8_t pads[] = {128, 16, 8, 24}; /* Start, A, Right, A+Right */
        uint8_t *seen = &x->pad_sites[pc >> 1];
        for (unsigned i = 0; i < sizeof pads; i++) {
            uint8_t bit = (uint8_t)(1u << i);
            if (!(*seen & bit) && push(x, next & 0xFFFFFFu, 0, 0)) {
                dp_mdmach_state_set_pad(x->last_state, pads[i]);
                *seen |= bit;
                r->pad_forks++;
            }
        }
    }
    if (!decoded) return;
    if ((in.cls == DP_M68K_JMP || in.cls == DP_M68K_JSR) && !in.has_target && decoded == len && !x->replay) {
        // observed indirect target: the next pc after a jmp/jsr through An/(d,An)/(An,Xn)
        if (x->ind_pairs.cnt >= 65536) r->indirect_overflow++;
        else {
            if (dp_set_insert(&x->ind_sites, (uint64_t)pc + 1) > 0) r->indirect_sites++;
            if (dp_set_insert(&x->ind_pairs, ((uint64_t)(pc + 1) << 24) | (next & 0xFFFFFFu)) > 0) r->indirect_targets++;
        }
    }
    if ((in.cls != DP_M68K_BCC && in.cls != DP_M68K_DBCC) || !in.has_target) return;
    if (decoded != len) return; /* do not invent a fall-through from a disputed length */
    if (in.cls == DP_M68K_DBCC && in.cond == 0) return; /* DBT cannot branch */
    uint32_t fall = (pc + in.len) & 0xFFFFFFu;
    uint32_t alt = ((next & 0xFFFFFFu) == fall) ? in.target : fall;
    if (alt == fall && in.target == fall) return;
    if (alt >= r->rom_size || (alt & 1)) { r->fork_off_rom++; return; }
    size_t si = pc >> 1;
    if (x->site[si] >= x->o->fork_cap) { r->sat_site++; return; }
    if (push(x, alt, 0, 0)) {
        x->site[si]++;
        r->forks++;
        if (in.cls == DP_M68K_BCC)
            dp_mdmach_state_force_cc(x->last_state, x->c->rom[pc] & 15, alt != fall);
        if (in.cls == DP_M68K_DBCC)
            dp_mdmach_state_force_dbcc(x->last_state, (unsigned)in.cond,
                                       x->c->rom[pc + 1] & 7, alt != fall,
                                       x->dbcc_dn_before, x->sr_before);
    }
}

static void on_read(void *u, uint32_t a, unsigned size) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    a &= 0xFFFFFFu;
    if ((a == 0xA10002 || a == 0xA10003) && x->pc < r->rom_size) x->pad_read = 1;
    if (!dp_mdmach_rom_visible(x->m, a)) { r->sram_reads++; return; }
    if (a < r->rom_size) {
        if ((x->op & 0xF000) == 0x3000 && size == 2 && a + 1 < r->rom_size &&
            dp_mdmach_rom_visible(x->m, a) && dp_mdmach_rom_visible(x->m, a + 1)) {
            x->move_src = a;
            x->move_src_valid = 1;
        }
        x->nlr = 0;
        for (unsigned i = 0; i < size && i < 4 && a + i < r->rom_size; i++) x->lr[x->nlr++] = a + i;
        unsigned k;
        for (k = 0; k < r->nreaders && r->readers[k].pc != x->pc; k++) {}
        if (k == r->nreaders && k < DP_MD_READER_CAP) {
            r->readers[k].pc = x->pc;
            r->readers[k].lo = a;
            r->nreaders++;
        }
        if (k < r->nreaders) {
            DpMdReader *reader = &r->readers[k];
            uint32_t end = a + size - 1;
            if (end >= r->rom_size) end = (uint32_t)r->rom_size - 1;
            if (a < reader->lo) reader->lo = a;
            if (end > reader->hi) reader->hi = end;
            reader->accesses++;
            if (!x->forced) reader->natural_accesses++;
        } else r->reader_overflow++;
        for (unsigned i = 0; i < size && a + i < r->rom_size; i++)
        {
            r->flags[a + i] |= DP_MDF_DATA | (x->forced ? 0 : DP_MDF_DATA_NATURAL);
            if (x->pc != x->o->exclude_read_pc) r->nonexcluded_reads[a + i] |= x->replay ? 2 : 1;
        }
        if (!x->forced && x->pc != x->o->exclude_read_pc) stream_read(x, a);
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
    if (x->register_move_valid && size == 2 && a >= 0xC00000 && a <= 0xC00003 && !(a & 1) &&
        (dp_mdmach_vdp_code(x->m) & 15) == 1 &&
        v == ((uint32_t)x->c->rom[x->previous_move_src] << 8 | x->c->rom[x->previous_move_src + 1])) {
        r->flags[x->previous_move_src] |= DP_MDF_CPU_VRAM;
        r->flags[x->previous_move_src + 1] |= DP_MDF_CPU_VRAM;
        r->cpu_vram_words++;
        x->register_move_valid = 0;
    }
    if (x->move_src_valid && size == 2 && a >= 0xC00000 && a <= 0xC00003 && !(a & 1) &&
        (dp_mdmach_vdp_code(x->m) & 15) == 1 &&
        v == ((uint32_t)x->c->rom[x->move_src] << 8 | x->c->rom[x->move_src + 1])) {
        r->flags[x->move_src] |= DP_MDF_CPU_VRAM;
        r->flags[x->move_src + 1] |= DP_MDF_CPU_VRAM;
        r->cpu_vram_words++;
        x->move_src_valid = 0;
    }
    if (x->o->watch_write <= 0xFFFFFFu && a <= x->o->watch_write &&
        x->o->watch_write - a < size) {
        x->watch_pc = x->pc; x->watch_addr = a; x->watch_value = v; x->watch_size = size;
        x->watch_d0 = dp_mdmach_dn(x->m, 0);
        x->watch_a1 = dp_mdmach_an(x->m, 1); x->watch_a2 = dp_mdmach_an(x->m, 2);
    }
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
        /* The bus DMA reads bypass the CPU trace hook. Record the commanded
         * ROM source separately: a later bus fault can truncate the transfer. */
        for (uint32_t i = 0; i < c.len * 2; i++) {
            uint32_t a = (c.src + i) & 0xFFFFFFu;
            if (a < r->rom_size && dp_mdmach_rom_visible(x->m, a)) r->flags[a] |= DP_MDF_DMA_SOURCE;
        }
        for (int i = 0; i < r->ncopy; i++)
            if (!memcmp(&r->copies[i], &c, sizeof c)) return;
        if (r->ncopy >= DP_MD_COPY_CAP) { r->copy_overflow++; return; }
        r->copies[r->ncopy++] = c;
    }
}

static void on_z80_step(void *u, uint16_t pc, unsigned len, uint32_t rom_pc) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    x->in_z80 = 1;
    r->z80_steps++;
    x->zfetch_lo = x->zfetch_hi = UINT32_MAX;
    if (pc < 0x4000u) {
        r->z80_exec_ram++;
        for (unsigned i = 0; i < len; i++) {
            int32_t s = x->zsrc[(pc + i) & 0x1FFFu];
            if (s >= 0 && (size_t)s < r->rom_size) r->z80_flags[s] |= DP_MDZ_CODE_RAM;
        }
    } else if (rom_pc != UINT32_MAX) {
        r->z80_exec_bank++;
        x->zfetch_lo = rom_pc;
        x->zfetch_hi = rom_pc + len;
        for (unsigned i = 0; i < len && rom_pc + i < r->rom_size; i++) r->z80_flags[rom_pc + i] |= DP_MDZ_EXEC_ROM;
    } else r->z80_exec_other++;
}

static void on_z80_end(void *u) { ((X *)u)->in_z80 = 0; }

static void on_z80_rom(void *u, uint32_t a) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted || a >= r->rom_size) return;
    r->z80_rom_reads++;
    if (a >= x->zfetch_lo && a < x->zfetch_hi) return;   /* instruction fetch */
    r->z80_flags[a] |= DP_MDZ_DATA;
    x->z80_last_read = a;
    x->z80_last_valid = 1;
    if ((a >> 15) < sizeof x->z80_bank_seen * 8) x->z80_bank_seen[a >> 18] |= (uint8_t)(1u << ((a >> 15) & 7));
}

static void on_sound_store(void *u, uint16_t addr, uint8_t v, int ymreg) {
    X *x = u;
    DpMdResults *r = x->r;
    if (x->halted) return;
    if (addr < 0x4000u) {
        int32_t src = -1;
        if (!x->in_z80)
            for (unsigned i = x->nlr; i-- > 0;)
                if (x->lr[i] < r->rom_size && x->c->rom[x->lr[i]] == v) { src = (int32_t)x->lr[i]; break; }
        x->zsrc[addr & 0x1FFFu] = src;
        if (src >= 0) { r->z80_flags[src] |= DP_MDZ_COPIED; r->z80_ram_copied++; }
        return;
    }
    if (ymreg == -1 || (ymreg >= 0 && !(addr & 1u))) return;
    if (ymreg == -2) r->z80_psg_writes++; else r->z80_ym_writes++;
    uint32_t src = UINT32_MAX;
    if (x->in_z80) {
        if (x->z80_last_valid && x->c->rom[x->z80_last_read] == v) src = x->z80_last_read;
    } else
        for (unsigned i = x->nlr; i-- > 0;)
            if (x->lr[i] < r->rom_size && x->c->rom[x->lr[i]] == v) { src = x->lr[i]; break; }
    if (src == UINT32_MAX) return;
    if (ymreg == 0x2A) { r->z80_flags[src] |= DP_MDZ_SAMPLE; r->z80_typed_sample++; }
    else { r->z80_flags[src] |= DP_MDZ_MUSIC; r->z80_typed_music++; }
}

void dp_md_options_default(DpMdOptions *o) {
    o->max_total = 20000000;
    o->path_steps = 2000000;
    o->queue_cap = 256;
    o->queue_max = 0;
    o->fork_cap = 2;
    o->visit_cap = 64;
    o->watch_write = UINT32_MAX;
    o->exclude_read_pc = UINT32_MAX;
    o->movie = NULL;
    o->lot_path = NULL;
    o->lot_drain = 0;
    o->entry_count = 0;
    o->pad_cut = 0;
}

static void run_path(X *x, Item *it, uint64_t budget, int *cut) {
    DpMdResults *r = x->r;
    int fault = 0;
    x->halted = 0;
    x->previous_move_valid = x->register_move_valid = 0;
    x->forced = it ? it->forced : 0;
    x->origin = it ? it->origin : UINT32_MAX;
    for (unsigned i = 0; i < 8; i++) x->an_writer[i] = it ? it->an_writer[i] : UINT32_MAX;
    x->watch_pc = it ? it->watch_pc : UINT32_MAX;
    x->watch_addr = it ? it->watch_addr : 0;
    x->watch_value = it ? it->watch_value : 0;
    x->watch_size = it ? it->watch_size : 0;
    x->watch_d0 = it ? it->watch_d0 : 0;
    x->watch_a1 = it ? it->watch_a1 : 0; x->watch_a2 = it ? it->watch_a2 : 0;
    x->trace_pos = x->trace_len = 0;
    next_generation(x);
    if (it) dp_mdmach_load(x->m, it->state);
    r->paths++;
    uint64_t pb = (x->movie_active || !x->forced) ? budget / 2 : (x->o->path_steps < budget ? x->o->path_steps : budget);
    uint64_t n = dp_mdmach_run(x->m, pb, cut, &fault);
    r->steps += n;
    if (*cut) {
        if (n >= budget) r->sat_total++;
        else r->sat_path++;
    }
    if (fault) {
        r->faults++;
        uint32_t fpc = dp_mdmach_fault_pc(x->m);
        unsigned i;
        for (i = 0; i < r->nfault_sites; i++)
            if (r->fault_sites[i].pc == x->pc && r->fault_sites[i].fault_pc == fpc &&
                r->fault_sites[i].code == fault) break;
        if (i == r->nfault_sites && i < DP_MD_FAULT_SITE_CAP) {
            r->fault_sites[i] = (DpMdFaultSite){.pc = x->pc, .fault_pc = fpc, .origin = x->origin, .code = fault};
            r->fault_sites[i].trace_len = x->trace_len;
            for (unsigned j = 0; j < x->trace_len; j++)
                r->fault_sites[i].trace[j] = x->trace[(x->trace_pos - x->trace_len + j) % 8];
            for (unsigned j = 0; j < 8; j++) {
                r->fault_sites[i].an[j] = dp_mdmach_an(x->m, j);
                r->fault_sites[i].an_writer[j] = x->an_writer[j];
            }
            r->fault_sites[i].watch_pc = x->watch_pc;
            r->fault_sites[i].watch_addr = x->watch_addr;
            r->fault_sites[i].watch_value = x->watch_value;
            r->fault_sites[i].watch_size = x->watch_size;
            r->fault_sites[i].watch_d0 = x->watch_d0;
            r->fault_sites[i].watch_a1 = x->watch_a1; r->fault_sites[i].watch_a2 = x->watch_a2;
            r->nfault_sites++;
        }
        if (i < r->nfault_sites) {
            r->fault_sites[i].count++;
            if (!x->forced) r->fault_sites[i].natural++;
        } else r->fault_sites_overflow++;
        for (i = 0; i < r->nfault_origins; i++)
            if (r->fault_origins[i].pc == x->pc && r->fault_origins[i].fault_pc == fpc &&
                r->fault_origins[i].code == fault && r->fault_origins[i].origin == x->origin) break;
        if (i == r->nfault_origins && i < DP_MD_FAULT_SITE_CAP) {
            r->fault_origins[i] = (DpMdFaultSite){.pc = x->pc, .fault_pc = fpc, .origin = x->origin, .code = fault};
            r->nfault_origins++;
        }
        if (i < r->nfault_origins) {
            r->fault_origins[i].count++;
            if (!x->forced) r->fault_origins[i].natural++;
        } else r->fault_origins_overflow++;
    }
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A table entry is a pointer into the ROM past the header. */
static int rom_ptr(const X *x, size_t at, uint32_t *v) {
    if (at + 4 > x->c->size) return 0;
    *v = rd32(x->c->rom + at);
    return *v >= 0x200 && *v < x->c->size;
}

static void replay_one(X *x, unsigned d, uint32_t start, uint64_t *left) {
    DpMdResults *r = x->r;
    uint64_t cap = x->o->path_steps < *left ? x->o->path_steps : *left;
    if (!cap) { r->sat_replay_total++; return; }
    void *s = malloc(ssz);
    if (!s) { r->sat_replay_total++; return; }
    memcpy(s, x->dec[d].snap, ssz);
    dp_mdmach_state_set_an(s, x->dec[d].reg, start);
    dp_mdmach_load(x->m, s);
    free(s);
    x->replay = 1; x->forced = 1; x->halted = 0;
    x->replay_sp = x->dec[d].snap_sp;
    x->previous_move_valid = x->register_move_valid = 0;
    next_generation(x);
    int cut = 0, fault = 0;
    uint64_t steps = dp_mdmach_run(x->m, cap, &cut, &fault);
    x->replay = 0;
    r->replay_runs++;
    r->replay_steps += steps;
    r->steps += steps;
    *left -= steps < *left ? steps : *left;
    if (cut) { if (cap < x->o->path_steps) r->sat_replay_total++; else r->sat_replay_steps++; }
    if (fault) r->replay_faults++;
}

/* Stream replay (bounded): the observed starts of each consumer locate ROM
 * pointer tables (two observed starts at a constant stride of 4..16 bytes);
 * the consumer is run again from its entry snapshot on every other entry. */
static void replay_streams(X *x, uint64_t budget) {
    DpMdResults *r = x->r;
    const size_t n = x->c->size;
    uint8_t *tried = malloc(n);
    if (!tried) return;
    uint64_t left = budget;
    for (unsigned d = 0; d < x->ndec; d++) {
        if (!x->dec[d].snap) { r->decoders_without_snapshot++; continue; }
        if (x->dec[d].nstarts < 2) continue;
        memset(tried, 0, n);
        for (unsigned j = 0; j < x->dec[d].nstarts; j++) tried[x->dec[d].starts[j]] = 1;
        /* positions of the observed starts in the ROM */
        size_t pos[256]; unsigned np = 0;
        for (size_t at = 0; at + 4 <= n && np < 256; at += 2) {
            uint32_t v = rd32(x->c->rom + at);
            if (v >= n) continue;
            for (unsigned j = 0; j < x->dec[d].nstarts; j++)
                if (x->dec[d].starts[j] == v) { pos[np++] = at; break; }
        }
        for (unsigned i = 0; i + 1 < np; i++) {
            size_t k = pos[i + 1] - pos[i];
            if (k < 4 || k > 16) continue;
            r->replay_tables++;
            size_t lo = pos[i], hi = pos[i + 1];
            uint32_t v;
            while (lo >= k && rom_ptr(x, lo - k, &v)) lo -= k;
            while (rom_ptr(x, hi + k, &v)) hi += k;
            for (size_t at = lo; at <= hi; at += k) {
                if (!rom_ptr(x, at, &v) || tried[v]) continue;
                tried[v] = 1;
                r->replay_candidates++;
                replay_one(x, d, v, &left);
            }
        }
    }
    free(tried);
}

int dp_md_explore(const DpMdCart *c, const DpMdOptions *o, DpMdResults *r) {
    memset(r, 0, sizeof *r);
    r->rom_size = c->size;
    r->flags = calloc(c->size, 1);
    r->nonexcluded_reads = calloc(c->size, 1);
    r->z80_flags = calloc(c->size, 1);
    r->copies = calloc(DP_MD_COPY_CAP, sizeof *r->copies);
    X x = {.c = c, .o = o, .r = r, .qcap = o->queue_cap};
    x.q = calloc(o->queue_cap ? o->queue_cap : 1, sizeof *x.q);
    if (o->lot_path && !(x.lot = dp_lf_open(o->lot_path, "MD", 1)))
        fprintf(stderr, "deepeel: cannot open the lot %s\n", o->lot_path);
    x.gen = calloc(c->size / 2 + 1, sizeof *x.gen);
    x.vis = calloc(c->size / 2 + 1, sizeof *x.vis);
    dp_set_init(&x.ind_sites, 1 << 8);
    dp_set_init(&x.ind_pairs, 1 << 10);
    x.site = calloc(c->size / 2 + 1, sizeof *x.site);
    x.pad_sites = calloc(c->size / 2 + 1, 1);
    x.m = dp_mdmach_open(c->rom, c->size);
    if (!r->flags || !r->z80_flags || !r->nonexcluded_reads || !r->copies || !x.q || !x.gen || !x.vis || !x.site || !x.pad_sites || !x.m) {
        dp_mdmach_close(x.m);
        free(x.q); free(x.gen); free(x.vis); free(x.site); free(x.pad_sites); dp_set_free(&x.ind_sites); dp_set_free(&x.ind_pairs);
        dp_md_results_free(r);
        return -1;
    }
    ssz = dp_mdmach_state_size();
    for (unsigned i = 0; i < 8192; i++) x.zsrc[i] = -1;
    DpMdHooks h = {&x, on_step, on_end, on_read, on_write, on_frame, on_dma_read,
                   on_z80_step, on_z80_end, on_z80_rom, on_sound_store};
    dp_mdmach_set_hooks(x.m, &h);
    dp_mdmach_reset(x.m);
    if (o->movie && o->movie->frames) {
        x.movie_active = 1;
        dp_mdmach_set_pad(x.m, o->movie->pads[0]);
    }
    int cut = 0;
    int drain = o->lot_drain && dp_lf_pending(x.lot); // saved states only: no reset path, no stream replay
    if (!drain) run_path(&x, NULL, o->max_total, &cut);
    x.movie_active = 0;
    r->decoders = x.ndec;
    if (!drain && r->steps < o->max_total) replay_streams(&x, (o->max_total - r->steps) / 2);
    for (unsigned d = 0; d < x.ndec; d++) free(x.dec[d].snap);
    // explicit entries (G3): each starts a forced path from the power-on machine
    if (!drain)
        for (unsigned e = 0; e < o->entry_count; e++) {
            uint32_t a = o->entries[e];
            if ((a & 1) || a >= r->rom_size) { r->entries_refused++; continue; }
            dp_mdmach_reset(x.m);
            if (push(&x, a, 0, 0)) r->entries_seeded++; else r->entries_refused++;
        }
    for (;;) {
        spill_flush(&x);
        if (r->steps >= o->max_total) break;
        if (!x.nq && x.npark) { x.q[x.nq++] = x.park[--x.npark]; r->parked_resumed++; }
        if (!x.nq && dp_lf_pending(x.lot)) { // the disk backlog comes back
            Item m; uint32_t ml = 0, bl = 0; uint8_t *blob = NULL;
            if (dp_lf_get(x.lot, &m, &ml, &blob, &bl)) {
                if (ml == sizeof m && bl == ssz) { m.state = blob; x.q[x.nq++] = m; r->parked_resumed++; }
                else free(blob);
            }
            if (!x.nq) continue;
        }
        if (!x.nq) break;
        Item it = x.q[--x.nq];
        run_path(&x, &it, o->max_total - r->steps, &cut);
        free(it.state);
    }
    // leftovers of a wall go to the lot when there is one (the next run resumes them)
    for (uint32_t i = 0; i < x.npark; i++) {
        if (!lot_spill(&x, &x.park[i])) r->dropped_pending++;
        free(x.park[i].state);
    }
    free(x.park);
    for (uint32_t i = 0; i < x.nq; i++) {
        if (!lot_spill(&x, &x.q[i])) r->dropped_pending++;
        free(x.q[i].state);
    }
    if (x.lot) {
        r->lot_written = x.lot->written; r->lot_read = x.lot->read_back;
        r->lot_pending = dp_lf_pending(x.lot);
        dp_lf_close(x.lot, o->lot_path);
    }
    // S5 copy siblings: a longword table entry (read as data) naming the source of a
    // recorded copy of >= 16 bytes marks a table of streams; neighbouring entries
    // that point at unobserved ROM are sibling streams of the same length.
    // Counted (bitmap local to this pass); at most 64 entries a table.
    {
        uint8_t *claimed = calloc(c->size, 1);
        for (int k = 0; claimed && k < r->ncopy && k < DP_MD_COPY_CAP; k++) {
            uint32_t src = r->copies[k].src, len = r->copies[k].len * 2u;   // words -> bytes
            if (len < 16 || src >= c->size) continue;
            for (size_t t = 0; t + 4 <= c->size; t += 2) {
                if (!(r->flags[t] & DP_MDF_DATA)) continue;
                uint32_t v = (uint32_t)c->rom[t] << 24 | (uint32_t)c->rom[t + 1] << 16 | (uint32_t)c->rom[t + 2] << 8 | c->rom[t + 3];
                if (v != src) continue;
                size_t t0 = t, t1 = t + 4;
                unsigned n = 1;
                for (;;) {   // walk down
                    if (t0 < 4 || n >= 64) break;
                    uint32_t w = (uint32_t)c->rom[t0 - 4] << 24 | (uint32_t)c->rom[t0 - 3] << 16 | (uint32_t)c->rom[t0 - 2] << 8 | c->rom[t0 - 1];
                    if (w < 0x200 || w >= c->size || (w & 1) || r->flags[w]) break;
                    t0 -= 4; n++;
                }
                for (;;) {   // walk up
                    if (t1 + 4 > c->size || n >= 64) break;
                    uint32_t w = (uint32_t)c->rom[t1] << 24 | (uint32_t)c->rom[t1 + 1] << 16 | (uint32_t)c->rom[t1 + 2] << 8 | c->rom[t1 + 3];
                    if (w < 0x200 || w >= c->size || (w & 1) || r->flags[w]) break;
                    t1 += 4; n++;
                }
                if (n >= 64) r->sib_caps++;
                if (n < 2) continue;
                r->sib_tables++;
                for (size_t e = t0; e < t1; e += 4) {
                    uint32_t w = (uint32_t)c->rom[e] << 24 | (uint32_t)c->rom[e + 1] << 16 | (uint32_t)c->rom[e + 2] << 8 | c->rom[e + 3];
                    if (w == src) continue;
                    r->sib_entries++;
                    for (uint32_t q = w; q < w + len && q < c->size; q++)
                        if (!r->flags[q] && !claimed[q]) { claimed[q] = 1; r->sib_bytes++; }
                }
                break;
            }
        }
        free(claimed);
    }
    for (size_t i = 0; i < c->size; i++) {
        uint8_t f = r->flags[i];
        if (f & (DP_MDF_NATURAL | DP_MDF_FORCED)) r->exec_bytes++;
        if ((f & DP_MDF_FORCED) && !(f & DP_MDF_NATURAL)) r->forced_only_bytes++;
        if (f & DP_MDF_DATA) r->dread_bytes++;
        if (f & DP_MDF_DATA_NATURAL) r->natural_data_bytes++;
        if (f & DP_MDF_DMA_SOURCE) r->dma_source_bytes++;
        if (f & DP_MDF_DMA_READ) r->dma_read_bytes++;
        if (f & DP_MDF_CPU_VRAM) r->cpu_vram_bytes++;
        if (f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DATA | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM))
            r->observed_bytes++;
        if (f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM))
            r->code_or_vram_bytes++;
        else if (f & DP_MDF_DATA) r->data_only_bytes++;
        if ((f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM)) || r->nonexcluded_reads[i])
            r->filtered_observed_bytes++;
        if (!(f & (DP_MDF_NATURAL | DP_MDF_FORCED | DP_MDF_DMA_READ | DP_MDF_CPU_VRAM)) && r->nonexcluded_reads[i] == 2)
            r->replay_only_bytes++;
    }
    for (size_t i = 0; i < c->size; i++) {
        uint8_t z = r->z80_flags[i];
        if (z & DP_MDZ_COPIED) r->z80_copied_bytes++;
        if (z & DP_MDZ_CODE_RAM) r->z80_code_bytes++;
        if (z & DP_MDZ_EXEC_ROM) r->z80_exec_rom_bytes++;
        if (z & DP_MDZ_DATA) r->z80_data_bytes++;
        if (z & DP_MDZ_SAMPLE) r->z80_sample_bytes++;
        if (z & DP_MDZ_MUSIC) r->z80_music_bytes++;
    }
    for (unsigned b = 0; b < sizeof x.z80_bank_seen * 8; b++)
        if (x.z80_bank_seen[b >> 3] & (1u << (b & 7))) r->z80_banks_read++;
    dp_mdmach_close(x.m);
    free(x.q); free(x.gen); free(x.vis); free(x.site); free(x.pad_sites); dp_set_free(&x.ind_sites); dp_set_free(&x.ind_pairs);
    return 0;
}

void dp_md_results_free(DpMdResults *r) {
    free(r->flags);
    free(r->nonexcluded_reads);
    free(r->z80_flags);
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
    fprintf(out, "pad probes: %llu (four button patterns per read site)\n", (unsigned long long)r->pad_forks);
    if (o->movie) fprintf(out, "GMV replay: %u / %u frames%s\n", r->movie_frames, o->movie->frames,
                          r->movie_frames == o->movie->frames ? "" : " (incomplete)");
    fprintf(out, "saturation (a non-zero line means the exploration is incomplete):\n");
    if (r->lot_written || r->lot_read || r->lot_pending)
        fprintf(out, "  lot: %llu written, %llu read back, %llu left on disk\n", (unsigned long long)r->lot_written,
                (unsigned long long)r->lot_read, (unsigned long long)r->lot_pending);
    fprintf(out, "  entries: %llu seeded, %llu refused (odd or outside the ROM)\n", (unsigned long long)r->entries_seeded, (unsigned long long)r->entries_refused);
    fprintf(out, "  frontier: parked %llu resumed %llu lost %llu\n", (unsigned long long)r->parked,
            (unsigned long long)r->parked_resumed, (unsigned long long)r->parked_lost);
    fprintf(out, "  total budget: %llu  pending forks unrun: %llu\n", (unsigned long long)r->sat_total,
            (unsigned long long)r->dropped_pending);
    fprintf(out, "  path budget: %llu  loop bound: %llu\n", (unsigned long long)r->sat_path,
            (unsigned long long)r->sat_visit);
    fprintf(out, "  queue full: %llu  per-site cap: %llu\n", (unsigned long long)r->sat_queue,
            (unsigned long long)r->sat_site);
    fprintf(out, "  queue growths: %llu (initial %u, ceiling %u)\n", (unsigned long long)r->queue_growths,
            o->queue_cap, o->queue_max > o->queue_cap ? o->queue_max : o->queue_cap);
    fprintf(out, "  copy siblings: %llu tables, %llu entries, %llu bytes, %llu table caps\n", (unsigned long long)r->sib_tables, (unsigned long long)r->sib_entries, (unsigned long long)r->sib_bytes, (unsigned long long)r->sib_caps);
    fprintf(out, "  padding cuts: %llu\n", (unsigned long long)r->pad_cuts);
    fprintf(out, "  indirect jumps: %llu sites, %llu targets, overflow %llu\n", (unsigned long long)r->indirect_sites, (unsigned long long)r->indirect_targets, (unsigned long long)r->indirect_overflow);
    fprintf(out, "  copy records dropped: %llu\n", (unsigned long long)r->copy_overflow);
    fprintf(out, "  stream replay: step cap %llu  total budget %llu  consumers dropped %llu  starts dropped %llu  call frames dropped %llu\n",
            (unsigned long long)r->sat_replay_steps, (unsigned long long)r->sat_replay_total,
            (unsigned long long)r->decoder_overflow, (unsigned long long)r->start_overflow,
            (unsigned long long)r->frame_overflow);
    fprintf(out, "not losses: alternative already seen %llu, alternative outside rom %llu\n",
            (unsigned long long)r->fork_seen, (unsigned long long)r->fork_off_rom);
    if (r->faults) {
        fprintf(out, "paths ended by a machine fault: %llu (unrecorded: %llu)\n",
                (unsigned long long)r->faults, (unsigned long long)r->fault_sites_overflow);
        for (unsigned i = 0; i < r->nfault_sites; i++) {
            const DpMdFaultSite *f = &r->fault_sites[i];
            fprintf(out, "  code %d instruction %06X machine-pc %06X count %llu (natural %llu)\n",
                    f->code, f->pc, f->fault_pc, (unsigned long long)f->count, (unsigned long long)f->natural);
            fprintf(out, "    sample trace:");
            for (unsigned j = 0; j < f->trace_len; j++) fprintf(out, " %06X", f->trace[j]);
            fputc('\n', out);
            if (o->watch_write <= 0xFFFFFFu) {
                if (f->watch_pc == UINT32_MAX) fprintf(out, "    watched write %06X: none on this path\n", o->watch_write);
                else fprintf(out, "    watched write %06X: pc %06X addr %06X size %u value %08X\n",
                             o->watch_write, f->watch_pc, f->watch_addr, f->watch_size, f->watch_value);
                if (f->watch_pc != UINT32_MAX)
                    fprintf(out, "    at write: D0=%08X A1=%08X A2=%08X\n", f->watch_d0, f->watch_a1, f->watch_a2);
            }
            fprintf(out, "    sample address registers (value@last-change):");
            for (unsigned j = 0; j < 8; j++) {
                fprintf(out, " A%u=%08X@", j, f->an[j]);
                if (f->an_writer[j] == UINT32_MAX) fprintf(out, "reset");
                else fprintf(out, "%06X", f->an_writer[j]);
            }
            fputc('\n', out);
        }
        fprintf(out, "fault origins (unrecorded: %llu):\n", (unsigned long long)r->fault_origins_overflow);
        for (unsigned i = 0; i < r->nfault_origins; i++) {
            const DpMdFaultSite *f = &r->fault_origins[i];
            fprintf(out, "  code %d instruction %06X machine-pc %06X origin ", f->code, f->pc, f->fault_pc);
            if (f->origin == UINT32_MAX) fprintf(out, "reset");
            else fprintf(out, "%06X", f->origin);
            fprintf(out, " count %llu (natural %llu)\n", (unsigned long long)f->count, (unsigned long long)f->natural);
        }
    }
    fprintf(out, "executed rom bytes: %u (%.1f%%), of which only via forks: %u; data-read: %u\n", r->exec_bytes,
            100.0 * r->exec_bytes / r->rom_size, r->forced_only_bytes, r->dread_bytes);
    fprintf(out, "data-read on natural path: %u; commanded DMA source bytes: %u (may include an interrupted transfer)\n",
            r->natural_data_bytes, r->dma_source_bytes);
    fprintf(out, "DMA ROM bytes actually read: %u\n", r->dma_read_bytes);
    fprintf(out, "CPU MOVE.W ROM->VRAM (direct or consecutive register move): %llu words, %u distinct source bytes\n",
            (unsigned long long)r->cpu_vram_words, r->cpu_vram_bytes);
    fprintf(out, "observed union (execution, CPU reads, DMA reads, VRAM copies): %u / %zu (%.2f%%)\n",
            r->observed_bytes, r->rom_size, 100.0 * r->observed_bytes / r->rom_size);
    fprintf(out, "  code or observed VRAM source: %u (%.2f%%); CPU-read-only: %u; unobserved: %zu\n",
            r->code_or_vram_bytes, 100.0 * r->code_or_vram_bytes / r->rom_size,
            r->data_only_bytes, r->rom_size - r->observed_bytes);
    fprintf(out, "  CPU reads include integrity scans; observed does not mean played or understood.\n");
    if (o->exclude_read_pc <= 0xFFFFFFu)
        fprintf(out, "observed union excluding CPU reads from pc %06X: %u / %zu (%.2f%%)\n",
                o->exclude_read_pc, r->filtered_observed_bytes, r->rom_size,
                100.0 * r->filtered_observed_bytes / r->rom_size);
    fprintf(out, "stream replay: %u consumers (%u without entry snapshot), %llu tables, %llu candidates, %llu runs, %llu steps\n",
            r->decoders, r->decoders_without_snapshot, (unsigned long long)r->replay_tables,
            (unsigned long long)r->replay_candidates, (unsigned long long)r->replay_runs,
            (unsigned long long)r->replay_steps);
    fprintf(out, "  bytes read only by replays: %u (%.2f%%); faults %llu\n", r->replay_only_bytes,
            100.0 * r->replay_only_bytes / r->rom_size, (unsigned long long)r->replay_faults);
    fprintf(out, "ROM read sites: %u (unrecorded accesses: %llu)\n", r->nreaders,
            (unsigned long long)r->reader_overflow);
    for (unsigned i = 0; i < r->nreaders; i++) {
        const DpMdReader *p = &r->readers[i];
        fprintf(out, "  pc %06X range %06X-%06X accesses %llu (natural %llu)\n", p->pc, p->lo, p->hi,
                (unsigned long long)p->accesses, (unsigned long long)p->natural_accesses);
    }
    fprintf(out, "exec outside rom: %llu (ram: %llu)\n", (unsigned long long)r->exec_outside,
            (unsigned long long)r->ram_exec);
    fprintf(out, "SRAM: %llu instructions, %llu CPU reads\n",
            (unsigned long long)r->sram_exec, (unsigned long long)r->sram_reads);
    fprintf(out, "decoder/execution length mismatches: %llu\n", (unsigned long long)r->decode_length_mismatches);
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
    fprintf(out, "z80 sound cpu: %llu steps (RAM %llu, bank window %llu, other %llu); inside the 68000 budget, not counted in it\n",
            (unsigned long long)r->z80_steps, (unsigned long long)r->z80_exec_ram,
            (unsigned long long)r->z80_exec_bank, (unsigned long long)r->z80_exec_other);
    fprintf(out, "  ROM bytes copied to Z80 RAM by the 68000: %u (%llu stores); executed there (code-ramcopy): %u\n",
            r->z80_copied_bytes, (unsigned long long)r->z80_ram_copied, r->z80_code_bytes);
    fprintf(out, "  executed from the bank window: %u bytes; data read through it: %u bytes in %u bank(s) of 32 KiB (%llu reads)\n",
            r->z80_exec_rom_bytes, r->z80_data_bytes, r->z80_banks_read, (unsigned long long)r->z80_rom_reads);
    fprintf(out, "  YM2612 stores %llu, PSG stores %llu; ROM bytes typed sample (DAC): %u, music (FM/PSG data): %u\n",
            (unsigned long long)r->z80_ym_writes, (unsigned long long)r->z80_psg_writes,
            r->z80_sample_bytes, r->z80_music_bytes);
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
    static const struct { uint8_t bit; const char *name; } zk[] = {
        {DP_MDZ_CODE_RAM, "z80-code-ramcopy"}, {DP_MDZ_EXEC_ROM, "z80-code-rom"}, {DP_MDZ_SAMPLE, "z80-sample"},
        {DP_MDZ_MUSIC, "z80-music"}, {DP_MDZ_DATA, "z80-data"}, {DP_MDZ_COPIED, "z80-ram-source"}};
    for (unsigned k = 0; k < sizeof zk / sizeof *zk; k++) {
        size_t a = 0;
        while (a < r->rom_size) {
            if (!(r->z80_flags[a] & zk[k].bit)) { a++; continue; }
            size_t b = a;
            while (b < r->rom_size && (r->z80_flags[b] & zk[k].bit)) b++;
            fprintf(f, "%s %06zX-%06zX\n", zk[k].name, a, b - 1);
            a = b;
        }
    }
    for (int k = 0; k < r->ncopy; k++)
        fprintf(f, "copy pc=%06X src=%06X words=%u code=%02X\n", r->copies[k].pc, r->copies[k].src,
                r->copies[k].len, r->copies[k].code);
    fclose(f);
    return 0;
}
