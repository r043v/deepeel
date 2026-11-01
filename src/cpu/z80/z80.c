#include "z80.h"
#include <stdio.h>
#include <string.h>

typedef struct { const uint8_t *bytes; size_t size, reads; uint16_t pc; int short_read; } View;
static uint8_t read_view(void *user, uint16_t addr) {
    View *v = user;
    size_t offset = (uint16_t)(addr - v->pc);
    v->reads++;
    if (offset >= v->size) { v->short_read = 1; return 0; }
    return v->bytes[offset];
}
int dp_z80_decode(const uint8_t *bytes, size_t size, uint16_t pc, DpZ80Instruction *out) {
    if (!bytes || !size || !out) return 0;
    View v = {bytes, size > 65536 ? 65536 : size, 0, pc, 0};
    z80_t c = {0}; c.user = &v; c.read = read_view; c.pc = pc;
    memset(out, 0, sizeof(*out)); out->condition = -1;
    if (!z80_decode(&c, &out->raw) || v.short_read) return 0;
    unsigned op = out->raw.opcode, y = (op >> 3) & 7;
    if (out->raw.group == 1) return 1;
    if (out->raw.group == 2) {
        if ((op & 0xc7) == 0x45) out->flow = DP_Z80_RETURN; // RETN/RETI aliases
        else if ((op & 0xf4) == 0xb0) {
            out->flow = DP_Z80_REPEAT; out->target = (uint16_t)(pc + out->raw.length - 2);
        }
        return 1;
    }
    if ((op & 0xc7) == 0xc0) { out->flow = DP_Z80_RETURN; out->condition = y; }
    else if (op == 0xc9) out->flow = DP_Z80_RETURN;
    else if (op == 0xe9) out->flow = DP_Z80_INDIRECT;
    else if (op == 0x76) out->flow = DP_Z80_HALT;
    else if (op == 0xc3 || (op & 0xc7) == 0xc2) {
        out->flow = DP_Z80_JUMP; out->target = out->raw.immediate;
        if (op != 0xc3) out->condition = y;
    } else if (op == 0xcd || (op & 0xc7) == 0xc4) {
        out->flow = DP_Z80_CALL; out->target = out->raw.immediate;
        if (op != 0xcd) out->condition = y;
    } else if ((op & 0xc7) == 0xc7) { out->flow = DP_Z80_CALL; out->target = op & 0x38; }
    else if (op == 0x10 || op == 0x18 || (op & 0xe7) == 0x20) {
        out->flow = DP_Z80_JUMP;
        out->target = (uint16_t)(pc + out->raw.length + (int8_t)out->raw.immediate);
        if (op == 0x10) out->condition = 8;
        else if (op != 0x18) out->condition = (op >> 3) & 3;
    }
    return 1;
}
static const uint8_t flags[] = { Z80_Z, Z80_C, Z80_PV, Z80_S };
int dp_z80_condition(int condition, const z80_t *cpu) {
    if (condition == 8) return (uint8_t)(cpu->b - 1) != 0;
    if (condition < 0 || condition > 7) return condition == -1;
    return !!(cpu->f & flags[condition >> 1]) == (condition & 1);
}
int dp_z80_force(int condition, z80_t *cpu, int taken) {
    if (condition < 0 || condition > 8) return 0;
    if (condition == 8) { cpu->b = taken ? 2 : 1; return 1; }
    unsigned mask = flags[condition >> 1];
    if (!!taken == (condition & 1)) cpu->f |= mask;
    else cpu->f &= (uint8_t)~mask;
    return 1;
}
static const char *const cc[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
int dp_z80_input_port(const z80_decoded_t *d,const z80_t *c,uint16_t *port) {
    if(d->group==0 && d->opcode==0xdb) {
        *port=(uint16_t)((c->a<<8)|(uint8_t)d->immediate); return 1;
    }
    if(d->group==2 && ((d->opcode&0xc7)==0x40 || (d->opcode&0xe7)==0xa2)) {
        *port=c->bc; return 1;
    }
    return 0;
}
int dp_z80_repeat_taken(const DpZ80Instruction *in,const z80_t *c,uint8_t source) {
    if(in->flow!=DP_Z80_REPEAT) return 0;
    unsigned type=in->raw.opcode&3;
    if(type>=2) return (uint8_t)(c->b-1)!=0;
    return (uint16_t)(c->bc-1)!=0 && (type!=1 || c->a!=source);
}
int dp_z80_force_repeat(const DpZ80Instruction *in,z80_t *c,uint8_t source,int taken) {
    if(in->flow!=DP_Z80_REPEAT) return 0;
    unsigned type=in->raw.opcode&3;
    if(type>=2) c->b=taken?2:1;
    else {
        c->bc=taken?2:1;
        if(taken && type==1 && c->a==source) c->a^=1;
    }
    return 1;
}
static const char *const regs[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
static const char *const pairs[] = {"BC", "DE", "HL", "SP"};
static const char *const alu[] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
static void regname(char *out, size_t n, unsigned r, const z80_decoded_t *d, int substitute) {
    const char *ix = d->index == 1 ? "IX" : "IY";
    if (d->index && r == 6) snprintf(out, n, "(%s%+d)", ix, (int8_t)d->displacement);
    else if (d->index && substitute && (r == 4 || r == 5)) snprintf(out, n, "%s%c", ix, r == 4 ? 'H' : 'L');
    else snprintf(out, n, "%s", regs[r]);
}
void dp_z80_format(const DpZ80Instruction *in, char *out, size_t size) {
    if (!out || !size) return;
    const z80_decoded_t *d = &in->raw;
    unsigned op=d->opcode, x=op>>6, y=(op>>3)&7, z=op&7, p=y>>1, q=y&1;
    const char *hl = !d->index ? "HL" : d->index == 1 ? "IX" : "IY";
    const char *pair = p == 2 ? hl : pairs[p];
    char a[24], b[24];
    regname(a,sizeof(a),y,d,!(x==1 && z==6));
    regname(b,sizeof(b),z,d,!(x==1 && y==6));
#define F(...) snprintf(out,size,__VA_ARGS__)
    if (d->group==1) {
        static const char *const rot[]={"RLC","RRC","RL","RR","SLA","SRA","SLL","SRL"};
        char mem[24]; regname(mem,sizeof(mem),d->index?6:z,d,0);
        if (!x) F("%s %s",rot[y],mem); else F("%s %u,%s",x==1?"BIT":x==2?"RES":"SET",y,mem);
        if(d->index && z!=6 && x!=1) {
            size_t used = strlen(out);
            if(used<size) snprintf(out+used,size-used,",%s",regs[z]);
        }
        return;
    }
    if (d->group==2) {
        if(x==1) {
            switch(z) {
            case 0: if(y==6) F("IN (C)"); else F("IN %s,(C)",regs[y]); return;
            case 1: F("OUT (C),%s",y==6?"0":regs[y]); return;
            case 2: F("%s HL,%s",q?"ADC":"SBC",pairs[p]); return;
            case 3: if(q) F("LD %s,($%04X)",pairs[p],d->immediate); else F("LD ($%04X),%s",d->immediate,pairs[p]); return;
            case 4: F("NEG"); return;
            case 5: F("%s",op==0x4d?"RETI":"RETN"); return;
            case 6: { static const int im[]={0,0,1,2,0,0,1,2}; F("IM %d",im[y]); return; }
            case 7: { static const char *const s[]={"LD I,A","LD R,A","LD A,I","LD A,R","RRD","RLD","NOP [ED]","NOP [ED]"}; F("%s",s[y]); return; }
            }
        }
        if(x==2 && y>=4 && z<=3) {
            static const char *const block[4][4]={{"LDI","CPI","INI","OUTI"},{"LDD","CPD","IND","OUTD"},{"LDIR","CPIR","INIR","OTIR"},{"LDDR","CPDR","INDR","OTDR"}};
            F("%s",block[y-4][z]); return;
        }
        F("NOP [ED $%02X]",op); return;
    }
    if(x==0) {
        switch(z) {
        case 0:
            if(y<2) F("%s",y?"EX AF,AF'":"NOP");
            else if(y==2) F("DJNZ $%04X",in->target);
            else if(y==3) F("JR $%04X",in->target);
            else F("JR %s,$%04X",cc[y-4],in->target);
            return;
        case 1: if(q) F("ADD %s,%s",hl,pair); else F("LD %s,$%04X",pair,d->immediate); return;
        case 2:
            if(p<2) { if(q) F("LD A,(%s)",pairs[p]); else F("LD (%s),A",pairs[p]); }
            else { const char *r=p==2?hl:"A"; if(q) F("LD %s,($%04X)",r,d->immediate); else F("LD ($%04X),%s",d->immediate,r); }
            return;
        case 3: F("%s %s",q?"DEC":"INC",pair); return;
        case 4: case 5: F("%s %s",z==4?"INC":"DEC",a); return;
        case 6: F("LD %s,$%02X",a,d->immediate); return;
        case 7: { static const char *const s[]={"RLCA","RRCA","RLA","RRA","DAA","CPL","SCF","CCF"}; F("%s",s[y]); return; }
        }
    }
    if(x==1) { if(op==0x76) F("HALT"); else F("LD %s,%s",a,b); return; }
    if(x==2) { F("%s%s",alu[y],b); return; }
    switch(z) {
    case 0: F("RET %s",cc[y]); return;
    case 1:
        if(!q) F("POP %s",p==3?"AF":pair);
        else if(p==0) F("RET"); else if(p==1) F("EXX"); else if(p==2) F("JP (%s)",hl); else F("LD SP,%s",hl);
        return;
    case 2: F("JP %s,$%04X",cc[y],d->immediate); return;
    case 3:
        switch(y) {
        case 0: F("JP $%04X",d->immediate); return;
        case 2: F("OUT ($%02X),A",d->immediate); return;
        case 3: F("IN A,($%02X)",d->immediate); return;
        case 4: F("EX (SP),%s",hl); return;
        case 5: F("EX DE,HL"); return;
        case 6: F("DI"); return;
        case 7: F("EI"); return;
        }
        break;
    case 4: F("CALL %s,$%04X",cc[y],d->immediate); return;
    case 5: if(!q) F("PUSH %s",p==3?"AF":pair); else F("CALL $%04X",d->immediate); return;
    case 6: F("%s$%02X",alu[y],d->immediate); return;
    case 7: F("RST $%02X",op&0x38); return;
    }
    F("DB $%02X",op);
#undef F
}
