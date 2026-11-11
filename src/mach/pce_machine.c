// pce_machine.c: the PC Engine m0boy runtime as a DpMachine (spec §7.2).
// The explorer still drives the runtime singleton directly; this wrapper is
// the kernel-facing view over the same instance (exploration is sequential).
#include <stdlib.h>
#include <string.h>

#include "../core/dpmachine.h"
#include "../platform/pce/pce.h"
#include "../explore/core.h"

typedef struct {
    const DpObservers *obs;
    void *user;
    // clones keep the image and initialise the runtime lazily in the worker thread
    // that uses it: the PCE machine state is thread-local
    uint8_t *img; size_t img_size; int inited;
    uint64_t frames;   // completed frames (relative clock for sessions)
    const uint8_t *src; size_t src_size;
} PceWrap;
static void pce_ensure(DpMachine *m) {
    PceWrap *w = m->impl;
    if (w->img && !w->inited) { dp_pce_init(w->img, w->img_size); w->inited = 1; }
}

static uint32_t pce_state_bytes(DpMachine *m) { (void)m; return (uint32_t)dp_pce_state_size() + 8; }
static void pce_save(DpMachine *m, uint8_t *buf) { pce_ensure(m); dp_pce_save(buf); memcpy(buf + dp_pce_state_size(), &((PceWrap *)m->impl)->frames, 8); }
static void pce_load(DpMachine *m, const uint8_t *buf) { pce_ensure(m); dp_pce_load(buf); memcpy(&((PceWrap *)m->impl)->frames, buf + dp_pce_state_size(), 8); }
static uint8_t pce_fetch(DpMachine *m, uint32_t a) { pce_ensure(m); return dp_pce_peek((uint16_t)a); }
static void pce_poke(DpMachine *m, uint32_t a, uint8_t v) { pce_ensure(m); dp_pce_poke((uint16_t)a, v); }
static int64_t pce_rom_index(DpMachine *m, uint32_t a) { pce_ensure(m); return dp_pce_rom_index((uint16_t)a); }

// one step (an instruction or an interrupt dispatch)
static int pce_run(DpMachine *m) {
    pce_ensure(m);
    int frame_done = 0;
    int ok = dp_pce_step(&frame_done) > 0;
    if (frame_done) ((PceWrap *)m->impl)->frames++;
    return ok;
}

// kernel mask (up down left right button1 button2 start) -> m0boy PCE pad (I, II, Select, Run, Up, Right, Down, Left)
static void pce_set_input(DpMachine *m, uint32_t k) {
    pce_ensure(m);
    uint8_t p = 0;
    if (k & 0x10) p |= 0x01;
    if (k & 0x20) p |= 0x02;
    if (k & 0x40) p |= 0x08;
    if (k & 0x01) p |= 0x10;
    if (k & 0x08) p |= 0x20;
    if (k & 0x02) p |= 0x40;
    if (k & 0x04) p |= 0x80;
    dp_pce_input(p);
}
static uint64_t pce_frame(DpMachine *m) { return ((PceWrap *)m->impl)->frames; }

// the CPU core touches memory without calling the bus, so the data accesses the kernel observes
// (technique 1 reads, the ROM->RAM copy record, technique 6 streams) are decoded from the instruction
// before it runs: every 65C02/HuC6280 addressing mode of the loads, stores and compares, and the
// block transfers (TII/TDD/TIN/TIA/TAI).  Hardware registers are never peeked (a read could have effects).
static int pce_readable(uint16_t a) { return (a >= 0x2000 && a < 0x4000) || dp_pce_rom_index(a) >= 0; }
static uint16_t pce_zp(uint8_t z) { return (uint16_t)(0x2000 + z); }
static uint16_t pce_w16(uint16_t a) { return (uint16_t)(dp_pce_peek(a) | dp_pce_peek((uint16_t)(a + 1)) << 8); }

