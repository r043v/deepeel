#include "closure.h"
#include "../../core/dpmachine.h"
#include "explore.h"
#include "../../cpu/z80/provenance.h"
#include <stdlib.h>
#include <string.h>

// A 1 KiB segment describes fixed prefixes, 8/16 KiB mapper/slot windows,
// mirrors and partial banks without conflating CPU and physical keys.
typedef struct {
    int32_t base[64];
    uint16_t mask;
    uint8_t mapper,cart;
    uint64_t present,immutable,ram;
} View;
typedef struct {
    uint64_t known;
    uint16_t pc,sp,view;
    uint8_t sp_known,c,c_known;
    uint8_t depth;
    uint16_t target[5]; // HL, IX, IY, BC, DE constants
    uint16_t target_known; // two bits per pair, high then low byte
    uint8_t a,a_known; // A is established by static transfers, not seeded from RAM state
    struct { uint16_t pc,cell; } returns[DP_SEGA_CLOSURE_STACK];
} Node;
typedef struct { uint32_t index; uint16_t pc,view,count,seeds; uint8_t used; } Group;
struct DpSegaClosure {
    const DpSegaBackend *backend;
    const DpSegaSystem *system;
    struct DpSegaClosure *targets,*blocks;
    int target_enabled,block_mode,finalized;
    const uint8_t *rom;
    uint8_t *flags;
    size_t size,hash_size,view_hash_size;
    size_t cursor;
    DpSegaClosureStats *s;
    View *views;
    Node *nodes;
    uint32_t *hash,*view_hash;
    Group *groups;
    unsigned current_view;
    int current_valid,is_gg;
    uint16_t ram_mask;
};
static uint64_t mix(uint64_t h,uint64_t v) { return (h^v)*1099511628211ull; }
static uint64_t view_hash(const View *v) {
    uint64_t h=14695981039346656037ull;
    for(unsigned i=0;i<64;i++) h=mix(h,(uint32_t)v->base[i]);
    return mix(mix(mix(mix(h,v->mask),v->mapper),v->cart),v->ram);
}
static uint64_t node_hash(Node n) {
    uint64_t h=14695981039346656037ull;
    h=mix(mix(mix(h,n.known),n.pc),n.view);
    h=mix(mix(mix(mix(mix(h,n.sp),n.sp_known),n.c),n.c_known),n.depth);
    h=mix(mix(mix(h,n.target_known),n.a_known),n.a);
    for(unsigned i=0;i<5;i++) h=mix(h,n.target[i]);
    for(unsigned i=0;i<n.depth;i++) h=mix(mix(h,n.returns[i].pc),n.returns[i].cell);
    return h;
}
static int same_node(Node a,Node b) {
    if(a.known!=b.known || a.pc!=b.pc || a.view!=b.view || a.sp!=b.sp ||
       a.sp_known!=b.sp_known || a.c!=b.c || a.c_known!=b.c_known || a.depth!=b.depth || a.target_known!=b.target_known || a.a!=b.a || a.a_known!=b.a_known ||
       memcmp(a.target,b.target,sizeof(a.target))) return 0;
    for(unsigned i=0;i<a.depth;i++) if(a.returns[i].pc!=b.returns[i].pc || a.returns[i].cell!=b.returns[i].cell) return 0;
    return 1;
}
static size_t table_size(size_t entries) { size_t n=1; while(n<entries*2) n*=2; return n; }
void dp_sega_closure_free(DpSegaClosure *c) {
    if(!c) return;
    dp_sega_closure_free(c->targets); dp_sega_closure_free(c->blocks);
    free(c->views); free(c->nodes); free(c->hash); free(c->view_hash); free(c->groups); free(c);
}
DpSegaClosure *dp_sega_closure_create(const DpSegaBackend *b,const DpSegaSystem *system,
    const uint8_t *rom,size_t size,uint8_t *flags,DpSegaClosureOptions o,DpSegaClosureStats *s) {
    if(!b||!system||!rom||!size||!flags||!s||!o.nodes||o.nodes>1048576||!o.seeds||o.seeds>1048576||
       !o.views||o.views>4096||!o.contexts||o.contexts>64) return NULL;
    if(o.seeds>o.nodes) o.seeds=o.nodes;
    DpSegaClosure *c=calloc(1,sizeof(*c)); if(!c) return NULL;
    c->backend=b; c->system=system; c->rom=rom; c->size=size; c->flags=flags; c->s=s; c->is_gg=!strcmp(system->id,"gg");
    c->ram_mask=!strcmp(system->id,"msx")?65535:!strcmp(system->id,"sg")?1023:8191;
    memset(s,0,sizeof(*s)); s->limits=o;
    c->hash_size=table_size(o.nodes); c->view_hash_size=table_size(o.views);
    c->views=calloc(o.views,sizeof(*c->views)); c->nodes=malloc((size_t)o.nodes*sizeof(*c->nodes));
    c->hash=calloc(c->hash_size,sizeof(*c->hash)); c->view_hash=calloc(c->view_hash_size,sizeof(*c->view_hash));
    c->groups=calloc(c->hash_size,sizeof(*c->groups));
    if(!c->views||!c->nodes||!c->hash||!c->view_hash||!c->groups) { dp_sega_closure_free(c); return NULL; }
    return c;
}
int dp_sega_closure_targets(DpSegaClosure *c,DpSegaClosureStats *stats) {
    if(!c || !stats || stats==c->s || c->finalized || c->target_enabled || c->targets || c->s->nodes || c->current_valid) return 0;
    DpSegaClosure *targets=dp_sega_closure_create(c->backend,c->system,c->rom,c->size,c->flags,c->s->limits,stats);
    if(!targets) return 0;
    targets->target_enabled=1; c->targets=targets; return 1;
}
int dp_sega_closure_blocks(DpSegaClosure *c,DpSegaClosureStats *stats) {
    if(!c || !stats || !c->targets || c->target_enabled || c->blocks || c->finalized ||
       stats==c->s || stats==c->targets->s || c->s->nodes || c->current_valid) return 0;
    DpSegaClosure *blocks=dp_sega_closure_create(c->backend,c->system,c->rom,c->size,c->flags,c->s->limits,stats);
    if(!blocks) return 0;
    blocks->target_enabled=blocks->block_mode=1; c->blocks=blocks; return 1;
}
void dp_sega_closure_invalidate(DpSegaClosure *c) {
    if(c) { c->current_valid=0; dp_sega_closure_invalidate(c->targets); dp_sega_closure_invalidate(c->blocks); }
}
static unsigned capture(DpSegaClosure *c) {
    if(c->current_valid) return c->current_view;
    View v={0}; v.mask=1023; v.mapper=(uint8_t)c->backend->mapper();
    if(c->backend->cartridge_windows) v.cart=c->backend->cartridge_windows();
    if(c->size<1024 && !(c->size&(c->size-1)) && c->backend->rom_index((uint16_t)c->size)==0)
        v.mask=(uint16_t)(c->size-1);
    for(unsigned i=0;i<64;i++) {
        v.base[i]=(int32_t)c->backend->rom_index((uint16_t)(i*1024));
        if(v.base[i]>=0) v.present|=UINT64_C(1)<<i;
        int32_t ram=c->backend->ram_index((uint16_t)(i*1024));
        if(ram>=0 && (size_t)ram<c->backend->wram_tags) v.ram|=UINT64_C(1)<<i;
    }
    v.immutable=v.mapper==DP_SEGA_MAPPER_NONE?v.present:v.mapper==DP_SEGA_MAPPER_SEGA?(v.present&1):0;
    size_t bucket=(size_t)view_hash(&v)&(c->view_hash_size-1);
    unsigned id=c->s->limits.views;
    for(size_t probe=0;probe<c->view_hash_size;probe++,bucket=(bucket+1)&(c->view_hash_size-1)) {
        uint32_t entry=c->view_hash[bucket];
        if(entry) {
            const View *old=&c->views[entry-1];
            if(old->mask!=v.mask || old->mapper!=v.mapper || old->cart!=v.cart || old->ram!=v.ram ||
               memcmp(old->base,v.base,sizeof(v.base))) continue;
            id=entry-1;
        } else if(c->s->views<c->s->limits.views) {
            id=c->s->views++; c->views[id]=v; c->view_hash[bucket]=id+1;
        }
        break;
    }
    if(id==c->s->limits.views) c->s->view_skips++;
    c->current_valid=1; c->current_view=id; return id;
}
// -1 unmapped/image tail; -2 a previously observed window invalidated by a write.
static int64_t index_of(const DpSegaClosure *c,Node n,uint16_t address) {
    const View *v=&c->views[n.view]; unsigned chunk=address>>10;
    if(v->base[chunk]<0) return -1;
    if(!(n.known&(UINT64_C(1)<<chunk))) return -2;
    uint32_t index=(uint32_t)v->base[chunk]+(address&v->mask);
    return index<c->size?(int64_t)index:-1;
}
static int peek(const DpSegaClosure *c,Node n,uint16_t address,uint8_t *out) {
    int64_t index=index_of(c,n,address); if(index<0) return 0;
    // Only explicit constant reads produce static data evidence. Instruction
    // decoding and padding probes bypass this path and never mark data.
    c->s->static_data_reads++;
    if(!(c->flags[index]&DP_SEGA_STATIC_DATA)) c->s->static_data_bytes++;
    c->flags[index]|=DP_SEGA_STATIC_DATA;
    *out=c->rom[index]; return 1;
}
static int word(const DpSegaClosure *c,Node n,uint16_t address,uint16_t *out) {
    uint8_t lo,hi;
    if(!peek(c,n,address,&lo)||!peek(c,n,(uint16_t)(address+1),&hi)) return 0;
    *out=(uint16_t)(lo|(hi<<8)); return 1;
}
static void enqueue(DpSegaClosure *c,Node n,int seed) {
    int64_t index=index_of(c,n,n.pc);
    if(index<0) { if(index==-2) c->s->ambiguous++; else c->s->unmapped++; return; }
    if(!n.sp_known) n.sp=0;
    if(!n.c_known) n.c=0;
    size_t bucket=(size_t)node_hash(n)&(c->hash_size-1),empty=c->hash_size;
    for(size_t probe=0;probe<c->hash_size;probe++,bucket=(bucket+1)&(c->hash_size-1)) {
        uint32_t entry=c->hash[bucket];
        if(!entry) { empty=bucket; break; }
        if(same_node(c->nodes[entry-1],n)) { c->s->duplicates++; return; }
    }
    if(seed && c->s->seeds>=c->s->limits.seeds) { c->s->seed_skips++; return; }
    if(empty==c->hash_size || c->s->nodes>=c->s->limits.nodes) { c->s->node_cuts++; return; }
    uint64_t h=mix(mix(mix(14695981039346656037ull,(uint32_t)index),n.pc),n.view);
    bucket=(size_t)h&(c->hash_size-1); Group *g=NULL;
    for(size_t probe=0;probe<c->hash_size;probe++,bucket=(bucket+1)&(c->hash_size-1)) {
        Group *candidate=&c->groups[bucket];
        if(!candidate->used) {
            *candidate=(Group){(uint32_t)index,n.pc,n.view,0,0,1}; g=candidate; break;
        }
        if(candidate->index==(uint32_t)index && candidate->pc==n.pc && candidate->view==n.view) { g=candidate; break; }
    }
    unsigned seed_contexts=c->s->limits.contexts>1?c->s->limits.contexts/2:1;
    if(!g || g->count>=c->s->limits.contexts || (seed && g->seeds>=seed_contexts)) { c->s->context_cuts++; return; }
    if(seed) g->seeds++;
    g->count++; c->nodes[c->s->nodes]=n; c->hash[empty]=(uint32_t)++c->s->nodes;
    if(seed) c->s->seeds++;
}
void dp_sega_closure_observe(DpSegaClosure *c) {
    if(!c) return;
    if(c->finalized) { c->s->seed_skips++; return; }
    dp_sega_closure_observe(c->targets); dp_sega_closure_observe(c->blocks);
    const z80_t *cpu=c->backend->cpu(); if(cpu->halted || cpu->fault) return;
    if(c->s->seeds>=c->s->limits.seeds) { c->s->seed_skips++; return; }
    unsigned view=capture(c); if(view==c->s->limits.views) return;
    Node n={.known=c->views[view].present,.pc=cpu->pc,.sp=cpu->sp,.view=(uint16_t)view,
        .sp_known=1,.c=cpu->c,.c_known=1};
    if(c->target_enabled) { n.target[0]=cpu->hl; n.target[1]=cpu->ix; n.target[2]=cpu->iy; n.target_known=63; if(c->block_mode) { n.target[3]=cpu->c; n.target_known|=128; } }
    enqueue(c,n,1);
}
static int is_ram(const DpSegaClosure *c,Node n,uint16_t address) {
    return !!(c->views[n.view].ram&(UINT64_C(1)<<(address>>10)));
}
static void unknown_store(DpSegaClosure *c,Node *n) { c->s->stack_invalidations+=n->depth; n->depth=0; }
static void known_store(DpSegaClosure *c,Node *n,uint16_t address,unsigned bytes) {
    for(unsigned j=0;j<bytes;j++) {
        uint16_t a=(uint16_t)(address+j); if(!is_ram(c,*n,a)) continue;
        unsigned cell=a&c->ram_mask;
        for(unsigned i=0;i<n->depth;) {
            unsigned at=n->returns[i].cell;
            if(cell!=at && cell!=((at+1)&c->ram_mask)) { i++; continue; }
            memmove(&n->returns[i],&n->returns[i+1],(n->depth-i-1)*sizeof(n->returns[0]));
            n->depth--; c->s->stack_invalidations++;
        }
    }
}
static void stack_store(DpSegaClosure *c,Node before,Node *after,int push) {
    if(!before.sp_known) { unknown_store(c,after); return; }
    if(push) {
        known_store(c,after,(uint16_t)(before.sp-1),1); known_store(c,after,(uint16_t)(before.sp-2),1);
    } else known_store(c,after,before.sp,2);
}
static uint64_t memory_unknown(const View *v) {
    if(v->mapper==DP_Z80_MAPPER_MSX_PLAIN) return 0;
    if(v->mapper==DP_Z80_MAPPER_MSX_KONAMI) return v->present&(UINT64_C(0xffffff)<<24);
    return v->present&~v->immutable;
}
static uint64_t address_effect(const View *v,uint16_t address) {
    if(v->mapper>=DP_Z80_MAPPER_MSX_PLAIN) {
        if(address<0x4000 || address>=0xc000 || !(v->cart&(1u<<(address>>13)))) return 0;
        unsigned window;
        switch(v->mapper) {
        case DP_Z80_MAPPER_MSX_ASCII8:
            if(address<0x6000 || address>=0x8000) return 0;
            window=2+((address>>11)&3); break;
        case DP_Z80_MAPPER_MSX_ASCII16:
            if(address<0x6000 || address>=0x8000 || (address&0x800)) return 0;
            return UINT64_C(0xffff)<<((address&0x1000)?32:16);
        case DP_Z80_MAPPER_MSX_KONAMI:
            if(address<0x6000) return 0;
            window=address>>13; break;
        case DP_Z80_MAPPER_MSX_SCC:
            if(address<0x5000 || (address&0x1800)!=0x1000) return 0;
            window=address>>13; break;
        default: return 0;
        }
        return UINT64_C(0xff)<<(window*8);
    }
    if(v->mapper==DP_SEGA_MAPPER_SEGA) {
        if(address==0xfffc || address==0xffff) return UINT64_C(0xffff)<<32;
        if(address==0xfffe) return UINT64_C(0xffff)<<16;
        if(address==0xfffd) return UINT64_C(0xfffe);
    } else if(v->mapper==DP_SEGA_MAPPER_CODEMASTERS && address<0xc000 && !(address&0x3fff))
        return UINT64_C(0xffff)<<(address>>10);
    else if(v->mapper==DP_SEGA_MAPPER_KOREAN150 && (address==0xfff3 || address==0xfffc))
        return UINT64_C(0xffffffffffff);
    return 0;
}
static uint64_t store_effect(const View *v,uint16_t address,unsigned bytes) {
    uint64_t effect=0;
    for(unsigned i=0;i<bytes;i++) effect|=address_effect(v,(uint16_t)(address+i));
    return effect;
}
static uint64_t stack_effect(const View *v,Node n,int push) {
    if(!n.sp_known) return memory_unknown(v);
    return push?address_effect(v,(uint16_t)(n.sp-1))|address_effect(v,(uint16_t)(n.sp-2)):store_effect(v,n.sp,2);
}
static uint64_t port_effect(DpSegaClosure *c,const View *v,Node *after,int known,uint8_t port) {
    if(v->mapper>=DP_Z80_MAPPER_MSX_PLAIN) {
        if(known && port!=0xa8) return 0;
        unknown_store(c,after); return v->present;
    }
    if(v->mapper==DP_SEGA_MAPPER_NONE) return 0;
    if(known && (c->is_gg?port!=0x3e:(port>=0x40 || (port&1)))) return 0;
    // A memory-control write can hide cartridge ROM and internal stack RAM.
    unknown_store(c,after);
    return v->present;
}
static int target_value(Node n,unsigned pair,uint16_t *value);
typedef struct { DpSegaClosure *closure; Node node; uint16_t address; unsigned bytes; } ConstantRead;
// A memory read is allowed only for the pre-instruction address proven below.
// Unknown addresses must not alias a fake zero-valued CPU register into ROM.
static DpZ80Origin constant_read(void *user,uint16_t address) {
    ConstantRead *read=user; uint8_t value;
    if((uint16_t)(address-read->address)>=read->bytes || !peek(read->closure,read->node,address,&value)) return 0;
    return (DpZ80Origin)value+1;
}
static void target_pair(Node *n,unsigned pair,uint16_t value) {
    n->target[pair]=value; n->target_known|=(uint16_t)(3u<<(pair*2));
}
static void target_byte(Node *n,unsigned pair,unsigned low,uint8_t value) {
    unsigned shift=low?0:8;
    n->target[pair]=(uint16_t)((n->target[pair]&~(255u<<shift))|((unsigned)value<<shift));
    n->target_known|=(uint16_t)(1u<<(pair*2+low));
}
// Preserve the established target search before the BC/DE refinement gets
// the remaining budget. These transfer rules deliberately leave BC/DE unknown.
static void target_constants_basic(DpSegaClosure *c,Node before,Node *after,const z80_decoded_t *d) {
    static const unsigned tags[]={DP_Z80_TAG_H,DP_Z80_TAG_L,DP_Z80_TAG_IXH,DP_Z80_TAG_IXL,DP_Z80_TAG_IYH,DP_Z80_TAG_IYL};
    DpZ80Provenance constants={0}; z80_t cpu={0}; cpu.pc=before.pc;
    cpu.hl=before.target[0]; cpu.ix=before.target[1]; cpu.iy=before.target[2];
    ConstantRead read={c,before,0,0}; uint16_t address;
    unsigned op=d->opcode;
    if(!d->group && (op>>6)==1 && (op&7)==6 && op!=0x76 && target_value(before,d->index,&address)) {
        read.address=(uint16_t)(address+(d->index?(int8_t)d->displacement:0)); read.bytes=1;
    } else if(!d->group && op==0x3a) { read.address=d->immediate; read.bytes=1; }
    if(before.a_known) constants.reg[DP_Z80_TAG_A]=(DpZ80Origin)before.a+1;
    for(unsigned i=0;i<6;i++) if(before.target_known&(1u<<i))
        constants.reg[tags[i]]=((before.target[i/2]>>(i&1?0:8))&255)+1;
    dp_z80_provenance_begin(&constants,&cpu,d,constant_read,&read);
    after->a_known=constants.reg[DP_Z80_TAG_A]!=0;
    after->a=after->a_known?(uint8_t)(constants.reg[DP_Z80_TAG_A]-1):0;
    after->target_known=0; memset(after->target,0,sizeof(after->target));
    for(unsigned i=0;i<6;i++) if(constants.reg[tags[i]]) {
        after->target_known|=(uint16_t)(1u<<i);
        after->target[i/2]|=(uint16_t)((constants.reg[tags[i]]-1)<<(i&1?0:8));
    }
    unsigned pair=d->index;
    if(!d->group && op==0x3e) { after->a=(uint8_t)d->immediate; after->a_known=1; }
    if(!d->group && (op==0x23 || op==0x2b) && target_value(before,pair,&address))
        target_pair(after,pair,(uint16_t)(address+(op==0x2b?-1:1)));
    else if(!d->group && op==0x21) target_pair(after,pair,d->immediate);
    else if(!d->group && (op==0x26 || op==0x2e)) target_byte(after,pair,op==0x2e,(uint8_t)d->immediate);
    else if((!d->group && op==0x2a) || (d->group==2 && op==0x6b)) {
        uint16_t value;
        if(word(c,before,d->immediate,&value)) target_pair(after,d->group?0:pair,value);
    }
}
static unsigned target_rp(unsigned pair,unsigned index) { return pair<2?pair+3:index; }
static unsigned target_reg(unsigned reg,unsigned index) { return reg<4?3+reg/2:index; }
static void advance_pair(Node before,Node *after,unsigned pair,int delta) {
    uint16_t value;
    if(target_value(before,pair,&value)) target_pair(after,pair,(uint16_t)(value+delta));
    else if(before.target_known&(2u<<(pair*2))) target_byte(after,pair,1,(uint8_t)(before.target[pair]+delta));
}
static void target_constants(DpSegaClosure *c,Node before,Node *after,const z80_decoded_t *d) {
    static const unsigned tags[]={DP_Z80_TAG_H,DP_Z80_TAG_L,DP_Z80_TAG_IXH,DP_Z80_TAG_IXL,
        DP_Z80_TAG_IYH,DP_Z80_TAG_IYL,DP_Z80_TAG_B,DP_Z80_TAG_C,DP_Z80_TAG_D,DP_Z80_TAG_E};
    DpZ80Provenance constants={0}; z80_t cpu={0}; cpu.pc=before.pc;
    cpu.hl=before.target[0]; cpu.ix=before.target[1]; cpu.iy=before.target[2];
    cpu.bc=before.target[3]; cpu.de=before.target[4];
    ConstantRead read={c,before,0,0}; uint16_t address;
    unsigned op=d->opcode,x=op>>6,y=(op>>3)&7,z=op&7,p=y>>1,q=y&1;
    if(!d->group && x==1 && z==6 && op!=0x76 && target_value(before,d->index,&address)) {
        read.address=(uint16_t)(address+(d->index?(int8_t)d->displacement:0)); read.bytes=1;
    } else if(!d->group && (op==0x0a || op==0x1a) && target_value(before,op==0x0a?3:4,&address)) {
        read.address=address; read.bytes=1;
    } else if(!d->group && op==0x3a) { read.address=d->immediate; read.bytes=1; }
    else if((!d->group && op==0x2a) || (d->group==2 && x==1 && z==3 && q && p<3)) {
        read.address=d->immediate; read.bytes=2;
    } else if(d->group==2 && x==2 && y>=4 && (z==0 || z==3) && target_value(before,0,&address)) {
        read.address=address; read.bytes=1;
    }
    if(before.a_known) constants.reg[DP_Z80_TAG_A]=(DpZ80Origin)before.a+1;
    for(unsigned i=0;i<10;i++) if(before.target_known&(1u<<i))
        constants.reg[tags[i]]=((before.target[i/2]>>(i&1?0:8))&255)+1;
    dp_z80_provenance_begin(&constants,&cpu,d,constant_read,&read);
    after->a_known=constants.reg[DP_Z80_TAG_A]!=0;
    after->a=after->a_known?(uint8_t)(constants.reg[DP_Z80_TAG_A]-1):0;
    after->target_known=0; memset(after->target,0,sizeof(after->target));
    for(unsigned i=0;i<10;i++) if(constants.reg[tags[i]]) {
        after->target_known|=(uint16_t)(1u<<i);
        after->target[i/2]|=(uint16_t)((constants.reg[tags[i]]-1)<<(i&1?0:8));
    }
    if(!d->group && x==0) {
        if(z==1 && !q && p<3) target_pair(after,target_rp(p,d->index),d->immediate);
        else if(z==3 && p<3) advance_pair(before,after,target_rp(p,d->index),q?-1:1);
        else if(z==6 && y!=6) {
            if(y==7) { after->a=(uint8_t)d->immediate; after->a_known=1; }
            else target_byte(after,target_reg(y,d->index),y&1,(uint8_t)d->immediate);
        } else if((z==4 || z==5) && y!=6) {
            if(y==7 && before.a_known) { after->a=(uint8_t)(before.a+(z==5?-1:1)); after->a_known=1; }
            else if(y<6) {
                unsigned pair=target_reg(y,d->index),low=y&1;
                if(before.target_known&(1u<<(pair*2+low)))
                    target_byte(after,pair,low,(uint8_t)((before.target[pair]>>(low?0:8))+(z==5?-1:1)));
            }
        } else if(op==0x10 && (before.target_known&(1u<<6)))
            target_byte(after,3,0,(uint8_t)((before.target[3]>>8)-1)); // DJNZ
    } else if(d->group==2 && x==2 && y>=4 && z<=3) {
        int direction=y&1?-1:1;
        advance_pair(before,after,0,direction);
        if(z==0) advance_pair(before,after,4,direction);
        if(z<=1) advance_pair(before,after,3,-1);
        else if(before.target_known&(1u<<6)) target_byte(after,3,0,(uint8_t)((before.target[3]>>8)-1));
    }
}
static int target_value(Node n,unsigned pair,uint16_t *value) {
    unsigned mask=3u<<(pair*2);
    if((n.target_known&mask)!=mask) return 0;
    *value=n.target[pair]; return 1;
}
static uint64_t indirect_store(DpSegaClosure *c,Node before,Node *after,unsigned pair,int displacement) {
    const View *v=&c->views[before.view]; uint16_t address;
    if(target_value(before,pair,&address)) {
        address=(uint16_t)(address+displacement); c->s->memory_resolved++;
        known_store(c,after,address,1); return address_effect(v,address);
    }
    unknown_store(c,after); return memory_unknown(v);
}
// Register-byte constants refine barriers, block transfers and indirect
// targets. Only proven ROM reads, transfers and exact INC/DEC arithmetic
// are used; unmodelled transforms and RAM-derived guesses stay unknown.
static uint64_t effects(DpSegaClosure *c,Node before,Node *after,const z80_decoded_t *d) {
    const View *v=&c->views[before.view];
    if(c->block_mode) target_constants(c,before,after,d);
    else if(c->target_enabled) target_constants_basic(c,before,after,d);
    unsigned op=d->opcode,x=op>>6,y=(op>>3)&7,z=op&7,p=y>>1,q=y&1;
    uint64_t effect=0;
    if(d->group==1) {
        if(x!=1) {
            if(d->index || z==6) effect=indirect_store(c,before,after,d->index,d->index?(int8_t)d->displacement:0);
            if(z==1) after->c_known=0;
        }
    } else if(d->group==2) {
        if(x==1) {
            if(z==0 && y==1) after->c_known=0;
            else if(z==1) effect=port_effect(c,v,after,before.c_known,before.c);
            else if(z==3) {
                if(!q) { effect=store_effect(v,d->immediate,2); known_store(c,after,d->immediate,2); }
                else if(p==3) after->sp_known=(uint8_t)word(c,before,d->immediate,&after->sp);
                else if(p==0) after->c_known=(uint8_t)peek(c,before,d->immediate,&after->c);
            } else if(z==7 && (y==4 || y==5)) effect=indirect_store(c,before,after,0,0);
        } else if(x==2 && y>=4 && z<=3) {
            if(z<=1) after->c_known=0;
            if(z==0) effect=indirect_store(c,before,after,4,0);
            else if(z==2) effect=indirect_store(c,before,after,0,0);
            else if(z==3) effect=port_effect(c,v,after,before.c_known,before.c);
        }
    } else if(x==0) {
        if(z==1 && !q) {
            if(p==3) { after->sp=d->immediate; after->sp_known=1; }
            else if(p==0) { after->c=(uint8_t)d->immediate; after->c_known=1; }
        } else if(z==2 && !q) {
            if(p<2) effect=indirect_store(c,before,after,p+3,0);
            else { effect=store_effect(v,d->immediate,p==2?2:1); known_store(c,after,d->immediate,p==2?2:1); }
        }
        else if(z==3) {
            if(p==3 && after->sp_known) after->sp=(uint16_t)(after->sp+(q?-1:1));
            else if(p==0 && after->c_known) after->c=(uint8_t)(after->c+(q?-1:1));
        } else if(z==4 || z==5) {
            if(y==6) effect=indirect_store(c,before,after,d->index,d->index?(int8_t)d->displacement:0);
            else if(y==1 && after->c_known) after->c=(uint8_t)(after->c+(z==5?-1:1));
        } else if(z==6) {
            if(y==6) effect=indirect_store(c,before,after,d->index,d->index?(int8_t)d->displacement:0);
            else if(y==1) { after->c=(uint8_t)d->immediate; after->c_known=1; }
        }
    } else if(x==1 && op!=0x76) {
        if(y==6) effect=indirect_store(c,before,after,d->index,d->index?(int8_t)d->displacement:0);
        else if(y==1 && z!=1) after->c_known=0;
    } else if(x==3) {
        if(z==1) {
            if(!q) {
                if(p==0) after->c_known=before.sp_known && peek(c,before,before.sp,&after->c);
                if(after->sp_known) after->sp=(uint16_t)(after->sp+2);
            } else if(p==1) after->c_known=0; // EXX
            else if(p==3) after->sp_known=(uint8_t)target_value(before,d->index,&after->sp); // LD SP,HL/IX/IY
        } else if(z==3) {
            if(y==2) effect=port_effect(c,v,after,1,(uint8_t)d->immediate);
            else if(y==4) { effect=stack_effect(v,before,0); stack_store(c,before,after,0); }
        } else if(z==5 && !q) {
            effect=stack_effect(v,before,1);
            stack_store(c,before,after,1);
            if(after->sp_known) after->sp=(uint16_t)(after->sp-2);
        }
    }
    if(c->block_mode) {
        if(after->target_known&(1u<<7)) { after->c=(uint8_t)after->target[3]; after->c_known=1; }
        else if(after->c_known) target_byte(after,3,1,after->c);
    }
    return effect;
}
typedef struct { DpSegaClosure *c; Node n; uint8_t *bytes; int bad; } Decode;
static uint8_t decode_read(void *user,uint16_t address) {
    Decode *d=user;
    int64_t index=index_of(d->c,d->n,address);
    if(index<0) { if(!d->bad) d->bad=index==-2?2:1; return 0; }
    uint8_t value=d->c->rom[index];
    d->bytes[(uint16_t)(address-d->n.pc)]=value; return value;
}
static int is_padding(DpSegaClosure *c,Node n) {
    int64_t start=index_of(c,n,n.pc); uint8_t value=c->rom[start];
    if(value!=0 && value!=0xff) return 0;
    for(unsigned j=1;j<8;j++) {
        int64_t index=index_of(c,n,(uint16_t)(n.pc+j));
        if(index!=start+j || (index>>14)!=(start>>14) || c->rom[index]!=value) return 0;
    }
    return 1;
}
static void process(DpSegaClosure *c,Node n) {
    DpSegaClosureStats *s=c->s; s->processed++;
    if(is_padding(c,n)) { s->padding++; return; }
    uint8_t bytes[65536]; Decode dec={c,n,bytes,0}; z80_t cpu={0};
    cpu.pc=n.pc; cpu.read=decode_read; cpu.user=&dec; z80_decoded_t raw;
    if(!z80_decode(&cpu,&raw) || dec.bad) {
        s->decode++; if(dec.bad==2) s->ambiguous++; else if(dec.bad==1) s->unmapped++;
        return;
    }
    // instruction classification through the shared DpIsa (spec §7.3,
    // src/cpu/z80/z80_isa.c): the classes below are the DpFlowInfo reading of
    // what used to be open-coded over DpZ80Instruction.  RET cc arrives as
    // DP_FLOW_COND with no target, JR cc/DJNZ as DP_FLOW_COND with a target,
    // the ED block repetitions as DP_FLOW_COND back to themselves.
    DpFlowInfo fi;
    if(dp_isa_z80()->flow(bytes,n.pc,&fi)<0 || fi.kind==DP_FLOW_ILLEGAL) { s->decode++; return; }
    for(unsigned j=0;j<raw.length;j++) {
        int64_t index=index_of(c,n,(uint16_t)(n.pc+j));
        uint8_t f=c->flags[index];
        if((f&DP_SEGA_COPIED) && !(f&DP_SEGA_NATURAL)) { s->data++; return; }
    }
    for(unsigned j=0;j<raw.length;j++) {
        int64_t index=index_of(c,n,(uint16_t)(n.pc+j));
        if(!(c->flags[index]&~DP_SEGA_STATIC_DATA)) s->new_bytes++;
        c->flags[index]|=DP_SEGA_STATIC;
    }
    Node next=n; next.pc=(uint16_t)(n.pc+fi.length);
    const View *v=&c->views[n.view];
    if(fi.halt) { s->halted++; return; }
    if(fi.kind==DP_FLOW_INDIRECT) {
        unsigned pair=raw.index,mask=3u<<(pair*2);
        if((n.target_known&mask)==mask) {
            next.pc=n.target[pair]; s->indirect_resolved++; enqueue(c,next,0);
        } else s->indirect++;
        return;
    }
    int is_ret = fi.kind==DP_FLOW_RET || (fi.kind==DP_FLOW_COND && fi.target<0);
    if(is_ret) {
        uint16_t target; int found=0; Node returned=next;
        if(n.sp_known && is_ram(c,n,n.sp) && is_ram(c,n,(uint16_t)(n.sp+1))) {
            for(unsigned i=n.depth;i>0;i--) if(n.returns[i-1].cell==(n.sp&c->ram_mask)) {
                target=n.returns[i-1].pc; returned.depth=(uint8_t)(i-1); found=1; break;
            }
        }
        if(!found && n.sp_known) found=word(c,n,n.sp,&target);
        if(found) {
            returned.pc=target; returned.sp=(uint16_t)(n.sp+2);
            s->returns_resolved++; enqueue(c,returned,0);
        } else s->indirect++;
        if(fi.cond>=0) enqueue(c,next,0);
        return;
    }
    if(fi.kind==DP_FLOW_CALL) {
        Node called=next; called.pc=(uint16_t)fi.target;
        uint64_t effect=stack_effect(v,n,1);
        stack_store(c,n,&called,1);
        if(effect&called.known) s->map_barriers++;
        called.known&=~effect;
        if(called.sp_known) {
            called.sp=(uint16_t)(called.sp-2);
            if(is_ram(c,called,called.sp) && is_ram(c,called,(uint16_t)(called.sp+1))) {
                if(called.depth==DP_SEGA_CLOSURE_STACK) s->stack_cuts++;
                else {
                    called.returns[called.depth].pc=next.pc;
                    called.returns[called.depth++].cell=(uint16_t)(called.sp&c->ram_mask);
                }
            }
        }
        enqueue(c,called,0);
        if(fi.cond>=0) enqueue(c,next,0);
        return;
    }
    uint64_t effect=effects(c,n,&next,&raw);
    if(effect&next.known) s->map_barriers++;
    next.known&=~effect;
    int jr_cc = fi.kind==DP_FLOW_COND && fi.target>=0 && fi.cond>=0;          // JR cc / DJNZ
    int repeat = fi.kind==DP_FLOW_COND && fi.cond<0 && fi.target==(int32_t)n.pc;   // ED block repetition
    if(fi.kind==DP_FLOW_JUMP || jr_cc || repeat) {
        Node branch=next; branch.pc=(uint16_t)fi.target; enqueue(c,branch,0);
        if(fi.cond>=0 || repeat) enqueue(c,next,0);
    } else enqueue(c,next,0);
}
static void remaining_budget(DpSegaClosure *blocks,const DpSegaClosureStats *targets) {
    DpSegaClosureStats *s=blocks->s; uint64_t seeds=s->nodes;
    s->limits.nodes=targets->limits.nodes-(uint32_t)targets->nodes;
    s->limits.seeds=targets->limits.seeds-(uint32_t)targets->seeds;
    s->limits.views=targets->limits.views-targets->views;
    if(s->limits.seeds>s->limits.nodes) s->limits.seeds=s->limits.nodes;
    if(s->views>s->limits.views) s->views=s->limits.views;
    memset(blocks->hash,0,blocks->hash_size*sizeof(*blocks->hash));
    memset(blocks->groups,0,blocks->hash_size*sizeof(*blocks->groups));
    s->nodes=s->seeds=0;
    // All queued nodes are observations, not processed descendants. Rebuild
    // their tables in place; output positions never overtake unread seeds.
    for(uint64_t i=0;i<seeds;i++) {
        Node n=blocks->nodes[i];
        if(n.view>=s->limits.views) s->view_skips++;
        else enqueue(blocks,n,1);
    }
}
void dp_sega_closure_run(DpSegaClosure *c) {
    if(!c || c->finalized) return;
    for(;c->cursor<c->s->nodes;c->cursor++) process(c,c->nodes[c->cursor]);
    dp_sega_closure_run(c->targets);
    if(c->blocks) { remaining_budget(c->blocks,c->targets->s); dp_sega_closure_run(c->blocks); }
    c->finalized=1;
}
