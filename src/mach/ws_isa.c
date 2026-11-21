// ws_isa.c: the V30MZ as a DpIsa (spec §7.3).  Wraps the static decoder
// (src/cpu/v30mz); the constant-transfer function models the immediate
// loads, the register-register copies and the stack arithmetic — everything
// else kills all registers, never guesses (the huc6280 rule).
#include "../core/dpmachine.h"
#include "../cpu/v30mz/v30mz.h"
#include <string.h>

static void kill_all(DpRegState *out) {
    memset(out->known, 0, sizeof out->known);
}
static void step_reg(DpRegState *s, unsigned slot, unsigned bytes, int32_t delta) {
    if (slot >= DP_REG_N || bytes > DP_REG_MAX_BYTES) return;
    uint8_t mask = (uint8_t)((1u << bytes) - 1);
    if ((s->known[slot] & mask) == mask) {
        s->v[slot] = (uint32_t)((int64_t)s->v[slot] + delta);
        s->known[slot] |= mask;
    } else {
        s->known[slot] &= (uint8_t)~mask & 0x0F;
    }
}
static void set_reg(DpRegState *s, unsigned slot, unsigned bytes, uint32_t v) {
    if (slot >= DP_REG_N) return;
    s->v[slot] = v;
    s->known[slot] |= (uint8_t)((1u << bytes) - 1);
}

static int isa_length(uint8_t op) { return dp_v30mz_length(op); }

static int isa_flow(const uint8_t *bytes, uint32_t pc, DpFlowInfo *o) {
    DpV30Insn r;
    memset(o, 0, sizeof *o);
    unsigned len = dp_v30mz_decode(bytes, 15, pc, &r);
    if (!len) return 0;
    o->length = (uint8_t)len;
    o->cond = r.cond;
    o->pair = (uint8_t)DP_REG_PAIR_NONE;
    switch (r.cls) {
        case DP_V30_SEQ: o->kind = DP_FLOW_SEQ; o->fall = 1; break;
        case DP_V30_JCC: o->kind = DP_FLOW_COND; o->fall = 1; o->target = (int32_t)r.target; break;
        case DP_V30_JMP: o->kind = DP_FLOW_JUMP; o->target = (int32_t)r.target; break;
        case DP_V30_CALL: o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->target = (int32_t)r.target; break;
        case DP_V30_RET: o->kind = DP_FLOW_RET; break;
        case DP_V30_INDIRECT: o->kind = DP_FLOW_INDIRECT; break;
        case DP_V30_INT:
            // the vector cell sits at (vector*4) in segment 0; the handler
            // address is read from it (the NES/PCE convention)
            o->kind = DP_FLOW_CALL; o->call = 1; o->fall = 1; o->rst = 1; o->target = -1;
            break;
        case DP_V30_HALT: o->kind = DP_FLOW_SEQ; o->halt = 1; break;
        default: o->kind = DP_FLOW_ILLEGAL; o->fall = 1; break;
    }
    return (int)len;
}

static int isa_cond_of(uint8_t op) {
    return (op >= 0x70 && op <= 0x7f) ? (int)(op & 15) : -1;
}

/* the primary flag each condition tests; the runtime's flag bits are
 * CF=1 PF=4 AF=16 ZF=64 SF=128 OF=2048 — the polarity rides the low bit */
static uint8_t isa_cond_flag(int cond) {
    static const uint16_t flag[16] = {
        0x800, 0x800, 0x01, 0x01, 0x40, 0x40, 0x41, 0x41,   // O NO B NB Z NZ BE A
        0x80, 0x80, 0x04, 0x04, 0x880, 0x880, 0x8C0, 0x8C0, // S NS P NP L NL LE G
    };
    if (cond < 0 || cond > 15) return 0;
    uint16_t f = flag[cond];
    if (!f) return 0;
    return (uint8_t)((f & ~1u) | (cond & 1));
}

/* C3: the constant-transfer function.  Modelled: the immediate loads
 * (b0-bf), the register-register mov/xchg (88-8b, 86/87 register forms),
 * the stack moves (push/pop r16, call/ret) and the register inc/dec.
 * Everything else kills all registers. */
