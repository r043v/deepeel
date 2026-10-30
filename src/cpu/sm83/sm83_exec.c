// sm83_exec.c: complete SM83 semantics with per-access effect recording.
// Every memory touch goes through the machine (traced), every control-flow
// outcome is reported through the step trace, so the explorer can fork on
// conditionals and follow indirect jumps.  Flag semantics follow the gbl
// runtime interpreter (targets/meta/runtime/gbl_cpu.c in m0boy).
#include "sm83.h"

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

static uint8_t get_r(MsMachine *m, int idx) {
    switch (idx) {
    case 0: return m->b;
    case 1: return m->c;
    case 2: return m->d;
    case 3: return m->e;
    case 4: return m->h;
    case 5: return m->l;
    case 6: return ms_read(m, r_hl(m), MS_ACC_DATA);
    default: return m->a;
    }
}

static void set_r(MsMachine *m, int idx, uint8_t v) {
    switch (idx) {
    case 0: m->b = v; break;
    case 1: m->c = v; break;
    case 2: m->d = v; break;
    case 3: m->e = v; break;
    case 4: m->h = v; break;
    case 5: m->l = v; break;
    case 6: ms_write(m, r_hl(m), v, MS_ACC_DATA); break;
    default: m->a = v; break;
    }
}

static uint16_t get_rr(MsMachine *m, int idx) {
    switch (idx) {
    case 0: return r_bc(m);
    case 1: return r_de(m);
    case 2: return r_hl(m);
    default: return m->sp;
    }
}

static void set_rr(MsMachine *m, int idx, uint16_t v) {
    switch (idx) {
    case 0: m->b = v >> 8; m->c = v & 0xFF; break;
    case 1: m->d = v >> 8; m->e = v & 0xFF; break;
    case 2: m->h = v >> 8; m->l = v & 0xFF; break;
    default: m->sp = v; break;
    }
}

static void step_done(MsMachine *m, uint16_t pc0, uint8_t len, MsCtrl ctrl, uint16_t target, uint8_t taken) {
    if (!m->trace || !m->trace->step) return;
    MsStepInfo si = { pc0, len, ms_peek(m, pc0), ctrl, target, taken };
    m->trace->step(m->trace_ctx, &si);
}

// ALU family on A: 0 ADD 1 ADC 2 SUB 3 SBC 4 AND 5 XOR 6 OR 7 CP
static void alu(MsMachine *m, int op, uint8_t r) {
    uint8_t a = m->a;
    switch (op) {
    case 0: {
        int res = a + r;
        m->f = (uint8_t)(((res & 0xFF) ? 0 : FZ) | ((((a & 0xF) + (r & 0xF)) > 0xF) ? FH : 0) | ((res > 0xFF) ? FC : 0));
        m->a = (uint8_t)res;
        break;
    }
    case 1: {
        int c = (m->f & FC) ? 1 : 0;
        int res = a + r + c;
        m->f = (uint8_t)(((res & 0xFF) ? 0 : FZ) | ((((a & 0xF) + (r & 0xF) + c) > 0xF) ? FH : 0) | ((res > 0xFF) ? FC : 0));
        m->a = (uint8_t)res;
        break;
    }
    case 2: {
        int res = a - r;
        m->f = (uint8_t)(FN | ((res & 0xFF) ? 0 : FZ) | (((a & 0xF) < (r & 0xF)) ? FH : 0) | ((res < 0) ? FC : 0));
        m->a = (uint8_t)res;
        break;
    }
    case 3: {
        int c = (m->f & FC) ? 1 : 0;
        int res = a - r - c;
        m->f = (uint8_t)(FN | ((res & 0xFF) ? 0 : FZ) | ((((a & 0xF) - (r & 0xF) - c) < 0) ? FH : 0) | ((res < 0) ? FC : 0));
        m->a = (uint8_t)res;
        break;
    }
    case 4: m->a = a & r; m->f = (uint8_t)(FH | (m->a ? 0 : FZ)); break;
    case 5: m->a = a ^ r; m->f = (uint8_t)(m->a ? 0 : FZ); break;
    case 6: m->a = a | r; m->f = (uint8_t)(m->a ? 0 : FZ); break;
    default:
        m->f = (uint8_t)(FN | ((((a - r) & 0xFF) ? 0 : FZ)) | (((a & 0xF) < (r & 0xF)) ? FH : 0) | ((a < r) ? FC : 0));
        break;
    }
}

