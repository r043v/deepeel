// hucdec.c: the HuC6280 analysis decoder — lengths, flow classes, branch
// conditions and formatting, for the explorer and the static closure.  It
// never touches the bus.  The opcode set is the one m0boy's interpreter
// runs (65C02 + Hudson: TAM/TMA, ST0/1/2 #imm, block moves, BBR/BBS,
// RMB/SMB, TST, BSR, SET, CLA/CLX/CLY, SXY/SAX/SAY, CSL/CSH); the opcodes
// it treats as two-cycle NOPs decode as DP_HUC_ILLEGAL so the explorer can
// tell data run as code from real code.
#include "pce.h"
#include <string.h>

static const char *const names[256] = {
    "BRK","ORA","SXY","ST0","TSB","ORA","ASL","RMB0","PHP","ORA","ASL",NULL,"TSB","ORA","ASL","BBR0",
    "BPL","ORA","ORA","ST1","TRB","ORA","ASL","RMB1","CLC","ORA","INC",NULL,"TRB","ORA","ASL","BBR1",
    "JSR","AND","SAX","ST2","BIT","AND","ROL","RMB2","PLP","AND","ROL",NULL,"BIT","AND","ROL","BBR2",
    "BMI","AND","AND",NULL,"BIT","AND","ROL","RMB3","SEC","AND","DEC",NULL,"BIT","AND","ROL","BBR3",
    "RTI","EOR","SAY","TMA","BSR","EOR","LSR","RMB4","PHA","EOR","LSR",NULL,"JMP","EOR","LSR","BBR4",
    "BVC","EOR","EOR","TAM","CSL","EOR","LSR","RMB5","CLI","EOR","PHY",NULL,NULL,"EOR","LSR","BBR5",
    "RTS","ADC","CLA",NULL,"STZ","ADC","ROR","RMB6","PLA","ADC","ROR",NULL,"JMP","ADC","ROR","BBR6",
    "BVS","ADC","ADC","TII","STZ","ADC","ROR","RMB7","SEI","ADC","PLY",NULL,"JMP","ADC","ROR","BBR7",
    "BRA","STA","CLX","TST","STY","STA","STX","SMB0","DEY","BIT","TXA",NULL,"STY","STA","STX","BBS0",
    "BCC","STA","STA","TST","STY","STA","STX","SMB1","TYA","STA","TXS",NULL,"STZ","STA","STZ","BBS1",
    "LDY","LDA","LDX","TST","LDY","LDA","LDX","SMB2","TAY","LDA","TAX",NULL,"LDY","LDA","LDX","BBS2",
    "BCS","LDA","LDA","TST","LDY","LDA","LDX","SMB3","CLV","LDA","TSX",NULL,"LDY","LDA","LDX","BBS3",
    "CPY","CMP","CLY","TDD","CPY","CMP","DEC","SMB4","INY","CMP","DEX",NULL,"CPY","CMP","DEC","BBS4",
    "BNE","CMP","CMP","TIN","CSH","CMP","DEC","SMB5","CLD","CMP","PHX",NULL,NULL,"CMP","DEC","BBS5",
    "CPX","SBC",NULL,"TIA","CPX","SBC","INC","SMB6","INX","SBC","NOP",NULL,"CPX","SBC","INC","BBS6",
    "BEQ","SBC","SBC","TAI","SET","SBC","INC","SMB7","SED","SBC","PLX",NULL,NULL,"SBC","INC","BBS7",
};

