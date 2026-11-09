// m6502.c: the 6502 executor and its bounded analysis twin.  One static
// opcode table drives both: dp_m6502_decode() reads the rows, the executor
// applies them against the live bus.  Addressing modes and flag semantics
// follow the 6502 with decimal mode disabled (2A03); undocumented opcodes
// jam, they are never guessed.
#include "m6502.h"
#include <stdio.h>
#include <string.h>

enum { ACC_R=0, ACC_W=1, ACC_RMW=2, ACC_NONE=3 };

// one row per opcode: addressing mode, access class, base cycles, flow and
// mnemonic.  Undefined rows keep mode 255 and decode as jams.
typedef struct {
    uint8_t mode, access, cycles;
    DpM6502Flow flow;
    const char *name;
} M6502Row;

#define IMP(m,c,n)     {DP_M6502_IMP, ACC_NONE, c, DP_M6502_NEXT, n}
#define IMM(m,c,n)     {DP_M6502_IMM, ACC_R, c, DP_M6502_NEXT, n}
#define RD1(m,ad,c,n)  {ad, ACC_R, c, DP_M6502_NEXT, n}
#define WR1(m,ad,c,n)  {ad, ACC_W, c, DP_M6502_NEXT, n}
#define RMW(m,ad,c,n)  {ad, ACC_RMW, c, DP_M6502_NEXT, n}
#define REL(c,n)       {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, n}
#define SPEC(m,ad,c,f,n) {ad, ACC_NONE, c, f, n}
// mode shorthands used by the table rows
#define JAMROW {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_JAM, "JAM"}
#define IMPR(c,n) {DP_M6502_IMP, ACC_NONE, c, DP_M6502_NEXT, n}
#define imp DP_M6502_IMP
#define zp DP_M6502_ZP
#define zpx DP_M6502_ZPX
#define zpy DP_M6502_ZPY
#define abs DP_M6502_ABS
#define absx DP_M6502_ABSX
#define absy DP_M6502_ABSY
#define indx DP_M6502_INDX
#define indy DP_M6502_INDY
#define ind DP_M6502_IND