static void inc_r(MsMachine *m, int idx) {
    uint8_t r = get_r(m, idx);
    uint8_t res = (uint8_t)(r + 1);
    m->f = (uint8_t)((m->f & FC) | (res ? 0 : FZ) | (((r & 0xF) == 0xF) ? FH : 0));
    set_r(m, idx, res);
}

static void dec_r(MsMachine *m, int idx) {
    uint8_t r = get_r(m, idx);
    uint8_t res = (uint8_t)(r - 1);
    m->f = (uint8_t)((m->f & FC) | FN | (res ? 0 : FZ) | (((r & 0xF) == 0) ? FH : 0));
    set_r(m, idx, res);
}

static void add_hl(MsMachine *m, uint16_t rr) {
    uint16_t hl = r_hl(m);
    uint32_t res = hl + rr;
    m->f = (uint8_t)((m->f & FZ) | ((((hl & 0xFFF) + (rr & 0xFFF)) > 0xFFF) ? FH : 0) | ((res > 0xFFFF) ? FC : 0));
    m->h = (uint8_t)(res >> 8);
    m->l = (uint8_t)res;
}

static int cond_ok(MsMachine *m, int y) {
    switch (y) {
    case 0: return !(m->f & FZ);
    case 1: return !!(m->f & FZ);
    case 2: return !(m->f & FC);
    default: return !!(m->f & FC);
    }
}

static void exec_cb(MsMachine *m, uint8_t cb) {
    int x = cb >> 6, y = (cb >> 3) & 7, z = cb & 7;
    uint8_t r = get_r(m, z);
    uint8_t res = r, c;
    switch (x) {
    case 0:
        switch (y) {
        case 0: c = r >> 7; res = (uint8_t)((r << 1) | c); m->f = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0)); break;
        case 1: c = r & 1; res = (uint8_t)((r >> 1) | (c << 7)); m->f = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0)); break;
        case 2: c = r >> 7; res = (uint8_t)((r << 1) | ((m->f & FC) ? 1 : 0)); m->f = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0)); break;
        case 3: c = r & 1; res = (uint8_t)((r >> 1) | ((m->f & FC) ? 0x80 : 0)); m->f = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0)); break;
        case 4: c = r >> 7; res = (uint8_t)(r << 1); m->f = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0)); break;
        case 5: c = r & 1; res = (uint8_t)((r >> 1) | (r & 0x80)); m->f = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0)); break;
        case 6: res = (uint8_t)((r >> 4) | (r << 4)); m->f = (uint8_t)(res ? 0 : FZ); break;
        default: c = r & 1; res = (uint8_t)(r >> 1); m->f = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0)); break;
        }
        set_r(m, z, res);
        break;
    case 1:
        m->f = (uint8_t)((m->f & FC) | FH | (((r >> y) & 1) ? 0 : FZ));
        break;
    case 2:
        set_r(m, z, (uint8_t)(r & ~(1 << y)));
        break;
    default:
        set_r(m, z, (uint8_t)(r | (1 << y)));
        break;
    }
}

static void push16(MsMachine *m, uint16_t v) {
    m->sp = (uint16_t)(m->sp - 2);
    ms_write(m, m->sp, (uint8_t)v, MS_ACC_STACK); // low first: the stack is little-endian
    ms_write(m, (uint16_t)(m->sp + 1), (uint8_t)(v >> 8), MS_ACC_STACK);
}