// effective address of a data access of `op` at `pc` (the operand bytes follow it); 0 when it has none.
// *wr is 1 for a pure store.  Read-modify-write and compare/load/ALU opcodes are reads.
static int pce_ea(uint16_t pc, uint8_t op, uint16_t *ea, int *wr) {
    DpPceCpu c = dp_pce_cpu();
    uint8_t b1 = dp_pce_peek((uint16_t)(pc + 1));
    uint16_t ab = pce_w16((uint16_t)(pc + 1));
    *wr = 0;
    switch (op) {
    case 0x9C: *ea = ab; *wr = 1; return 1;                                   // stz abs
    case 0x9E: *ea = (uint16_t)(ab + c.x); *wr = 1; return 1;                 // stz abs,x
    case 0x64: *ea = pce_zp(b1); *wr = 1; return 1;                           // stz zp
    case 0x74: *ea = pce_zp((uint8_t)(b1 + c.x)); *wr = 1; return 1;          // stz zp,x
    case 0x84: *ea = pce_zp(b1); *wr = 1; return 1;                           // sty zp
    case 0x94: *ea = pce_zp((uint8_t)(b1 + c.x)); *wr = 1; return 1;          // sty zp,x
    case 0x8C: *ea = ab; *wr = 1; return 1;                                   // sty abs
    case 0x86: *ea = pce_zp(b1); *wr = 1; return 1;                           // stx zp
    case 0x96: *ea = pce_zp((uint8_t)(b1 + c.y)); *wr = 1; return 1;          // stx zp,y
    case 0x8E: *ea = ab; *wr = 1; return 1;                                   // stx abs
    case 0xB6: *ea = pce_zp((uint8_t)(b1 + c.y)); return 1;                   // ldx zp,y
    case 0xBE: *ea = (uint16_t)(ab + c.y); return 1;                          // ldx abs,y
    case 0x34: *ea = pce_zp((uint8_t)(b1 + c.x)); return 1;                   // bit zp,x
    case 0x3C: *ea = (uint16_t)(ab + c.x); return 1;                          // bit abs,x
    case 0xB4: *ea = pce_zp((uint8_t)(b1 + c.x)); return 1;                   // ldy zp,x
    case 0xBC: *ea = (uint16_t)(ab + c.x); return 1;                          // ldy abs,x
    case 0x04: case 0x14: case 0x0C: case 0x1C:                               // tsb/trb
        *ea = (op & 8) ? ab : pce_zp(b1); return 1;
    case 0x24: case 0xA4: case 0xC4: case 0xE4: case 0xA6: *ea = pce_zp(b1); return 1;
    case 0x2C: case 0xAC: case 0xCC: case 0xEC: case 0xAE: *ea = ab; return 1;
    default: break;
    }
    if ((op & 3) == 1) {                                                      // ora and adc sta lda cmp sbc
        int store = (op >> 5) == 4;
        switch ((op >> 2) & 7) {
        case 0: *ea = pce_w16(pce_zp((uint8_t)(b1 + c.x))); break;                          // (zp,x)
        case 1: *ea = pce_zp(b1); break;                                                    // zp
        case 2: return 0;                                                                   // immediate
        case 3: *ea = ab; break;                                                            // abs
        case 4: *ea = (uint16_t)(pce_w16(pce_zp(b1)) + c.y); break;                         // (zp),y
        case 5: *ea = pce_zp((uint8_t)(b1 + c.x)); break;                                   // zp,x
        case 6: *ea = (uint16_t)(ab + c.y); break;                                          // abs,y
        default: *ea = (uint16_t)(ab + c.x); break;                                         // abs,x
        }
        *wr = store;
        return 1;
    }
    if ((op & 0x1F) == 0x12) { *ea = pce_w16(pce_zp(b1)); *wr = op == 0x92; return 1; }   // (zp) forms
    if ((op & 3) == 2 && op != 0x82 && op != 0xA2 && op != 0xC2) {                        // asl rol lsr ror dec inc (rmw)
        switch ((op >> 2) & 7) {
        case 1: *ea = pce_zp(b1); return 1;
        case 3: *ea = ab; return 1;
        case 5: *ea = pce_zp((uint8_t)(b1 + c.x)); return 1;
        case 7: *ea = (uint16_t)(ab + c.x); return 1;
        default: return 0;
        }
    }
    return 0;
}

static void pce_data_access(PceWrap *w, uint16_t pc, uint8_t op) {
    DpPceCpu c = dp_pce_cpu();
    if (op == 0x73 || op == 0xC3 || op == 0xD3 || op == 0xE3 || op == 0xF3) {   // block transfers: src, dst, length
        uint16_t src = pce_w16((uint16_t)(pc + 1)), dst = pce_w16((uint16_t)(pc + 3)), len = pce_w16((uint16_t)(pc + 5));
        unsigned n = len ? len : 0x10000u;
        for (unsigned i = 0; i < n; i++) {
            uint16_t sa, da;
            switch (op) {
            case 0x73: sa = (uint16_t)(src + i); da = (uint16_t)(dst + i); break;          // tii
            case 0xC3: sa = (uint16_t)(src - i); da = (uint16_t)(dst - i); break;          // tdd
            case 0xD3: sa = (uint16_t)(src + i); da = dst; break;                          // tin
            case 0xE3: sa = (uint16_t)(src + i); da = (uint16_t)(dst + (i & 1)); break;    // tia: destination alternates
            default:   sa = (uint16_t)(src + (i & 1)); da = (uint16_t)(dst + i); break;    // tai: source alternates
            }
            int rd = pce_readable(sa);
            uint8_t v = rd ? dp_pce_peek(sa) : 0;
            if (rd && w->obs->on_read) w->obs->on_read(w->user, sa, v);
            if (w->obs->on_write) w->obs->on_write(w->user, da, v);
        }
        return;
    }
    uint16_t ea; int wr;
    if (!pce_ea(pc, op, &ea, &wr)) return;
    if (wr) {
        uint8_t v = op == 0x9C || op == 0x9E || op == 0x64 || op == 0x74 ? 0 :
                    (op == 0x84 || op == 0x94 || op == 0x8C) ? c.y :
                    (op == 0x86 || op == 0x96 || op == 0x8E) ? c.x : c.a;
        if (w->obs->on_write) w->obs->on_write(w->user, ea, v);
    } else if (pce_readable(ea) && w->obs->on_read) w->obs->on_read(w->user, ea, dp_pce_peek(ea));
}

