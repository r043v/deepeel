// sm83_isa.c: the SM83 as a DpIsa (spec §7.3).  Wraps the existing helpers;
// the classification mirrors what the static closure used to open-code.
#include "../../core/dpmachine.h"
#include "sm83.h"

static int is_illegal(uint8_t op) {
    switch (op) {
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4:
    case 0xEB: case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
        return 1;
    default:
        return 0;
    }
}

static int isa_cond_of(uint8_t op) { return sm83_cond_of(op); }
static uint8_t isa_cond_flag(int c) { return sm83_cond_flag(c); }
static int isa_illegal(uint8_t op) { return is_illegal(op); }

// conservative kill tables: anything not proven kept is lost, never guessed
static int hl_writes(uint8_t op) {
    if (op == 0xCB) return 1; // rot/shift/bit ops on h/l: untracked
    if (op >= 0x60 && op <= 0x66) return 1; // ld h,r except ld h,(hl) handled
    if (op >= 0x68 && op <= 0x6E) return 1; // ld l,r except ld l,(hl) handled
    switch (op) {
    case 0x09: case 0x19: case 0x29: case 0x39: // add hl,rr
    case 0x22:                                  // ldi (hl),a
    case 0xE1:                                  // pop hl
    case 0xEB:                                  // ex de,hl
    case 0xF8:                                  // ld hl,sp+r8
        return 1;
    default:
        return 0;
    }
}

static int a_writes(uint8_t op) {
    if (op == 0xCB) return 1;
    if ((op >= 0x80 && op <= 0xBF)) return 1;         // alu (except handled (hl) reads)
    if (op >= 0x04 && op <= 0x3D && op != 0x76) return 1; // single-arg alu/rot/daa/cpl/scf..
    switch (op) {
    case 0x0A: case 0x1A:      // ld a,(bc) / ld a,(de)
    case 0xF0: case 0xF2:      // ldh a,(n) / ld a,(C)
    case 0x07: case 0x0F: case 0x17: case 0x1F: // rotates through a
        return 1;
    default:
        return 0;
    }
}

// a provable (hl)/(nn) read: the byte lands in A, unknown otherwise
static void load_a(DpProvRegs *r, const DpProvRom *rom, uint16_t addr) {
    uint8_t v;
    if (rom->read(rom->ctx, addr, &v)) { r->a = v; r->aknown = 1; } else r->aknown = 0;
}

static void isa_effect(const uint8_t *b, DpProvRegs *r, const DpProvRom *rom) {
    uint8_t op = b[0], v;
    switch (op) {
    case 0x21: r->p = (uint16_t)(b[1] | (b[2] << 8)); r->pmask = 3; break;
    case 0x26: r->p = (uint16_t)((r->p & 0x00FF) | (b[1] << 8)); r->pmask |= 2; break;
    case 0x2E: r->p = (uint16_t)((r->p & 0xFF00) | b[1]); r->pmask |= 1; break;
    case 0x23: if (r->pmask == 3) r->p++; break;
    case 0x2B: if (r->pmask == 3) r->p--; break;
    case 0x2A: // ld a,(hli)
        if (r->pmask == 3) { load_a(r, rom, r->p); r->p++; } else r->aknown = 0;
        break;
    case 0x7E: // ld a,(hl)
        if (r->pmask == 3) load_a(r, rom, r->p); else r->aknown = 0;
        break;
    case 0x66: // ld h,(hl)
        if (r->pmask == 3 && rom->read(rom->ctx, r->p, &v)) {
            r->p = (uint16_t)((r->p & 0x00FF) | (v << 8)); r->pmask |= 2;
        } else r->pmask &= (uint8_t)~2;
        break;
    case 0x6E: // ld l,(hl)
        if (r->pmask == 3 && rom->read(rom->ctx, r->p, &v)) {
            r->p = (uint16_t)((r->p & 0xFF00) | v); r->pmask |= 1;
        } else r->pmask &= (uint8_t)~1;
        break;
    case 0x3E: r->a = b[1]; r->aknown = 1; break; // ld a,n
    case 0x67: // ld h,a
        if (r->aknown) { r->p = (uint16_t)((r->p & 0x00FF) | (r->a << 8)); r->pmask |= 2; }
        else r->pmask &= (uint8_t)~2;
        break;
    case 0x6F: // ld l,a
        if (r->aknown) { r->p = (uint16_t)((r->p & 0xFF00) | r->a); r->pmask |= 1; }
        else r->pmask &= (uint8_t)~1;
        break;
    case 0xFA: // ld a,(nn): a word read at an absolute address
        load_a(r, rom, (uint16_t)(b[1] | (b[2] << 8)));
        break;
    case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: case 0x10: // control flow: untouched
        break;
    default:
        if (hl_writes(op)) r->pmask = 0;
        if (a_writes(op)) r->aknown = 0;
        break;
    }
}

static int isa_length(uint8_t op) { return sm83_len(op); }