static uint16_t pop16(MsMachine *m) {
    uint16_t lo = ms_read(m, m->sp, MS_ACC_STACK);
    uint16_t hi = ms_read(m, (uint16_t)(m->sp + 1), MS_ACC_STACK);
    m->sp = (uint16_t)(m->sp + 2);
    return lo | (hi << 8);
}

static void hl_inc(MsMachine *m) { if (++m->l == 0) ++m->h; }
static void hl_dec(MsMachine *m) { if (m->l-- == 0) --m->h; }

int sm83_step(MsMachine *m) {
    uint16_t pc0 = m->pc;
    if (m->ime_delay) { m->ime = 1; m->ime_delay = 0; }
    uint8_t op = ms_fetch(m, m->pc);
    m->pc = (uint16_t)(m->pc + 1);
    int bad = 0;

    switch (op) {
    case 0x00: break; // NOP
    case 0x01: case 0x11: case 0x21: case 0x31:
        set_rr(m, (op >> 4) & 3, ms_fetch16(m, m->pc));
        m->pc = (uint16_t)(m->pc + 2);
        break;
    case 0x02: ms_write(m, r_bc(m), m->a, MS_ACC_DATA); break;
    case 0x12: ms_write(m, r_de(m), m->a, MS_ACC_DATA); break;
    case 0x22: ms_write(m, r_hl(m), m->a, MS_ACC_DATA); hl_inc(m); break;
    case 0x32: ms_write(m, r_hl(m), m->a, MS_ACC_DATA); hl_dec(m); break;
    case 0x03: case 0x13: case 0x23: case 0x33: set_rr(m, (op >> 4) & 3, (uint16_t)(get_rr(m, (op >> 4) & 3) + 1)); break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: set_rr(m, (op >> 4) & 3, (uint16_t)(get_rr(m, (op >> 4) & 3) - 1)); break;
    case 0x07: { uint8_t c = m->a >> 7; m->a = (uint8_t)((m->a << 1) | c); m->f = (uint8_t)(c ? FC : 0); break; }
    case 0x0F: { uint8_t c = m->a & 1; m->a = (uint8_t)((m->a >> 1) | (c << 7)); m->f = (uint8_t)(c ? FC : 0); break; }
    case 0x17: { uint8_t c = m->a >> 7; m->a = (uint8_t)((m->a << 1) | ((m->f & FC) ? 1 : 0)); m->f = (uint8_t)(c ? FC : 0); break; }
    case 0x1F: { uint8_t c = m->a & 1; m->a = (uint8_t)((m->a >> 1) | ((m->f & FC) ? 0x80 : 0)); m->f = (uint8_t)(c ? FC : 0); break; }
    case 0x08: { uint16_t nn = ms_fetch16(m, m->pc); m->pc = (uint16_t)(m->pc + 2); ms_write16(m, nn, m->sp, MS_ACC_DATA); break; }
    case 0x09: case 0x19: case 0x29: case 0x39: add_hl(m, get_rr(m, (op >> 4) & 3)); break;
    case 0x0A: m->a = ms_read(m, r_bc(m), MS_ACC_DATA); break;
    case 0x1A: m->a = ms_read(m, r_de(m), MS_ACC_DATA); break;
    case 0x2A: m->a = ms_read(m, r_hl(m), MS_ACC_DATA); hl_inc(m); break;
    case 0x3A: m->a = ms_read(m, r_hl(m), MS_ACC_DATA); hl_dec(m); break;
    case 0x10:
        m->halted = 1;
        step_done(m, pc0, 2, MS_CTRL_STOP, 0, 0);
        return 1;
    case 0x18: {
        int8_t e = (int8_t)ms_fetch(m, m->pc);
        m->pc = (uint16_t)(m->pc + 1 + e);
        step_done(m, pc0, 2, MS_CTRL_JR, m->pc, 1);
        return 0;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t e = (int8_t)ms_fetch(m, m->pc);
        m->pc = (uint16_t)(m->pc + 1);
        int taken = cond_ok(m, (op >> 3) & 3);
        step_done(m, pc0, 2, MS_CTRL_JR_CC, (uint16_t)(m->pc + e), (uint8_t)taken);
        if (taken) m->pc = (uint16_t)(m->pc + e);
        return 0;
    }
    case 0x27: {
        int a = m->a;
        if (!(m->f & FN)) {
            if ((m->f & FC) || a > 0x99) { a += 0x60; m->f |= FC; }
            if ((m->f & FH) || (a & 0x0F) > 0x09) a += 0x06;
        } else {
            if (m->f & FC) a -= 0x60;
            if (m->f & FH) a -= 0x06;
        }
        m->f &= (uint8_t)~(FZ | FH);
        if (!(a & 0xFF)) m->f |= FZ;
        m->a = (uint8_t)a;
        break;
    }
    case 0x2F: m->a = (uint8_t)~m->a; m->f |= FN | FH; break;
    case 0x37: m->f = (uint8_t)((m->f & FZ) | FC); break;
    case 0x3F: m->f = (uint8_t)((m->f & FZ) | ((m->f & FC) ? 0 : FC)); break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        inc_r(m, (op >> 3) & 7);
        break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        dec_r(m, (op >> 3) & 7);
        break;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
        set_r(m, (op >> 3) & 7, ms_fetch(m, m->pc++));
        break;
    case 0x40 ... 0x7F:
        if (op == 0x76) {
            m->halted = 1;
            step_done(m, pc0, 1, MS_CTRL_HALT, 0, 0);
            return 1;
        }
        set_r(m, (op >> 3) & 7, get_r(m, op & 7));
        break;
    case 0x80 ... 0xBF:
        alu(m, (op >> 3) & 7, get_r(m, op & 7));
        break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: {
        int taken = cond_ok(m, (op >> 3) & 3);
        step_done(m, pc0, 1, MS_CTRL_RET_CC, 0, (uint8_t)taken);
        if (taken) m->pc = pop16(m);
        return 0;
    }
    case 0xC1: case 0xD1: case 0xE1: case 0xF1: {
        uint16_t v = pop16(m);
        if (op == 0xF1) { m->a = v >> 8; m->f = v & 0xF0; }
        else set_rr(m, (op >> 4) & 3, v);
        break;
    }
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        uint16_t nn = ms_fetch16(m, m->pc);
        m->pc = (uint16_t)(m->pc + 2);
        int taken = cond_ok(m, (op >> 3) & 3);
        step_done(m, pc0, 3, MS_CTRL_JP_CC, nn, (uint8_t)taken);
        if (taken) m->pc = nn;
        return 0;
    }
    case 0xC3: {
        uint16_t nn = ms_fetch16(m, m->pc);
        m->pc = nn;
        step_done(m, pc0, 3, MS_CTRL_JP, nn, 1);
        return 0;
    }
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        uint16_t nn = ms_fetch16(m, m->pc);
        m->pc = (uint16_t)(m->pc + 2);
        int taken = cond_ok(m, (op >> 3) & 3);
        step_done(m, pc0, 3, MS_CTRL_CALL_CC, nn, (uint8_t)taken);
        if (taken) {
            push16(m, m->pc);
            m->pc = nn;
        }
        return 0;
    }
    case 0xC5: case 0xD5: case 0xE5: case 0xF5: {
        uint16_t v = (op == 0xF5) ? (uint16_t)((m->a << 8) | (m->f & 0xF0)) : get_rr(m, (op >> 4) & 3);
        push16(m, v);
        break;
    }
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu(m, (op >> 3) & 7, ms_fetch(m, m->pc++));
        break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF: {
        uint16_t vec = (uint16_t)((op >> 3) & 7) * 8;
        push16(m, m->pc);
        m->pc = vec;
        step_done(m, pc0, 1, MS_CTRL_RST, vec, 1);
        return 0;
    }
    case 0xC9:
        m->pc = pop16(m);
        step_done(m, pc0, 1, MS_CTRL_RET, m->pc, 1);
        return 0;
    case 0xD9:
        m->pc = pop16(m);
        m->ime = 1;
        m->ime_delay = 0;
        step_done(m, pc0, 1, MS_CTRL_RETI, m->pc, 1);
        return 0;
    case 0xCB: {
        uint8_t cb = ms_fetch(m, m->pc);
        m->pc = (uint16_t)(m->pc + 1);
        exec_cb(m, cb);
        break;
    }
    case 0xCD: {
        uint16_t nn = ms_fetch16(m, m->pc);
        m->pc = (uint16_t)(m->pc + 2);
        push16(m, m->pc);
        m->pc = nn;
        step_done(m, pc0, 3, MS_CTRL_CALL, nn, 1);
        return 0;
    }
    case 0xE0: { uint8_t n = ms_fetch(m, m->pc++); ms_write(m, (uint16_t)(0xFF00 + n), m->a, MS_ACC_IO); break; }
    case 0xE2: ms_write(m, (uint16_t)(0xFF00 + m->c), m->a, MS_ACC_IO); break;
    case 0xE8: {
        int8_t e = (int8_t)ms_fetch(m, m->pc++);
        int res = m->sp + e;
        m->f = (uint8_t)((((((m->sp & 0xF) + (e & 0xF)) > 0xF) ? FH : 0) |
                          ((((m->sp & 0xFF) + (e & 0xFF)) > 0xFF) ? FC : 0)));
        m->sp = (uint16_t)res;
        break;
    }
    case 0xE9:
        m->pc = r_hl(m);
        step_done(m, pc0, 1, MS_CTRL_JP_HL, m->pc, 1);
        return 0;
    case 0xEA: { uint16_t nn = ms_fetch16(m, m->pc); m->pc = (uint16_t)(m->pc + 2); ms_write(m, nn, m->a, MS_ACC_DATA); break; }
    case 0xF0: { uint8_t n = ms_fetch(m, m->pc++); m->a = ms_read(m, (uint16_t)(0xFF00 + n), MS_ACC_IO); break; }
    case 0xF2: m->a = ms_read(m, (uint16_t)(0xFF00 + m->c), MS_ACC_IO); break;
    case 0xF8: {
        int8_t e = (int8_t)ms_fetch(m, m->pc++);
        int res = m->sp + e;
        m->f = (uint8_t)((((((m->sp & 0xF) + (e & 0xF)) > 0xF) ? FH : 0) |
                          ((((m->sp & 0xFF) + (e & 0xFF)) > 0xFF) ? FC : 0)));
        m->h = (uint8_t)(res >> 8);
        m->l = (uint8_t)res;
        break;
    }
    case 0xF9: m->sp = r_hl(m); break;
    case 0xFA: { uint16_t nn = ms_fetch16(m, m->pc); m->pc = (uint16_t)(m->pc + 2); m->a = ms_read(m, nn, MS_ACC_DATA); break; }
    case 0xFB: m->ime_delay = 1; break; // EI: effective after the next instruction
    case 0xF3: m->ime = 0; m->ime_delay = 0; break;
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4: case 0xEB: case 0xEC: case 0xED:
    case 0xF4: case 0xFC: case 0xFD:
        bad = 1;
        break;
    default:
        break;
    }
    if (bad) {
        if (m->trace && m->trace->event) m->trace->event(m->trace_ctx, MS_EV_BADOP, pc0, op);
        step_done(m, pc0, 1, MS_CTRL_BAD, op, 0);
        m->halted = 1;
        return 2;
    }
    step_done(m, pc0, (uint8_t)(m->pc - pc0), MS_CTRL_NONE, 0, 0);
    return 0;
}
