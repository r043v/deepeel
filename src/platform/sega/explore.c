#include "explore.h"
#include "backend.h"
#include "visits.h"
#include "../msx/msx.h"
#include "../msx/explore.h"
#include "../../cpu/z80/provenance.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

typedef struct {
    DpZ80Provenance cpu;
    DpZ80Origin cram_latch;
    DpZ80Origin ram[];
} Origins;
typedef struct { void *state; uint8_t *cart; Origins *origins; int forced,skip_irq; } Path;
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
    uint32_t queued;
    Path *input_queue;
    uint32_t input_queued;
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
} Run;
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
static uint8_t read_bus(void *user,uint16_t a) {
    Run *x=user; uint8_t v=x->b->read(a);
    int64_t index=x->b->rom_index(a);
    if(!x->fetching && index>=0 && (uint64_t)index<x->r->size) {
        if(!(x->r->flags[index]&DP_SEGA_DATA)) x->discoveries++;
        x->r->flags[index]|=DP_SEGA_DATA; x->r->data_reads++;
    }
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
    Run *x=user; if(!x->r->io_read[p&255]) x->discoveries++;
    x->r->io_read[p&255]++; return x->b->in(p);
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
static int push_to(Run *x,int forced,int skip_irq,int input) {
    unsigned phase=input?1:x->phase;
    Path *queue=input?x->input_queue:x->queue;
    uint32_t *queued=input?&x->input_queued:&x->queued;
    if(x->created[phase]>=x->opt->max_paths) {
        if(input) x->r->input_path_cuts++; else x->r->path_cuts++;
        return 0;
    }
    if(*queued>=x->opt->queue_cap) {
        if(input) x->r->input_queue_cuts++; else x->r->queue_cuts++;
        return 0;
    }
    Path p={malloc(x->b->state_size),malloc(0x8000),malloc(x->origin_size),forced,skip_irq};
    if(!p.state||!p.cart||!p.origins) { free(p.state); free(p.cart); free(p.origins); return -1; }
    x->b->save(p.state); memcpy(p.cart,x->cart,0x8000);
    memcpy(p.origins,x->origins,x->origin_size);
    queue[(*queued)++]=p; x->created[phase]++; return 1;
}
static int push(Run *x,int forced,int skip_irq) { return push_to(x,forced,skip_irq,0); }
static size_t site(Run *x,uint16_t pc) {
    int64_t i=x->b->rom_index(pc);
    return i>=0 && (uint64_t)i<x->r->size ? (size_t)i : x->r->size+pc;
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
        int accepted=same_input(old,actual)?0:push_to(x,1,skip_irq,1);
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
    DpSegaOptions o={.max_total=20000000,.input_total=20000000,.max_steps=2000000,.max_paths=4096,.queue_cap=128,
        .loop_cap=262144,.fork_cap=2,.stagnation=32768,.copy_cap=DP_SEGA_COPY_CAP,
        .input_masks=8,.input_sites=4096,.seed_vectors=1,.target_closure=1,.block_closure=1,.msx_mapper=DP_MSX_AUTO,.closure={262144,65536,256,8}}; return o;
}
DpSegaOptions dp_msx_options(void) {
    DpSegaOptions o=dp_sega_options(); o.seed_vectors=0; return o;
}
void dp_sega_results_free(DpSegaResults *r) { free(r->flags); free(r->copies); memset(r,0,sizeof(*r)); }
int dp_sega_explore(const DpSegaSystem *s,const uint8_t *rom,size_t size,const DpSegaOptions *o,DpSegaResults *r) {
    if(!s||!rom||!size||!o||!r||!o->max_total||!o->max_steps||!o->max_paths||!o->queue_cap||!o->loop_cap||o->fork_cap>255) return 0;
    int sg=!strcmp(s->id,"sg"),msx=!strcmp(s->id,"msx");
    if(!sg && !msx && strcmp(s->id,"sms") && strcmp(s->id,"gg")) return 0;
    if(o->sega_mapper<0 || o->sega_mapper>DP_SEGA_MAPPER_KOREAN150 || ((sg || msx) && o->sega_mapper)) return 0;
    if(o->sega_mapper==DP_SEGA_MAPPER_KOREAN150 && size>0x200000) return 0;
    if(msx && (size>0x100000 || o->msx_mapper<DP_MSX_AUTO || o->msx_mapper>DP_MSX_KONAMI_SCC)) return 0;
    if((sg && size>0xc000) || size>0x400000 || o->queue_cap>4096 || o->copy_cap>65536 || o->input_masks>128 || o->input_sites>65536 ||
       (o->input_masks && o->input_total>UINT64_MAX-o->max_total)) return 0;
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
    x.origin_size=sizeof(Origins)+x.b->ram_tags*sizeof(DpZ80Origin);
    x.image=image; x.origins=calloc(1,x.origin_size);
    x.copy_hash_size=1;
    while(x.copy_hash_size<(size_t)o->copy_cap*2) x.copy_hash_size*=2;
    x.copy_hash=calloc(x.copy_hash_size,sizeof(*x.copy_hash));
    r->flags=calloc(size,1); x.cart=calloc(0x8000,1);
    r->copy_capacity=o->copy_cap;
    r->copies=calloc(o->copy_cap?o->copy_cap:1,sizeof(*r->copies));
    x.forks=calloc(sites,1); x.sides=calloc(sites,1); x.irq_forks=calloc(sites,1);
    x.queue=calloc(o->queue_cap,sizeof(*x.queue));
    x.input_queue=calloc(o->queue_cap,sizeof(*x.input_queue));
    int ok=image&&r->flags&&r->copies&&x.origins&&x.copy_hash&&x.cart&&x.forks&&x.sides&&x.irq_forks&&x.queue&&x.input_queue&&
        (!o->input_masks || !o->input_total || (x.input_sites&&x.input_scratch));
    if(!ok) goto cleanup;
    memset(image,0xff,padded); memcpy(image,rom,size);
    // Baseline SG boards have no cartridge RAM; larger board profiles are explicit future work.
    if(!x.b->init(image,size,sg?NULL:x.cart,msx?o->msx_mapper:!strcmp(s->id,"sms"))) { ok=0; goto cleanup; }
    if(o->sega_mapper && (!x.b->select_mapper || !x.b->select_mapper((unsigned)o->sega_mapper))) { ok=0; goto cleanup; }
    if(!sg && !msx) { r->sega_mapper=x.b->mapper(); r->sega_mapper_explicit=!!o->sega_mapper; }
    hook(&x);
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
    if(push(&x,0,0)<0) { ok=0; goto cleanup; }
    if(o->seed_vectors && !msx) {
        const uint16_t entries[]={8,16,24,32,40,48,56,0x66};
        for(unsigned j=0;j<sizeof(entries)/sizeof(entries[0]);j++) {
            uint16_t pc=entries[j]; int code=0;
            for(unsigned k=0;k<8;k++) { uint8_t v=x.b->read(pc+k); code|=(v!=0 && v!=0xff); }
            if(!code) continue;
            x.b->cpu()->pc=pc;
            if(push(&x,1,0)<0) { ok=0; goto cleanup; }
        }
    }
    // FIFO root first, then depth-first forks: seed creation above is reversed once.
    for(uint32_t i=0;i<x.queued/2;i++) { Path p=x.queue[i]; x.queue[i]=x.queue[x.queued-i-1]; x.queue[x.queued-i-1]=p; }
    for(unsigned phase=0;phase<2;phase++) {
        x.phase=phase;
        uint64_t start_steps=r->steps,start_paths=r->paths,budget=r->phase_limits[phase];
        if(phase && budget) { memset(x.forks,0,sites); memset(x.sides,0,sites); memset(x.irq_forks,0,sites); }
        while((x.queued || (phase && x.input_queued)) && r->steps-start_steps<budget) {
            // Keep input seeds and their branch children moving in turn. Their
            // separate frontier never displaces the original exploration pass.
            int from_input=phase && x.input_queued && (!x.queued || ((r->paths-start_paths)&1)==0);
            Path p=from_input?x.input_queue[--x.input_queued]:x.queue[--x.queued];
            if(from_input) x.input_epoch++;
            x.b->load(p.state); memcpy(x.cart,p.cart,0x8000);
            dp_sega_closure_invalidate(x.closure);
            memcpy(x.origins,p.origins,x.origin_size);
            free_path(p); hook(&x); x.forced=p.forced;
            dp_sega_visits_reset(&visits); r->paths++;
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
                        int accepted=push(&x,1,1);
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
                                int accepted=push(&x,1,service_suppressed);
                                *c=saved;
                                x.origins->cpu=saved_origins;
                                if(accepted<0) { ok=0; goto cleanup; }
                                if(accepted) { x.forks[at]++; r->forks++; }
                            } else r->fork_cuts++;
                        }
                    }
                    if(fork_inputs(&x,&d,at,service_suppressed)<0) { ok=0; goto cleanup; }
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
        }
        r->phase_steps[phase]=r->steps-start_steps; r->phase_paths[phase]=r->paths-start_paths;
        r->phase_pending[phase]+=x.queued+(phase?x.input_queued:0);
        r->global_cuts+=r->phase_pending[phase];
        for(size_t i=0;i<size;i++) r->phase_known[phase]+=!!r->flags[i];
        // A phase wall closes its pending branch work. Input seeds have their
        // own declared budget, so finishing the first pass is never regressed.
        for(uint32_t i=0;i<x.queued;i++) free_path(x.queue[i]);
        x.queued=0;
    }
    dp_sega_closure_run(x.closure);
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
    if(!ok) dp_sega_results_free(r);
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
    fprintf(out,"phases: base %"PRIu64"/%"PRIu64" steps, %"PRIu64" paths, %"PRIu64" known, %"PRIu64" pending; inputs %"PRIu64"/%"PRIu64" steps, %"PRIu64" paths, %"PRIu64" known, %"PRIu64" pending\n",r->phase_steps[0],r->phase_limits[0],r->phase_paths[0],r->phase_known[0],r->phase_pending[0],r->phase_steps[1],r->phase_limits[1],r->phase_paths[1],r->phase_known[1],r->phase_pending[1]);
    fprintf(out,"cuts: steps %"PRIu64"; loop %"PRIu64"; fork %"PRIu64"; queue %"PRIu64"; paths %"PRIu64"; global-pending %"PRIu64"; decode %"PRIu64"; faults %"PRIu64"\n",r->step_cuts,r->loop_cuts,r->fork_cuts,r->queue_cuts,r->path_cuts,r->global_cuts,r->decode_cuts,r->faults);
    fprintf(out,"loop contexts: CPU address + compact ROM index (outside: CPU address); peak %zu; buckets %zu; growths %"PRIu64"\n",
        r->loop_context_peak,r->loop_context_capacity,r->loop_context_growths);
    fprintf(out,"forced stagnation cuts %"PRIu64"\n",r->stagnation_cuts);
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
        fputs("Limits: partial simulated BIOS, zero RAM and fixed primary-slot profile; no real BIOS, expanded slots, RAM mapper, disk/tape or MSX2. No pointer/bank seeds, dynamic context dedup or sessions yet. Static closure uses observed slot/bank views and stops at BIOS, RAM, unresolved indirect jumps and mapping barriers. Pure CPU/RAM and selected BIOS copies only; VRAM read-back and transformed data have no origin. RETURN is polled, never NMI. Forced reachability is speculative.\n",out);
        return;
    }
    fprintf(out,"Limits: no pointer/bank seeds, dynamic context dedup or sessions yet. Static closure uses observed ROM views and stops unresolved indirect jumps and mapper effects. Provenance covers pure CPU/RAM transfers, not transformed data or VRAM read-back. Power-on inputs released; input probes hold their selected state until another probe. SMS pads use export digital TR/TH levels; GG has one pad. Zero RAM; no BIOS; disabled console bus reads $FF, without revision-specific floating bus effects. Hardware registry may describe devices absent from the embedded runtime (FM, link, BIOS/card/expansion, peripheral variants). Forced reachability is speculative.\n");
}
static int number(const char *s,uint64_t *n) {
    if(!*s||*s=='-') return 0;
    char *end; errno=0; unsigned long long v=strtoull(s,&end,10);
    if(errno||*end) return 0;
    *n=v; return 1;
}
int dp_sega_explore_command(int argc,char **argv) {
    if(argc<5) goto usage;
    const DpSegaSystem *s=!strcmp(argv[3],"msx")?dp_msx_system():dp_sega_system(argv[3]); if(!s) goto usage;
    DpSegaOptions o=!strcmp(s->id,"msx")?dp_msx_options():dp_sega_options(); const char *text=NULL,*map=NULL,*evidence=NULL;
    for(int i=5;i<argc;i++) {
        if(!strcmp(argv[i],"--no-vector-seeds")) { o.seed_vectors=0; continue; }
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
            if(!n && strcmp(key,"--fork-cap") && strcmp(key,"--stagnation") && strcmp(key,"--copy-cap") && strcmp(key,"--input-forks") && strcmp(key,"--input-sites") && strcmp(key,"--input-total") && strcmp(key,"--closure-nodes")) goto usage;
            if(!strcmp(key,"--max-total")) o.max_total=n;
            else if(!strcmp(key,"--input-total")) o.input_total=n;
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
    fprintf(stderr,"deepeel z80 explore <sg|sms|gg|msx> ROM [--mapper NAME (SMS/GG: auto|sega|codemasters|korean150; MSX: auto|plain|ascii8|ascii16|konami|konami-scc)] [--max-total N] [--max-steps N] [--max-paths N] [--queue N] [--loop-cap N] [--fork-cap N] [--stagnation N] [--copy-cap N] [--input-forks N] [--input-sites N] [--input-total N] [--no-closure] [--no-target-closure] [--no-block-closure] [--closure-nodes N] [--closure-seeds N] [--closure-views N] [--closure-contexts N] [--no-vector-seeds] [--text FILE] [--map FILE] [--evidence FILE]\n"); return 2;
}
