/* v30mz.c: the static V30MZ decoder.  One decode builds length, flow class,
 * condition and ModRM fields together; the DpIsa adapter (src/mach/ws_isa.c)
 * projects them onto the kernel's contracts.  The opcode coverage mirrors
 * m0boy's runtime dispatch — every opcode it dispatches decodes here, every
 * one it rejects (modrm forms named unsupported) decodes ILLEGAL. */
#include <stdio.h>

#include "v30mz.h"

#define NO_TARGET 0xFFFFFFFFu

/* the ALU group the first byte's bits 3-5 name */
static const char *const alu_name[8] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
static const char *const shift_name[8] = { "", "", "", "", "shl", "shr", "shl", "sar" };

/* the register names, word and byte views */
static const char *const r16[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
static const char *const r8[8] = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };
static const char *const sreg[4] = { "es", "cs", "ss", "ds" };
static const char *const cond_name[16] = {
    "o", "no", "b", "nb", "z", "nz", "be", "a", "s", "ns", "p", "np", "l", "nl", "le", "g" };

/* displacement bytes a ModRM mod/rm combination takes (mod 0 rm 6: the
 * direct 16-bit address, no base register) */
static unsigned disp_bytes(uint8_t mod, uint8_t rm) {
    if (mod == 1) return 1;
    if (mod == 2) return 2;
    if (mod == 0 && rm == 6) return 2;
    return 0;
}

/* the ModRM base+index forms, for the disassembler */
static const char *const rm_text[8] = {
    "[bx+si]", "[bx+di]", "[bp+si]", "[bp+di]", "[si]", "[di]", "[bp]", "[bx]" };

/* byte count of a ModRM instruction's immediate past the ModRM itself */
static unsigned operand_bytes(uint8_t op, uint8_t reg_field) {
    switch (op) {
        case 0x81: return 2;
        case 0x80: case 0x82: case 0x83: return 1;
        case 0xc0: case 0xc1: return 1;
        case 0xc6: return 1;
        case 0xc7: return 2;
        case 0x69: return 2;
        case 0x6b: return 1;
        case 0xf6: return reg_field == 0 ? 1 : 0;
        case 0xf7: return reg_field == 0 ? 2 : 0;
        default: return 0;
    }
}

/* modrm-capable opcodes whose register form the runtime refuses */
static int illegal_mod_form(uint8_t op, uint8_t reg, uint8_t mod) {
    if ((op == 0xf6 || op == 0xf7) && reg == 1) return 1;      /* test il2 */
    if (op == 0xfe && reg > 1) return 1;
    if (op == 0xff && reg == 7) return 1;
    if (op == 0xff && (reg == 3 || reg == 5) && mod == 3) return 1;
    if ((op == 0x8d || op == 0xc4 || op == 0xc5 || op == 0x62) && mod == 3) return 1;
    return 0;
}

