#include "explore.h"
#include "backend.h"
#include "visits.h"
#include "../msx/msx.h"
#include "../msx/explore.h"
#include "../../cpu/z80/provenance.h"
#include "../../core/hashset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

typedef struct {
    DpZ80Provenance cpu;
    DpZ80Origin cram_latch;
    DpZ80Origin ram[];
} Origins;
typedef struct { void *state; uint8_t *cart; Origins *origins; int forced,skip_irq; uint32_t choice; } Path;
// A seed spec: pc in a mapper view. exact=1 applies seed_window(pc,bank) and
// refuses any view where rom_index(pc) is not bank*0x4000+(pc&0x3FFF); exact=0
// keeps the boot view untouched.
typedef struct { uint16_t pc; uint16_t bank; uint8_t exact,kind; } SeedSpec;
// observed indirect jump site: up to 8 distinct physical targets
typedef struct { uint32_t site; uint8_t n,overflow; uint32_t target[8]; uint32_t hits; } DpSegaIndirectRec;
static uint64_t fnv1a(uint64_t h,const void *p,size_t n) {
    const uint8_t *b=p;
    for(size_t i=0;i<n;i++) { h^=b[i]; h*=1099511628211ull; }
    return h;
}
static uint64_t fnv1a_u16(uint64_t h,uint16_t v) { return fnv1a(h,&v,sizeof v); }
static uint64_t fnv1a_u32(uint64_t h,uint32_t v) { return fnv1a(h,&v,sizeof v); }
enum { INPUT_PAD1, INPUT_PAD2, INPUT_START, INPUT_HALT_PAUSE, INPUT_PAD2_UD, INPUT_GROUP_COUNT };
typedef struct { uint32_t key; uint8_t accepted,seen[16]; uint64_t blocked_epoch; } InputSite;
typedef struct {
    const DpSegaBackend *b;
    const DpSegaSystem *system;
    const DpSegaOptions *opt;
    DpSegaResults *r;
    uint8_t *cart,*forks,*sides,*irq_forks;
    const uint8_t *image;
    Origins *origins;
    size_t origin_size;
    uint32_t *copy_hash;
    size_t copy_hash_size;
    DpSegaCopy pending_copy;
    int has_pending_copy;
    Path *queue;
    uint32_t queued,queue_capacity;
    Path *input_queue;
    uint32_t input_queued;
    FILE *spool;
    uint64_t spool_wr,spool_rd,spool_bytes;
    uint32_t cur_choice;
    uint64_t created[2];
    unsigned phase;
    uint64_t discoveries;
    int forced,fetching;
    uint16_t pc;
    InputSite *input_sites;
    size_t input_hash_size;
    void *input_scratch;
    uint8_t input_catalog[128];
    uint64_t input_epoch;
    DpSegaClosure *closure;
    int msx,bios_active;
    unsigned bios_id;
    DpZ80Origin bios_a,bios_e,bios_read,bios_return;
    int64_t bios_mapping[10];
    SeedSpec *seeds;
    unsigned seed_count,seed_next,seed_capacity;
    void *seed_state;              // reset machine image seeds start from
    uint8_t *seed_cart;            // reset cartridge RAM alongside it
    Origins *seed_origins;         // reset provenance origins alongside it
    void *seed_scratch;            // running machine saved across materialization
    uint8_t *seed_scratch_cart;
    Origins *seed_scratch_origins;
    DpSegaSection *cur_sec;        // section of the instruction being executed
    size_t prev_at;                // last instruction's final physical byte
    int irq_flag,boot_path;        // applied at the next section touch
    // observed indirect jump sites: physical site -> distinct targets
    DpSegaIndirectRec *indirect;
    size_t indirect_hash;
} Run;