static void isa_reg_effect(const uint8_t *bytes, uint32_t pc, const DpRegState *in, DpRegState *out,
                           const DpProvRom *rom) {
    (void)pc; (void)rom;
    *out = *in;
    DpV30Insn r;
    if (!dp_v30mz_decode(bytes, 15, 0, &r)) { kill_all(out); return; }
    uint8_t op = r.op_byte;

    /* the immediate loads */
    if (op >= 0xb0 && op <= 0xb7) {
        unsigned reg = op - 0xb0, shift = (reg & 4) * 8, slot = DP_V30R_AX + (reg & 3);
        out->v[slot] = (out->v[slot] & ~(0xFFu << shift)) | ((uint32_t)bytes[1] << shift);
        out->known[slot] |= (uint8_t)(1u << (shift / 8));
        return;
    }
    if (op >= 0xb8 && op <= 0xbf) { set_reg(out, DP_V30R_AX + (op & 7), 2, bytes[1] | bytes[2] << 8); return; }
    if (op == 0x68) { step_reg(out, DP_V30R_SP, 2, -2); return; }
    if (op == 0x6a) { step_reg(out, DP_V30R_SP, 2, -2); return; }
    if (op >= 0x50 && op <= 0x57) { step_reg(out, DP_V30R_SP, 2, -2); return; }
    if (op >= 0x58 && op <= 0x5f) {
        memset(out->known, 0, sizeof out->known);      // the popped register is memory data
        out->known[DP_V30R_AX + (op & 7)] = 0;
        step_reg(out, DP_V30R_SP, 2, 2);
        return;
    }
    if (op >= 0x40 && op <= 0x4f) {
        unsigned bytes = op & 8 ? 2 : 1;               // dec r16 kills; inc r16 keeps CF but the value changes
        out->known[DP_V30R_AX + (op & 7)] = 0;
        (void)bytes;
        return;
    }
    if (op == 0xe8) { step_reg(out, DP_V30R_SP, 2, -2); return; }
    if (op == 0xc2 || op == 0xc3) { step_reg(out, DP_V30R_SP, 2, 2); return; }
    if (op == 0x9a) { step_reg(out, DP_V30R_SP, 2, -4); return; }
    if (op == 0xca || op == 0xcb || op == 0xcf) { step_reg(out, DP_V30R_SP, 2, op == 0xcf ? 6 : 4); return; }
    if (op == 0xc9) { step_reg(out, DP_V30R_SP, 2, 2); kill_all(out); return; }   // leave: SP = BP (unknown in general)
    if (op == 0x60) { step_reg(out, DP_V30R_SP, 2, -16); return; }
    if (op == 0x61) { kill_all(out); step_reg(out, DP_V30R_SP, 2, 16); return; }

    /* the register-register copies: the word forms copy their whole slot,
     * the byte forms kill the destination conservatively */
    if (r.has_modrm && r.mod == 3 && (op == 0x89 || op == 0x8b)) {
        unsigned dst = (op & 2) ? DP_V30R_AX + r.reg : DP_V30R_AX + r.rm;
        unsigned src = (op & 2) ? DP_V30R_AX + r.rm : DP_V30R_AX + r.reg;
        out->v[dst] = (out->v[dst] & 0xFFFF0000u) | (in->v[src] & 0xFFFFu);
        out->known[dst] = (uint8_t)((out->known[dst] & ~3) | (in->known[src] & 3));
        return;
    }
    if (r.has_modrm && r.mod == 3 && op >= 0x88 && op <= 0x8a) {
        unsigned dst = (op == 0x8a) ? DP_V30R_AX + r.reg : DP_V30R_AX + r.rm;
        out->known[dst] = 0;
        return;
    }
    if (r.has_modrm && r.mod == 3 && op == 0x87) {
        unsigned a = DP_V30R_AX + r.reg, bslot = DP_V30R_AX + r.rm;
        uint32_t t = out->v[a]; out->v[a] = out->v[bslot]; out->v[bslot] = t;
        uint8_t k = out->known[a]; out->known[a] = out->known[bslot]; out->known[bslot] = k;
        return;
    }
    kill_all(out);
}