static int mode_of(uint8_t op) {
    unsigned lo = op & 0x0F, hi = op >> 4;
    switch (lo) {
    case 0x0:
        if (op == 0x00 || op == 0x40 || op == 0x60) return DP_HUC_M_IMP;
        if (op == 0x20) return DP_HUC_M_ABS;
        if (op == 0xA0 || op == 0xC0 || op == 0xE0) return DP_HUC_M_IMM;
        return DP_HUC_M_REL; // odd rows + BRA ($80)
    case 0x1: return (hi & 1) ? DP_HUC_M_IZY : DP_HUC_M_IZX;
    case 0x2:
        if (hi & 1) return DP_HUC_M_IZP;
        return op == 0xA2 ? DP_HUC_M_IMM : DP_HUC_M_IMP;
    case 0x3:
        switch (op) {
        case 0x03: case 0x13: case 0x23: case 0x43: case 0x53: return DP_HUC_M_IMM;
        case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3: return DP_HUC_M_BLOCK;
        case 0x83: case 0xA3: return DP_HUC_M_TST_ZP;
        case 0x93: case 0xB3: return DP_HUC_M_TST_ABS;
        default: return DP_HUC_M_IMP;
        }
    case 0x4:
        if (op == 0x44) return DP_HUC_M_REL;
        if (op == 0x54 || op == 0xD4 || op == 0xF4) return DP_HUC_M_IMP;
        return (hi & 1) ? DP_HUC_M_ZPX : DP_HUC_M_ZP;
    case 0x5: return (hi & 1) ? DP_HUC_M_ZPX : DP_HUC_M_ZP;
    case 0x6:
        if (op == 0x96 || op == 0xB6) return DP_HUC_M_ZPY;
        return (hi & 1) ? DP_HUC_M_ZPX : DP_HUC_M_ZP;
    case 0x7: return DP_HUC_M_ZP;
    case 0x8: return DP_HUC_M_IMP;
    case 0x9: return (hi & 1) ? DP_HUC_M_ABSY : DP_HUC_M_IMM;
    case 0xA: return DP_HUC_M_IMP;
    case 0xB: return DP_HUC_M_IMP;
    case 0xC:
        if (op == 0x6C) return DP_HUC_M_IND;
        if (op == 0x7C) return DP_HUC_M_INDX;
        if (op == 0x5C || op == 0xDC || op == 0xFC) return DP_HUC_M_IMP;
        if (op == 0x9C) return DP_HUC_M_ABS;
        return (hi & 1) ? DP_HUC_M_ABSX : DP_HUC_M_ABS;
    case 0xD: return (hi & 1) ? DP_HUC_M_ABSX : DP_HUC_M_ABS;
    case 0xE:
        if (op == 0xBE) return DP_HUC_M_ABSY;
        if (op == 0x9E) return DP_HUC_M_ABSX;
        return (hi & 1) ? DP_HUC_M_ABSX : DP_HUC_M_ABS;
    default: return DP_HUC_M_ZPREL; // $xF: BBR/BBS
    }
}

static const uint8_t mode_len[] = {
    [DP_HUC_M_IMP] = 1, [DP_HUC_M_IMM] = 2, [DP_HUC_M_ZP] = 2, [DP_HUC_M_ZPX] = 2,
    [DP_HUC_M_ZPY] = 2, [DP_HUC_M_ABS] = 3, [DP_HUC_M_ABSX] = 3, [DP_HUC_M_ABSY] = 3,
    [DP_HUC_M_IZX] = 2, [DP_HUC_M_IZY] = 2, [DP_HUC_M_IZP] = 2, [DP_HUC_M_IND] = 3,
    [DP_HUC_M_INDX] = 3, [DP_HUC_M_REL] = 2, [DP_HUC_M_ZPREL] = 3, [DP_HUC_M_BLOCK] = 7,
    [DP_HUC_M_TST_ZP] = 3, [DP_HUC_M_TST_ABS] = 4,
};

static int flag_cond(uint8_t op) {
    switch (op) {
    case 0x10: return 0; case 0x30: return 4;
    case 0x50: return 1; case 0x70: return 5;
    case 0x90: return 2; case 0xB0: return 6;
    case 0xD0: return 3; case 0xF0: return 7;
    }
    return -1;
}