// observed indirect targets without execution evidence seed a further pass
// (GB dp_seed_indirect parity); every cap hit is counted, never silent
static void record_indirect(Run *x,size_t at,uint16_t target) {
    DpSegaResults *r=x->r;
    if(!x->indirect_hash) return;
    size_t bucket=(at*2654435761u)&(x->indirect_hash-1);
    for(size_t probe=0;probe<x->indirect_hash;probe++,bucket=(bucket+1)&(x->indirect_hash-1)) {
        DpSegaIndirectRec *rec=&x->indirect[bucket];
        if(rec->site!=at) {
            if(rec->site) continue;
            rec->site=(uint32_t)at;
        }
        rec->hits++;
        int64_t index=x->b->rom_index(target);
        if(index<0 || (uint64_t)index>=r->size) break; // target outside the ROM
        for(unsigned i=0;i<rec->n;i++) if(rec->target[i]==(uint32_t)index) return;
        if(rec->n<8) { rec->target[rec->n++]=(uint32_t)index; r->indirect_targets++; }
        else { rec->overflow=1; r->indirect_target_overflow++; }
        return;
    }
    x->r->indirect_site_overflow++;
}
static DpZ80Origin origin_read(void *user,uint16_t a) {
    Run *x=user;
    int64_t i=x->b->rom_index(a);
    if(i>=0 && (uint64_t)i<x->r->size) return (DpZ80Origin)i+1;
    int32_t ram=x->b->ram_index(a);
    return ram>=0 && (size_t)ram<x->b->ram_tags?x->origins->ram[ram]:0;
}
static DpZ80Origin valid_origin(Run *x,DpZ80Origin origin,uint8_t value) {
    if(!origin) return 0;
    if(origin>x->r->size || x->image[origin-1]!=value) {
        x->r->provenance_mismatches++; return 0;
    }
    return origin;
}
static void section_live_ram(Run *x,int32_t ram);
static void section_live_port(Run *x,uint16_t port);
static uint8_t read_bus(void *user,uint16_t a) {
    Run *x=user; uint8_t v=x->b->read(a);
    int64_t index=x->b->rom_index(a);
    if(!x->fetching && index>=0 && (uint64_t)index<x->r->size) {
        if(!(x->r->flags[index]&DP_SEGA_DATA)) x->discoveries++;
        x->r->flags[index]|=DP_SEGA_DATA; x->r->data_reads++;
    }
    if(!x->fetching) section_live_ram(x,x->b->ram_index(a));
    if(x->bios_active && x->bios_id==15) x->bios_read=valid_origin(x,origin_read(x,a),v);
    return v;
}
static uint64_t copy_hash(const DpSegaCopy *c) {
    uint64_t h=14695981039346656037ull;
    const uint64_t fields[]={c->path,c->pc,c->source,c->destination,c->length,
        (uint8_t)c->source_stride,(uint8_t)c->destination_stride,c->space,c->verbatim,c->forced};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++) { h^=fields[i]; h*=1099511628211ull; }
    return h;
}
static int same_copy(const DpSegaCopy *a,const DpSegaCopy *b) {
    return a->path==b->path && a->pc==b->pc && a->source==b->source &&
        a->destination==b->destination && a->length==b->length &&
        a->source_stride==b->source_stride && a->destination_stride==b->destination_stride &&
        a->space==b->space && a->verbatim==b->verbatim && a->forced==b->forced;
}
static void flush_copy(Run *x) {
    if(!x->has_pending_copy) return;
    DpSegaResults *r=x->r; DpSegaCopy *c=&x->pending_copy;
    size_t bucket=(size_t)copy_hash(c)&(x->copy_hash_size-1);
    for(size_t probe=0;probe<x->copy_hash_size;probe++,bucket=(bucket+1)&(x->copy_hash_size-1)) {
        uint32_t entry=x->copy_hash[bucket];
        if(entry) {
            DpSegaCopy *record=&r->copies[entry-1];
            if(!same_copy(record,c)) continue;
            record->occurrences++; record->last_step=c->last_step;
        } else if(r->copy_count<r->copy_capacity) {
            x->copy_hash[bucket]=r->copy_count+1;
            r->copies[r->copy_count++]=*c;
            if(x->cur_sec) x->cur_sec->copies++;
        } else r->copy_log_overflow+=c->length;
        x->has_pending_copy=0; return;
    }
    // The table is at most half full; retain an explicit guard if its shape
    // changes in the future instead of allowing an unbounded probe loop.
    r->copy_log_overflow+=c->length; x->has_pending_copy=0;
}
static void copied(Run *x,DpZ80Origin origin,uint8_t value,unsigned space,uint32_t destination,uint8_t mask) {
    if(!(origin=valid_origin(x,origin,value))) return;
    DpSegaResults *r=x->r;
    uint32_t source=origin-1;
    int verbatim=(value&mask)==value;
    if(verbatim) {
        if(!(r->flags[source]&DP_SEGA_COPIED)) x->discoveries++;
        r->flags[source]|=DP_SEGA_COPIED;
    } else r->copy_transformed++;
    if(space==DP_SEGA_VRAM || space==DP_SEGA_CRAM) {
        r->vdp_copies++;
        if(space==DP_SEGA_VRAM) r->vram_copies++; else r->cram_copies++;
    } else {
        r->ram_copies++;
        if(space==DP_SEGA_SRAM) r->sram_copies++;
    }
    if(x->has_pending_copy) {
        DpSegaCopy *last=&x->pending_copy;
        int64_t source_stride=last->source_stride,destination_stride=last->destination_stride;
        if(last->length==1) {
            source_stride=(int64_t)source-last->source;
            destination_stride=(int64_t)destination-last->destination;
        }
        if(last->length<UINT32_MAX && source_stride>=-1 && source_stride<=1 && destination_stride>=-1 && destination_stride<=1 &&
           last->path==r->paths && last->pc==x->pc && last->space==space && last->verbatim==verbatim &&
           (int64_t)last->source+source_stride*last->length==source &&
           (int64_t)last->destination+destination_stride*last->length==destination) {
            last->source_stride=(int8_t)source_stride; last->destination_stride=(int8_t)destination_stride;
            last->length++; last->last_step=r->steps; return;
        }
    }
    flush_copy(x);
    x->pending_copy=(DpSegaCopy){r->paths,r->steps,r->steps,1,source,destination,1,x->pc,0,0,(uint8_t)space,(uint8_t)verbatim,(uint8_t)x->forced};
    x->has_pending_copy=1;
}
static const uint16_t mapping_probe[]={0,0x400,0x2000,0x4000,0x6000,0x8000,0xa000,0x9800};
static void mapping_before(Run *x,int64_t before[10]) {
    for(unsigned i=0;i<8;i++) before[i]=x->b->rom_index(mapping_probe[i]);
    before[8]=x->msx?dp_msx_info().slots:0;
    before[9]=x->b->ram_index(0xc000);
}
static void mapping_after(Run *x,const int64_t before[10],uint16_t address,uint8_t value,unsigned kind) {
    int changed=0;
    for(unsigned i=0;i<8;i++) changed|=before[i]!=x->b->rom_index(mapping_probe[i]);
    changed|=x->msx && before[8]!=dp_msx_info().slots;
    changed|=before[9]!=x->b->ram_index(0xc000);
    if(!changed) return;
    dp_sega_closure_invalidate(x->closure);
    x->r->bank_changes++;
    unsigned n=x->r->bank_count;
    if(n<1024) {
        x->r->banks[n].step=x->r->steps; x->r->banks[n].pc=x->pc;
        x->r->banks[n].path=x->r->paths;
        x->r->banks[n].addr=address; x->r->banks[n].value=value;
        x->r->banks[n].kind=(uint8_t)kind; x->r->bank_count++;
    } else x->r->bank_log_overflow++;
}
static void ram_written(Run *x,int32_t ram,uint8_t value,DpZ80Origin origin) {
    if(ram<0 || (size_t)ram>=x->b->ram_tags) return;
    origin=valid_origin(x,origin,value); x->origins->ram[ram]=origin;
    copied(x,origin,value,(size_t)ram<x->b->wram_tags?DP_SEGA_WRAM:DP_SEGA_SRAM,
           (uint32_t)((size_t)ram<x->b->wram_tags?(size_t)ram:(size_t)ram-x->b->wram_tags),0xff);
}
static void write_bus(void *user,uint16_t a,uint8_t v) {
    Run *x=user;
    DpZ80Origin origin=dp_z80_provenance_write(&x->origins->cpu,a);
    int32_t ram=x->b->ram_index(a); int64_t before[10];
    mapping_before(x,before); x->b->write(a,v);
    ram_written(x,ram,v,origin); mapping_after(x,before,a,v,0);
}
static void msx_event(void *user,DpMsxEvent event) {
    Run *x=user; DpSegaResults *r=x->r;
    switch(event.kind) {
    case DP_MSX_BIOS_BEGIN:
        x->bios_active=1; x->bios_id=event.address;
        x->bios_a=x->origins->cpu.reg[DP_Z80_TAG_A];
        x->bios_e=x->origins->cpu.reg[DP_Z80_TAG_E];
        x->bios_read=x->bios_return=0;
        dp_z80_provenance_clear_plan(&x->origins->cpu);
        mapping_before(x,x->bios_mapping);
        if(event.address<64) r->bios_calls[event.address]++;
        break;
    case DP_MSX_BIOS_END:
        mapping_after(x,x->bios_mapping,(uint16_t)x->bios_id,dp_msx_info().slots,2);
        // C helpers change registers outside the Z80 executor. Only explicit
        // pure transfers below may establish new origins after the call.
        memset(&x->origins->cpu,0,sizeof(x->origins->cpu));
        if(x->bios_id==1) x->origins->cpu.reg[DP_Z80_TAG_A]=x->bios_return;
        x->bios_active=0;
        break;
    case DP_MSX_ROM_READ:
        if(event.address<r->size) {
            if(!(r->flags[event.address]&DP_SEGA_DATA)) x->discoveries++;
            r->flags[event.address]|=DP_SEGA_DATA; r->data_reads++;
            if(x->bios_active && x->bios_id==1)
                x->bios_return=valid_origin(x,event.address+1,event.value);
        }
        break;
    case DP_MSX_RAM_READ:
        if(x->bios_active && x->bios_id==1 && event.address<x->b->ram_tags)
            x->bios_return=valid_origin(x,x->origins->ram[event.address],event.value);
        break;
    case DP_MSX_RAM_WRITE:
        ram_written(x,(int32_t)event.address,event.value,
                    x->bios_active && x->bios_id==2?x->bios_e:0);
        break;
    case DP_MSX_AY_WRITE:
        if(event.address<16) {
            r->ay_writes[event.address]++;
            if(r->ay_count<DP_SEGA_HARDWARE_LOG_CAP)
                r->ay_log[r->ay_count++]=(DpSegaRegisterWrite){r->paths,r->steps,x->pc,
                    x->bios_active?0:0xa1,(uint8_t)event.address,event.value,0,x->forced};
            else r->ay_log_overflow++;
        }
        break;
    }
}
static uint8_t in_bus(void *user,uint16_t p) {
    Run *x=user;
    if(!x->r->io_read[p&255]) x->discoveries++;
    x->r->io_read[p&255]++;
    if(!x->fetching) section_live_port(x,p);
    return x->b->in(p);
}
static void out_bus(void *user,uint16_t p,uint8_t v) {
    Run *x=user;
    if(x->msx && dp_msx_bios_trap((uint16_t)(x->b->cpu()->pc-2),p)) { x->b->out(p,v); return; }
    uint8_t previous=0,effect_mask=0;
    int memory=x->b->memory_control && x->b->memory_control(p,&previous,&effect_mask);
    int remap=memory || (x->msx && (p&255)==0xa8);
    if(memory) {
        DpSegaResults *r=x->r;
        r->memory_writes++; r->memory_effect_changes+=!!((previous^v)&effect_mask);
        if(r->memory_count<DP_SEGA_HARDWARE_LOG_CAP)
            r->memory_log[r->memory_count++]=(DpSegaMemoryWrite){r->paths,r->steps,x->pc,p,previous,v,effect_mask,x->forced};
        else r->memory_log_overflow++;
    }
    int64_t before[10];
    if(remap) mapping_before(x,before);
    if(!x->r->io_write[p&255]) x->discoveries++;
    x->r->io_write[p&255]++;
    if((p&x->b->vdp_port_mask)==x->b->vdp_control_port) {
        uint8_t reg=0,value=0;
        unsigned command=x->b->vdp_command(v,&reg,&value);
        if(command) {
            DpSegaResults *r=x->r;
            if(command==1) r->vdp_register_writes[reg]++;
            else r->vdp_register_ignored++;
            if(r->register_count<DP_SEGA_HARDWARE_LOG_CAP)
                r->register_log[r->register_count++]=(DpSegaRegisterWrite){r->paths,r->steps,x->pc,p,reg,value,command==2,x->forced};
            else r->register_log_overflow++;
        }
    }
    if((p&x->b->vdp_port_mask)==x->b->vdp_data_port) {
        DpSegaVdpTarget t=x->b->vdp_target();
        DpZ80Origin candidate=x->origins->cpu.output;
        if(x->bios_active) candidate=x->bios_id==15?x->bios_read:
            x->bios_id==10 || x->bios_id==13?x->bios_a:0;
        DpZ80Origin origin=valid_origin(x,candidate,v);
        x->bios_read=0;
        if(t.latch) x->origins->cram_latch=origin;
        else {
            if(t.pair) copied(x,x->origins->cram_latch,t.latch_value,DP_SEGA_CRAM,t.address-1,0xff);
            copied(x,origin,v,t.space,t.address,t.mask);
        }
    }
    x->b->out(p,v);
    if(remap) mapping_after(x,before,p,v,1);
}
static void record_interrupt(Run *x,DpSegaInterrupt event) {
    DpSegaResults *r=x->r;
    event.target=x->b->cpu()->pc;
    event.target_rom=x->b->rom_index(event.target);
    r->interrupts[event.mode]++;
    if(r->interrupt_count<DP_SEGA_HARDWARE_LOG_CAP) r->interrupt_log[r->interrupt_count++]=event;
    else r->interrupt_log_overflow++;
}
static void hook(Run *x) {
    z80_t *c=x->b->cpu(); c->user=x; c->read=read_bus; c->write=write_bus; c->in=in_bus; c->out=out_bus;
}
static void free_path(Path p) { free(p.state); free(p.cart); free(p.origins); }
static size_t site(Run *x,uint16_t pc);
// choice tree: one node per queued path, parented on the path being run
static uint32_t tree_add(Run *x,unsigned kind) {
    DpSegaResults *r=x->r;
    if(r->tree_count==r->tree_capacity) {
        uint32_t cap=r->tree_capacity?r->tree_capacity*2:256;
        if(cap>1048576) return 0;
        DpSegaTreeNode *grown=realloc(r->tree_nodes,(size_t)cap*sizeof(*grown));
        if(!grown) return 0;
        r->tree_nodes=grown; r->tree_capacity=cap;
    }
    DpSegaTreeNode *n=&r->tree_nodes[r->tree_count];
    n->parent=x->cur_choice;
    n->pc=x->b->cpu()->pc; n->site=(uint32_t)site(x,n->pc);
    n->kind=(uint8_t)kind; n->issue=DP_SEGA_ISSUE_NONE;
    return (uint32_t)r->tree_count+++1;
}
static void tree_close(Run *x,uint32_t choice,int issue) {
    DpSegaResults *r=x->r;
    if(choice && choice<=r->tree_count) r->tree_nodes[choice-1].issue=(uint8_t)issue;
}
// disk spool record: a queued path survives a full frontier (--resume re-reads)
enum { DP_SPOOL_MAGIC=0x5a535044ull }; // 'DPSZ'
static int spool_write_path(Run *x,const Path *p,int flushed) {
    if(!x->spool) return 0;
    uint64_t rec=x->b->state_size+0x8000+x->origin_size+4*8;
    if(x->opt->spool_cap_mb && x->spool_bytes+rec>(uint64_t)x->opt->spool_cap_mb*1024*1024) {
        x->r->spool_refused++; return 0;
    }
    uint64_t hdr[4]={DP_SPOOL_MAGIC,
        (uint64_t)(p->forced?1:0)|((uint64_t)(p->skip_irq?1:0)<<1)|((uint64_t)p->choice<<32),
        (uint64_t)x->b->state_size,(uint64_t)x->origin_size};
    // one file, two cursors: records are appended at spool_wr, reads replay
    // from spool_rd — the explicit seek keeps writes from landing where the
    // last read stopped
    int ok=!fseek(x->spool,(long)x->spool_wr,SEEK_SET) &&
        fwrite(hdr,sizeof hdr,1,x->spool)==1 &&
        fwrite(p->state,1,x->b->state_size,x->spool)==x->b->state_size &&
        fwrite(p->cart,1,0x8000,x->spool)==0x8000 &&
        fwrite(p->origins,1,x->origin_size,x->spool)==x->origin_size;
    if(!ok) return 0;
    x->spool_wr+=rec; x->spool_bytes+=rec;
    // the path's outcome is known: it lives on disk until a run re-reads it
    tree_close(x,p->choice,DP_SEGA_ISSUE_SPOOLED);
    if(flushed) x->r->spool_flushed++;
    else x->r->spool_writes++;
    return 1;
}
static int queue_room(Run *x,int input) {
    if(input) return x->input_queued<x->opt->queue_cap;
    if(x->queued<x->queue_capacity) return 1;
    uint32_t want=x->queue_capacity?x->queue_capacity*2:64;
    uint32_t max=x->opt->queue_max>x->opt->queue_cap?x->opt->queue_max:x->opt->queue_cap;
    if(want>max) want=max;
    if(want>x->queue_capacity) {
        Path *grown=realloc(x->queue,(size_t)want*sizeof(*grown));
        if(grown) { x->queue=grown; x->queue_capacity=want; x->r->queue_growths++; }
    }
    return x->queued<x->queue_capacity;
}
static int push_to(Run *x,int forced,int skip_irq,int input,unsigned kind) {
    unsigned phase=input?1:x->phase;
    if(x->created[phase]>=x->opt->max_paths) {
        if(input) x->r->input_path_cuts++; else x->r->path_cuts++;
        return 0;
    }
    int room=queue_room(x,input);
    if(!room && !input) {
        // a full frontier is not the end: the state survives on disk
        uint32_t choice=tree_add(x,kind);
        Path p={malloc(x->b->state_size),malloc(0x8000),malloc(x->origin_size),forced,skip_irq,choice};
        if(p.state&&p.cart&&p.origins) {
            x->b->save(p.state); memcpy(p.cart,x->cart,0x8000);
            memcpy(p.origins,x->origins,x->origin_size);
            if(spool_write_path(x,&p,0)) { free_path(p); x->created[phase]++; return 1; }
        }
        free_path(p);
        x->r->queue_cuts++;
        return 0;
    }
    if(!room) { x->r->input_queue_cuts++; return 0; }
    uint32_t choice=tree_add(x,kind);
    Path p={malloc(x->b->state_size),malloc(0x8000),malloc(x->origin_size),forced,skip_irq,choice};
    if(!p.state||!p.cart||!p.origins) { free(p.state); free(p.cart); free(p.origins); return -1; }
    x->b->save(p.state); memcpy(p.cart,x->cart,0x8000);
    memcpy(p.origins,x->origins,x->origin_size);
    if(input) { x->input_queue[x->input_queued++]=p; }
    else { x->queue[x->queued++]=p; }
    x->created[phase]++; return 1;
}
static int push(Run *x,int forced,int skip_irq) { return push_to(x,forced,skip_irq,0,DP_SEGA_TREE_BRANCH); }
static size_t site(Run *x,uint16_t pc) {
    int64_t i=x->b->rom_index(pc);
    return i>=0 && (uint64_t)i<x->r->size ? (size_t)i : x->r->size+pc;
}
// GB is_padding_at parity: 8 identical $00/$FF bytes, or a view past the ROM.
static int seed_padding(const uint8_t *image,size_t size,uint64_t index) {
    if(index>=size) return 1;
    uint8_t b0=image[index];
    if(b0!=0x00 && b0!=0xff) return 0;
    for(unsigned i=1;i<8;i++) if(index+i>=size || image[index+i]!=b0) return 0;
    return 1;
}
// A seed enters below the pending frontier: branch forks keep popping first,
// and the reset path and vector entries still run ahead of every seed.
static int push_seed_front(Run *x) {
    const SeedSpec *s=&x->seeds[x->seed_next-1];
    if(x->created[x->phase]>=x->opt->max_paths) { x->r->path_cuts++; return 0; }
    if(x->queued>=x->opt->queue_cap) { x->r->queue_cuts++; return 0; }
    uint32_t choice=tree_add(x,s->kind);
    Path p={malloc(x->b->state_size),malloc(0x8000),malloc(x->origin_size),1,0,choice};
    if(!p.state||!p.cart||!p.origins) { free(p.state); free(p.cart); free(p.origins); return -1; }
    x->b->save(p.state); memcpy(p.cart,x->cart,0x8000); memcpy(p.origins,x->origins,x->origin_size);
    memmove(x->queue+1,x->queue,x->queued*sizeof(Path));
    x->queue[0]=p; x->queued++; x->created[x->phase]++; x->r->seeds_admitted++;
    return 1;
}
// Rebuild "reset state + mapper view + pc" for one seed between two running
// paths. The running machine, cartridge RAM and origins are fully restored;
// latch writes stay unobserved and out of the bank timeline (GB seed parity).
static int materialize_seed(Run *x) {
    const SeedSpec *s=&x->seeds[x->seed_next++];
    DpSegaResults *r=x->r;
    x->b->save(x->seed_scratch);
    memcpy(x->seed_scratch_cart,x->cart,0x8000);
    memcpy(x->seed_scratch_origins,x->origins,x->origin_size);
    x->b->load(x->seed_state); hook(x);
    memcpy(x->cart,x->seed_cart,0x8000);
    memcpy(x->origins,x->seed_origins,x->origin_size);
    if(s->exact && x->b->seed_window) (void)x->b->seed_window(s->pc,s->bank);
    int64_t index=x->b->rom_index(s->pc);
    int accepted=0;
    if(index<0 || (uint64_t)index>=r->size ||
       (s->exact && index!=(int64_t)((uint32_t)s->bank*0x4000+(s->pc&0x3fff)))) r->seeds_refused_view++;
    else {
        x->b->cpu()->pc=s->pc;
        accepted=push_seed_front(x);
        if(!accepted) r->seeds_refused_queue++;
    }
    x->b->load(x->seed_scratch); hook(x);
    memcpy(x->cart,x->seed_scratch_cart,0x8000);
    memcpy(x->origins,x->seed_scratch_origins,x->origin_size);
    return accepted<0?-1:0;
}
// --- sections: executed runs, liveness, verdicts (GB parity) -------------------
static void ctx_grew(Run *x,DpSegaSection *s) {
    // the liveness mask grew: previously seen contexts may now differ
    dp_set_free(&s->ctx_seen);
    dp_set_init(&s->ctx_seen,4096);
    x->r->ctx_grew++;
}
// creates or extends the section containing physical byte `at`; hi is the last
// physical byte of the instruction. Returns the section (never NULL on success).
static DpSegaSection *section_touch(Run *x,size_t at,size_t hi,int is_entry) {
    DpSegaResults *r=x->r;
    if(at>=r->size) return NULL;
    uint32_t sid=r->section_of[at];
    DpSegaSection *s;
    uint8_t bank=(uint8_t)((uint64_t)at>>14);
    uint16_t lo=(uint16_t)(at&0x3fff),hio=(uint16_t)(hi&0x3fff);
    if(sid) {
        s=&r->sections[sid-1];
        if(s->bank==bank && (uint16_t)(s->hi+1)>=lo && hio>s->hi) s->hi=hio;
    } else {
        // sequential execution continues the current run within one bank
        if(x->cur_sec && x->cur_sec->bank==bank && lo==(uint16_t)(x->cur_sec->hi+1)) {
            s=x->cur_sec;
            if(hio>s->hi) s->hi=hio;
        } else {
            if(r->section_count==r->section_capacity) {
                if(r->section_capacity>=DP_SEGA_SECTION_CAP) { r->section_cuts++; return NULL; }
                DpSegaSection *grown=realloc(r->sections,(size_t)r->section_capacity*2*sizeof(*grown));
                if(!grown) { r->section_cuts++; return NULL; }
                r->sections=grown; r->section_capacity*=2;
            }
            s=&r->sections[r->section_count];
            memset(s,0,sizeof(*s));
            s->id=r->section_count++;
            s->bank=bank; s->lo=lo; s->hi=hio;
            dp_set_init(&s->ctx_seen,4096);
        }
        r->section_of[at]=s->id+1;
    }
    s->hits++;
    if(x->forced) s->forced_hits++; else s->natural_hits++;
    if(is_entry) s->entries++;
    s->kind_bits|=(uint8_t)(x->forced?DP_SEGA_FORCED:DP_SEGA_NATURAL);
    if(x->boot_path && !x->forced) s->boot_hits++;
    if(x->irq_flag) s->kind_bits|=DP_SEGA_IRQ;
    x->irq_flag=0;
    return s;
}
static void section_live_ram(Run *x,int32_t ram) {
    DpSegaSection *s=x->cur_sec;
    if(!s||ram<0) return;
    size_t tag=(size_t)ram;
    if(tag<x->b->wram_tags) { // 8 KiB WRAM window, 256 B pages
        uint64_t bit=1ull<<(tag>>8);
        if(!(s->page_mask_wram&bit)) { s->page_mask_wram|=bit; ctx_grew(x,s); }
    } else { // cartridge RAM follows WRAM tags
        unsigned page=(unsigned)(tag-x->b->wram_tags)>>8;
        if(page<128 && !(s->page_mask_cart[page>>3]&(1u<<(page&7)))) {
            s->page_mask_cart[page>>3]|=(uint8_t)(1u<<(page&7));
            ctx_grew(x,s);
        }
    }
}
static void section_live_port(Run *x,uint16_t port) {
    DpSegaSection *s=x->cur_sec;
    if(!s) return;
    uint8_t bit=(uint8_t)(1u<<(port&7));
    if(!(s->io_read_live[(port&255)>>3]&bit)) {
        s->io_read_live[(port&255)>>3]|=bit;
        ctx_grew(x,s);
    }
}
// context projection: hash of the incoming state components this section
// reads (GB project_ctx parity). Registers, pending interrupt state and
// mapper views always join; WRAM, cartridge RAM and IN ports only when the
// section actually read them. Timing fields (R, cycles) never join.
static uint64_t project_ctx(Run *x,const DpSegaSection *s) {
    const z80_t *c=x->b->cpu();
    uint64_t h=0xcbf29ce484222325ull;
    h=fnv1a(h,&c->bc,sizeof c->bc);
    h=fnv1a(h,&c->de,sizeof c->de);
    h=fnv1a(h,&c->hl,sizeof c->hl);
    h=fnv1a(h,&c->a,sizeof c->a);
    h=fnv1a(h,&c->f,sizeof c->f);
    h=fnv1a(h,&c->ix,sizeof c->ix);
    h=fnv1a(h,&c->iy,sizeof c->iy);
    h=fnv1a_u16(h,c->sp);
    h=fnv1a(h,&c->i,sizeof c->i);
    h=fnv1a(h,&c->im,sizeof c->im);
    h=fnv1a(h,&c->iff1,sizeof c->iff1);
    h=fnv1a(h,&c->iff2,sizeof c->iff2);
    h=fnv1a(h,&c->halted,sizeof c->halted);
    h=fnv1a(h,&c->irq,sizeof c->irq);
    h=fnv1a(h,&c->nmi,sizeof c->nmi);
    h=fnv1a(h,&c->irq_vector,sizeof c->irq_vector);
    // held input state is context like the GB P1 register byte: input probes
    // fork on exactly this difference and must not be evicted as duplicates
    DpSegaInput held=x->b->input();
    h=fnv1a(h,held.pad,sizeof held.pad);
    h=fnv1a(h,&held.start,sizeof held.start);
    h=fnv1a(h,&held.pause,sizeof held.pause);
    h=fnv1a(h,&held.reset,sizeof held.reset);
    // mapper/slot views through passive physical probes
    for(unsigned i=0;i<sizeof(mapping_probe)/sizeof(mapping_probe[0]);i++)
        h=fnv1a_u32(h,(uint32_t)x->b->rom_index(mapping_probe[i]));
    if(x->msx) h=fnv1a_u32(h,dp_msx_info().slots);
    h=fnv1a_u32(h,(uint32_t)x->b->ram_index(0xc000));
    for(unsigned pg=0;pg<32;pg++)
        if(s->page_mask_wram&(1ull<<pg))
            for(unsigned o=0;o<256;o++) { uint8_t v=x->b->read((uint16_t)(0xc000+pg*256+o)); h=fnv1a(h,&v,1); }
    for(unsigned pg=0;pg<128;pg++)
        if(s->page_mask_cart[pg>>3]&(1u<<(pg&7))) h=fnv1a(h,x->cart+pg*256,256);
    // IN port values are not hashed: reading arbitrary ports through the bus
    // could touch devices; the port set itself still separates sections.
    for(unsigned i=0;i<32;i++) h=fnv1a(h,&s->io_read_live[i],1);
    return h;
}
// same-context eviction at path entry: a section whose incoming context was
// already admitted is evicted (counted, never silent)
static int context_admitted(Run *x,size_t at) {
    if(x->opt->no_context_evict || at>=x->r->size) return 1;
    DpSegaResults *r=x->r;
    uint32_t sid=r->section_of[at];
    DpSegaSection *s;
    if(sid) {
        s=&r->sections[sid-1];
        if(s->bank!=(uint8_t)((uint64_t)at>>14) || (uint16_t)(at&0x3fff)>s->hi) sid=0;
    }
    if(sid) {
        uint64_t h=project_ctx(x,s);
        int ins=dp_set_insert(&s->ctx_seen,h);
        if(ins==0) { r->evicted_ctx++; return 0; }
        if(ins<0) { r->ctx_full++; return 1; }
        s->ctx_count++;
        return 1;
    }
    // new section: admit and create it below (its masks start empty)
    s=section_touch(x,at,at,1);
    if(!s) return 1;
    uint64_t h=project_ctx(x,s);
    int ins=dp_set_insert(&s->ctx_seen,h);
    if(ins==0) { r->evicted_ctx++; return 0; }
    if(ins<0) { r->ctx_full++; return 1; }
    s->ctx_count++;
    return 1;
}
static void sections_finalize(DpSegaResults *r) {
    for(uint32_t i=0;i<r->section_count;i++) {
        DpSegaSection *s=&r->sections[i];
        if(s->boot_hits>0 || (s->kind_bits&DP_SEGA_IRQ) || s->copies>0)
            s->verdict=DP_SEGA_VERDICT_REQUIRED;
        else s->verdict=DP_SEGA_VERDICT_FORCED; // PLAYED needs sessions
    }
}
static InputSite *input_site(Run *x,size_t at,unsigned group) {
    uint32_t key=(uint32_t)at*INPUT_GROUP_COUNT+group+1;
    size_t bucket=(key*2654435761u)&(x->input_hash_size-1);
    for(size_t probe=0;probe<x->input_hash_size;probe++,bucket=(bucket+1)&(x->input_hash_size-1)) {
        InputSite *s=&x->input_sites[bucket];
        if(s->key==key) return s;
        if(!s->key) {
            if(x->r->input_site_count==x->opt->input_sites) break;
            s->key=key; x->r->input_site_count++; return s;
        }
    }
    x->r->input_site_drops++; return NULL;
}
static int same_input(DpSegaInput a,DpSegaInput b) {
    return a.pad[0]==b.pad[0] && a.pad[1]==b.pad[1] && a.start==b.start && a.pause==b.pause && a.reset==b.reset;
}
static int probe_inputs(Run *x,unsigned group,uint16_t port,size_t at,int skip_irq) {
    if(!x->opt->input_masks || !x->opt->input_total) return 1;
    InputSite *s=input_site(x,at,group); if(!s) return 1;
    if(s->blocked_epoch==x->input_epoch) { x->r->input_retry_skips++; return 1; }
    unsigned count=x->opt->input_masks;
    unsigned available=group==INPUT_PAD2_UD?4:group>=INPUT_START?2:128;
    if(count>available) count=available;
    if(s->accepted==count) { x->r->input_cap_cuts++; return 1; }
    DpSegaInput old=x->b->input(); int saved=0;
    for(unsigned i=0;i<count;i++) {
        if(s->seen[i>>3]&(1u<<(i&7))) continue;
        unsigned mask=group==INPUT_PAD2_UD?i:group>=INPUT_START?(i?64:0):x->input_catalog[i];
        DpSegaInput next=old;
        if(group==INPUT_START) next.start=!!mask;
        else if(group==INPUT_HALT_PAUSE) next.pause=!!mask;
        else if(group==INPUT_PAD2_UD) next.pad[1]=(uint8_t)((old.pad[1]&~3u)|mask);
        else {
            next.pad[group]=(uint8_t)(mask&63);
            if(group==INPUT_PAD2 && !strcmp(x->system->id,"sms")) next.reset=!!(mask&64);
            else if(!strcmp(x->system->id,"gg") || x->msx) next.start=!!(mask&64);
            else next.pause=!!(mask&64);
        }
        if(same_input(old,next)) continue;
        // A tight input poll must not save/restore a machine for every
        // rejected mask while the same pending frontier remains full.
        if(x->created[1]>=x->opt->max_paths || x->input_queued>=x->opt->queue_cap) {
            if(x->created[1]>=x->opt->max_paths) x->r->input_path_cuts++;
            else x->r->input_queue_cuts++;
            x->r->input_drops++; s->blocked_epoch=x->input_epoch;
            return 1;
        }
        if(!saved) { x->b->save(x->input_scratch); saved=1; }
        x->b->set_input(next);
        DpSegaInput actual=x->b->input();
        int accepted=same_input(old,actual)?0:push_to(x,1,skip_irq,1,DP_SEGA_TREE_INPUT);
        // A Pause edge can set NMI and reschedule devices. Releasing the
        // key alone would not undo these effects on the parent's machine.
        x->b->load(x->input_scratch); hook(x);
        if(accepted<0) return -1;
        if(!accepted) { x->r->input_drops++; continue; }
        s->seen[i>>3]|=(uint8_t)(1u<<(i&7)); s->accepted++; x->r->input_forks++;
        unsigned n=x->r->input_count;
        if(n<DP_SEGA_INPUT_LOG_CAP) {
            x->r->inputs[n].parent_path=x->r->paths; x->r->inputs[n].step=x->r->steps;
            x->r->inputs[n].pc=x->pc; x->r->inputs[n].port=port;
            x->r->inputs[n].input=actual; x->r->input_count++;
            x->r->inputs[n].halted=group==INPUT_HALT_PAUSE;
        } else x->r->input_log_overflow++;
    }
    return 1;
}
static int fork_inputs(Run *x,const z80_decoded_t *d,size_t at,int skip_irq) {
    if(!x->opt->input_masks || !x->opt->input_total) return 1;
    if(x->msx) {
        z80_t *cpu=x->b->cpu(); DpMsxInfo info=dp_msx_info();
        if(!d->group && d->opcode==0xd3 && dp_msx_bios_trap(cpu->pc,d->immediate)) {
            unsigned id=d->immediate&255,group;
            if(id==31) group=cpu->a==2?INPUT_PAD2:INPUT_PAD1;
            else if(id==32) group=cpu->a==2 || cpu->a==4?INPUT_PAD2:INPUT_PAD1;
            else if(id==36 && cpu->a==7) group=INPUT_START;
            else if((id==36 && cpu->a==8) || id==37 || id==38) group=INPUT_PAD1;
            else return 1;
            return probe_inputs(x,group,(uint16_t)id,at,skip_irq);
        }
        uint16_t port;
        if(!dp_z80_input_port(d,cpu,&port)) return 1;
        if((port&255)==0xa2 && info.ay_latch==14)
            return probe_inputs(x,(info.ay[15]&64)?INPUT_PAD2:INPUT_PAD1,port,at,skip_irq);
        if((port&255)==0xa9) {
            if((info.ppi_c&15)==7) return probe_inputs(x,INPUT_START,port,at,skip_irq);
            if((info.ppi_c&15)==8) return probe_inputs(x,INPUT_PAD1,port,at,skip_irq);
        }
        return 1;
    }
    uint16_t port;
    if(!dp_z80_input_port(d,x->b->cpu(),&port)) return 1;
    const DpSegaPort *reg=dp_sega_port(x->system,port,0);
    if(!reg) return 1;
    if(!strcmp(reg->name,"JOYPAD_A")) {
        if(probe_inputs(x,INPUT_PAD1,port,at,skip_irq)<0) return -1;
        if(strcmp(x->system->id,"gg")) return probe_inputs(x,INPUT_PAD2_UD,port,at,skip_irq);
        return 1;
    }
    if(!strcmp(reg->name,"JOYPAD_B") && strcmp(x->system->id,"gg")) return probe_inputs(x,INPUT_PAD2,port,at,skip_irq);
    if(!strcmp(reg->name,"GG_START")) return probe_inputs(x,INPUT_START,port,at,skip_irq);
    return 1;
}
DpSegaOptions dp_sega_options(void) {
    // Boot code often runs nested 16-bit delay loops before touching the VDP.
    DpSegaOptions o={.max_total=20000000,.input_total=20000000,.indirect_total=20000000,.max_steps=2000000,.max_paths=4096,.queue_cap=128,
        .loop_cap=262144,.fork_cap=2,.stagnation=32768,.copy_cap=DP_SEGA_COPY_CAP,
        .input_masks=8,.input_sites=4096,.seed_vectors=1,.seed_cap=1024,.queue_max=4096,.target_closure=1,.block_closure=1,.msx_mapper=DP_MSX_AUTO,.closure={262144,65536,256,8}}; return o;
}
DpSegaOptions dp_msx_options(void) {
    DpSegaOptions o=dp_sega_options(); o.seed_vectors=0; return o;
}
// GB dp_seed_indirect parity: every jp (hl)/(ix)/(iy) target observed at
// runtime without execution evidence seeds a further pass; the phase's
// budget refuses the remainder, counted, never silent.
static void generate_indirect_specs(Run *x) {
    DpSegaResults *r=x->r;
    DpSegaSeedGeometry g;
    int banked=x->b->seed_geometry && x->b->seed_geometry(&g) && g.banks;
    DpHashSet seen; dp_set_init(&seen,1024);
    for(size_t i=0;i<x->indirect_hash;i++) {
        const DpSegaIndirectRec *rec=&x->indirect[i];
        if(!rec->site) continue;
        for(unsigned t=0;t<rec->n;t++) {
            uint32_t target=rec->target[t];
            if(r->flags[target]&(DP_SEGA_NATURAL|DP_SEGA_FORCED)) continue;
            if(x->seed_count>=x->seed_capacity || r->indirect_seeds>=8192) { r->indirect_refused_budget++; continue; }
            if(dp_set_insert(&seen,target)!=1) continue;
            uint16_t pc=target<0x4000?(uint16_t)target:(uint16_t)(0x4000u|(target&0x3fff));
            x->seeds[x->seed_count++]=(SeedSpec){pc,(uint16_t)(target>>14),(uint8_t)!!banked,DP_SEGA_TREE_POINTER};
            r->indirect_seeds++;
        }
    }
    dp_set_free(&seen);
}
// Pointer-table scan, bank sweep and explicit entries (GB dp_seed_gb parity).
// A ROM word pointing into cartridge space seeds its target; a word flanked by
// two other switchable-window words is a dispatch entry, so every switchable
// bank's view of the target is seeded, an isolated word the boot view only.
// Dedup is per physical byte key; padding targets are skipped; every cap hit
// or refused view is counted, never silent.
static void generate_seed_specs(Run *x,const uint8_t *image,size_t size) {
    const DpSegaOptions *o=x->opt; DpSegaResults *r=x->r;
    DpSegaSeedGeometry g;
    int geo=x->b->seed_geometry && x->b->seed_geometry(&g);
    DpHashSet seen; dp_set_init(&seen,1<<16);
    if(o->seed_cap && !o->no_pointer_scan) {
        uint16_t lo=geo?g.window_lo:0, hi=geo?(uint16_t)(g.window_hi+0x4000):0;
        for(size_t a=0;a+1<size;a++) {
            uint16_t v=(uint16_t)(image[a]|(image[a+1]<<8));
            if(v<8 || v>=0xc000) continue;
            uint16_t pv=a>=2?(uint16_t)(image[a-2]|(image[a-1]<<8)):0;
            uint16_t nv=a+3<size?(uint16_t)(image[a+2]|(image[a+3]<<8)):0;
            int tab=geo && g.banks && v>=lo && v<hi && pv>=lo && pv<hi && nv>=lo && nv<hi;
            if(tab) {
                for(unsigned b=0;b<g.banks;b++) {
                    uint64_t key=(uint64_t)b*0x4000+(v&0x3fff);
                    if(key>=size) continue; // a view into the padded tail
                    if(seed_padding(image,size,key)) { r->seeds_skipped_padding++; continue; }
                    if(x->seed_count>=x->seed_capacity || r->seeds_pointer>=o->seed_cap) { r->seeds_refused_cap++; continue; }
                    if(dp_set_insert(&seen,key)!=1) continue;
                    x->seeds[x->seed_count++]=(SeedSpec){v,(uint16_t)b,1,DP_SEGA_TREE_POINTER};
                    r->seeds_pointer++;
                }
            } else {
                int64_t key=x->b->rom_index(v); // the boot view, machine at reset
                if(key<0 || (uint64_t)key>=size) continue;
                if(seed_padding(image,size,(uint64_t)key)) { r->seeds_skipped_padding++; continue; }
                if(x->seed_count>=x->seed_capacity || r->seeds_pointer>=o->seed_cap) { r->seeds_refused_cap++; continue; }
                if(dp_set_insert(&seen,(uint64_t)key)!=1) continue;
                x->seeds[x->seed_count++]=(SeedSpec){v,0,0,DP_SEGA_TREE_POINTER};
                r->seeds_pointer++;
            }
        }
    }
    if(o->seed_banks && geo && g.banks) {
        for(uint32_t w=g.window_lo;w<=g.window_hi;w+=0x4000) {
            for(unsigned b=0;b<g.banks && x->seed_count<x->seed_capacity;b++) {
                uint64_t key=(uint64_t)b*0x4000; // w is a window base: offset 0
                if(key>=size) continue;
                if(seed_padding(image,size,key)) { r->seeds_skipped_padding++; continue; }
                if(dp_set_insert(&seen,key)!=1) continue;
                x->seeds[x->seed_count++]=(SeedSpec){(uint16_t)w,(uint16_t)b,1,DP_SEGA_TREE_BANK};
                r->seeds_bank++;
            }
        }
    }
    for(unsigned i=0;i<o->entry_count && x->seed_count<x->seed_capacity;i++) {
        x->seeds[x->seed_count++]=(SeedSpec){o->entry[i].addr,(uint16_t)o->entry[i].bank,1,DP_SEGA_TREE_ENTRY};
        r->seeds_entry++;
    }
    dp_set_free(&seen);
}
void dp_sega_results_free(DpSegaResults *r) {
    for(uint32_t i=0;i<r->section_count;i++) dp_set_free(&r->sections[i].ctx_seen);
    free(r->sections); free(r->section_of); free(r->tree_nodes);
    free(r->flags); free(r->copies); memset(r,0,sizeof(*r));
}
int dp_sega_explore(const DpSegaSystem *s,const uint8_t *rom,size_t size,const DpSegaOptions *o,DpSegaResults *r) {
    if(!s||!rom||!size||!o||!r||!o->max_total||!o->max_steps||!o->max_paths||!o->queue_cap||!o->loop_cap||o->fork_cap>255) return 0;
    int sg=!strcmp(s->id,"sg"),msx=!strcmp(s->id,"msx");
    if(!sg && !msx && strcmp(s->id,"sms") && strcmp(s->id,"gg")) return 0;
    if(o->sega_mapper<0 || o->sega_mapper>DP_SEGA_MAPPER_KOREAN150 || ((sg || msx) && o->sega_mapper)) return 0;
    if(o->sega_mapper==DP_SEGA_MAPPER_KOREAN150 && size>0x200000) return 0;
    if(msx && (size>0x100000 || o->msx_mapper<DP_MSX_AUTO || o->msx_mapper>DP_MSX_KONAMI_SCC)) return 0;
    if((sg && size>0xc000) || size>0x400000 || o->queue_cap>4096 || o->copy_cap>65536 || o->input_masks>128 || o->input_sites>65536 ||
       o->seed_cap>65536 || o->entry_count>32 || o->queue_max>65536 || o->queue_max<o->queue_cap ||
       (o->input_masks && o->input_total>UINT64_MAX-o->max_total)) return 0;
    for(unsigned i=0;i<o->entry_count;i++) if(o->entry[i].bank>0xffff) return 0;
    if(o->closure.nodes && (o->closure.nodes>1048576 || !o->closure.seeds || o->closure.seeds>1048576 ||
       !o->closure.views || o->closure.views>4096 || !o->closure.contexts || o->closure.contexts>64)) return 0;
    memset(r,0,sizeof(*r)); r->size=size;
    size_t padded=(size+0x3fff)&~(size_t)0x3fff, sites=size+65536;
    uint8_t *image=malloc(padded); DpSegaVisits visits={0};
    Run x={0}; x.b=msx?dp_msx_backend():sg?dp_sega_backend_sg():dp_sega_backend_gg(); x.msx=msx; x.system=s; x.opt=o; x.r=r; x.input_epoch=1;
    r->input_site_capacity=o->input_sites; r->input_mask_limit=o->input_masks;
    r->phase_limits[0]=o->max_total; r->phase_limits[1]=o->input_masks?o->input_total:0;
    if(o->input_masks && o->input_total) {
        x.input_hash_size=1;
        while(x.input_hash_size<(size_t)o->input_sites*2) x.input_hash_size*=2;
        x.input_sites=calloc(x.input_hash_size,sizeof(*x.input_sites));
        x.input_scratch=malloc(x.b->state_size);
        const uint8_t first[]={0,16,64,32,1,2,4,8};
        memcpy(x.input_catalog,first,sizeof(first)); unsigned count=sizeof(first);
        for(unsigned mask=0;mask<128;mask++) {
            int duplicate=0;
            for(unsigned i=0;i<sizeof(first);i++) duplicate|=first[i]==mask;
            if(!duplicate) x.input_catalog[count++]=(uint8_t)mask;
        }
    }
    x.indirect_hash=8192;
    x.indirect=calloc(x.indirect_hash,sizeof(*x.indirect));
    x.origin_size=sizeof(Origins)+x.b->ram_tags*sizeof(DpZ80Origin);
    x.image=image; x.origins=calloc(1,x.origin_size);
    x.copy_hash_size=1;
    while(x.copy_hash_size<(size_t)o->copy_cap*2) x.copy_hash_size*=2;
    x.copy_hash=calloc(x.copy_hash_size,sizeof(*x.copy_hash));
    r->flags=calloc(size,1); x.cart=calloc(0x8000,1);
    r->section_of=calloc(size,sizeof(*r->section_of));
    r->section_capacity=256;
    r->sections=calloc(r->section_capacity,sizeof(*r->sections));
    r->copy_capacity=o->copy_cap;
    r->copies=calloc(o->copy_cap?o->copy_cap:1,sizeof(*r->copies));
    x.forks=calloc(sites,1); x.sides=calloc(sites,1); x.irq_forks=calloc(sites,1);
    x.queue_capacity=o->queue_cap;
    x.queue=calloc(o->queue_cap,sizeof(*x.queue));
    x.input_queue=calloc(o->queue_cap,sizeof(*x.input_queue));
    int ok=image&&r->flags&&r->copies&&x.origins&&x.copy_hash&&x.cart&&x.forks&&x.sides&&x.irq_forks&&x.queue&&x.input_queue&&
        r->section_of&&r->sections&&x.indirect&&
        (!o->input_masks || !o->input_total || (x.input_sites&&x.input_scratch));
    if(!ok) goto cleanup;
    memset(image,0xff,padded); memcpy(image,rom,size);
    // Baseline SG boards have no cartridge RAM; larger board profiles are explicit future work.
    if(!x.b->init(image,size,sg?NULL:x.cart,msx?o->msx_mapper:!strcmp(s->id,"sms"))) { ok=0; goto cleanup; }
    if(o->sega_mapper && (!x.b->select_mapper || !x.b->select_mapper((unsigned)o->sega_mapper))) { ok=0; goto cleanup; }
    if(!sg && !msx) { r->sega_mapper=x.b->mapper(); r->sega_mapper_explicit=!!o->sega_mapper; }
    hook(&x);
    if(o->spool_path) {
        int resume;
        if(o->resume) {
            x.spool=fopen(o->spool_path,"r+b");
            resume=x.spool && !fseek(x.spool,0,SEEK_END) && (x.spool_wr=(uint64_t)ftell(x.spool))>0;
            if(resume) x.spool_bytes=x.spool_wr;
            else { if(x.spool) fclose(x.spool); x.spool=NULL; }
        } else {
            x.spool=fopen(o->spool_path,"w+b");
            resume=0;
        }
        if(!x.spool) { ok=0; goto cleanup; }
    }
    if(msx) { r->msx_mapper=dp_msx_info().mapper; dp_msx_observe(msx_event,&x); }
    r->closure.limits=o->closure; r->target_closure.limits=o->closure;
    if(!o->target_closure) r->target_closure.limits.nodes=0;
    r->block_closure.limits=o->closure;
    if(!o->target_closure || !o->block_closure) r->block_closure.limits.nodes=0;
    if(o->closure.nodes) {
        x.closure=dp_sega_closure_create(x.b,s,image,size,r->flags,o->closure,&r->closure);
        if(!x.closure || (o->target_closure && !dp_sega_closure_targets(x.closure,&r->target_closure)) ||
           (o->target_closure && o->block_closure && !dp_sega_closure_blocks(x.closure,&r->block_closure))) { ok=0; goto cleanup; }
    }
    // Seed base: the reset state every seed starts from. Generation reads the
    // boot view through rom_index while the machine has not run yet.
    if(o->seed_cap || o->entry_count || o->seed_banks || o->indirect_total) {
        x.seed_capacity=o->seed_cap+o->entry_count+3*256+8192;
        x.seeds=malloc((size_t)x.seed_capacity*sizeof(*x.seeds));
        x.seed_state=malloc(x.b->state_size); x.seed_scratch=malloc(x.b->state_size);
        x.seed_cart=malloc(0x8000); x.seed_scratch_cart=malloc(0x8000);
        x.seed_origins=malloc(x.origin_size); x.seed_scratch_origins=malloc(x.origin_size);
        if(!x.seeds||!x.seed_state||!x.seed_scratch||!x.seed_cart||!x.seed_scratch_cart||
           !x.seed_origins||!x.seed_scratch_origins) { ok=0; goto cleanup; }
        x.b->save(x.seed_state);
        memcpy(x.seed_cart,x.cart,0x8000);
        memcpy(x.seed_origins,x.origins,x.origin_size);
        generate_seed_specs(&x,image,size);
    }
    if(push_to(&x,0,0,0,DP_SEGA_TREE_RESET)<0) { ok=0; goto cleanup; }
    if(o->seed_vectors && !msx) {
        const uint16_t entries[]={8,16,24,32,40,48,56,0x66};
        for(unsigned j=0;j<sizeof(entries)/sizeof(entries[0]);j++) {
            uint16_t pc=entries[j]; int code=0;
            for(unsigned k=0;k<8;k++) { uint8_t v=x.b->read(pc+k); code|=(v!=0 && v!=0xff); }
            if(!code) continue;
            x.b->cpu()->pc=pc;
            if(push_to(&x,1,0,0,DP_SEGA_TREE_VECTOR)<0) { ok=0; goto cleanup; }
        }
    }
    // FIFO root first, then depth-first forks: seed creation above is reversed once.
    for(uint32_t i=0;i<x.queued/2;i++) { Path p=x.queue[i]; x.queue[i]=x.queue[x.queued-i-1]; x.queue[x.queued-i-1]=p; }
    r->phase_limits[2]=o->indirect_total;
    for(unsigned phase=0;phase<3;phase++) {
        x.phase=phase;
        uint64_t start_steps=r->steps,start_paths=r->paths,budget=r->phase_limits[phase];
        if(phase==2 && budget) generate_indirect_specs(&x);
        if(phase && budget) { memset(x.forks,0,sites); memset(x.sides,0,sites); memset(x.irq_forks,0,sites); }
        while((x.queued || (phase && x.input_queued) ||
               (phase!=1 && x.seed_next<x.seed_count) ||
               (!phase && x.spool_rd<x.spool_wr)) && r->steps-start_steps<budget) {
            // Seeds materialize below the pending frontier while the base
            // phase has room: forks keep popping first, reset and vectors run
            // ahead of every seed, and leftovers close as counted pending work.
            // The indirect pass is phase 2: its specs were generated from the
            // observations of the passes before it.
            if(phase!=1) while(x.seed_next<x.seed_count && x.queued<x.opt->queue_cap) {
                if(materialize_seed(&x)<0) { ok=0; goto cleanup; }
            }
            // Keep input seeds and their branch children moving in turn. Their
            // separate frontier never displaces the original exploration pass.
            // spooled states come back while the frontier runs low
            // spooled states come back while the base frontier runs low;
            // a later phase leaves them to --resume rather than re-funding
            // the exhausted pass with its own budget
            while(!phase && x.queued<x.queue_capacity && x.spool_rd<x.spool_wr) {
                uint64_t hdr[4];
                if(fseek(x.spool,(long)x.spool_rd,SEEK_SET) ||
                   fread(hdr,sizeof hdr,1,x.spool)!=1 || hdr[0]!=DP_SPOOL_MAGIC) break;
                if(hdr[2]!=x.b->state_size || hdr[3]!=x.origin_size) break;
                Path p={malloc(x.b->state_size),malloc(0x8000),malloc(x.origin_size),
                    (int)(hdr[1]&1),(int)((hdr[1]>>1)&1),(uint32_t)(hdr[1]>>32)};
                if(!p.state||!p.cart||!p.origins) { free_path(p); break; }
                if(fread(p.state,1,x.b->state_size,x.spool)!=x.b->state_size ||
                   fread(p.cart,1,0x8000,x.spool)!=0x8000 ||
                   fread(p.origins,1,x.origin_size,x.spool)!=x.origin_size) { free_path(p); break; }
                x.spool_rd+=4*8+hdr[2]+0x8000+hdr[3]; r->spool_reads++;
                memmove(x.queue+1,x.queue,x.queued*sizeof(Path));
                x.queue[0]=p; x.queued++;
            }
                        int from_input=phase && x.input_queued && (!x.queued || ((r->paths-start_paths)&1)==0);
            Path p=from_input?x.input_queue[--x.input_queued]:x.queue[--x.queued];
            if(from_input) x.input_epoch++;
            x.b->load(p.state); memcpy(x.cart,p.cart,0x8000);
            dp_sega_closure_invalidate(x.closure);
            memcpy(x.origins,p.origins,x.origin_size);
            free_path(p); hook(&x); x.forced=p.forced;
            dp_sega_visits_reset(&visits); r->paths++;
            x.cur_choice=p.choice;
            x.boot_path=r->paths==1 && !p.forced; // natural work on the reset path
            // same-context eviction: the entry section of an already admitted
            // incoming context is closed without running (counted, GB parity)
            if(!context_admitted(&x,site(&x,x.b->cpu()->pc))) {
                x.boot_path=0;
                tree_close(&x,p.choice,DP_SEGA_ISSUE_EVICTED);
                continue;
            }
            x.cur_sec=NULL;
            uint32_t n=0,quiet=0;
            int closed=0;
            for(;n<o->max_steps && r->steps-start_steps<budget;n++) {
                uint64_t before_discoveries=x.discoveries;
                z80_t *c=x.b->cpu(); x.pc=c->pc; x.fetching=0;
                dp_z80_provenance_clear_plan(&x.origins->cpu);
                if(c->fault) { r->faults++; break; }
                size_t at=site(&x,c->pc);
                int admitted=dp_sega_visits_enter(&visits,(uint32_t)at,c->pc,o->loop_cap);
                if(admitted<0) { ok=0; goto cleanup; }
                if(!admitted) { r->loop_cuts++; break; }
                if(c->halted && (sg || !strcmp(s->id,"sms")) && probe_inputs(&x,INPUT_HALT_PAUSE,0,at,p.skip_irq)<0) { ok=0; goto cleanup; }
                int interrupt=c->nmi || (c->irq&&c->iff1&&!c->ei_delay);
                DpSegaInterrupt irq_event={.path=r->paths,.step=r->steps,.pc=c->pc,.sp=c->sp,
                    .mode=c->nmi?3:c->im,.vector=c->irq_vector,.i=c->i,
                    .sources=interrupt&&!c->nmi?x.b->irq_sources():0,.forced=x.forced};
                if(interrupt && !p.skip_irq) {
                    if(x.irq_forks[at]<o->fork_cap) {
                        int accepted=push_to(&x,1,1,0,DP_SEGA_TREE_IRQ);
                        if(accepted<0) { ok=0; goto cleanup; }
                        if(accepted) { x.irq_forks[at]++; r->irq_forks++; }
                    } else r->fork_cuts++;
                }
                unsigned cycles;
                int service_suppressed=p.skip_irq;
                if(p.skip_irq) {
                    if(interrupt) r->interrupt_suppressed++;
                    uint8_t irq=c->irq,nmi=c->nmi; c->irq=c->nmi=0;
                    cycles=z80_service(c); c->irq=irq; c->nmi=nmi;
                } else cycles=z80_service(c);
                if(interrupt && !p.skip_irq && cycles) {
                    dp_z80_provenance_stack_changed(&x.origins->cpu);
                    record_interrupt(&x,irq_event);
                    x.irq_flag=1; // the handler's section runs inside an IRQ
                }
                p.skip_irq=0;
                if(c->fault) { r->faults++; break; }
                if(!cycles) {
                    DpZ80Instruction in; uint8_t bytes[65536];
                    // Side-effect-free analysis cannot enter RAM/IO through the static helper.
                    // The runtime decoder fetches from the actual mapped instruction bus once.
                    x.fetching=1;
                    z80_decoded_t d;
                    if(!z80_decode(c,&d)) { x.fetching=0; r->decode_cuts++; break; }
                    x.fetching=0;
                    // Classification uses the exact bytes fetched from this machine view.
                    for(unsigned j=0;j<d.length;j++) bytes[j]=x.b->read((uint16_t)(c->pc+j));
                    if(!dp_z80_decode(bytes,d.length,c->pc,&in)) { r->decode_cuts++; break; }
                    // every instruction belongs to the section its pc is in
                    x.cur_sec=section_touch(&x,at,at+d.length-1,n==0 || at!=x.prev_at+1);
                    x.prev_at=at+d.length-1;
                    if(in.condition>=0 || in.flow==DP_Z80_REPEAT) {
                        uint8_t source=in.flow==DP_Z80_REPEAT?x.b->read(c->hl):0;
                        int taken=in.flow==DP_Z80_REPEAT?dp_z80_repeat_taken(&in,c,source):dp_z80_condition(in.condition,c);
                        x.sides[at]|=(uint8_t)(1u<<taken);
                        if(!(x.sides[at]&(1u<<!taken))) {
                            if(x.forks[at]<o->fork_cap) {
                                z80_t saved=*c;
                                DpZ80Provenance saved_origins=x.origins->cpu;
                                if(in.flow==DP_Z80_REPEAT) dp_z80_force_repeat(&in,c,source,!taken);
                                else dp_z80_force(in.condition,c,!taken);
                                dp_z80_provenance_force(&x.origins->cpu,&in,&saved,c);
                                int accepted=push_to(&x,1,service_suppressed,0,DP_SEGA_TREE_BRANCH);
                                *c=saved;
                                x.origins->cpu=saved_origins;
                                if(accepted<0) { ok=0; goto cleanup; }
                                if(accepted) { x.forks[at]++; r->forks++; }
                            } else r->fork_cuts++;
                        }
                    }
                    // input probes belong to their dedicated pass; the
                    // indirect phase never opens a second input frontier
                    if(phase!=2 && fork_inputs(&x,&d,at,service_suppressed)<0) { ok=0; goto cleanup; }
                    int outside=0;
                    for(unsigned j=0;j<d.length;j++) {
                        int64_t index=x.b->rom_index((uint16_t)(c->pc+j));
                        if(index>=0 && (uint64_t)index<size) {
                            if(!(r->flags[index]&(DP_SEGA_NATURAL|DP_SEGA_FORCED))) x.discoveries++;
                            r->flags[index]|=x.forced?DP_SEGA_FORCED:DP_SEGA_NATURAL;
                        }
                        else outside=1;
                    }
                    if(outside) r->exec_outside++;
                    dp_sega_closure_observe(x.closure);
                    dp_z80_provenance_begin(&x.origins->cpu,c,&d,origin_read,&x);
                    cycles=z80_execute_decoded(c,&d); z80_finish(c,cycles);
                    if(in.flow==DP_Z80_INDIRECT) record_indirect(&x,at,c->pc);
                }
                r->steps++; x.b->advance(cycles);
                quiet=x.discoveries==before_discoveries?quiet+1:0;
                // Leave the concrete reset path its full budget: initialisation
                // can contain long delay loops. Forced probes yield on stagnation.
                if(x.forced && o->stagnation && quiet>=o->stagnation) { r->stagnation_cuts++; closed=1; break; }
            }
            if(n==o->max_steps) r->step_cuts++;
            // The first successor after a concrete budget cut still has an
            // observed mapping, including a mapper write or interrupt push.
            dp_sega_closure_observe(x.closure);
            if(r->steps-start_steps==budget && n<o->max_steps && !closed) r->phase_pending[phase]++;
            flush_copy(&x);
            tree_close(&x,x.cur_choice,DP_SEGA_ISSUE_DONE);
            x.cur_choice=0;
        }
        r->phase_steps[phase]=r->steps-start_steps; r->phase_paths[phase]=r->paths-start_paths;
        if(phase!=1) r->seeds_pending=x.seed_count-x.seed_next;
        if(phase==2) r->indirect_pending=r->seeds_pending;
        r->phase_pending[phase]+=x.queued+(phase==1?x.input_queued:0)+
            (phase!=1?r->seeds_pending:0);
        r->global_cuts+=r->phase_pending[phase];
        for(size_t i=0;i<size;i++) r->phase_known[phase]+=!!r->flags[i];
        // A phase wall closes its pending branch work. Input seeds have their
        // own declared budget, so finishing the first pass is never regressed.
        // An open spool keeps the remainders resumable instead of dropping them.
        for(uint32_t i=0;i<x.queued;i++) {
            if(x.spool) spool_write_path(&x,&x.queue[i],1);
            else tree_close(&x,x.queue[i].choice,DP_SEGA_ISSUE_BUDGET);
            free_path(x.queue[i]);
        }
        x.queued=0;
    }
    dp_sega_closure_run(x.closure);
    sections_finalize(r);
    // distinct observed indirect sites, counted whatever the pass budgets did
    for(size_t i=0;i<x.indirect_hash;i++) if(x.indirect[i].site) r->indirect_sites++;
    if(o->tree_path && r->tree_count) {
        FILE *t=fopen(o->tree_path,"wb");
        if(t) {
            uint32_t hdr[2]={0x5a545044u,r->tree_count}; // 'DPTZ' + version 1 in low bits
            hdr[1]=1;
            if(fwrite(hdr,sizeof hdr,1,t)==1) {
                for(uint32_t i=0;i<r->tree_count;i++) {
                    const DpSegaTreeNode *n=&r->tree_nodes[i];
                    uint32_t rec[2]={n->parent,n->site};
                    uint16_t pc=n->pc; uint8_t ki[2]={n->kind,n->issue};
                    if(fwrite(rec,sizeof rec,1,t)!=1 || fwrite(&pc,2,1,t)!=1 ||
                       fwrite(ki,1,2,t)!=2) break;
                }
            }
            ok&=!ferror(t);
            if(fclose(t)) ok=0;
        } else ok=0;
    }
cleanup:
    if(x.b->close) x.b->close();
    dp_sega_closure_free(x.closure);
    if(x.queue) for(uint32_t i=0;i<x.queued;i++) free_path(x.queue[i]);
    if(x.input_queue) for(uint32_t i=0;i<x.input_queued;i++) free_path(x.input_queue[i]);
    free(x.queue); free(x.forks); free(x.sides); free(x.irq_forks); free(x.cart); free(image);
    r->loop_context_peak=visits.peak; r->loop_context_capacity=visits.capacity; r->loop_context_growths=visits.growths;
    dp_sega_visits_free(&visits);
    free(x.origins);
    free(x.copy_hash);
    free(x.input_sites); free(x.input_scratch);
    free(x.input_queue);
    free(x.indirect);
    free(x.seeds); free(x.seed_state); free(x.seed_cart); free(x.seed_origins);
    free(x.seed_scratch); free(x.seed_scratch_cart); free(x.seed_scratch_origins);
    if(x.spool) fclose(x.spool);
    if(!ok) { free(r->sections); free(r->section_of); dp_sega_results_free(r); }
    return ok;
}
static const char *class_of(uint8_t f) {
    return f&DP_SEGA_NATURAL?"code-natural":f&DP_SEGA_COPIED?"data-copied":
        f&DP_SEGA_FORCED?"code-forced":f&DP_SEGA_DATA?"data-read":f&DP_SEGA_STATIC?"code-static":f&DP_SEGA_STATIC_DATA?"data-static":"unknown";
}
void dp_sega_map(FILE *out,const DpSegaResults *r) {
    fprintf(out,"# physical-bank offset-start offset-end class (inclusive, 16 KiB banks)\n");
    for(size_t at=0;at<r->size;) {
        size_t end=at+1;
        while(end<r->size && (end>>14)==(at>>14) && !strcmp(class_of(r->flags[at]),class_of(r->flags[end]))) end++;
        fprintf(out,"%04zX %04zX %04zX %s\n",at>>14,at&0x3fff,(end-1)&0x3fff,class_of(r->flags[at])); at=end;
    }
}
void dp_sega_evidence(FILE *out,const DpSegaResults *r) {
    fputs("# physical-bank offset-start offset-end evidence-mask (inclusive, 16 KiB banks)\n"
          "# 01=code-natural 02=code-forced 04=data-read 08=data-copied 10=code-static 20=data-static\n",out);
    for(size_t at=0;at<r->size;) {
        size_t end=at+1;
        while(end<r->size && (end>>14)==(at>>14) && r->flags[end]==r->flags[at]) end++;
        fprintf(out,"%04zX %04zX %04zX %02X\n",at>>14,at&0x3fff,(end-1)&0x3fff,(unsigned)r->flags[at]);
        at=end;
    }
}
void dp_sega_report(FILE *out,const DpSegaSystem *s,const DpSegaResults *r) {
    uint64_t natural=0,forced=0,data=0,known=0,static_code=0,static_data=0;
    for(size_t i=0;i<r->size;i++) {
        uint8_t f=r->flags[i]; known+=!!f;
        if(f&DP_SEGA_NATURAL) natural++;
        else if(f&DP_SEGA_COPIED) data++;
        else if(f&DP_SEGA_FORCED) forced++;
        else if(f&DP_SEGA_DATA) data++;
        else if(f&DP_SEGA_STATIC) static_code++;
        else if(f&DP_SEGA_STATIC_DATA) static_data++;
    }
    fprintf(out,"%s — bounded Z80 exploration (m0boy machine observations)\n",s->name);
    fprintf(out,"ROM %zu bytes; known %"PRIu64" (%.2f%%); unknown %"PRIu64"\n",r->size,known,100.0*known/r->size,(uint64_t)r->size-known);
    fprintf(out,"code-natural %"PRIu64"; code-forced %"PRIu64"; code-static %"PRIu64"; data %"PRIu64"; data-static %"PRIu64"\n",natural,forced,static_code,data,static_data);
    if(!strcmp(s->id,"sms") || !strcmp(s->id,"gg")) {
        static const char *const names[]={"none","sega","codemasters","korean150"};
        fprintf(out,"Sega mapper: %s (%s)\n",r->sega_mapper<4?names[r->sega_mapper]:"unknown",r->sega_mapper_explicit?"explicit":"auto");
    }
    const DpSegaClosureStats *cs=&r->closure;
    if(!strcmp(s->id,"msx")) {
        static const char *const names[]={"plain","ascii8","ascii16","konami","konami-scc"};
        fprintf(out,"MSX mapper: %s\n",r->msx_mapper<5?names[r->msx_mapper]:"unknown");
        for(unsigned i=0;i<64;i++) if(r->bios_calls[i]) fprintf(out,"  simulated BIOS id $%02X calls %"PRIu64"\n",i,r->bios_calls[i]);
        fprintf(out,"AY register timeline overflow %"PRIu64"\n",r->ay_log_overflow);
        for(unsigned i=0;i<16;i++) if(r->ay_writes[i]) fprintf(out,"  AY R%u writes %"PRIu64"\n",i,r->ay_writes[i]);
        for(unsigned i=0;i<r->ay_count;i++) {
            const DpSegaRegisterWrite *e=&r->ay_log[i];
            fprintf(out,"  AY path %"PRIu64" step %"PRIu64" PC $%04X R%u=$%02X source %s %s\n",e->path,e->step,e->pc,e->reg,e->value,e->port?"IO":"BIOS",e->forced?"forced":"natural");
        }
    }
    fprintf(out,"static closure: %"PRIu64" new code bytes; seeds %"PRIu64"/%u; nodes %"PRIu64"/%u; processed %"PRIu64"; views %u/%u; contexts/site/view %u\n",cs->new_bytes,cs->seeds,cs->limits.seeds,cs->nodes,cs->limits.nodes,cs->processed,cs->views,cs->limits.views,cs->limits.contexts);
    fprintf(out,"closure cuts: seed skips %"PRIu64"; views %"PRIu64"; contexts %"PRIu64"; nodes %"PRIu64"; unmapped %"PRIu64"; ambiguous %"PRIu64"; data %"PRIu64"; padding %"PRIu64"; decode %"PRIu64"; indirect %"PRIu64"; HALT %"PRIu64"; mapping barriers %"PRIu64"; returns resolved %"PRIu64"\n",cs->seed_skips,cs->view_skips,cs->context_cuts,cs->node_cuts,cs->unmapped,cs->ambiguous,cs->data,cs->padding,cs->decode,cs->indirect,cs->halted,cs->map_barriers,cs->returns_resolved);
    fprintf(out,"closure stack: proofs cap %u; invalidations %"PRIu64"; cuts %"PRIu64"\n",DP_SEGA_CLOSURE_STACK,cs->stack_invalidations,cs->stack_cuts);
    fprintf(out,"closure ROM data: %"PRIu64" static reads; %"PRIu64" new data proofs\n",cs->static_data_reads,cs->static_data_bytes);
    const DpSegaClosureStats *ts=&r->target_closure;
    fprintf(out,"target closure: %"PRIu64" new code bytes; seeds %"PRIu64"/%u; nodes %"PRIu64"/%u; processed %"PRIu64"; views %u/%u; contexts/site/view %u\n",ts->new_bytes,ts->seeds,ts->limits.seeds,ts->nodes,ts->limits.nodes,ts->processed,ts->views,ts->limits.views,ts->limits.contexts);
    fprintf(out,"target cuts: seeds %"PRIu64"; views %"PRIu64"; contexts %"PRIu64"; nodes %"PRIu64"; unmapped %"PRIu64"; ambiguous %"PRIu64"; data %"PRIu64"; padding %"PRIu64"; decode %"PRIu64"; indirect %"PRIu64"; HALT %"PRIu64"; mapping barriers %"PRIu64"; returns resolved %"PRIu64"\n",ts->seed_skips,ts->view_skips,ts->context_cuts,ts->node_cuts,ts->unmapped,ts->ambiguous,ts->data,ts->padding,ts->decode,ts->indirect,ts->halted,ts->map_barriers,ts->returns_resolved);
    fprintf(out,"target stack: proofs cap %u; invalidations %"PRIu64"; cuts %"PRIu64"; indirect resolved %"PRIu64"; memory addresses resolved %"PRIu64"\n",DP_SEGA_CLOSURE_STACK,ts->stack_invalidations,ts->stack_cuts,ts->indirect_resolved,ts->memory_resolved);
    fprintf(out,"target ROM data: %"PRIu64" static reads; %"PRIu64" new data proofs\n",ts->static_data_reads,ts->static_data_bytes);
    const DpSegaClosureStats *bs=&r->block_closure;
    fprintf(out,"block closure: %"PRIu64" new code bytes; seeds %"PRIu64"/%u; nodes %"PRIu64"/%u; processed %"PRIu64"; views %u/%u; contexts/site/view %u (remaining target budgets)\n",bs->new_bytes,bs->seeds,bs->limits.seeds,bs->nodes,bs->limits.nodes,bs->processed,bs->views,bs->limits.views,bs->limits.contexts);
    fprintf(out,"block cuts: seeds %"PRIu64"; views %"PRIu64"; contexts %"PRIu64"; nodes %"PRIu64"; unmapped %"PRIu64"; ambiguous %"PRIu64"; data %"PRIu64"; padding %"PRIu64"; decode %"PRIu64"; indirect %"PRIu64"; HALT %"PRIu64"; mapping barriers %"PRIu64"; returns resolved %"PRIu64"\n",bs->seed_skips,bs->view_skips,bs->context_cuts,bs->node_cuts,bs->unmapped,bs->ambiguous,bs->data,bs->padding,bs->decode,bs->indirect,bs->halted,bs->map_barriers,bs->returns_resolved);
    fprintf(out,"block stack: proofs cap %u; invalidations %"PRIu64"; cuts %"PRIu64"; indirect resolved %"PRIu64"; memory addresses resolved %"PRIu64"\n",DP_SEGA_CLOSURE_STACK,bs->stack_invalidations,bs->stack_cuts,bs->indirect_resolved,bs->memory_resolved);
    fprintf(out,"block ROM data: %"PRIu64" static reads; %"PRIu64" new data proofs\n",bs->static_data_reads,bs->static_data_bytes);
    fprintf(out,"steps %"PRIu64"; paths %"PRIu64"; branch forks %"PRIu64"; IRQ forks %"PRIu64"; exec_outside %"PRIu64"\n",r->steps,r->paths,r->forks,r->irq_forks,r->exec_outside);
    fprintf(out,"interrupts accepted: IM0 %"PRIu64"; IM1 %"PRIu64"; IM2 %"PRIu64"; NMI %"PRIu64"; delayed probes %"PRIu64"; timeline overflow %"PRIu64"\n",r->interrupts[0],r->interrupts[1],r->interrupts[2],r->interrupts[3],r->interrupt_suppressed,r->interrupt_log_overflow);
    for(unsigned j=0;j<r->interrupt_count;j++) {
        const DpSegaInterrupt *e=&r->interrupt_log[j];
        fprintf(out,"  interrupt path %"PRIu64" step %"PRIu64" PC $%04X -> $%04X ROM-index %"PRId64" SP $%04X %s",e->path,e->step,e->pc,e->target,e->target_rom,e->sp,e->mode==3?"NMI":"INT");
        if(e->mode!=3) fprintf(out," IM%u I=$%02X bus=$%02X frame=%u line=%u",e->mode,e->i,e->vector,!!(e->sources&DP_SEGA_IRQ_FRAME),!!(e->sources&DP_SEGA_IRQ_LINE));
        fprintf(out," %s\n",e->forced?"forced":"natural");
    }
    fprintf(out,"VDP register commands: ignored %"PRIu64"; timeline overflow %"PRIu64"\n",r->vdp_register_ignored,r->register_log_overflow);
    for(unsigned j=0;j<16;j++) if(r->vdp_register_writes[j])
        fprintf(out,"  VDP R%u writes %"PRIu64"\n",j,r->vdp_register_writes[j]);
    for(unsigned j=0;j<r->register_count;j++) {
        const DpSegaRegisterWrite *e=&r->register_log[j];
        fprintf(out,"  VDP path %"PRIu64" step %"PRIu64" PC $%04X port $%04X R%u=$%02X %s %s\n",e->path,e->step,e->pc,e->port,e->reg,e->value,e->ignored?"ignored":"written",e->forced?"forced":"natural");
    }
    fprintf(out,"phases: base %"PRIu64"/%"PRIu64" steps, %"PRIu64" paths, %"PRIu64" known, %"PRIu64" pending; inputs %"PRIu64"/%"PRIu64" steps, %"PRIu64" paths, %"PRIu64" known, %"PRIu64" pending; indirect %"PRIu64"/%"PRIu64" steps, %"PRIu64" paths, %"PRIu64" known, %"PRIu64" pending\n",
        r->phase_steps[0],r->phase_limits[0],r->phase_paths[0],r->phase_known[0],r->phase_pending[0],
        r->phase_steps[1],r->phase_limits[1],r->phase_paths[1],r->phase_known[1],r->phase_pending[1],
        r->phase_steps[2],r->phase_limits[2],r->phase_paths[2],r->phase_known[2],r->phase_pending[2]);
    fprintf(out,"indirect jumps: sites %"PRIu64"; targets %"PRIu64"; seeded %"PRIu64"; refused budget %"PRIu64"; pending %"PRIu64"; site overflow %"PRIu64"; target overflow %"PRIu64"\n",
        r->indirect_sites,r->indirect_targets,r->indirect_seeds,r->indirect_refused_budget,r->indirect_pending,r->indirect_site_overflow,r->indirect_target_overflow);
    fprintf(out,"cuts: steps %"PRIu64"; loop %"PRIu64"; fork %"PRIu64"; queue %"PRIu64"; paths %"PRIu64"; global-pending %"PRIu64"; decode %"PRIu64"; faults %"PRIu64"\n",r->step_cuts,r->loop_cuts,r->fork_cuts,r->queue_cuts,r->path_cuts,r->global_cuts,r->decode_cuts,r->faults);
    fprintf(out,"seeds: pointer %"PRIu64"; bank sweep %"PRIu64"; entries %"PRIu64"; admitted %"PRIu64"; skipped padding %"PRIu64"; refused view %"PRIu64", queue %"PRIu64", cap %"PRIu64"; pending %"PRIu64"\n",
        r->seeds_pointer,r->seeds_bank,r->seeds_entry,r->seeds_admitted,r->seeds_skipped_padding,r->seeds_refused_view,r->seeds_refused_queue,r->seeds_refused_cap,r->seeds_pending);
    fprintf(out,"sections: %u; cap cuts %"PRIu64"; contexts evicted %"PRIu64"; grew %"PRIu64"; full %"PRIu64"\n",
        r->section_count,r->section_cuts,r->evicted_ctx,r->ctx_grew,r->ctx_full);
    unsigned printed=r->section_count<256?r->section_count:256;
    for(unsigned i=0;i<printed;i++) {
        const DpSegaSection *s=&r->sections[i];
        fprintf(out,"  section %u bank %u $%04X-$%04X hits %"PRIu64" natural %"PRIu64" forced %"PRIu64" boot %"PRIu64" entries %"PRIu64" copies %"PRIu64" ctx %"PRIu64"%s%s verdict %s\n",
            s->id,s->bank,s->lo,s->hi,s->hits,s->natural_hits,s->forced_hits,s->boot_hits,
            s->entries,s->copies,s->ctx_count,
            s->kind_bits&DP_SEGA_IRQ?" irq":"",s->kind_bits&DP_SEGA_FORCED?" forced":"",
            dp_sega_verdict_names[s->verdict]);
    }
    if(r->section_count>printed) fprintf(out,"  ... %u more sections\n",r->section_count-printed);
    fprintf(out,"loop contexts: CPU address + compact ROM index (outside: CPU address); peak %zu; buckets %zu; growths %"PRIu64"\n",
        r->loop_context_peak,r->loop_context_capacity,r->loop_context_growths);
    fprintf(out,"forced stagnation cuts %"PRIu64"\n",r->stagnation_cuts);
    fprintf(out,"queue growths %"PRIu64"; spool writes %"PRIu64", reads %"PRIu64", flushed %"PRIu64", refused %"PRIu64"; tree nodes %u\n",
        r->queue_growths,r->spool_writes,r->spool_reads,r->spool_flushed,r->spool_refused,r->tree_count);
    fprintf(out,"input forks %"PRIu64"; masks/site %u; site-cap cuts %"PRIu64"; queue/path drops %"PRIu64"; sites %u/%u; site drops %"PRIu64"; input log overflow %"PRIu64"\n",r->input_forks,r->input_mask_limit,r->input_cap_cuts,r->input_drops,r->input_site_count,r->input_site_capacity,r->input_site_drops,r->input_log_overflow);
    fprintf(out,"input frontier cuts: queue %"PRIu64"; paths %"PRIu64"; deferred retry skips %"PRIu64"\n",r->input_queue_cuts,r->input_path_cuts,r->input_retry_skips);
    for(unsigned i=0;i<r->input_count;i++) {
        const DpSegaInput *v=&r->inputs[i].input;
        fprintf(out,"  input parent %"PRIu64" step %"PRIu64" PC $%04X",r->inputs[i].parent_path,r->inputs[i].step,r->inputs[i].pc);
        if(r->inputs[i].halted) fputs(" HALT",out);
        else if(!strcmp(s->id,"msx") && r->inputs[i].port<64) fprintf(out," BIOS id $%02X",r->inputs[i].port);
        else fprintf(out," port $%04X",r->inputs[i].port);
        if(!strcmp(s->id,"msx")) fprintf(out," pads $%02X/$%02X Return %u\n",v->pad[0],v->pad[1],v->start);
        else fprintf(out," pads $%02X/$%02X Start %u Pause %u Reset %u\n",v->pad[0],v->pad[1],v->start,v->pause,v->reset);
    }
    fprintf(out,"ROM data reads %"PRIu64"; provenance copy bytes: VDP %"PRIu64" (VRAM %"PRIu64", CRAM %"PRIu64"), RAM %"PRIu64" (SRAM %"PRIu64")\n",r->data_reads,r->vdp_copies,r->vram_copies,r->cram_copies,r->ram_copies,r->sram_copies);
    fprintf(out,"copy records %u/%u; bytes not recorded %"PRIu64"; masked colour bytes %"PRIu64"; provenance mismatches %"PRIu64"\n",r->copy_count,r->copy_capacity,r->copy_log_overflow,r->copy_transformed,r->provenance_mismatches);
    static const char *const spaces[]={"WRAM","SRAM","VRAM","CRAM"};
    for(unsigned i=0;i<r->copy_count;i++) {
        const DpSegaCopy *c=&r->copies[i];
        fprintf(out,"  copy path %"PRIu64" steps %"PRIu64"..%"PRIu64" PC $%04X ROM+$%06X -> %s+$%04X length %u strides %+d/%+d occurrences %"PRIu64" %s %s\n",c->path,c->first_step,c->last_step,c->pc,c->source,spaces[c->space],c->destination,c->length,c->source_stride,c->destination_stride,c->occurrences,c->verbatim?"verbatim":"masked",c->forced?"forced":"natural");
    }
    if(!strcmp(s->id,"sms") || !strcmp(s->id,"gg")) {
        fprintf(out,"memory control writes %"PRIu64"; effective latch changes %"PRIu64"; records %u/%u; overflow %"PRIu64"\n",
            r->memory_writes,r->memory_effect_changes,r->memory_count,DP_SEGA_HARDWARE_LOG_CAP,r->memory_log_overflow);
        for(unsigned i=0;i<r->memory_count;i++) {
            const DpSegaMemoryWrite *e=&r->memory_log[i];
            fprintf(out,"  memory path %"PRIu64" step %"PRIu64" PC $%04X OUT $%04X latch $%02X->$%02X effect mask $%02X %s\n",
                e->path,e->step,e->pc,e->port,e->previous,e->value,e->effect_mask,e->forced?"forced":"natural");
        }
    }
    fprintf(out,"bank changes %"PRIu64" (timeline overflow %"PRIu64")\n",r->bank_changes,r->bank_log_overflow);
    for(unsigned i=0;i<r->bank_count;i++) {
        fprintf(out,"  path %"PRIu64" step %"PRIu64" PC $%04X ",r->banks[i].path,r->banks[i].step,r->banks[i].pc);
        fprintf(out,r->banks[i].kind==2?"BIOS id $%02X slots=$%02X\n":r->banks[i].kind==1?"OUT $%04X=$%02X\n":"write $%04X=$%02X\n",r->banks[i].addr,r->banks[i].value);
    }
    for(unsigned p=0;p<256;p++) if(r->io_read[p]||r->io_write[p]) {
        const DpSegaPort *rd=dp_sega_port(s,p,0), *wr=dp_sega_port(s,p,1);
        fprintf(out,"  port $%02X read %"PRIu64" (%s), write %"PRIu64" (%s)\n",p,r->io_read[p],rd?rd->name:"unmapped",r->io_write[p],wr?wr->name:"unmapped");
    }
    if(!strcmp(s->id,"msx")) {
        fputs("Limits: partial simulated BIOS, zero RAM and fixed primary-slot profile; no real BIOS, expanded slots, RAM mapper, disk/tape or MSX2. No sessions yet. Context projection hashes registers, held input, mapper views and the pages/ports a section read; IN port values are not hashed. Static closure uses observed slot/bank views and stops at BIOS, RAM, indirect jumps its constants cannot prove and mapping barriers; targets observed at runtime are seeded in the indirect pass. Pure CPU/RAM and selected BIOS copies only; VRAM read-back and transformed data have no origin. RETURN is polled, never NMI. Forced reachability is speculative.\n",out);
        return;
    }
    fprintf(out,"Limits: no sessions yet. Context projection hashes registers, held input, mapper views and the pages/ports a section read; IN port values are not hashed. Static closure uses observed ROM views and stops indirect jumps its constants cannot prove and mapper effects; targets observed at runtime are seeded in the indirect pass. Provenance covers pure CPU/RAM transfers, not transformed data or VRAM read-back. Power-on inputs released; input probes hold their selected state until another probe. SMS pads use export digital TR/TH levels; GG has one pad. Zero RAM; no BIOS; disabled console bus reads $FF, without revision-specific floating bus effects. Hardware registry may describe devices absent from the embedded runtime (FM, link, BIOS/card/expansion, peripheral variants). Forced reachability is speculative.\n");
}
static int number(const char *s,uint64_t *n) {
    if(!*s||*s=='-') return 0;
    char *end; errno=0; unsigned long long v=strtoull(s,&end,10);
    if(errno||*end) return 0;
    *n=v; return 1;
}
// GB parse_entry parity: bank in strtoul base 0, CPU address in hex.
static int parse_entry(const char *s,DpSegaOptions *o) {
    char *end;
    unsigned long bank=strtoul(s,&end,0);
    if(end==s || *end!=':' || bank>0xffff) return 0;
    const char *as=end+1;
    unsigned long addr=strtoul(as,&end,16);
    if(end==as || *end || addr>0xffff) return 0;
    o->entry[o->entry_count].bank=(uint32_t)bank;
    o->entry[o->entry_count].addr=(uint16_t)addr;
    o->entry_count++;
    return 1;
}
int dp_sega_explore_command(int argc,char **argv) {
    if(argc<5) goto usage;
    const DpSegaSystem *s=!strcmp(argv[3],"msx")?dp_msx_system():dp_sega_system(argv[3]); if(!s) goto usage;
    DpSegaOptions o=!strcmp(s->id,"msx")?dp_msx_options():dp_sega_options(); const char *text=NULL,*map=NULL,*evidence=NULL;
    for(int i=5;i<argc;i++) {
        if(!strcmp(argv[i],"--no-vector-seeds")) { o.seed_vectors=0; continue; }
        if(!strcmp(argv[i],"--no-context-evict")) { o.no_context_evict=1; continue; }
        if(!strcmp(argv[i],"--spool") || !strcmp(argv[i],"--resume")) {
            if(i+1>=argc) goto usage;
            o.spool_path=argv[++i]; o.resume=argv[i-1][2]=='r';
            continue;
        }
        if(!strcmp(argv[i],"--tree")) { if(i+1>=argc) goto usage; o.tree_path=argv[++i]; continue; }
        if(!strcmp(argv[i],"--seed-banks")) { o.seed_banks=1; continue; }
        if(!strcmp(argv[i],"--no-pointer-scan")) { o.no_pointer_scan=1; continue; }
        if(!strcmp(argv[i],"--entry")) {
            if(i+1>=argc || o.entry_count>=32 || !parse_entry(argv[++i],&o)) goto usage;
            continue;
        }
        if(!strcmp(argv[i],"--no-block-closure")) { o.block_closure=0; continue; }
        if(!strcmp(argv[i],"--no-target-closure")) { o.target_closure=0; continue; }
        if(!strcmp(argv[i],"--no-closure")) { o.closure.nodes=0; continue; }
        if(i+1>=argc) goto usage;
        const char *key=argv[i++],*value=argv[i];
        if(!strcmp(key,"--text")) text=value;
        else if(!strcmp(key,"--map")) map=value;
        else if(!strcmp(key,"--evidence")) evidence=value;
        else if(!strcmp(key,"--mapper") && (!strcmp(s->id,"sms") || !strcmp(s->id,"gg"))) {
            const char *names[]={"auto","sega","codemasters","korean150"};
            int found=0;
            for(unsigned j=0;j<sizeof(names)/sizeof(*names);j++) if(!strcmp(value,names[j])) { o.sega_mapper=(int)j; found=1; }
            if(!found) goto usage;
        }
        else if(!strcmp(key,"--mapper") && !strcmp(s->id,"msx")) {
            const char *names[]={"auto","plain","ascii8","ascii16","konami","konami-scc"};
            int found=0;
            for(unsigned j=0;j<sizeof(names)/sizeof(*names);j++) if(!strcmp(value,names[j])) { o.msx_mapper=(int)j-1; found=1; }
            if(!found) goto usage;
        }
        else {
            uint64_t n; if(!number(value,&n)) goto usage;
            if(!n && strcmp(key,"--fork-cap") && strcmp(key,"--stagnation") && strcmp(key,"--copy-cap") && strcmp(key,"--input-forks") && strcmp(key,"--input-sites") && strcmp(key,"--input-total") && strcmp(key,"--closure-nodes") && strcmp(key,"--seed-cap")) goto usage;
            if(!strcmp(key,"--max-total")) o.max_total=n;
            else if(!strcmp(key,"--input-total")) o.input_total=n;
            else if(!strcmp(key,"--seed-cap")) { if(n>65536) goto usage; o.seed_cap=(uint32_t)n; }
            else if(n>UINT32_MAX) goto usage;
            else if(!strcmp(key,"--max-steps")) o.max_steps=(uint32_t)n;
            else if(!strcmp(key,"--max-paths")) o.max_paths=(uint32_t)n;
            else if(!strcmp(key,"--queue")) o.queue_cap=(uint32_t)n;
            else if(!strcmp(key,"--loop-cap")) o.loop_cap=(uint32_t)n;
            else if(!strcmp(key,"--stagnation")) o.stagnation=(uint32_t)n;
            else if(!strcmp(key,"--copy-cap")) o.copy_cap=(uint32_t)n;
            else if(!strcmp(key,"--input-forks")) o.input_masks=(uint32_t)n;
            else if(!strcmp(key,"--input-sites")) o.input_sites=(uint32_t)n;
            else if(!strcmp(key,"--closure-nodes")) o.closure.nodes=(uint32_t)n;
            else if(!strcmp(key,"--closure-seeds")) o.closure.seeds=(uint32_t)n;
            else if(!strcmp(key,"--closure-views")) o.closure.views=(uint32_t)n;
            else if(!strcmp(key,"--closure-contexts")) o.closure.contexts=(uint32_t)n;
            else if(!strcmp(key,"--fork-cap") && n<=255) o.fork_cap=(uint32_t)n;
            else if(!strcmp(key,"--indirect-total")) o.indirect_total=n;
            else if(!strcmp(key,"--queue-max")) { if(n>65536 || n<o.queue_cap) goto usage; o.queue_max=(uint32_t)n; }
            else if(!strcmp(key,"--spool-cap-mb") && n<=1048576) o.spool_cap_mb=(uint32_t)n;
            else goto usage;
        }
    }
    FILE *f=fopen(argv[4],"rb"); if(!f) { perror(argv[4]); return 1; }
    if(fseek(f,0,SEEK_END)) { fclose(f); return 1; }
    long end=ftell(f); if(end<=0||end>0x400200||fseek(f,0,SEEK_SET)) { fclose(f); return 2; }
    uint8_t *image=malloc((size_t)end); if(!image) { fclose(f); return 1; }
    int read_ok=fread(image,1,(size_t)end,f)==(size_t)end; fclose(f);
    if(!read_ok) { free(image); return 1; }
    size_t header=(!strcmp(s->id,"sms") || !strcmp(s->id,"gg")) && ((size_t)end&0x3fff)==512?512:0;
    DpSegaResults r;
    int ok=dp_sega_explore(s,image+header,(size_t)end-header,&o,&r); free(image);
    if(!ok) { fprintf(stderr,"invalid machine profile/budgets/ROM size, or allocation failure\n"); return 1; }
    dp_sega_report(stdout,s,&r);
    const char *paths[]={text,map,evidence};
    for(unsigned i=0;i<3;i++) if(paths[i]) {
        FILE *out=fopen(paths[i],"w"); if(!out) { perror(paths[i]); ok=0; continue; }
        if(i==2) dp_sega_evidence(out,&r); else if(i==1) dp_sega_map(out,&r); else dp_sega_report(out,s,&r);
        if(ferror(out)) ok=0;
        if(fclose(out)) ok=0;
    }
    dp_sega_results_free(&r); return ok?0:1;
usage:
    fprintf(stderr,"deepeel z80 explore <sg|sms|gg|msx> ROM [--mapper NAME (SMS/GG: auto|sega|codemasters|korean150; MSX: auto|plain|ascii8|ascii16|konami|konami-scc)] [--max-total N] [--max-steps N] [--max-paths N] [--queue N] [--loop-cap N] [--fork-cap N] [--stagnation N] [--copy-cap N] [--input-forks N] [--input-sites N] [--input-total N] [--seed-cap N] [--seed-banks] [--no-pointer-scan] [--entry BANK:ADDR] [--no-closure] [--no-target-closure] [--no-block-closure] [--closure-nodes N] [--closure-seeds N] [--closure-views N] [--closure-contexts N] [--no-vector-seeds] [--indirect-total N] [--queue-max N] [--spool FILE] [--resume FILE] [--spool-cap-mb N] [--tree FILE] [--text FILE] [--map FILE] [--evidence FILE]\n"); return 2;
}
