// huc_isa.c: the HuC6280 as a DpIsa (spec §7.3).  Wraps the PCE decoder
// (src/platform/pce/hucdec.c); conditions are the decoder's own encoding
// (0..7 flag branches, 8+bit BBR/BBS, 16+bit BBS — see DpHucIns.cond).
#include "../core/dpmachine.h"
#include "../platform/pce/pce.h"
#include <string.h>

static int isa_length(uint8_t op) { return dp_huc_length(op); }

static int isa_flow(const uint8_t *bytes, uint32_t pc, DpFlowInfo *o) {
    DpHucIns in;
    o->kind = DP_FLOW_SEQ; o->fall = 1; o->call = 0; o->rst = 0; o->halt = 0;
    o->target = -1; o->cond = -1; o->length = 1; o->pair = DP_REG_PAIR_NONE;
    if (!dp_huc_decode(bytes, 7, pc, &in)) { o->kind = DP_FLOW_ILLEGAL; o->fall = 0; return 1; }
    o->length = in.length;
    o->cond = in.cond;
    switch (in.flow) {
    case DP_HUC_NEXT:
        break;
    case DP_HUC_JUMP:
        o->kind = DP_FLOW_JUMP; o->fall = 0; o->target = in.target;
        break;
    case DP_HUC_INDIRECT:
        o->kind = DP_FLOW_INDIRECT; o->fall = 0;
        break;
    case DP_HUC_CALL:
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->target = in.target;
        break;
    case DP_HUC_RETURN:
        o->kind = DP_FLOW_RET; o->fall = 0;
        break;
    case DP_HUC_BRANCH:
        o->kind = DP_FLOW_COND; o->fall = 1; o->target = in.target;
        break;
    case DP_HUC_BRK:
        // a software interrupt through $FFF6: the vector is outside the ROM
        o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1;
        break;
    case DP_HUC_ILLEGAL:
        o->kind = DP_FLOW_ILLEGAL; o->fall = 1;   // a two-cycle NOP on the machine
        break;
    }
    return o->length;
}

static int isa_cond_of(uint8_t op) {
    DpHucIns in;
    uint8_t bytes[7] = { op, 0 };
    if (!dp_huc_decode(bytes, sizeof bytes, 0, &in)) return -1;
    return in.flow == DP_HUC_BRANCH ? in.cond : -1;
}

// the tested flag (dp_huc_taken's own convention); BBR/BBS test zero-page bits
static uint8_t isa_cond_flag(int cond) {
    static const uint8_t flag[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 }; // C Z D I T V N
    if (cond >= 8) return 0;   // a zero-page bit: no flag
    return flag[cond & 7];
}

static int isa_illegal(uint8_t op) {
    DpHucIns in;
    uint8_t bytes[7] = { op, 0 };
    if (!dp_huc_decode(bytes, sizeof bytes, 0, &in)) return 1;
    return in.flow == DP_HUC_ILLEGAL;
}