unsigned dp_v30mz_decode(const uint8_t *b, unsigned avail, uint32_t pc, DpV30Insn *o) {
    uint8_t op = b[0];
    o->len = 1; o->cls = DP_V30_SEQ; o->cond = -1; o->target = NO_TARGET;
    o->mod = o->reg = o->rm = 0; o->has_modrm = 0; o->op_byte = op;

    /* a leading prefix is its own one-byte fall-through step for the static
     * walk; the runtime folds prefixes into the following instruction */
    switch (op) {
        case 0x26: case 0x2e: case 0x36: case 0x3e:      /* segment */
        case 0xf0:                                        /* lock */
            return 1;
        case 0xf2: case 0xf3:                             /* rep/repne */
            /* the repeat is a self-loop on CX; the runtime restarts the
             * opcode each slice, so the transfer target is the instruction */
            o->cond = DP_V30_CX_COND;
            o->target = pc;
            return 1;
        default: break;
    }

    unsigned d, imm;
    switch (op) {
        /* the eight ALU groups and the other ModRM instructions */
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0a: case 0x0b:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1a: case 0x1b:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2a: case 0x2b:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3a: case 0x3b:
        case 0x84: case 0x85:                 /* test */
        case 0x86: case 0x87:                 /* xchg */
        case 0x88: case 0x89: case 0x8a: case 0x8b:   /* mov */
        case 0x8c: case 0x8e:                 /* mov sreg */
        case 0x8d:                            /* lea */
        case 0x8f:                            /* pop r/m */
        case 0xc4: case 0xc5:                 /* les/lds */
        case 0xfe: case 0xff:                 /* inc/dec/call/jmp/push groups */
        case 0xd0: case 0xd1: case 0xd2: case 0xd3:   /* shifts */
        case 0xc0: case 0xc1:                 /* shifts by imm8 */
        case 0xf6: case 0xf7:                 /* mul/div/not/neg/test group */
        case 0x62:                            /* bound */
        case 0xc6: case 0xc7:                 /* mov r/m,imm */
            if (avail < 2) { o->cls = DP_V30_ILLEGAL; return 0; }
            {
                uint8_t m = b[1];
                o->has_modrm = 1;
                o->mod = m >> 6; o->reg = (m >> 3) & 7; o->rm = m & 7;
                d = disp_bytes(o->mod, o->rm);
                imm = operand_bytes(op, o->reg);
                o->len = (unsigned)(2 + d + imm);
                if (o->len > avail) { o->cls = DP_V30_ILLEGAL; return 0; }
            }
            if (illegal_mod_form(op, o->reg, o->mod)) { o->cls = DP_V30_ILLEGAL; return (int)o->len; }
            /* the indirect transfers of ff: /2 call near, /3 call far,
             * /4 jmp near, /5 jmp far */
            if (op == 0xff && (o->reg == 2 || o->reg == 3 || o->reg == 4 || o->reg == 5))
                o->cls = DP_V30_INDIRECT;
            return o->len;
        case 0x04: case 0x0c: case 0x14: case 0x1c:
        case 0x24: case 0x2c: case 0x34: case 0x3c:   /* ALU al,imm8 */
            o->len = 2; return 2;
        case 0x05: case 0x0d: case 0x15: case 0x1d:
        case 0x25: case 0x2d: case 0x35: case 0x3d:   /* ALU ax,imm16 */
        case 0xa9:                                     /* test ax,imm16 */
            o->len = 3; return 3;
        case 0xa8:                                     /* test al,imm8 */
            o->len = 2; return 2;
        case 0x06: case 0x07: case 0x0e: case 0x16: case 0x17:
        case 0x1e: case 0x1f:                          /* push/pop sreg */
        case 0x27: case 0x2f: case 0x37: case 0x3f:    /* decimal adjusts */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:    /* inc r16 */
        case 0x48: case 0x49: case 0x4a: case 0x4b:
        case 0x4c: case 0x4d: case 0x4e: case 0x4f:    /* dec r16 */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:    /* push r16 */
        case 0x58: case 0x59: case 0x5a: case 0x5b:
        case 0x5c: case 0x5d: case 0x5e: case 0x5f:    /* pop r16 */
        case 0x60: case 0x61:                          /* pusha/popa */
        case 0x6c: case 0x6d: case 0x6e: case 0x6f:    /* ins/outs */
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:    /* xchg ax,r16 */
        case 0x98: case 0x99:                          /* cbw/cwd */
        case 0x9b:                                     /* wait (POLLB held) */
        case 0x9c: case 0x9d: case 0x9e: case 0x9f:    /* pushf..lahf */
        case 0xa4: case 0xa5: case 0xa6: case 0xa7:    /* movs/cmps */
        case 0xaa: case 0xab: case 0xac: case 0xad:
        case 0xae: case 0xaf:                          /* stos/lods/scas */
        case 0xc3: case 0xcb: case 0xcf:               /* ret/retf/iret */
        case 0xc9:                                     /* leave */
        case 0xcc: case 0xce:                          /* int3/into */
        case 0xd6:                                     /* salc */
        case 0xd7:                                     /* xlat */
        case 0xec: case 0xed: case 0xee: case 0xef:    /* in/out dx */
        case 0xf4: case 0xf5:                          /* hlt/cmc */
        case 0xf8: case 0xf9: case 0xfa: case 0xfb:
        case 0xfc: case 0xfd:                          /* flag ops */
            if (op == 0xc3 || op == 0xcb || op == 0xcf) o->cls = DP_V30_RET;
            return 1;
        case 0x0f:                                     /* pop cs: unsupported */
        case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
            o->cls = DP_V30_ILLEGAL;
            return 1;
        case 0x68:                                     /* push imm16 */
            o->len = 3; return 3;
        case 0x6a:                                     /* push imm8 */
            o->len = 2; return 2;
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b:
        case 0x7c: case 0x7d: case 0x7e: case 0x7f:
            o->cls = DP_V30_JCC;
            o->cond = (int8_t)(op & 15);
            o->target = (uint32_t)((int32_t)pc + 2 + (int8_t)b[1]);
            o->len = 2; return 2;
        case 0xa0: case 0xa1: case 0xa2: case 0xa3:    /* mov al/ax <-> moffs */
            o->len = 3; return 3;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3:
        case 0xb4: case 0xb5: case 0xb6: case 0xb7:    /* mov r8,imm8 */
            o->len = 2; return 2;
        case 0xb8: case 0xb9: case 0xba: case 0xbb:
        case 0xbc: case 0xbd: case 0xbe: case 0xbf:    /* mov r16,imm16 */
            o->len = 3; return 3;
        case 0xc2:                                     /* ret imm16 */
        case 0xca:                                     /* retf imm16 */
            o->cls = DP_V30_RET;
            o->len = 3; return 3;
        case 0xc8:                                     /* enter */
            o->len = 4; return 4;
        case 0xcd:                                     /* int n */
            o->cls = DP_V30_INT;
            o->len = 2; return 2;
        case 0xd4: case 0xd5:                          /* aam/aad */
            o->len = 2; return 2;
        case 0xe0: case 0xe1: case 0xe2:               /* loopnz/loope/loop */
            o->cls = DP_V30_JCC;
            o->cond = DP_V30_CX_COND;
            o->target = (uint32_t)((int32_t)pc + 2 + (int8_t)b[1]);
            o->len = 2; return 2;
        case 0xe3:                                     /* jcxz */
            o->cls = DP_V30_JCC;
            o->cond = DP_V30_JCXZ_COND;
            o->target = (uint32_t)((int32_t)pc + 2 + (int8_t)b[1]);
            o->len = 2; return 2;
        case 0xe4: case 0xe5: case 0xe6: case 0xe7:    /* in/out imm8 */
            o->len = 2; return 2;
        case 0xe8:                                     /* call rel16 */
            o->cls = DP_V30_CALL;
            o->target = (uint32_t)((int32_t)pc + 3 + (int16_t)(b[1] | b[2] << 8));
            o->len = 3; return 3;
        case 0xe9:                                     /* jmp rel16 */
            o->cls = DP_V30_JMP;
            o->target = (uint32_t)((int32_t)pc + 3 + (int16_t)(b[1] | b[2] << 8));
            o->len = 3; return 3;
        case 0x9a:                                     /* call far */
            o->cls = DP_V30_CALL;
            o->target = (uint32_t)((b[3] | b[4] << 8) << 4 | (b[1] | b[2] << 8));
            o->len = 5; return 5;
        case 0xea:                                     /* jmp far */
            o->cls = DP_V30_JMP;
            o->target = (uint32_t)((b[3] | b[4] << 8) << 4 | (b[1] | b[2] << 8));
            o->len = 5; return 5;
        case 0xeb:                                     /* jmp rel8 */
            o->cls = DP_V30_JMP;
            o->target = (uint32_t)((int32_t)pc + 2 + (int8_t)b[1]);
            o->len = 2; return 2;
        default:
            o->cls = DP_V30_ILLEGAL;
            return 1;
    }
}