static const M6502Row rows[256] = {
    [0x00] = {DP_M6502_IMP, ACC_NONE, 7, DP_M6502_NEXT, "BRK"},
    [0x01] = {DP_M6502_INDY, ACC_R, 6, DP_M6502_NEXT, "ORA"},
    [0x02] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x03] = {DP_M6502_INDX, ACC_RMW, 8, DP_M6502_NEXT, "SLO"},
    [0x04] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "NOP"},
    [0x05] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "ORA"},
    [0x06] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "ASL"},
    [0x07] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "SLO"},
    [0x08] = {DP_M6502_IMP, ACC_NONE, 3, DP_M6502_NEXT, "PHP"},
    [0x09] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "ORA"},
    [0x0A] = {DP_M6502_IMP, ACC_RMW, 2, DP_M6502_NEXT, "ASL"},
    [0x0B] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "ANC"},
    [0x0C] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x0D] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "ORA"},
    [0x0E] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "ASL"},
    [0x0F] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "SLO"},
    [0x10] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BPL"},
    [0x11] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "ORA"},
    [0x12] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x13] = {DP_M6502_INDY, ACC_RMW, 8, DP_M6502_NEXT, "SLO"},
    [0x14] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x15] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "ORA"},
    [0x16] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "ASL"},
    [0x17] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "SLO"},
    [0x18] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "CLC"},
    [0x19] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "ORA"},
    [0x1A] = {DP_M6502_IMP, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0x1B] = {DP_M6502_ABSY, ACC_RMW, 7, DP_M6502_NEXT, "SLO"},
    [0x1C] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x1D] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "ORA"},
    [0x1E] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "ASL"},
    [0x1F] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "SLO"},
    [0x20] = {DP_M6502_ABS, ACC_NONE, 6, DP_M6502_CALL, "JSR"},
    [0x21] = {DP_M6502_INDX, ACC_R, 6, DP_M6502_NEXT, "AND"},
    [0x22] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x23] = {DP_M6502_INDX, ACC_RMW, 8, DP_M6502_NEXT, "RLA"},
    [0x24] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "BIT"},
    [0x25] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "AND"},
    [0x26] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "ROL"},
    [0x27] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "RLA"},
    [0x28] = {DP_M6502_IMP, ACC_NONE, 4, DP_M6502_NEXT, "PLP"},
    [0x29] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "AND"},
    [0x2A] = {DP_M6502_IMP, ACC_RMW, 2, DP_M6502_NEXT, "ROL"},
    [0x2B] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "ANC"},
    [0x2C] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "BIT"},
    [0x2D] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "AND"},
    [0x2E] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "ROL"},
    [0x2F] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "RLA"},
    [0x30] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BMI"},
    [0x31] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "AND"},
    [0x32] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x33] = {DP_M6502_INDY, ACC_RMW, 8, DP_M6502_NEXT, "RLA"},
    [0x34] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x35] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "AND"},
    [0x36] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "ROL"},
    [0x37] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "RLA"},
    [0x38] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "SEC"},
    [0x39] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "AND"},
    [0x3A] = {DP_M6502_IMP, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0x3B] = {DP_M6502_ABSY, ACC_RMW, 7, DP_M6502_NEXT, "RLA"},
    [0x3C] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x3D] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "AND"},
    [0x3E] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "ROL"},
    [0x3F] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "RLA"},
    [0x40] = {DP_M6502_IMP, ACC_NONE, 6, DP_M6502_RETURN, "RTI"},
    [0x41] = {DP_M6502_INDX, ACC_R, 6, DP_M6502_NEXT, "EOR"},
    [0x42] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x43] = {DP_M6502_INDX, ACC_RMW, 8, DP_M6502_NEXT, "SRE"},
    [0x44] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "NOP"},
    [0x45] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "EOR"},
    [0x46] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "LSR"},
    [0x47] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "SRE"},
    [0x48] = {DP_M6502_IMP, ACC_NONE, 3, DP_M6502_NEXT, "PHA"},
    [0x49] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "EOR"},
    [0x4A] = {DP_M6502_IMP, ACC_RMW, 2, DP_M6502_NEXT, "LSR"},
    [0x4B] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "ALR"},
    [0x4C] = {DP_M6502_ABS, ACC_NONE, 3, DP_M6502_JUMP, "JMP"},
    [0x4D] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "EOR"},
    [0x4E] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "LSR"},
    [0x4F] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "SRE"},
    [0x50] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BVC"},
    [0x51] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "EOR"},
    [0x52] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x53] = {DP_M6502_INDY, ACC_RMW, 8, DP_M6502_NEXT, "SRE"},
    [0x54] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x55] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "EOR"},
    [0x56] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "LSR"},
    [0x57] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "SRE"},
    [0x58] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "CLI"},
    [0x59] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "EOR"},
    [0x5A] = {DP_M6502_IMP, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0x5B] = {DP_M6502_ABSY, ACC_RMW, 7, DP_M6502_NEXT, "SRE"},
    [0x5C] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x5D] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "EOR"},
    [0x5E] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "LSR"},
    [0x5F] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "SRE"},
    [0x60] = {DP_M6502_IMP, ACC_NONE, 6, DP_M6502_RETURN, "RTS"},
    [0x61] = {DP_M6502_INDX, ACC_R, 6, DP_M6502_NEXT, "ADC"},
    [0x62] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x63] = {DP_M6502_INDX, ACC_RMW, 8, DP_M6502_NEXT, "RRA"},
    [0x64] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "NOP"},
    [0x65] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "ADC"},
    [0x66] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "ROR"},
    [0x67] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "RRA"},
    [0x68] = {DP_M6502_IMP, ACC_NONE, 4, DP_M6502_NEXT, "PLA"},
    [0x69] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "ADC"},
    [0x6A] = {DP_M6502_IMP, ACC_RMW, 2, DP_M6502_NEXT, "ROR"},
    [0x6B] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "ARR"},
    [0x6C] = {DP_M6502_IND, ACC_NONE, 5, DP_M6502_INDIRECT, "JMP"},
    [0x6D] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "ADC"},
    [0x6E] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "ROR"},
    [0x6F] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "RRA"},
    [0x70] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BVS"},
    [0x71] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "ADC"},
    [0x72] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x73] = {DP_M6502_INDY, ACC_RMW, 8, DP_M6502_NEXT, "RRA"},
    [0x74] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x75] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "ADC"},
    [0x76] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "ROR"},
    [0x77] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "RRA"},
    [0x78] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "SEI"},
    [0x79] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "ADC"},
    [0x7A] = {DP_M6502_IMP, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0x7B] = {DP_M6502_ABSY, ACC_RMW, 7, DP_M6502_NEXT, "RRA"},
    [0x7C] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0x7D] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "ADC"},
    [0x7E] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "ROR"},
    [0x7F] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "RRA"},
    [0x80] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0x81] = {DP_M6502_INDX, ACC_W, 6, DP_M6502_NEXT, "STA"},
    [0x82] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0x83] = {DP_M6502_INDX, ACC_W, 6, DP_M6502_NEXT, "SAX"},
    [0x84] = {DP_M6502_ZP, ACC_W, 3, DP_M6502_NEXT, "STY"},
    [0x85] = {DP_M6502_ZP, ACC_W, 3, DP_M6502_NEXT, "STA"},
    [0x86] = {DP_M6502_ZP, ACC_W, 3, DP_M6502_NEXT, "STX"},
    [0x87] = {DP_M6502_ZP, ACC_W, 3, DP_M6502_NEXT, "SAX"},
    [0x88] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "DEY"},
    [0x89] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0x8A] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "TXA"},
    [0x8B] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "XAA"},
    [0x8C] = {DP_M6502_ABS, ACC_W, 4, DP_M6502_NEXT, "STY"},
    [0x8D] = {DP_M6502_ABS, ACC_W, 4, DP_M6502_NEXT, "STA"},
    [0x8E] = {DP_M6502_ABS, ACC_W, 4, DP_M6502_NEXT, "STX"},
    [0x8F] = {DP_M6502_ABS, ACC_W, 4, DP_M6502_NEXT, "SAX"},
    [0x90] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BCC"},
    [0x91] = {DP_M6502_INDY, ACC_W, 6, DP_M6502_NEXT, "STA"},
    [0x92] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0x93] = {DP_M6502_INDY, ACC_W, 6, DP_M6502_NEXT, "SHA"},
    [0x94] = {DP_M6502_ZPX, ACC_W, 4, DP_M6502_NEXT, "STY"},
    [0x95] = {DP_M6502_ZPX, ACC_W, 4, DP_M6502_NEXT, "STA"},
    [0x96] = {DP_M6502_ZPY, ACC_W, 4, DP_M6502_NEXT, "STX"},
    [0x97] = {DP_M6502_ZPY, ACC_W, 4, DP_M6502_NEXT, "SAX"},
    [0x98] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "TYA"},
    [0x99] = {DP_M6502_ABSY, ACC_W, 5, DP_M6502_NEXT, "STA"},
    [0x9A] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "TXS"},
    [0x9B] = {DP_M6502_ABSY, ACC_W, 5, DP_M6502_NEXT, "TAS"},
    [0x9C] = {DP_M6502_ABSX, ACC_W, 5, DP_M6502_NEXT, "SHY"},
    [0x9D] = {DP_M6502_ABSX, ACC_W, 5, DP_M6502_NEXT, "STA"},
    [0x9E] = {DP_M6502_ABSY, ACC_W, 5, DP_M6502_NEXT, "SHX"},
    [0x9F] = {DP_M6502_ABSY, ACC_W, 5, DP_M6502_NEXT, "SHA"},
    [0xA0] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "LDY"},
    [0xA1] = {DP_M6502_INDX, ACC_R, 6, DP_M6502_NEXT, "LDA"},
    [0xA2] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "LDX"},
    [0xA3] = {DP_M6502_INDX, ACC_R, 6, DP_M6502_NEXT, "LAX"},
    [0xA4] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "LDY"},
    [0xA5] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "LDA"},
    [0xA6] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "LDX"},
    [0xA7] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "LAX"},
    [0xA8] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "TAY"},
    [0xA9] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "LDA"},
    [0xAA] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "TAX"},
    [0xAB] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "LXA"},
    [0xAC] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "LDY"},
    [0xAD] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "LDA"},
    [0xAE] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "LDX"},
    [0xAF] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "LAX"},
    [0xB0] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BCS"},
    [0xB1] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "LDA"},
    [0xB2] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0xB3] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "LAX"},
    [0xB4] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "LDY"},
    [0xB5] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "LDA"},
    [0xB6] = {DP_M6502_ZPY, ACC_R, 4, DP_M6502_NEXT, "LDX"},
    [0xB7] = {DP_M6502_ZPY, ACC_R, 4, DP_M6502_NEXT, "LAX"},
    [0xB8] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "CLV"},
    [0xB9] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "LDA"},
    [0xBA] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "TSX"},
    [0xBB] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "LAS"},
    [0xBC] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "LDY"},
    [0xBD] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "LDA"},
    [0xBE] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "LDX"},
    [0xBF] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "LAX"},
    [0xC0] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "CPY"},
    [0xC1] = {DP_M6502_INDX, ACC_R, 6, DP_M6502_NEXT, "CMP"},
    [0xC2] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0xC3] = {DP_M6502_INDX, ACC_RMW, 8, DP_M6502_NEXT, "DCP"},
    [0xC4] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "CPY"},
    [0xC5] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "CMP"},
    [0xC6] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "DEC"},
    [0xC7] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "DCP"},
    [0xC8] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "INY"},
    [0xC9] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "CMP"},
    [0xCA] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "DEX"},
    [0xCB] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "SBX"},
    [0xCC] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "CPY"},
    [0xCD] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "CMP"},
    [0xCE] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "DEC"},
    [0xCF] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "DCP"},
    [0xD0] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BNE"},
    [0xD1] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "CMP"},
    [0xD2] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0xD3] = {DP_M6502_INDY, ACC_RMW, 8, DP_M6502_NEXT, "DCP"},
    [0xD4] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0xD5] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "CMP"},
    [0xD6] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "DEC"},
    [0xD7] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "DCP"},
    [0xD8] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "CLD"},
    [0xD9] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "CMP"},
    [0xDA] = {DP_M6502_IMP, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0xDB] = {DP_M6502_ABSY, ACC_RMW, 7, DP_M6502_NEXT, "DCP"},
    [0xDC] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0xDD] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "CMP"},
    [0xDE] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "DEC"},
    [0xDF] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "DCP"},
    [0xE0] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "CPX"},
    [0xE1] = {DP_M6502_INDX, ACC_R, 6, DP_M6502_NEXT, "SBC"},
    [0xE2] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0xE3] = {DP_M6502_INDX, ACC_RMW, 8, DP_M6502_NEXT, "ISB"},
    [0xE4] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "CPX"},
    [0xE5] = {DP_M6502_ZP, ACC_R, 3, DP_M6502_NEXT, "SBC"},
    [0xE6] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "INC"},
    [0xE7] = {DP_M6502_ZP, ACC_RMW, 5, DP_M6502_NEXT, "ISB"},
    [0xE8] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "INX"},
    [0xE9] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "SBC"},
    [0xEA] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "NOP"},
    [0xEB] = {DP_M6502_IMM, ACC_R, 2, DP_M6502_NEXT, "SBC"},  // USBC: SBC immediate alias
    [0xEC] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "CPX"},
    [0xED] = {DP_M6502_ABS, ACC_R, 4, DP_M6502_NEXT, "SBC"},
    [0xEE] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "INC"},
    [0xEF] = {DP_M6502_ABS, ACC_RMW, 6, DP_M6502_NEXT, "ISB"},
    [0xF0] = {DP_M6502_REL, ACC_NONE, 2, DP_M6502_BRANCH, "BEQ"},
    [0xF1] = {DP_M6502_INDY, ACC_R, 5, DP_M6502_NEXT, "SBC"},
    [0xF2] = {255, ACC_NONE, 2, DP_M6502_JAM, "JAM"},
    [0xF3] = {DP_M6502_INDY, ACC_RMW, 8, DP_M6502_NEXT, "ISB"},
    [0xF4] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0xF5] = {DP_M6502_ZPX, ACC_R, 4, DP_M6502_NEXT, "SBC"},
    [0xF6] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "INC"},
    [0xF7] = {DP_M6502_ZPX, ACC_RMW, 6, DP_M6502_NEXT, "ISB"},
    [0xF8] = {DP_M6502_IMP, ACC_NONE, 2, DP_M6502_NEXT, "SED"},
    [0xF9] = {DP_M6502_ABSY, ACC_R, 4, DP_M6502_NEXT, "SBC"},
    [0xFA] = {DP_M6502_IMP, ACC_R, 2, DP_M6502_NEXT, "NOP"},
    [0xFB] = {DP_M6502_ABSY, ACC_RMW, 7, DP_M6502_NEXT, "ISB"},
    [0xFC] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "NOP"},
    [0xFD] = {DP_M6502_ABSX, ACC_R, 4, DP_M6502_NEXT, "SBC"},
    [0xFE] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "INC"},
    [0xFF] = {DP_M6502_ABSX, ACC_RMW, 7, DP_M6502_NEXT, "ISB"},
};
#undef IMP
#undef IMM
#undef RD1
#undef WR1
#undef RMW
#undef REL
#undef SPEC