// --- C3: constant transfer over the DpRegState lattice ----------------------
// The 6502 subset plus the bank window system: TAM/TMA move A into/out of an
// MPR register (the PCE's mapper state), CLX/CLY zero X/Y.  Same soundness
// rule as the 6502: unmodelled writes kill, nothing is guessed.
static void h_set(DpRegState *s, unsigned slot, uint8_t v) { s->v[slot] = v; s->known[slot] |= 1; }
static void h_kill(DpRegState *s, unsigned slot) { s->known[slot] = 0; s->v[slot] = 0; }
static void h_step(DpRegState *s, unsigned slot, int delta) {
    if (s->known[slot] & 1) h_set(s, slot, (uint8_t)(s->v[slot] + delta));
    else h_kill(s, slot);
}
static void isa_reg_effect(const uint8_t *b, uint32_t pc, const DpRegState *in, DpRegState *out, const DpProvRom *rom) {
    (void)pc; (void)rom;
    *out = *in;
    uint8_t op = b[0];
    switch (op) {
    case 0xA9: h_set(out, DP_HUCR_A, b[1]); return;
    case 0xA2: h_set(out, DP_HUCR_X, b[1]); return;
    case 0xA0: h_set(out, DP_HUCR_Y, b[1]); return;
    case 0xAA: if (out->known[DP_HUCR_A] & 1) h_set(out, DP_HUCR_X, (uint8_t)out->v[DP_HUCR_A]); else h_kill(out, DP_HUCR_X); return;
    case 0xA8: if (out->known[DP_HUCR_A] & 1) h_set(out, DP_HUCR_Y, (uint8_t)out->v[DP_HUCR_A]); else h_kill(out, DP_HUCR_Y); return;
    case 0x8A: if (out->known[DP_HUCR_X] & 1) h_set(out, DP_HUCR_A, (uint8_t)out->v[DP_HUCR_X]); else h_kill(out, DP_HUCR_A); return;
    case 0x98: if (out->known[DP_HUCR_Y] & 1) h_set(out, DP_HUCR_A, (uint8_t)out->v[DP_HUCR_Y]); else h_kill(out, DP_HUCR_A); return;
    case 0xBA: if (out->known[DP_HUCR_S] & 1) h_set(out, DP_HUCR_X, (uint8_t)out->v[DP_HUCR_S]); else h_kill(out, DP_HUCR_X); return;
    case 0x9A: if (out->known[DP_HUCR_X] & 1) h_set(out, DP_HUCR_S, (uint8_t)out->v[DP_HUCR_X]); else h_kill(out, DP_HUCR_S); return;
    case 0xE8: h_step(out, DP_HUCR_X, 1); return;
    case 0xCA: h_step(out, DP_HUCR_X, -1); return;
    case 0xC8: h_step(out, DP_HUCR_Y, 1); return;
    case 0x88: h_step(out, DP_HUCR_Y, -1); return;
    case 0x82: h_set(out, DP_HUCR_X, 0); return;   // clx
    case 0xC2: h_set(out, DP_HUCR_Y, 0); return;   // cly
    case 0x53 ... 0x5A:   // tam #n: MPR[n] <- A (the PCE's bank register)
        if (out->known[DP_HUCR_A] & 1) h_set(out, DP_HUCR_MPR0 + (op & 7), (uint8_t)out->v[DP_HUCR_A]);
        else h_kill(out, DP_HUCR_MPR0 + (op & 7));
        return;
    case 0x43 ... 0x4A:   // tma #n: A <- MPR[n]
        if (out->known[DP_HUCR_MPR0 + (op & 7)] & 1) h_set(out, DP_HUCR_A, (uint8_t)out->v[DP_HUCR_MPR0 + (op & 7)]);
        else h_kill(out, DP_HUCR_A);
        return;
    }
    unsigned write = 0;
    if (op == 0x68) write = 1u << DP_HUCR_A;                          // pla
    else if ((op & 0x1F) == 0x01) {
        unsigned grp = (op >> 5) & 7;
        if (grp == 5) write = 1u << DP_HUCR_A;
        else if (grp < 4 || grp == 7) write = 1u << DP_HUCR_A;
    }
    if ((op & 0x9F) == 0x0A && op != 0x8A) write = 1u << DP_HUCR_A;   // asl/lsr/rol/ror A
    if (op == 0xAE || op == 0xA6 || op == 0xBE || op == 0xB6) write = 1u << DP_HUCR_X;
    if (op == 0xAC || op == 0xA4 || op == 0xBC || op == 0xB4) write = 1u << DP_HUCR_Y;
    if (write) { h_kill(out, write == (1u << DP_HUCR_X) ? DP_HUCR_X : write == (1u << DP_HUCR_Y) ? DP_HUCR_Y : DP_HUCR_A); }
    else {
        static const uint16_t keeps[] = {   // the documented no-write space
            0xEA, 0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA,   // NOP family
            0x18, 0x38, 0x58, 0x78, 0xB8, 0xD8, 0xF8,   // CLC SEC CLI SEI CLV CLD SED
            0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0,   // branches
            0x4C, 0x6C, 0x7C, 0x20, 0x60, 0x40, 0x00,   // jmp (ind)/(ind,x) jsr rts rti brk
            0x24, 0x2C, 0xC0, 0xE0, 0xC4, 0xE4,         // bit cpx cpy
            0x81, 0x91, 0x85, 0x86, 0x84, 0x8C, 0x8E, 0x8D,   // stores
            0xC1, 0xC5, 0xCD, 0xD1, 0xD5, 0xD9, 0xDD,   // cmp
            0x73, 0xC3, 0xD3, 0xE3, 0xF3,               // TDD TII TIN TIA (block transfers write memory only)
        };
        unsigned keep = 0;
        for (size_t i = 0; i < sizeof keeps / sizeof *keeps; i++) if (keeps[i] == op) keep = 1;
        if (!keep) { h_kill(out, DP_HUCR_A); h_kill(out, DP_HUCR_X); h_kill(out, DP_HUCR_Y); h_kill(out, DP_HUCR_S);
                     for (unsigned m = 0; m < 8; m++) h_kill(out, DP_HUCR_MPR0 + m); }   // unmodelled: never guessed
    }
}

static int isa_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    DpHucIns in;
    uint8_t bytes[7] = { 0 };
    unsigned avail = rom_size > addr ? (unsigned)(rom_size - addr) : 0;
    if (!avail) return 0;
    if (avail > sizeof bytes) avail = sizeof bytes;
    memcpy(bytes, rom + addr, avail);
    if (!dp_huc_decode(bytes, sizeof bytes, addr, &in)) return 0;
    if ((unsigned)in.length > avail) return 0;
    dp_huc_format(&in, bytes, out, outsz);
    return in.length;
}

