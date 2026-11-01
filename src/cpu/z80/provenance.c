#include "provenance.h"

static unsigned slot(unsigned r,unsigned index,int plain) {
    if(index && !plain && (r==4 || r==5))
        return (index==1?DP_Z80_TAG_IXH:DP_Z80_TAG_IYH)+(r-4);
    return r;
}
static unsigned high(unsigned pair,unsigned index,int stack) {
    if(pair<2) return pair*2;
    if(pair==2) return slot(4,index,0);
    return stack?DP_Z80_TAG_A:DP_Z80_TAG_SPH;
}
static unsigned low(unsigned pair,unsigned index,int stack) {
    if(pair==3 && stack) return DP_Z80_TAG_F;
    return high(pair,index,stack)+1;
}
static void kill_pair(DpZ80Provenance *p,unsigned pair,unsigned index) {
    p->reg[high(pair,index,0)]=p->reg[low(pair,index,0)]=0;
}
static void swap(DpZ80Origin *a,DpZ80Origin *b) { DpZ80Origin t=*a; *a=*b; *b=t; }
static void write(DpZ80Provenance *p,uint16_t a,DpZ80Origin origin) {
    if(p->write_count<2) {
        p->writes[p->write_count].address=a;
        p->writes[p->write_count++].origin=origin;
    }
}
static void store_pair(DpZ80Provenance *p,unsigned pair,unsigned index,int stack,uint16_t a,int push) {
    DpZ80Origin h=p->reg[high(pair,index,stack)],l=p->reg[low(pair,index,stack)];
    if(push) { write(p,(uint16_t)(a-1),h); write(p,(uint16_t)(a-2),l); }
    else { write(p,a,l); write(p,(uint16_t)(a+1),h); }
}
static void load_pair(DpZ80Provenance *p,unsigned pair,unsigned index,int stack,
                      uint16_t a,DpZ80OriginRead read,void *user) {
    DpZ80Origin l=read(user,a),h=read(user,(uint16_t)(a+1));
    p->reg[high(pair,index,stack)]=h; p->reg[low(pair,index,stack)]=l;
}
void dp_z80_provenance_clear_plan(DpZ80Provenance *p) {
    p->write_count=p->write_at=0; p->output=0;
}
void dp_z80_provenance_stack_changed(DpZ80Provenance *p) { kill_pair(p,3,0); }
DpZ80Origin dp_z80_provenance_write(DpZ80Provenance *p,uint16_t address) {
    if(p->write_at>=p->write_count) return 0;
    unsigned at=p->write_at++;
    return p->writes[at].address==address?p->writes[at].origin:0;
}
void dp_z80_provenance_force(DpZ80Provenance *p,const DpZ80Instruction *in,
                             const z80_t *before,const z80_t *after) {
    if(in->flow==DP_Z80_REPEAT) {
        if((in->raw.opcode&3)>=2) p->reg[DP_Z80_TAG_B]=0;
        else kill_pair(p,0,0);
        if(before->a!=after->a) p->reg[DP_Z80_TAG_A]=0;
    } else if(in->condition==8) p->reg[DP_Z80_TAG_B]=0;
    else if(in->condition>=0) p->reg[DP_Z80_TAG_F]=0;
}
void dp_z80_provenance_begin(DpZ80Provenance *t,const z80_t *c,
                             const z80_decoded_t *d,DpZ80OriginRead read,void *u) {
    unsigned op=d->opcode,x=op>>6,y=(op>>3)&7,z=op&7,p=y>>1,q=y&1;
    unsigned index=d->index;
    uint16_t hl=index==1?c->ix:index==2?c->iy:c->hl;
    uint16_t address=(uint16_t)(hl+(index?(int8_t)d->displacement:0));
    uint16_t immediate=(uint16_t)(c->pc+d->length-1);
    dp_z80_provenance_clear_plan(t);
    if(d->group==1) {
        if(x==0 || x==1) t->reg[DP_Z80_TAG_F]=0;
        if(x!=1) {
            if(index || z==6) write(t,address,0);
            else t->reg[slot(z,0,1)]=0;
            if(index && z!=6) t->reg[slot(z,0,1)]=0;
        }
        return;
    }
    if(d->group==2) {
        if(x==1) switch(z) {
        case 0:
            if(y!=6) t->reg[slot(y,0,1)]=0;
            t->reg[DP_Z80_TAG_F]=0; break;
        case 1: t->output=y==6?0:t->reg[slot(y,0,1)]; break;
        case 2: kill_pair(t,2,0); t->reg[DP_Z80_TAG_F]=0; break;
        case 3:
            if(q) load_pair(t,p,0,0,d->immediate,read,u);
            else store_pair(t,p,0,0,d->immediate,0);
            break;
        case 4: t->reg[DP_Z80_TAG_A]=t->reg[DP_Z80_TAG_F]=0; break;
        case 5: dp_z80_provenance_stack_changed(t); break;
        case 7:
            if(y==0) t->reg[DP_Z80_TAG_I]=t->reg[DP_Z80_TAG_A];
            // R is refreshed by every instruction, including LD A,R.
            else if(y==2 || y==3) {
                t->reg[DP_Z80_TAG_A]=y==2?t->reg[DP_Z80_TAG_I]:0;
                t->reg[DP_Z80_TAG_F]=0;
            } else if(y==4 || y==5) {
                write(t,c->hl,0); t->reg[DP_Z80_TAG_A]=t->reg[DP_Z80_TAG_F]=0;
            }
            break;
        }
        else if(x==2 && y>=4 && z<=3) {
            if(z==0) {
                write(t,c->de,read(u,c->hl)); kill_pair(t,1,0); kill_pair(t,0,0);
            } else if(z==1) kill_pair(t,0,0);
            else {
                if(z==2) write(t,c->hl,0);
                else t->output=read(u,c->hl);
                t->reg[DP_Z80_TAG_B]=0;
            }
            kill_pair(t,2,0); t->reg[DP_Z80_TAG_F]=0;
        }
        return;
    }
    if(x==0) switch(z) {
    case 0:
        if(y==1) {
            swap(&t->reg[DP_Z80_TAG_A],&t->reg[DP_Z80_TAG_A2]);
            swap(&t->reg[DP_Z80_TAG_F],&t->reg[DP_Z80_TAG_F2]);
        } else if(y==2) t->reg[DP_Z80_TAG_B]=0;
        break;
    case 1:
        if(q) { kill_pair(t,2,index); t->reg[DP_Z80_TAG_F]=0; }
        else load_pair(t,p,index,0,(uint16_t)(immediate-1),read,u);
        break;
    case 2: {
        uint16_t a=p==0?c->bc:p==1?c->de:d->immediate;
        if(p==2) {
            if(q) load_pair(t,2,index,0,a,read,u); else store_pair(t,2,index,0,a,0);
        } else if(q) t->reg[DP_Z80_TAG_A]=read(u,a);
        else write(t,a,t->reg[DP_Z80_TAG_A]);
        break;
    }
    case 3: kill_pair(t,p,index); break;
    case 4: case 5:
        if(y==6) write(t,address,0); else t->reg[slot(y,index,0)]=0;
        t->reg[DP_Z80_TAG_F]=0; break;
    case 6:
        if(y==6) write(t,address,read(u,immediate));
        else t->reg[slot(y,index,0)]=read(u,immediate);
        break;
    case 7:
        if(y<6) t->reg[DP_Z80_TAG_A]=0;
        t->reg[DP_Z80_TAG_F]=0; break;
    }
    else if(x==1) {
        if(op!=0x76) {
            int plain=y==6 || z==6;
            DpZ80Origin origin=z==6?read(u,address):t->reg[slot(z,index,plain)];
            if(y==6) write(t,address,origin); else t->reg[slot(y,index,plain)]=origin;
        }
    } else if(x==2) {
        if(y!=7) t->reg[DP_Z80_TAG_A]=0;
        t->reg[DP_Z80_TAG_F]=0;
    } else switch(z) {
    case 0:
        if(dp_z80_condition((int)y,c)) dp_z80_provenance_stack_changed(t);
        break;
    case 1:
        if(!q) {
            load_pair(t,p,index,1,c->sp,read,u); dp_z80_provenance_stack_changed(t);
        } else if(p==0) dp_z80_provenance_stack_changed(t);
        else if(p==1) {
            for(unsigned r=0;r<6;r++) swap(&t->reg[r],&t->reg[DP_Z80_TAG_B2+r]);
        } else if(p==3) {
            t->reg[DP_Z80_TAG_SPH]=t->reg[slot(4,index,0)];
            t->reg[DP_Z80_TAG_SPL]=t->reg[slot(5,index,0)];
        }
        break;
    case 3:
        if(y==2) t->output=t->reg[DP_Z80_TAG_A];
        else if(y==3) t->reg[DP_Z80_TAG_A]=0;
        else if(y==4) {
            DpZ80Origin l=read(u,c->sp),h=read(u,(uint16_t)(c->sp+1));
            store_pair(t,2,index,0,c->sp,0);
            t->reg[slot(4,index,0)]=h; t->reg[slot(5,index,0)]=l;
        } else if(y==5) {
            swap(&t->reg[DP_Z80_TAG_D],&t->reg[DP_Z80_TAG_H]);
            swap(&t->reg[DP_Z80_TAG_E],&t->reg[DP_Z80_TAG_L]);
        }
        break;
    case 4:
        if(dp_z80_condition((int)y,c)) {
            write(t,(uint16_t)(c->sp-1),0); write(t,(uint16_t)(c->sp-2),0);
            dp_z80_provenance_stack_changed(t);
        }
        break;
    case 5:
        if(!q) store_pair(t,p,index,1,c->sp,1);
        else { write(t,(uint16_t)(c->sp-1),0); write(t,(uint16_t)(c->sp-2),0); }
        dp_z80_provenance_stack_changed(t); break;
    case 6:
        if(y!=7) t->reg[DP_Z80_TAG_A]=0;
        t->reg[DP_Z80_TAG_F]=0; break;
    case 7:
        write(t,(uint16_t)(c->sp-1),0); write(t,(uint16_t)(c->sp-2),0);
        dp_z80_provenance_stack_changed(t); break;
    }
}