static int isa_illegal(uint8_t op) {
    return op == 0x0f || (op >= 0x63 && op <= 0x67) || op == 0xf1;
}

static int isa_disasm(const uint8_t *rom, uint32_t rom_size, uint16_t addr, char *out, size_t outsz) {
    uint32_t at = addr;
    if (at >= rom_size) return 0;
    return dp_v30mz_disasm(rom + at, rom_size - at, at, out, (unsigned)outsz);
}

/* the data operand: only the absolute forms are reported (mov al/ax <->moffs,
 * the mod 0 rm 6 ModRM forms, the mod 3 nothing); the register-indirect
 * forms degrade (the kernel records them from the runtime observers) */
static int isa_mem_operand(const uint8_t *bytes, DpMemOp *out) {
    memset(out, 0, sizeof *out);
    DpV30Insn r;
    if (!dp_v30mz_decode(bytes, 15, 0, &r)) return 0;
    uint8_t op = r.op_byte;
    unsigned access = 0, width = 0;
    uint32_t addr = 0;
    int abs = 0;
    if (op >= 0xa0 && op <= 0xa3) {
        abs = 1; addr = bytes[1] | bytes[2] << 8;
        width = (op & 1) ? 2 : 1;
        access = (op & 2) ? DP_MA_WRITE : DP_MA_READ;
    } else if (r.has_modrm && r.mod == 0 && r.rm == 6) {
        abs = 1; addr = bytes[2] | bytes[3] << 8;
        if (op <= 0x3d && (op & 7) <= 3) {
            width = (op & 1) ? 2 : 1;
            access = ((op >> 3) & 7) == 7 ? DP_MA_READ :        // cmp
                     ((op & 2) ? DP_MA_READ : DP_MA_RW);
        } else if (op >= 0x88 && op <= 0x8b) {
            width = (op & 1) ? 2 : 1;
            access = (op & 2) ? DP_MA_READ : DP_MA_WRITE;
        } else if (op == 0xc6 || op == 0xc7) {
            width = (op == 0xc7) ? 2 : 1;
            access = DP_MA_WRITE;
        } else if (op == 0x80 || op == 0x81 || op == 0x82 || op == 0x83) {
            width = (op == 0x81) ? 2 : 1;
            access = ((r.reg & 7) == 7) ? DP_MA_READ : DP_MA_RW;
        } else if (op == 0x84 || op == 0x85 || op == 0xf6 || op == 0xf7) {
            width = (op & 1) ? 2 : 1;
            access = DP_MA_READ;
        } else if (op == 0xfe || op == 0xff) {
            width = (op & 1) ? 2 : 1;
            access = (r.reg <= 1) ? DP_MA_RW : r.reg == 6 ? DP_MA_READ : DP_MA_READ;
        } else if (op == 0xd0 || op == 0xd1 || op == 0xd2 || op == 0xd3 || op == 0xc0 || op == 0xc1) {
            width = (op & 1) ? 2 : 1;
            access = DP_MA_RW;
        } else if (op == 0x86 || op == 0x87 || op == 0x8f) {
            width = (op & 1) ? 2 : 1;
            access = DP_MA_RW;
        } else if (op == 0xc4 || op == 0xc5) {
            width = 4;
            access = DP_MA_READ;
        } else if (op == 0x62) {
            width = 4;
            access = DP_MA_READ;
        } else {
            abs = 0;
        }
    }
    if (!abs) return 0;
    out->access = (uint8_t)access;
    out->width = (uint8_t)width;
    out->mode = DP_AM_ABS;
    out->addr = addr;
    return 1;
}