static int isa_mem_operand(const uint8_t *b, DpMemOp *o) {
    o->access = DP_MA_NONE; o->mode = DP_AM_NONE; o->pair = 0; o->post = 0; o->width = 0; o->addr = 0;
    DpHucIns in;
    if (!dp_huc_decode(b, 7, 0, &in)) return 0;
    switch (in.mode) {
    case DP_HUC_M_ZP: case DP_HUC_M_ZPX: case DP_HUC_M_ZPY:
        o->mode = DP_AM_ABS; o->addr = in.operand; o->width = 1; break;
    case DP_HUC_M_ABS: case DP_HUC_M_ABSX: case DP_HUC_M_ABSY: case DP_HUC_M_IND: case DP_HUC_M_INDX:
        o->mode = DP_AM_ABS; o->addr = in.operand; o->width = 1; break;
    case DP_HUC_M_IZX: case DP_HUC_M_IZY: case DP_HUC_M_IZP:
        o->mode = DP_AM_PAIR; o->pair = 2; o->width = 1; break;   // zero-page pointer: no static address
    case DP_HUC_M_IMP: case DP_HUC_M_IMM: case DP_HUC_M_REL: case DP_HUC_M_ZPREL:
    case DP_HUC_M_BLOCK: case DP_HUC_M_TST_ZP: case DP_HUC_M_TST_ABS:
    default:
        return 0;
    }
    // the read/write split comes from the opcode class the decoder knows;
    // conservative here: reads only, writes via the runtime observer
    o->access = DP_MA_READ;
    return 1;
}

static int isa_imm_byte(const uint8_t *b, uint8_t *v) {   // B9: lda/ldx/ldy #imm (a split pointer is loaded in two halves)
    if (b[0] != 0xA9 && b[0] != 0xA2 && b[0] != 0xA0) return 0;
    *v = b[1]; return 1;
}

static int isa_acc_imm(const uint8_t *b, uint8_t *v) { if (b[0] != 0xA9) return 0; *v = b[1]; return 1; }   // lda #n
static int isa_st_acc_abs(const uint8_t *b, uint32_t *a) { if (b[0] != 0x8D) return 0; *a = (uint32_t)b[1] | (uint32_t)b[2] << 8; return 1; }   // sta abs
// M1: direct RAM loads into a register — `lda/ldx/ldy abs` and `abs,X` / `abs,Y`, `zp` and `zp,X` / `zp,Y`
// (techniques 3, 4, 7).  Indexed modes set *idx_reg: the kernel skips the immediate-compare signature then.
static int isa_ram_load(const uint8_t *b, uint32_t *addr, int *idx_reg, int *width, int *dreg) {
    (void)width; (void)dreg;
    if (b[0] == 0xAD || b[0] == 0xAE || b[0] == 0xAC) { *addr = b[1] | (b[2] << 8); *idx_reg = b[0] != 0xAD; return 3; }
    if (b[0] == 0xA5 || b[0] == 0xA6 || b[0] == 0xA4) { *addr = b[1]; *idx_reg = b[0] != 0xA5; return 2; }
    return 0;
}
// M1: proven length (in entries) of the word table whose base `lda abs,X` / `lda abs,Y` (a ROM base)
// follows the load; an entry is proven while its word designates the ROM at or above $8000
// (the machine's ROM window — this ISA's code floor on every profile using tables)
static unsigned isa_table_proven(const DpIsa *isa, DpMachine *m, uint32_t pc, int len) {
    (void)isa;
    for (unsigned i = 0; i < 13; i++) {
        uint8_t op = m->fetch(m, pc + len + i);
        if (op != 0xBD && op != 0xB9) continue;
        uint32_t base = m->fetch(m, pc + len + i + 1) | (uint32_t)m->fetch(m, pc + len + i + 2) << 8;
        if (m->rom_index(m, base) < 0) continue;
        unsigned n = 0;
        while (n < 128) {
            uint32_t t = m->fetch(m, base + 2 * n) | (uint32_t)m->fetch(m, base + 2 * n + 1) << 8;
            if (t < 0x8000u || m->rom_index(m, t) < 0) break;
            n++;
        }
        return n >= 2 ? n : 0;
    }
    return 0;
}
// M1: a ROM table access indexed by the loaded value follows within 16 bytes:
// abs,X / abs,Y load/ALU from a ROM base address
static int isa_rom_table_follows(const DpIsa *isa, DpMachine *m, uint32_t pc, int len) {
    (void)isa;
    uint8_t win[16];
    for (unsigned i = 0; i < sizeof win; i++) win[i] = m->fetch(m, pc + len + i);
    for (unsigned i = 0; i + 2 < sizeof win; i++) {
        uint8_t o = win[i];
        if ((o == 0xBD || o == 0xB9 || o == 0x7D || o == 0x79 || o == 0x3D || o == 0x39 || o == 0xDD || o == 0xD9) &&
            m->rom_index(m, win[i + 1] | (win[i + 2] << 8)) >= 0) return 1;
    }
    return 0;
}
static const DpIsa isa = { "huc6280", 2, 0, isa_length, isa_flow, isa_cond_of, isa_cond_flag,
                           isa_reg_effect, isa_illegal, isa_disasm, isa_mem_operand, 0, 0, 1, NULL, isa_imm_byte, isa_acc_imm, isa_st_acc_abs,
                           .ram_load = isa_ram_load, .cmp_imm = 0xC9, .table_proven = isa_table_proven, .rom_table_follows = isa_rom_table_follows };
const DpIsa *dp_isa_huc6280(void) { return &isa; }