// branch conditions, keyed by the four branch opcodes' bit 6:3 group
static int branch_condition(uint8_t op) {
    switch (op) {
    case 0x10: return DP_M6502_CC_N; case 0x30: return DP_M6502_CS_N;
    case 0x50: return DP_M6502_CC_V; case 0x70: return DP_M6502_CS_V;
    case 0x90: return DP_M6502_CC_C; case 0xB0: return DP_M6502_CS_C;
    case 0xD0: return DP_M6502_CC_Z; case 0xF0: return DP_M6502_CS_Z;
    }
    return -1;
}

int dp_m6502_decode(const uint8_t *bytes, size_t size, uint16_t pc, DpM6502Instruction *out) {
    if (!bytes || !size || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->condition = -1;
    uint8_t op = bytes[0];
    const M6502Row *row = &rows[op];
    out->op = op;
    if (row->mode == 255) { // undocumented: a jam of length 1
        out->flow = DP_M6502_JAM;
        out->length = 1;
        return 1;
    }
    out->addr_mode = row->mode;
    out->access = row->access;
    out->base_cycles = row->cycles;
    switch (row->mode) {
    case DP_M6502_IMM: case DP_M6502_ZP: case DP_M6502_ZPX: case DP_M6502_ZPY:
    case DP_M6502_INDX: case DP_M6502_INDY: case DP_M6502_REL: out->length = 2; break;
    case DP_M6502_ABS: case DP_M6502_ABSX: case DP_M6502_ABSY: case DP_M6502_IND:
        out->length = 3; break;
    default: out->length = 1; break;
    }
    if (size < out->length) return 0;
    uint8_t lo = out->length > 1 ? bytes[1] : 0, hi = out->length > 2 ? bytes[2] : 0;
    uint16_t word = (uint16_t)(lo | (hi << 8));
    out->flow = row->flow;
    switch (out->flow) {
    case DP_M6502_JUMP: case DP_M6502_CALL:
        out->target = word;
        break;
    case DP_M6502_BRANCH:
        out->condition = branch_condition(op);
        out->target = (uint16_t)(pc + 2 + (int8_t)lo);
        break;
    default:
        break;
    }
    return 1;
}

int dp_m6502_condition(int condition, const m6502_t *cpu) {
    static const uint8_t masks[4] = {0x80, 0x40, 0x01, 0x02};
    int bit = cpu->p & masks[condition & 3];
    return !!bit == ((condition >> 2) & 1);
}

static void set_flag(m6502_t *cpu, uint8_t mask, int value) {
    if (value) cpu->p |= mask; else cpu->p &= (uint8_t)~mask;
}

int dp_m6502_force(int condition, m6502_t *cpu, int taken) {
    static const uint8_t masks[4] = {0x80, 0x40, 0x01, 0x02};
    uint8_t mask = masks[condition & 3];
    int previous = !!(cpu->p & mask);
    int bit = ((condition >> 2) & 1) ? !!taken : !taken;
    set_flag(cpu, mask, bit);
    return previous;
}

void dp_m6502_format(const DpM6502Instruction *in, uint8_t lo, uint8_t hi, char *out, size_t size) {
    const M6502Row *row = &rows[in->op];
    const char *name = row->name ? row->name : "JAM";
    if (row->mode == 255) { snprintf(out, size, ".db $%02X", in->op); return; }
    if (in->op == 0x00) { snprintf(out, size, "BRK"); return; }
    switch (row->mode) {
    case DP_M6502_IMP: snprintf(out, size, "%s", name); break;
    case DP_M6502_IMM: snprintf(out, size, "%s #$%02X", name, lo); break;
    case DP_M6502_ZP: snprintf(out, size, "%s $%02X", name, lo); break;
    case DP_M6502_ZPX: snprintf(out, size, "%s $%02X,X", name, lo); break;
    case DP_M6502_ZPY: snprintf(out, size, "%s $%02X,Y", name, lo); break;
    case DP_M6502_ABS: snprintf(out, size, "%s $%04X", name, lo | (hi << 8)); break;
    case DP_M6502_ABSX: snprintf(out, size, "%s $%04X,X", name, lo | (hi << 8)); break;
    case DP_M6502_ABSY: snprintf(out, size, "%s $%04X,Y", name, lo | (hi << 8)); break;
    case DP_M6502_INDX: snprintf(out, size, "%s ($%02X,X)", name, lo); break;
    case DP_M6502_INDY: snprintf(out, size, "%s ($%02X),Y", name, lo); break;
    case DP_M6502_IND: snprintf(out, size, "%s ($%04X)", name, lo | (hi << 8)); break;
    case DP_M6502_REL: snprintf(out, size, "%s $%04X", name, in->target); break;
    default: snprintf(out, size, "%s", name); break;
    }
}

// --- executor ---------------------------------------------------------------

#define RD(a) cpu->read(cpu->user, (uint16_t)(a))
#define WR(a,v) cpu->write(cpu->user, (uint16_t)(a), (v))

// the executor accounts cycles per instruction: row base + page-cross
// penalties; the fetch helpers below only move PC.
static uint8_t fetch8(m6502_t *cpu) {
    uint8_t v = RD(cpu->pc);
    cpu->pc = (uint16_t)(cpu->pc + 1);
    return v;
}

static void push(m6502_t *cpu, uint8_t v) { WR((uint16_t)(0x0100 | cpu->s), v); cpu->s--; }
static uint8_t pop(m6502_t *cpu) { cpu->s++; return RD((uint16_t)(0x0100 | cpu->s)); }

static void set_zn(m6502_t *cpu, uint8_t v) {
    set_flag(cpu, 0x02, !(v & 0x7F));
    set_flag(cpu, 0x80, v & 0x80);
}

// effective address of a memory-reference instruction
static uint16_t effective(m6502_t *cpu, uint8_t mode, uint8_t lo, uint8_t hi, unsigned *penalty) {
    uint16_t addr;
    switch (mode) {
    case DP_M6502_IMM: return (uint16_t)(cpu->pc - 1);
    case DP_M6502_ZP: return lo;
    case DP_M6502_ZPX: return (uint8_t)(lo + cpu->x);
    case DP_M6502_ZPY: return (uint8_t)(lo + cpu->y);
    case DP_M6502_ABS: return (uint16_t)(lo | ((uint16_t)hi << 8));
    case DP_M6502_ABSX: case DP_M6502_ABSY: {
        uint16_t base = (uint16_t)(lo | ((uint16_t)hi << 8));
        uint8_t idx = mode == DP_M6502_ABSX ? cpu->x : cpu->y;
        addr = (uint16_t)(base + idx);
        *penalty += (base & 0xFF00) != (addr & 0xFF00);
        return addr;
    }
    case DP_M6502_INDX: {
        uint8_t zp = (uint8_t)(lo + cpu->x);
        return (uint16_t)(RD(zp) | ((uint16_t)RD((uint8_t)(zp + 1)) << 8));
    }
    case DP_M6502_INDY: {
        uint16_t base = (uint16_t)(RD(lo) | ((uint16_t)RD((uint8_t)(lo + 1)) << 8));
        addr = (uint16_t)(base + cpu->y);
        *penalty += (base & 0xFF00) != (addr & 0xFF00);
        return addr;
    }
    case DP_M6502_IND: {
        // the 6502 page-boundary alias: the pointer high byte never increments
        uint16_t base = (uint16_t)(lo | ((uint16_t)hi << 8));
        uint8_t l = RD(base);
        uint8_t h = RD((uint16_t)((base & 0xFF00) | (uint8_t)(base + 1)));
        return (uint16_t)(l | ((uint16_t)h << 8));
    }
    default: return 0;
    }
}

static void branch(m6502_t *cpu, int condition, uint8_t offset, unsigned *cycles) {
    if (!dp_m6502_condition(condition, cpu)) return;
    uint16_t base = cpu->pc;
    cpu->pc = (uint16_t)(cpu->pc + (int8_t)offset);
    *cycles += (base & 0xFF00) != (cpu->pc & 0xFF00);
}

static void compare(m6502_t *cpu, uint8_t reg, uint8_t v) {
    uint8_t r = (uint8_t)(reg - v);
    set_flag(cpu, 0x01, reg >= v);
    set_zn(cpu, r);
}

static uint8_t adc(m6502_t *cpu, uint8_t a, uint8_t v) {
    unsigned sum = a + v + (cpu->p & 0x01);
    set_flag(cpu, 0x01, sum > 0xFF);
    set_flag(cpu, 0x40, (~(a ^ v) & (a ^ sum)) & 0x80);
    set_zn(cpu, (uint8_t)sum);
    return (uint8_t)sum;
}

// borrow propagates through the unsigned wraparound: diff > 0xFF means the
// true result was negative
static uint8_t sbc(m6502_t *cpu, uint8_t a, uint8_t v) {
    unsigned diff = a - v - !(cpu->p & 0x01);
    set_flag(cpu, 0x01, diff <= 0xFF);
    set_flag(cpu, 0x40, ((a ^ v) & (a ^ diff)) & 0x80);
    set_zn(cpu, (uint8_t)diff);
    return (uint8_t)diff;
}

static uint8_t asl(m6502_t *cpu, uint8_t v) { set_flag(cpu, 0x01, v & 0x80); v = (uint8_t)(v << 1); set_zn(cpu, v); return v; }
static uint8_t lsr(m6502_t *cpu, uint8_t v) { set_flag(cpu, 0x01, v & 0x01); v = (uint8_t)(v >> 1); set_zn(cpu, v); return v; }
static uint8_t rol(m6502_t *cpu, uint8_t v) {
    uint8_t c = (uint8_t)(cpu->p & 0x01);
    set_flag(cpu, 0x01, v & 0x80);
    v = (uint8_t)((v << 1) | c);
    set_zn(cpu, v);
    return v;
}
static uint8_t ror(m6502_t *cpu, uint8_t v) {
    uint8_t c = (uint8_t)((cpu->p & 0x01) << 7);
    set_flag(cpu, 0x01, v & 0x01);
    v = (uint8_t)((v >> 1) | c);
    set_zn(cpu, v);
    return v;
}

static void bit(m6502_t *cpu, uint8_t a, uint8_t v) {
    set_flag(cpu, 0x80, v & 0x80);
    set_flag(cpu, 0x40, v & 0x40);
    set_flag(cpu, 0x02, !(a & v));
}

// hardware dispatch: push PC and P (B set), set I, vector through `vector`
static void dispatch(m6502_t *cpu, uint16_t vector) {
    push(cpu, (uint8_t)(cpu->pc >> 8));
    push(cpu, (uint8_t)cpu->pc);
    push(cpu, (uint8_t)(cpu->p | 0x10));
    set_flag(cpu, 0x04, 1);
    cpu->pc = (uint16_t)(RD(vector) | ((uint16_t)RD((uint16_t)(vector + 1)) << 8));
    cpu->cyc += 7;
}

unsigned m6502_service(m6502_t *cpu) {
    if (cpu->nmi && !cpu->nmi_level) cpu->nmi_pending = 1;
    cpu->nmi_level = cpu->nmi;
    if (cpu->nmi_pending) { cpu->nmi_pending = 0; dispatch(cpu, 0xFFFA); return 7; }
    if (cpu->irq && !(cpu->p & 0x04)) { dispatch(cpu, 0xFFFE); return 7; }
    return 2;
}

void m6502_reset(m6502_t *cpu) {
    cpu->p = 0x34; // I set, bit 5 always 1
    cpu->s = 0xFD;
    cpu->nmi_pending = 0;
    cpu->nmi_level = 0;
    cpu->jammed = 0;
    cpu->pc = (uint16_t)(RD(0xFFFC) | ((uint16_t)RD(0xFFFD) << 8));
}

unsigned m6502_step(m6502_t *cpu) {
    if (cpu->jammed) return 2;
    uint8_t op = fetch8(cpu);
    const M6502Row *row = &rows[op];
    if (row->mode == 255) { cpu->jammed = 1; return 0; }
    unsigned cycles = row->cycles, penalty = 0;
    uint8_t lo = 0, hi = 0;
    if (row->mode != DP_M6502_IMP) lo = fetch8(cpu);
    if (row->mode == DP_M6502_ABS || row->mode == DP_M6502_ABSX ||
        row->mode == DP_M6502_ABSY || row->mode == DP_M6502_IND) hi = fetch8(cpu);
    uint16_t addr;
    switch (op) {
    case 0x00: // BRK: software dispatch through the IRQ vector
        fetch8(cpu); // padding byte
        dispatch(cpu, 0xFFFE);
        break;
    case 0x20: // JSR: push the last operand byte's address (pc - 1)
        push(cpu, (uint8_t)((cpu->pc - 1) >> 8));
        push(cpu, (uint8_t)(cpu->pc - 1));
        cpu->pc = (uint16_t)(lo | ((uint16_t)hi << 8));
        break;
    case 0x40: // RTI
        cpu->p = (uint8_t)((pop(cpu) | 0x20) & ~0x10);
        cpu->pc = (uint16_t)(pop(cpu) | ((uint16_t)pop(cpu) << 8));
        break;
    case 0x60: // RTS: the return address is off by one
        cpu->pc = (uint16_t)(pop(cpu) | ((uint16_t)pop(cpu) << 8));
        cpu->pc = (uint16_t)(cpu->pc + 1);
        break;
    case 0x48: push(cpu, cpu->a); break;
    case 0x08: push(cpu, (uint8_t)(cpu->p | 0x10)); break;
    case 0x68: cpu->a = pop(cpu); set_zn(cpu, cpu->a); break;
    case 0x28: cpu->p = (uint8_t)((pop(cpu) | 0x20) & ~0x10); break;
    case 0x4C: cpu->pc = (uint16_t)(lo | ((uint16_t)hi << 8)); break;
    case 0x6C: cpu->pc = effective(cpu, DP_M6502_IND, lo, hi, &penalty); break;
    case 0x10: case 0x30: case 0x50: case 0x70:
    case 0x90: case 0xB0: case 0xD0: case 0xF0:
        cycles += 1; // unconditional branch cost
        branch(cpu, branch_condition(op), lo, &cycles);
        break;
    case 0x0A: cpu->a = asl(cpu, cpu->a); break;
    case 0x2A: cpu->a = rol(cpu, cpu->a); break;
    case 0x4A: cpu->a = lsr(cpu, cpu->a); break;
    case 0x6A: cpu->a = ror(cpu, cpu->a); break;
    case 0x18: set_flag(cpu, 0x01, 0); break;
    case 0x38: set_flag(cpu, 0x01, 1); break;
    case 0x58: set_flag(cpu, 0x04, 0); break;
    case 0x78: set_flag(cpu, 0x04, 1); break;
    case 0xB8: set_flag(cpu, 0x40, 0); break;
    case 0xD8: set_flag(cpu, 0x08, 0); break;
    case 0xF8: set_flag(cpu, 0x08, 1); break;
    case 0xEA: break;
    case 0x8A: cpu->a = cpu->x; set_zn(cpu, cpu->a); break;
    case 0x98: cpu->a = cpu->y; set_zn(cpu, cpu->a); break;
    case 0xA8: cpu->y = cpu->a; set_zn(cpu, cpu->y); break;
    case 0xAA: cpu->x = cpu->a; set_zn(cpu, cpu->x); break;
    case 0xBA: cpu->x = cpu->s; set_zn(cpu, cpu->x); break;
    case 0x9A: cpu->s = cpu->x; break;
    case 0xE8: cpu->x++; set_zn(cpu, cpu->x); break;
    case 0xC8: cpu->y++; set_zn(cpu, cpu->y); break;
    case 0xCA: cpu->x--; set_zn(cpu, cpu->x); break;
    case 0x88: cpu->y--; set_zn(cpu, cpu->y); break;
    default: {
        addr = effective(cpu, row->mode, lo, hi, &penalty);
        switch (row->access) {
        case ACC_R: {
            uint8_t v = RD(addr);
            switch (op) {
            case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9:
            case 0xA1: case 0xB1: cpu->a = v; set_zn(cpu, v); break;
            case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE: cpu->x = v; set_zn(cpu, v); break;
            case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC: cpu->y = v; set_zn(cpu, v); break;
            case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9:
            case 0xC1: case 0xD1: compare(cpu, cpu->a, v); break;
            case 0xE0: case 0xE4: case 0xEC: compare(cpu, cpu->x, v); break;
            case 0xC0: case 0xC4: case 0xCC: compare(cpu, cpu->y, v); break;
            case 0x24: case 0x2C: bit(cpu, cpu->a, v); break;
            case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39:
            case 0x21: case 0x31: cpu->a &= v; set_zn(cpu, cpu->a); break;
            case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19:
            case 0x01: case 0x11: cpu->a |= v; set_zn(cpu, cpu->a); break;
            case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59:
            case 0x41: case 0x51: cpu->a ^= v; set_zn(cpu, cpu->a); break;
            case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79:
            case 0x61: case 0x71: cpu->a = adc(cpu, cpu->a, v); break;
            case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9:
            case 0xE1: case 0xF1: case 0xEB: cpu->a = sbc(cpu, cpu->a, v); break; // $EB: USBC alias
            // illegal loads
            case 0xA3: case 0xA7: case 0xAB: case 0xAF: case 0xB3: case 0xB7:
            case 0xBF: cpu->a = cpu->x = v; set_zn(cpu, v); break; // LAX/LXA
            case 0x0B: case 0x2B: // ANC: AND, C = N
                cpu->a &= v; set_zn(cpu, cpu->a); set_flag(cpu, 0x01, cpu->a & 0x80); break;
            case 0x4B: // ALR: AND then LSR A
                cpu->a &= v; cpu->a = lsr(cpu, cpu->a); break;
            case 0x6B: // ARR: AND then ROR with C from bit 6/7 blend
                cpu->a &= v;
                cpu->a = ror(cpu, cpu->a);
                set_flag(cpu, 0x01, cpu->a & 0x80);
                set_flag(cpu, 0x40, ((cpu->a >> 7) ^ (cpu->a >> 6)) & 1);
                break;
            case 0x8B: // XAA (the unstable one settles to A = X & M)
                cpu->a = (uint8_t)(cpu->x & v); set_zn(cpu, cpu->a); break;
            case 0xCB: { // SBX: X = (A & X) - M
                uint8_t t = (uint8_t)(cpu->a & cpu->x);
                compare(cpu, t, v);
                cpu->x = (uint8_t)(t - v);
                break;
            }
            case 0x04: case 0x14: case 0x44: case 0x64: case 0x34: case 0x54:
            case 0x74: case 0xD4: case 0xF4: case 0x0C: case 0x1C: case 0x3C:
            case 0x5C: case 0x7C: case 0xDC: case 0xFC: case 0x80: case 0x82:
            case 0x89: case 0xC2: case 0xE2:
            case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
                break; // NOP family: operand fetch only, no effect
            case 0xBB: // LAS: A = X = S = M & S
                cpu->a = cpu->x = cpu->s = (uint8_t)(v & cpu->s);
                set_zn(cpu, cpu->a);
                break;
            default: cpu->jammed = 1; break;
            }
            break;
        }
        case ACC_W:
            switch (op) {
            case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81:
            case 0x91: WR(addr, cpu->a); break;
            case 0x86: case 0x96: case 0x8E: WR(addr, cpu->x); break;
            case 0x84: case 0x94: case 0x8C: WR(addr, cpu->y); break;
            // illegal stores: SAX is A & X; SHA/SHX/SHY and TAS use the
            // high byte of the effective address plus one (the unstable
            // A12-borrow variants settle to this on every real 6502)
            case 0x83: case 0x87: case 0x8F: case 0x97:
                WR(addr, (uint8_t)(cpu->a & cpu->x));
                break;
            case 0x93: case 0x9B: case 0x9F:
                cpu->s = (uint8_t)(cpu->a & cpu->x);
                WR(addr, (uint8_t)(cpu->s & (((addr >> 8) + 1) & 0xFF)));
                break;
            case 0x9C: WR(addr, (uint8_t)(cpu->y & (((addr >> 8) + 1) & 0xFF))); break;
            case 0x9E: WR(addr, (uint8_t)(cpu->x & (((addr >> 8) + 1) & 0xFF))); break;
            default: cpu->jammed = 1; break;
            }
            break;
        case ACC_RMW: {
            if (row->mode == DP_M6502_IMP) { cpu->jammed = 1; break; }
            uint8_t v = RD(addr);
            switch (op) {
            case 0x06: case 0x16: case 0x0E: case 0x1E: v = asl(cpu, v); break;
            case 0x26: case 0x36: case 0x2E: case 0x3E: v = rol(cpu, v); break;
            case 0x46: case 0x56: case 0x4E: case 0x5E: v = lsr(cpu, v); break;
            case 0x66: case 0x76: case 0x6E: case 0x7E: v = ror(cpu, v); break;
            case 0xC6: case 0xD6: case 0xCE: case 0xDE: v = (uint8_t)(v - 1); set_zn(cpu, v); break;
            case 0xE6: case 0xF6: case 0xEE: case 0xFE: v = (uint8_t)(v + 1); set_zn(cpu, v); break;
            // illegal RMW combos: memory op then register ALU op
            case 0x03: case 0x07: case 0x0F: case 0x13: case 0x17: case 0x1B: case 0x1F:
                v = asl(cpu, v); cpu->a |= v; set_zn(cpu, cpu->a); break; // SLO
            case 0x23: case 0x27: case 0x2F: case 0x33: case 0x37: case 0x3B: case 0x3F:
                v = rol(cpu, v); cpu->a &= v; set_zn(cpu, cpu->a); break; // RLA
            case 0x43: case 0x47: case 0x4F: case 0x53: case 0x57: case 0x5B: case 0x5F:
                v = lsr(cpu, v); cpu->a ^= v; set_zn(cpu, cpu->a); break; // SRE
            case 0x63: case 0x67: case 0x6F: case 0x73: case 0x77: case 0x7B: case 0x7F:
                v = ror(cpu, v); cpu->a = adc(cpu, cpu->a, v); break;     // RRA
            case 0xC3: case 0xC7: case 0xCF: case 0xD3: case 0xD7: case 0xDB: case 0xDF:
                v = (uint8_t)(v - 1); compare(cpu, cpu->a, v); break;     // DCP
            case 0xE3: case 0xE7: case 0xEF: case 0xF3: case 0xF7: case 0xFB: case 0xFF:
                v = (uint8_t)(v + 1); cpu->a = sbc(cpu, cpu->a, v); break; // ISB
            default: cpu->jammed = 1; break;
            }
            WR(addr, v);
            break;
        }
        default:
            cpu->jammed = 1;
            break;
        }
        break;
    }
    }
    cpu->cyc += cycles + penalty;
    return cycles + penalty;
}