/* B9: a full 16-bit pointer immediate (mov r16,imm16) */
static int isa_imm_ptr(const uint8_t *bytes, uint32_t *imm) {
    if (bytes[0] >= 0xb8 && bytes[0] <= 0xbf) {
        *imm = (uint32_t)(bytes[1] | bytes[2] << 8);
        return 1;
    }
    return 0;
}
/* B9: an 8-bit immediate load (mov r8,imm8) */
static int isa_imm_byte(const uint8_t *bytes, uint8_t *v) {
    if (bytes[0] >= 0xb0 && bytes[0] <= 0xb7) { *v = bytes[1]; return 1; }
    return 0;
}
/* C3: mov al,imm8 */
static int isa_acc_imm(const uint8_t *bytes, uint8_t *v) {
    if (bytes[0] == 0xb0) { *v = bytes[1]; return 1; }
    return 0;
}
/* C3: mov [moffs],al and mov r/m8,imm8 in its absolute form */
static int isa_st_acc_abs(const uint8_t *bytes, uint32_t *addr) {
    if (bytes[0] == 0xa2) { *addr = bytes[1] | bytes[2] << 8; return 1; }
    if (bytes[0] == 0xc6 && bytes[1] == 0x06) { *addr = bytes[2] | bytes[3] << 8; return 1; }
    return 0;
}

/* "zero data marked code": runs of nop padding or erased 0xff never head
 * real code */
static int isa_entry_data_sig(const uint8_t *w, unsigned n) {
    if (n < 4) return 0;
    int all_nop = 1, all_ff = 1;
    for (unsigned i = 0; i < n; i++) { all_nop &= w[i] == 0x90; all_ff &= w[i] == 0xff; }
    return all_nop || all_ff;
}

/* C3 concrete-stack returns: ret/c3 (2), ret n/c2 (2), retf/cb (4),
 * retf n/ca (4), iret/cf (6, the PC sits at the SP) */
static int isa_sp_ret(const uint8_t *bytes, unsigned *width, unsigned *bias, unsigned *off) {
    switch (bytes[0]) {
        case 0xc3: *width = 2; *bias = 0; *off = 0; return 1;
        case 0xc2: *width = 2; *bias = 0; *off = 0; return 1;
        case 0xcb: *width = 4; *bias = 0; *off = 0; return 1;
        case 0xca: *width = 4; *bias = 0; *off = 0; return 1;
        case 0xcf: *width = 6; *bias = 0; *off = 0; return 1;
        default: return 0;
    }
}

/* the OUT forms: e6/e7 name the port immediately, ee/ef ride DX */
static int isa_out_port(const uint8_t *bytes, int *port_known, uint16_t *port) {
    if (bytes[0] == 0xe6 || bytes[0] == 0xe7) {
        *port_known = 1; *port = bytes[1]; return 1;
    }
    if (bytes[0] == 0xee || bytes[0] == 0xef) {
        *port_known = 0; *port = 0; return 1;
    }
    return 0;
}

/* M1: a direct RAM load — mov al/ax,[moffs16] (a0/a1) */
static int isa_ram_load(const uint8_t *bytes, uint32_t *addr, int *idx_reg, int *width, int *dreg) {
    if (bytes[0] == 0xa0 || bytes[0] == 0xa1) {
        *addr = bytes[1] | bytes[2] << 8;
        *idx_reg = 0;
        *width = bytes[0] == 0xa1 ? 2 : 1;
        *dreg = DP_V30R_AX;
        return bytes[0] == 0xa1 ? 3 : 2;
    }
    return 0;
}

static const DpIsa isa = {
    "v30mz", 2, 0,
    isa_length, isa_flow, isa_cond_of, isa_cond_flag, isa_reg_effect, isa_illegal, isa_disasm,
    isa_mem_operand,
    0x0000,        // code_min: code lives at any linear address (RAM or ROM)
    0, 0,          // hl_jp_table, abs_ind_table
    isa_imm_ptr, isa_imm_byte, isa_acc_imm, isa_st_acc_abs,
    DP_V30R_SP, 2, 0xFFFF, 0,   // the SP slot, 16-bit flat stack
    isa_entry_data_sig, isa_sp_ret,
    isa_out_port,
    isa_ram_load, 0x3c, NULL,   // cmp al,imm8 pairs the a0 load
    NULL, NULL,                 // table_proven, rom_table_follows
    2,             // table_entry_width: word tables
    NULL,          // table_dispatch
    NULL, NULL,    // code_table, code_ptr_store
};
const DpIsa *dp_isa_v30mz(void) { return &isa; }