int dp_huc_decode(const uint8_t *b, size_t size, uint16_t pc, DpHucIns *out) {
    if (!b || !size || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->cond = -1;
    uint8_t op = b[0];
    out->op = op;
    out->mode = (uint8_t)mode_of(op);
    out->length = mode_len[out->mode];
    if (size < out->length) return 0;
    if (!names[op]) { out->flow = DP_HUC_ILLEGAL; out->length = 1; return 1; }
    uint16_t w = out->length >= 3 ? (uint16_t)(b[1] | b[2] << 8) : (out->length == 2 ? b[1] : 0);
    out->operand = w;
    out->flow = DP_HUC_NEXT;
    switch (op) {
    case 0x00: out->flow = DP_HUC_BRK; break;
    case 0x20: out->flow = DP_HUC_CALL; out->target = w; break;
    case 0x44: out->flow = DP_HUC_CALL; out->target = (uint16_t)(pc + 2 + (int8_t)b[1]); break;
    case 0x4C: out->flow = DP_HUC_JUMP; out->target = w; break;
    case 0x80: out->flow = DP_HUC_JUMP; out->target = (uint16_t)(pc + 2 + (int8_t)b[1]); break;
    case 0x6C: case 0x7C: out->flow = DP_HUC_INDIRECT; break;
    case 0x40: case 0x60: out->flow = DP_HUC_RETURN; break;
    case 0x53: out->mapchange = 1; break;
    default:
        if (out->mode == DP_HUC_M_REL) {
            out->flow = DP_HUC_BRANCH;
            out->cond = (int8_t)flag_cond(op);
            out->target = (uint16_t)(pc + 2 + (int8_t)b[1]);
        } else if (out->mode == DP_HUC_M_ZPREL) {
            out->flow = DP_HUC_BRANCH;
            out->cond = (int8_t)(((op & 0x80) ? 16 : 8) + ((op >> 4) & 7));
            out->operand = b[1];
            out->target = (uint16_t)(pc + 3 + (int8_t)b[2]);
        }
        break;
    }
    return 1;
}

void dp_huc_format(const DpHucIns *in, const uint8_t *b, char *out, size_t size) {
    const char *n = names[in->op];
    if (!n) { snprintf(out, size, ".db $%02X", in->op); return; }
    unsigned w = in->length >= 3 ? (unsigned)(b[1] | b[2] << 8) : 0;
    switch (in->mode) {
    case DP_HUC_M_IMP: snprintf(out, size, "%s", n); break;
    case DP_HUC_M_IMM: snprintf(out, size, "%s #$%02X", n, b[1]); break;
    case DP_HUC_M_ZP: snprintf(out, size, "%s <$%02X", n, b[1]); break;
    case DP_HUC_M_ZPX: snprintf(out, size, "%s <$%02X,X", n, b[1]); break;
    case DP_HUC_M_ZPY: snprintf(out, size, "%s <$%02X,Y", n, b[1]); break;
    case DP_HUC_M_ABS: snprintf(out, size, "%s $%04X", n, w); break;
    case DP_HUC_M_ABSX: snprintf(out, size, "%s $%04X,X", n, w); break;
    case DP_HUC_M_ABSY: snprintf(out, size, "%s $%04X,Y", n, w); break;
    case DP_HUC_M_IZX: snprintf(out, size, "%s [$%02X,X]", n, b[1]); break;
    case DP_HUC_M_IZY: snprintf(out, size, "%s [$%02X],Y", n, b[1]); break;
    case DP_HUC_M_IZP: snprintf(out, size, "%s [$%02X]", n, b[1]); break;
    case DP_HUC_M_IND: snprintf(out, size, "%s [$%04X]", n, w); break;
    case DP_HUC_M_INDX: snprintf(out, size, "%s [$%04X,X]", n, w); break;
    case DP_HUC_M_REL: snprintf(out, size, "%s $%04X", n, in->target); break;
    case DP_HUC_M_ZPREL: snprintf(out, size, "%s <$%02X,$%04X", n, b[1], in->target); break;
    case DP_HUC_M_BLOCK:
        snprintf(out, size, "%s $%04X,$%04X,$%04X", n, b[1] | b[2] << 8, b[3] | b[4] << 8, b[5] | b[6] << 8);
        break;
    case DP_HUC_M_TST_ZP: snprintf(out, size, "%s #$%02X,<$%02X%s", n, b[1], b[2], in->op == 0xA3 ? ",X" : ""); break;
    case DP_HUC_M_TST_ABS:
        snprintf(out, size, "%s #$%02X,$%04X%s", n, b[1], b[2] | b[3] << 8, in->op == 0xB3 ? ",X" : "");
        break;
    default: snprintf(out, size, "%s", n); break;
    }
}

uint8_t dp_huc_length(uint8_t op) { return names[op] ? mode_len[mode_of(op)] : 1; }
int dp_huc_defined(uint8_t op) { return names[op] != NULL; }