static void pce_trace_fn(void *user, uint16_t pc, uint8_t op) {
    PceWrap *w = user;
    if (w->obs) pce_data_access(w, pc, op);
    if (w->obs && w->obs->on_step) w->obs->on_step(w->user, pc, op, 0);
}
static void pce_observe(DpMachine *m, const DpObservers *o, void *user) {
    PceWrap *w = m->impl;
    pce_ensure(m);
    w->obs = o; w->user = user;
    dp_pce_trace(o ? pce_trace_fn : NULL, w);
}

static void pce_close(DpMachine *m) {
    PceWrap *w = m->impl;
    if (w->img) { if (w->inited) dp_pce_close(); free(w->img); }
    free(m->impl);
    free(m);
}

// wraps the live runtime singleton (dp_pce_init already done, or call it first)
static uint32_t pce_pc(DpMachine *m) { pce_ensure(m); return dp_pce_cpu().pc; }
static void pce_set_pc(DpMachine *m, uint32_t pc) { pce_ensure(m); dp_pce_set_pc((uint16_t)pc); }

// technique 2 (interrupts): where the CPU unmasks (CLI, PLP, RTI, or a write to the interrupt
// mask $1402) one alternative per source (1 = VDC, 2 = timer) is forked with that source raised,
// as the old PCE engine's fork_irq
static int pce_event_probe(void *user, DpMachine *m, uint32_t pc, const uint8_t *bytes, unsigned k, uint8_t *save) {
    (void)user; (void)pc;
    if (k >= 2) return 0;
    int unmask = bytes[0] == 0x58 || bytes[0] == 0x28 || bytes[0] == 0x40;
    if (!unmask && (bytes[0] == 0x8D || bytes[0] == 0x9C) && (((bytes[2] << 8) | bytes[1]) & 0x1FFF) == 0x1402) unmask = 1;
    if (!unmask) return 0;
    uint8_t *tmp = malloc(m->state_bytes(m));
    if (!tmp) return 0;
    m->save(m, tmp);
    dp_pce_raise((int)k + 1);
    m->save(m, save);
    m->load(m, tmp);
    free(tmp);
    return 1;
}
// techniques 9 / IRQ fork: when the next step would dispatch an interrupt, the state where it lands `delay`
// instructions later (the I flag held for those instructions; the latch stays raised)
static int pce_irq_alt(void *user, DpMachine *m, unsigned delay, uint8_t *save) {
    (void)user;
    pce_ensure(m);
    if (!dp_pce_irq_due()) return 0;
    uint8_t *tmp = malloc(m->state_bytes(m));
    if (!tmp) return 0;
    m->save(m, tmp);
    uint8_t p0 = dp_pce_cpu().p;
    dp_pce_set_flags((uint8_t)(p0 | 0x04));
    int ok = 1;
    for (unsigned i = 0; i < delay && ok; i++) ok = m->run(m);
    if (ok) {
        dp_pce_set_flags((uint8_t)((dp_pce_cpu().p & ~0x04) | (p0 & 0x04)));
        m->save(m, save);
    }
    m->load(m, tmp);
    free(tmp);
    return ok;
}
const DpPlatformHooks *dp_machine_pce_hooks(void) {
    static const DpPlatformHooks h = { .event_probe = pce_event_probe, .irq_alt = pce_irq_alt };
    return &h;
}

static int pce_attach(DpMachine *m) { pce_ensure(m); return 1; }
static DpMachine *pce_clone(DpMachine *m, unsigned worker) {
    (void)worker;
    PceWrap *o = m->impl, *w = calloc(1, sizeof *w);
    DpMachine *c = calloc(1, sizeof *c);
    if (!w || !c || !o->src) { free(w); free(c); return NULL; }
    *c = *m;
    w->img = malloc(o->src_size);
    if (!w->img) { free(w); free(c); return NULL; }
    memcpy(w->img, o->src, o->src_size);
    w->img_size = o->src_size;
    c->impl = w;
    c->attach = pce_attach;
    return c;
}
void dp_machine_pce_source(DpMachine *m, const uint8_t *data, size_t size) {
    PceWrap *w = m->impl;
    w->src = data; w->src_size = size;
    m->clone = pce_clone;
}

DpMachine *dp_machine_pce(void) {
    PceWrap *w = calloc(1, sizeof *w);
    DpMachine *m = calloc(1, sizeof *m);
    if (!w || !m) { free(w); free(m); return NULL; }
    m->name = "pce";
    m->impl = w;             // the huc6280 DpIsa wrapper is not extracted yet
    m->state_bytes = pce_state_bytes;
    m->save = pce_save;
    m->load = pce_load;
    m->fetch = pce_fetch;
    m->poke = pce_poke;
    m->rom_index = pce_rom_index;
    m->run = pce_run;
    m->observe = pce_observe;
    m->close = pce_close;
    m->isa = dp_isa_huc6280();
    m->pc = pce_pc;
    m->set_pc = pce_set_pc;
    m->frame = pce_frame;
    m->set_input = pce_set_input;
    return m;
}