static int isa_flow(const uint8_t *b, uint16_t pc, DpFlowInfo *o) {
    uint8_t op = b[0];
    int len = sm83_len(op);
    o->kind = DP_FLOW_SEQ; o->length = (uint8_t)len; o->fall = 1; o->call = 0;
    o->target = -1; o->cond = (int8_t)sm83_cond_of(op); o->rst = 0; o->halt = op == 0x76 || op == 0x10;
    if (is_illegal(op)) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return len; }
    switch (op) {
    case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: // JR [cc]
        o->target = (int32_t)((pc + 2 + (int8_t)b[1]) & 0xFFFF);
        o->kind = op == 0x18 ? DP_FLOW_JUMP : DP_FLOW_COND;
        o->fall = op != 0x18;
        break;
    case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA: // JP [cc]
        o->target = b[1] | (b[2] << 8);
        o->kind = op == 0xC3 ? DP_FLOW_JUMP : DP_FLOW_COND;
        o->fall = op != 0xC3;
        break;
    case 0xCD: case 0xC4: case 0xCC: case 0xD4: case 0xDC: // CALL [cc]
        o->target = b[1] | (b[2] << 8);
        o->kind = DP_FLOW_CALL; o->call = 1;
        o->fall = 1;
        break;
    case 0xC9: case 0xD9: // RET, RETI
        o->kind = DP_FLOW_RET; o->fall = 0;
        break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: // RET cc
        o->kind = DP_FLOW_COND;
        break;
    case 0xE9: // JP HL
        o->kind = DP_FLOW_INDIRECT; o->fall = 0;
        break;
    default:
        if ((op & 0xC7) == 0xC7) { // RST
            o->target = op & 0x38; o->kind = DP_FLOW_CALL; o->call = 1; o->rst = 1;
        }
        break;
    }
    return len;
}

// data operand of an SM83 instruction (spec §7.3 mem_operand)
static int isa_mem_operand(const uint8_t *b, DpMemOp *o) {
    uint8_t op = b[0];
    *o = (DpMemOp){ 0 };
    o->width = 1;
    #define PAIR(acc, pr, po) do { o->access = (acc); o->mode = DP_AM_PAIR; o->pair = (pr); o->post = (po); return 1; } while (0)
    switch (op) {
    case 0x02: PAIR(DP_MA_WRITE, 0, 0);
    case 0x12: PAIR(DP_MA_WRITE, 1, 0);
    case 0x0A: PAIR(DP_MA_READ, 0, 0);
    case 0x1A: PAIR(DP_MA_READ, 1, 0);
    case 0x22: PAIR(DP_MA_WRITE, 2, 1);
    case 0x32: PAIR(DP_MA_WRITE, 2, -1);
    case 0x2A: PAIR(DP_MA_READ, 2, 1);
    case 0x3A: PAIR(DP_MA_READ, 2, -1);
    case 0x34: case 0x35: PAIR(DP_MA_RW, 2, 0);
    case 0x36: PAIR(DP_MA_WRITE, 2, 0);
    case 0xEA: o->access = DP_MA_WRITE; o->mode = DP_AM_ABS; o->addr = (uint16_t)(b[1] | b[2] << 8); return 1;
    case 0xFA: o->access = DP_MA_READ; o->mode = DP_AM_ABS; o->addr = (uint16_t)(b[1] | b[2] << 8); return 1;
    case 0x08: o->access = DP_MA_WRITE; o->mode = DP_AM_ABS; o->width = 2; o->addr = (uint16_t)(b[1] | b[2] << 8); return 1;
    case 0xE0: o->access = DP_MA_WRITE; o->mode = DP_AM_HIGH_IMM; o->addr = (uint16_t)(0xFF00 + b[1]); return 1;
    case 0xF0: o->access = DP_MA_READ; o->mode = DP_AM_HIGH_IMM; o->addr = (uint16_t)(0xFF00 + b[1]); return 1;
    case 0xE2: o->access = DP_MA_WRITE; o->mode = DP_AM_HIGH_REG; return 1;
    case 0xF2: o->access = DP_MA_READ; o->mode = DP_AM_HIGH_REG; return 1;
    case 0xC5: case 0xD5: case 0xE5: case 0xF5:
    case 0xC4: case 0xCC: case 0xCD: case 0xD4: case 0xDC:
        o->access = DP_MA_WRITE; o->mode = DP_AM_STACK; o->pair = 3; o->width = 2; return 1;
    case 0xC1: case 0xD1: case 0xE1: case 0xF1:
    case 0xC9: case 0xD9: case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        o->access = DP_MA_READ; o->mode = DP_AM_STACK; o->pair = 3; o->width = 2; return 1;
    case 0xCB: // (HL) forms: BIT only reads, the rest read-modify-write
        if ((b[1] & 7) != 6) return 0;
        PAIR((b[1] >= 0x40 && b[1] < 0x80) ? DP_MA_READ : DP_MA_RW, 2, 0);
    }
    if (op >= 0xC7 && (op & 7) == 7) { // rst
        o->access = DP_MA_WRITE; o->mode = DP_AM_STACK; o->pair = 3; o->width = 2; return 1;
    }
    if (op >= 0x40 && op < 0xC0 && (op & 7) == 6 && op != 0x76) // ld r,(hl) / alu a,(hl)
        PAIR(DP_MA_READ, 2, 0);
    if (op >= 0x70 && op <= 0x77 && op != 0x76) PAIR(DP_MA_WRITE, 2, 0);
    #undef PAIR
    return 0;
}

static const DpIsa isa = { "sm83", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag, isa_effect, isa_illegal, sm83_disasm, isa_mem_operand };
const DpIsa *dp_isa_sm83(void) { return &isa; }