int dp_v30mz_length(uint8_t op) {
    DpV30Insn o;
    /* the widest ModRM (disp16 + imm16) bounds the length from one byte */
    uint8_t b[2] = { op, 0x9f };   /* mod 2, rm 5 */
    unsigned len = dp_v30mz_decode(b, 8, 0, &o);
    return len ? (int)o.len : 2;
}

int dp_v30mz_disasm(const uint8_t *b, unsigned avail, uint32_t pc, char *out, unsigned outsz) {
    DpV30Insn o;
    unsigned len = dp_v30mz_decode(b, avail, pc, &o);
    if (!len) return 0;
    unsigned op = o.op_byte;
    /* dp: the ModRM byte's displacement (the direct address for mod 0 rm 6);
     * x: the immediate past it (the first operand byte without a ModRM) */
    const uint8_t *dp = o.has_modrm ? b + 2 : b + 1;
    const uint8_t *x = b + (o.has_modrm ? 2 + disp_bytes(o.mod, o.rm) : 1);
    char m[24] = "";
    if (o.has_modrm) {
        if (o.mod == 3)
            snprintf(m, sizeof m, "%s", (op & 1) ? r16[o.rm] : r8[o.rm]);
        else if (o.mod == 0 && o.rm == 6)
            snprintf(m, sizeof m, "[0x%04x]", dp[0] | dp[1] << 8);
        else if (o.mod == 1)
            snprintf(m, sizeof m, "%s%+d", rm_text[o.rm], (int8_t)dp[0]);
        else if (o.mod == 2)
            snprintf(m, sizeof m, "%s%+d", rm_text[o.rm], (int16_t)(dp[0] | dp[1] << 8));
        else
            snprintf(m, sizeof m, "%s", rm_text[o.rm]);
    }
    switch (o.cls) {
        case DP_V30_JCC:
            if (o.cond == DP_V30_CX_COND) {
                static const char *const lp[3] = { "loopnz", "loope", "loop" };
                snprintf(out, outsz, "%s 0x%04x", lp[op & 3],
                         (unsigned)((int32_t)pc + o.len + (int8_t)b[1]));
            } else if (o.cond == DP_V30_JCXZ_COND)
                snprintf(out, outsz, "jcxz 0x%04x", (unsigned)((int32_t)pc + o.len + (int8_t)b[1]));
            else
                snprintf(out, outsz, "j%s 0x%04x", cond_name[o.cond & 15],
                         (unsigned)((int32_t)pc + o.len + (int8_t)b[1]));
            return (int)len;
        case DP_V30_JMP:
            if (len == 2) snprintf(out, outsz, "jmp 0x%04x", (unsigned)((int32_t)pc + o.len + (int8_t)b[1]));
            else if (len == 3) snprintf(out, outsz, "jmp 0x%04x", (unsigned)((int32_t)pc + o.len + (int16_t)(b[1] | b[2] << 8)));
            else snprintf(out, outsz, "jmp far 0x%04x:0x%04x", b[3] | b[4] << 8, b[1] | b[2] << 8);
            return (int)len;
        case DP_V30_CALL:
            if (len == 3) snprintf(out, outsz, "call 0x%04x", (unsigned)((int32_t)pc + o.len + (int16_t)(b[1] | b[2] << 8)));
            else snprintf(out, outsz, "call far 0x%04x:0x%04x", b[3] | b[4] << 8, b[1] | b[2] << 8);
            return (int)len;
        case DP_V30_RET:
            if (op == 0xcf) snprintf(out, outsz, "iret");
            else if (op == 0xcb) snprintf(out, outsz, "retf");
            else if (op == 0xca) snprintf(out, outsz, "retf 0x%04x", b[1] | b[2] << 8);
            else if (op == 0xc2) snprintf(out, outsz, "ret 0x%04x", b[1] | b[2] << 8);
            else snprintf(out, outsz, "ret");
            return (int)len;
        case DP_V30_INDIRECT:
            snprintf(out, outsz, o.reg == 2 ? "call %s" : o.reg == 3 ? "call far %s"
                          : o.reg == 4 ? "jmp %s" : "jmp far %s", m);
            return (int)len;
        case DP_V30_INT:
            snprintf(out, outsz, op == 0xcc ? "int3" : op == 0xce ? "into" : "int 0x%02x", b[1]);
            return (int)len;
        case DP_V30_HALT:
            snprintf(out, outsz, "hlt");
            return (int)len;
        case DP_V30_ILLEGAL:
            snprintf(out, outsz, "(bad)");
            return (int)len;
        default: break;
    }
    /* the sequential instructions */
    if (op <= 0x3d && (op & 7) <= 5) {
        unsigned g = (op >> 3) & 7, form = op & 7;
        if (form <= 3) {
            unsigned word = form & 1, dst_is_rm = !(form & 2);
            const char *a = m, *bb = (word ? r16[o.reg] : r8[o.reg]);
            snprintf(out, outsz, "%s %s, %s", alu_name[g], dst_is_rm ? a : bb, dst_is_rm ? bb : a);
        } else {
            snprintf(out, outsz, "%s %s, 0x%02x", alu_name[g], form == 4 ? "al" : "ax",
                     form == 4 ? x[0] : (unsigned)(x[0] | x[1] << 8));
        }
        return (int)len;
    }
    switch (op) {
        case 0x06: snprintf(out, outsz, "push es"); break;
        case 0x07: snprintf(out, outsz, "pop es"); break;
        case 0x0e: snprintf(out, outsz, "push cs"); break;
        case 0x16: snprintf(out, outsz, "push ss"); break;
        case 0x17: snprintf(out, outsz, "pop ss"); break;
        case 0x1e: snprintf(out, outsz, "push ds"); break;
        case 0x1f: snprintf(out, outsz, "pop ds"); break;
        case 0x27: snprintf(out, outsz, "daa"); break;
        case 0x2f: snprintf(out, outsz, "das"); break;
        case 0x37: snprintf(out, outsz, "aaa"); break;
        case 0x3f: snprintf(out, outsz, "aas"); break;
        case 0x60: snprintf(out, outsz, "pusha"); break;
        case 0x61: snprintf(out, outsz, "popa"); break;
        case 0x62: snprintf(out, outsz, "bound %s, %s", r16[o.reg], m); break;
        case 0x68: snprintf(out, outsz, "push 0x%04x", x[0] | x[1] << 8); break;
        case 0x69: snprintf(out, outsz, "imul %s, %s, 0x%04x", r16[o.reg], m, x[0] | x[1] << 8); break;
        case 0x6a: snprintf(out, outsz, "push 0x%02x", x[0]); break;
        case 0x6b: snprintf(out, outsz, "imul %s, %s, 0x%02x", r16[o.reg], m, x[0]); break;
        case 0x6c: snprintf(out, outsz, "insb"); break;
        case 0x6d: snprintf(out, outsz, "insw"); break;
        case 0x6e: snprintf(out, outsz, "outsb"); break;
        case 0x6f: snprintf(out, outsz, "outsw"); break;
        case 0x80: case 0x82:
            snprintf(out, outsz, "%s %s, 0x%02x", alu_name[o.reg], m, x[0]); break;
        case 0x81: snprintf(out, outsz, "%s %s, 0x%04x", alu_name[o.reg], m, x[0] | x[1] << 8); break;
        case 0x83: snprintf(out, outsz, "%s %s, 0x%02x", alu_name[o.reg], m, x[0]); break;
        case 0x84: snprintf(out, outsz, "test %s, %s", m, r8[o.reg]); break;
        case 0x85: snprintf(out, outsz, "test %s, %s", m, r16[o.reg]); break;
        case 0x86: snprintf(out, outsz, "xchg %s, %s", r8[o.reg], m); break;
        case 0x87: snprintf(out, outsz, "xchg %s, %s", r16[o.reg], m); break;
        case 0x88: snprintf(out, outsz, "mov %s, %s", m, r8[o.reg]); break;
        case 0x89: snprintf(out, outsz, "mov %s, %s", m, r16[o.reg]); break;
        case 0x8a: snprintf(out, outsz, "mov %s, %s", r8[o.reg], m); break;
        case 0x8b: snprintf(out, outsz, "mov %s, %s", r16[o.reg], m); break;
        case 0x8c: snprintf(out, outsz, "mov %s, %s", m, sreg[o.reg & 3]); break;
        case 0x8d: snprintf(out, outsz, "lea %s, %s", r16[o.reg], m); break;
        case 0x8e: snprintf(out, outsz, "mov %s, %s", sreg[o.reg & 3], m); break;
        case 0x8f: snprintf(out, outsz, "pop %s", m); break;
        case 0x90: snprintf(out, outsz, "nop"); break;
        case 0x98: snprintf(out, outsz, "cbw"); break;
        case 0x99: snprintf(out, outsz, "cwd"); break;
        case 0x9b: snprintf(out, outsz, "wait"); break;
        case 0x9c: snprintf(out, outsz, "pushf"); break;
        case 0x9d: snprintf(out, outsz, "popf"); break;
        case 0x9e: snprintf(out, outsz, "sahf"); break;
        case 0x9f: snprintf(out, outsz, "lahf"); break;
        case 0xa0: snprintf(out, outsz, "mov al, [0x%04x]", x[0] | x[1] << 8); break;
        case 0xa1: snprintf(out, outsz, "mov ax, [0x%04x]", x[0] | x[1] << 8); break;
        case 0xa2: snprintf(out, outsz, "mov [0x%04x], al", x[0] | x[1] << 8); break;
        case 0xa3: snprintf(out, outsz, "mov [0x%04x], ax", x[0] | x[1] << 8); break;
        case 0xa4: snprintf(out, outsz, "movsb"); break;
        case 0xa5: snprintf(out, outsz, "movsw"); break;
        case 0xa6: snprintf(out, outsz, "cmpsb"); break;
        case 0xa7: snprintf(out, outsz, "cmpsw"); break;
        case 0xaa: snprintf(out, outsz, "stosb"); break;
        case 0xab: snprintf(out, outsz, "stosw"); break;
        case 0xac: snprintf(out, outsz, "lodsb"); break;
        case 0xad: snprintf(out, outsz, "lodsw"); break;
        case 0xae: snprintf(out, outsz, "scasb"); break;
        case 0xaf: snprintf(out, outsz, "scasw"); break;
        case 0xc0: snprintf(out, outsz, "%s %s, 0x%02x", shift_name[o.reg], m, x[0]); break;
        case 0xc1: snprintf(out, outsz, "%s %s, 0x%02x", shift_name[o.reg], m, x[0]); break;
        case 0xc4: snprintf(out, outsz, "les %s, %s", r16[o.reg], m); break;
        case 0xc5: snprintf(out, outsz, "lds %s, %s", r16[o.reg], m); break;
        case 0xc6: snprintf(out, outsz, "mov %s, 0x%02x", m, x[0]); break;
        case 0xc7: snprintf(out, outsz, "mov %s, 0x%04x", m, x[0] | x[1] << 8); break;
        case 0xc8: snprintf(out, outsz, "enter 0x%04x, %u", x[0] | x[1] << 8, x[2] & 31); break;
        case 0xc9: snprintf(out, outsz, "leave"); break;
        case 0xcc: snprintf(out, outsz, "int3"); break;
        case 0xce: snprintf(out, outsz, "into"); break;
        case 0xcd: snprintf(out, outsz, "int 0x%02x", x[0]); break;
        case 0xd0: snprintf(out, outsz, "%s %s, 1", shift_name[o.reg], m); break;
        case 0xd1: snprintf(out, outsz, "%s %s, 1", shift_name[o.reg], m); break;
        case 0xd2: snprintf(out, outsz, "%s %s, cl", shift_name[o.reg], m); break;
        case 0xd3: snprintf(out, outsz, "%s %s, cl", shift_name[o.reg], m); break;
        case 0xd4: snprintf(out, outsz, "aam 0x%02x", x[0]); break;
        case 0xd5: snprintf(out, outsz, "aad 0x%02x", x[0]); break;
        case 0xd6: snprintf(out, outsz, "salc"); break;
        case 0xd7: snprintf(out, outsz, "xlat"); break;
        case 0xe4: snprintf(out, outsz, "in al, 0x%02x", x[0]); break;
        case 0xe5: snprintf(out, outsz, "in ax, 0x%02x", x[0]); break;
        case 0xe6: snprintf(out, outsz, "out 0x%02x, al", x[0]); break;
        case 0xe7: snprintf(out, outsz, "out 0x%02x, ax", x[0]); break;
        case 0xec: snprintf(out, outsz, "in al, dx"); break;
        case 0xed: snprintf(out, outsz, "in ax, dx"); break;
        case 0xee: snprintf(out, outsz, "out dx, al"); break;
        case 0xef: snprintf(out, outsz, "out dx, ax"); break;
        case 0xf0: snprintf(out, outsz, "lock"); break;
        case 0xf2: snprintf(out, outsz, "repne"); break;
        case 0xf3: snprintf(out, outsz, "rep"); break;
        case 0xf4: snprintf(out, outsz, "hlt"); break;
        case 0xf5: snprintf(out, outsz, "cmc"); break;
        case 0xf6: snprintf(out, outsz, o.reg == 0 ? "test %s, 0x%02x"
                                       : o.reg == 2 ? "not %s" : o.reg == 3 ? "neg %s"
                                       : o.reg == 4 ? "mul %s" : o.reg == 5 ? "imul %s"
                                       : o.reg == 6 ? "div %s" : "idiv %s", m, x[0]); break;
        case 0xf7: snprintf(out, outsz, o.reg == 0 ? "test %s, 0x%04x"
                                       : o.reg == 2 ? "not %s" : o.reg == 3 ? "neg %s"
                                       : o.reg == 4 ? "mul %s" : o.reg == 5 ? "imul %s"
                                       : o.reg == 6 ? "div %s" : "idiv %s", m, x[0] | x[1] << 8); break;
        case 0xf8: snprintf(out, outsz, "clc"); break;
        case 0xf9: snprintf(out, outsz, "stc"); break;
        case 0xfa: snprintf(out, outsz, "cli"); break;
        case 0xfb: snprintf(out, outsz, "sti"); break;
        case 0xfc: snprintf(out, outsz, "cld"); break;
        case 0xfd: snprintf(out, outsz, "std"); break;
        case 0xfe: snprintf(out, outsz, "%s %s", o.reg ? "dec" : "inc", m); break;
        case 0xff: {
            static const char *const ff[7] = { "inc", "dec", "call", "call far", "jmp", "jmp far", "push" };
            snprintf(out, outsz, "%s %s", ff[o.reg], m); break;
        }
        default:
            if (op >= 0x40 && op <= 0x47) snprintf(out, outsz, "inc %s", r16[op & 7]);
            else if (op >= 0x48 && op <= 0x4f) snprintf(out, outsz, "dec %s", r16[op & 7]);
            else if (op >= 0x50 && op <= 0x57) snprintf(out, outsz, "push %s", r16[op & 7]);
            else if (op >= 0x58 && op <= 0x5f) snprintf(out, outsz, "pop %s", r16[op & 7]);
            else if (op >= 0x91 && op <= 0x97) snprintf(out, outsz, "xchg ax, %s", r16[op & 7]);
            else if (op >= 0xb0 && op <= 0xb7) snprintf(out, outsz, "mov %s, 0x%02x", r8[op & 7], x[0]);
            else if (op >= 0xb8 && op <= 0xbf) snprintf(out, outsz, "mov %s, 0x%04x", r16[op & 7], x[0] | x[1] << 8);
            else if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e)
                snprintf(out, outsz, "%s:", sreg[(op >> 3) & 3]);
            else if (op == 0xf0)
                snprintf(out, outsz, "lock:");
            else snprintf(out, outsz, "(bad)");
            break;
    }
    return (int)len;
}
