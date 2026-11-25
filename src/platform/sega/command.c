// Sega 8-bit command (SG-1000, Master System, Game Gear, MSX): the generic kernel (src/explore/core.c) on the
// DpMachine view of the Z80 backends.  The provenance closure of these platforms runs inside the kernel
// (before_step / finish hooks); everything else (forks, sessions, TAS replay, copies, sections, reports) is
// the shared kernel.
#include "explore.h"
#include "../../explore/core.h"
#include "../../explore/seedset.h"
#include "../../core/dpmachine.h"
#include "../../core/music_index_seeds.h"
#include "backend.h"
#include "../../gui/fbwatch.h"
#include "tas.h"
#include "../msx/msx.h"
#include "../msx/explore.h"
#include "../../explore/coverage_defaults.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

DpSegaOptions dp_sega_options(void) {
    DpSegaOptions o={.seed_vectors=1,.target_closure=1,.block_closure=1,.msx_mapper=DP_MSX_AUTO,.closure={262144,65536,256,8},.jobs=1};
    return o;
}
static int number(const char *s,uint64_t *n) {
    if(!*s||*s=='-') return 0;
    char *end; errno=0; unsigned long long v=strtoull(s,&end,10);
    if(errno||*end) return 0;
    *n=v; return 1;
}
// bank in strtoul base 0, CPU address in hex (the same grammar as the GB front end)
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
// --core: the generic kernel (src/explore/core.c) on the DpMachine view of
// the SG/SMS/GG/MSX backend (MSX through the keyboard hooks).
static int core_is_sg, core_is_msx;
// only the first two instances are trusted: gg3..gg16 resolve the bank-switch
// fixture differently (observed 2026-11: fix_sega_mapper reaches other ROM indexes)
static const char *core_cov_path, *core_text_path, *core_map_path, *core_summary_path, *core_rom_name, *core_rtcode_path;
static int core_jobs_auto;
static uint32_t core_entry_pc[64]; static uint16_t core_entry_bank[64];
// C3 (option B): the provenance closure of this platform runs inside the kernel.  Every worker owns one
// closure (the backend is thread-local) fed before each instruction; at the end the closures run and their
// static bytes are folded into the kernel result as DP_CORE_STATIC.
typedef struct CoreClosure { const DpSegaBackend *b; DpSegaClosure *c; DpSegaClosureStats st, tst, bst; uint8_t *flags; int64_t sig[10]; struct CoreClosure *next; } CoreClosure;
static struct { DpMachine *main_m; const DpSegaBackend *main_b; const DpSegaSystem *sys; const uint8_t *rom; size_t size; DpSegaOptions opt; pthread_mutex_t mu; CoreClosure *list; int on; } core_clo = { .mu = PTHREAD_MUTEX_INITIALIZER };
static __thread CoreClosure *tl_clo;
static const void *sega_core_pick(unsigned w);
static CoreClosure *core_clo_get(DpMachine *m) {
    if (tl_clo) return tl_clo;
    CoreClosure *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->b = m == core_clo.main_m ? core_clo.main_b : sega_core_pick(0);   // worker 0 runs on the machine built by sega_core_run, the clones on the thread flavour
    k->flags = calloc(core_clo.size ? core_clo.size : 1, 1);
    const DpSegaOptions *o = &core_clo.opt;
    k->st.limits = o->closure; k->tst.limits = o->closure; k->bst.limits = o->closure;
    if (!o->target_closure) k->tst.limits.nodes = 0;
    if (!o->target_closure || !o->block_closure) k->bst.limits.nodes = 0;
    k->c = k->flags ? dp_sega_closure_create(k->b, core_clo.sys, core_clo.rom, core_clo.size, k->flags, o->closure, &k->st) : NULL;
    if (k->c && o->target_closure) { if (!dp_sega_closure_targets(k->c, &k->tst) || (o->block_closure && !dp_sega_closure_blocks(k->c, &k->bst))) { dp_sega_closure_free(k->c); k->c = NULL; } }
    if (!k->c) { free(k->flags); k->flags = NULL; }
    tl_clo = k;
    pthread_mutex_lock(&core_clo.mu); k->next = core_clo.list; core_clo.list = k; pthread_mutex_unlock(&core_clo.mu);
    return k;
}
static int core_clo_before(void *user, DpMachine *m, uint32_t pc) {
    (void)user; (void)pc;
    CoreClosure *k = tl_clo ? tl_clo : core_clo_get(m);
    if (!k || !k->c || k->st.seeds >= k->st.limits.seeds) return 1;   // saturated: no per-step cost
    const DpSegaBackend *b = k->b;
    static const uint16_t probe[] = {0, 0x400, 0x2000, 0x4000, 0x6000, 0x8000, 0xa000, 0x9800};
    int64_t sig[10];
    for (unsigned i = 0; i < 8; i++) sig[i] = b->rom_index(probe[i]);
    sig[8] = core_is_msx ? b->msx_info().slots : 0;
    sig[9] = b->ram_index(0xc000);
    if (memcmp(sig, k->sig, sizeof sig)) { dp_sega_closure_invalidate(k->c); memcpy(k->sig, sig, sizeof sig); }   // a mapper move or a state load: the cached view is stale
    dp_sega_closure_observe(k->c);
    return 1;
}
static void core_clo_finish(void *user, DpCoreResult *r) {
    (void)user;
    for (CoreClosure *k = core_clo.list; k; k = k->next) {
        if (!k->c) continue;
        dp_sega_closure_run(k->c);
        size_t n = core_clo.size < r->rom_size ? core_clo.size : r->rom_size;
        for (size_t i = 0; i < n; i++) {
            if (!(k->flags[i] & DP_SEGA_STATIC)) continue;
            r->src_flags[i] |= DP_SRC_CLOSURE;
            if (!(r->flags[i] & (DP_CORE_STATIC | DP_CORE_NATURAL | DP_CORE_FORCED))) { r->flags[i] |= DP_CORE_STATIC; r->closure_bytes++; }
        }
        r->closure_roots += k->st.seeds;
    }
    while (core_clo.list) { CoreClosure *k = core_clo.list; core_clo.list = k->next; if (k->c) dp_sega_closure_free(k->c); free(k->flags); free(k); }
    tl_clo = NULL;   // the caller thread's cached closure was just freed: a second kernel pass in the same process (--campaign phase 2) would reuse a dangling pointer
}
static DpPlatformHooks core_sega_hooks;
static const char *watch_dump;   /* --watch-dump FILE: the last published frame as a PPM, no window */
static int core_sms_wide;   /* --watch on the SMS: the 256-column runtime flavour */
static const void *sega_core_pick(unsigned w) { (void)w; if(core_sms_wide) return dp_sega_backend_smst(); if(core_is_msx) return dp_msx_backendt(); if(core_is_sg) return dp_sega_backend_sgt(); return dp_sega_backend_ggt(); }
static int sega_core_run(const DpSegaSystem *s,const uint8_t *rom,size_t size,const DpSegaOptions *o,const DpCoreConfig *kcfg,FILE *out) {
    int msx=!strcmp(s->id,"msx");
    core_sms_wide=kcfg->watch_every && !strcmp(s->id,"sms");
    const DpSegaBackend *b=msx?dp_msx_backend():!strcmp(s->id,"sg")?dp_sega_backend_sg():core_sms_wide?dp_sega_backend_smst():dp_sega_backend_gg();
    int init_arg=msx?o->msx_mapper:!strcmp(s->id,"sms");   // the backend's mode flag: Master System or Game Gear
    if(msx && (size>0x100000 || o->msx_mapper<DP_MSX_AUTO || o->msx_mapper>DP_MSX_KONAMI_SCC)) return 0;
    if(!b || b->init(rom,size,NULL,init_arg)!=1) return 0;
    DpMachine *m=dp_machine_sega(b); if(!m) return 0;
    m->plat=dp_platform_sega(s);
    dp_machine_sega_init_arg(m,init_arg);
    dp_machine_sega_set_msx(m,msx);
    dp_machine_sega_gg(m,!strcmp(s->id,"gg"));
    if(o->sega_mapper) { if(!b->select_mapper || !b->select_mapper((unsigned)o->sega_mapper)) { m->close(m); return 0; } dp_machine_sega_mapper(m,(unsigned)o->sega_mapper); }
    if(!msx && strcmp(s->id,"sg")) {   // SMS/GG: the cartridge mapper, detected from the image or forced by --mapper
        static const char *const names[]={"none","sega","codemasters","korean150"};
        unsigned mp=b->mapper?b->mapper():0;
        fprintf(out,"Sega mapper: %s (%s)\n",mp<4?names[mp]:"unknown",o->sega_mapper?"explicit":"auto");
    }
    core_is_msx=msx;
    core_is_sg=!strcmp(s->id,"sg");
    dp_machine_sega_source(m,rom,size,sega_core_pick);
    DpCoreConfig cfg=*kcfg; cfg.jobs=o->jobs>1?(unsigned)o->jobs:1; cfg.adaptive=core_jobs_auto;
    if(o->max_total) cfg.max_total=o->max_total;
    if(o->max_steps) cfg.max_path_steps=o->max_steps;
    cfg.fork_site_cap=o->fork_cap; if(cfg.queue_auto) cfg.queue_max=o->queue_max;   // an explicit --queue-max was read by the kernel parser (queue_auto 0): keep it
    if(cfg.unbounded) { cfg.max_total=0; cfg.fork_site_cap=0; }   // coverage first: only exhaustion or SIGINT stops; the path shape stays (A8)
    static const uint32_t vec[]={0x08,0x10,0x18,0x20,0x28,0x30,0x38,0x66};
    // Seeds: the music-rip entry points (--music-index: the $4000 window shows each rip's block, the init call
    // carries the song number in A on the rip's stack), the user's --entry bank:addr and, with --seed-banks, the
    // start of every switchable window/bank pair; the reset/RST vectors when nothing else was asked.
    DpMusicKernelSeeds mks={0};
    if(o->music_seed_count) dp_music_seeds_kernel(&mks,o->music_seeds,o->music_seed_count,0x4000,NULL);
    unsigned nbanks=0; uint16_t wlo=0;
    if(o->seed_banks && b->seed_geometry) { DpSegaSeedGeometry g; if(b->seed_geometry(&g) && g.banks) { nbanks=g.banks; wlo=g.window_lo; } }
    // the reset/RST vectors seed the run unless a music rip or an explicit --entry already names the entry points
    size_t nv=(o->seed_vectors && !mks.n && !o->entry_count)?sizeof vec/sizeof *vec:0;
    DpSeedSet kseeds; size_t ne=0;
    for(size_t i=0;i<nv;i++,ne++) { core_entry_pc[ne]=vec[i]; core_entry_bank[ne]=0xFFFF; }
    for(size_t i=0;i<o->entry_count;i++,ne++) { core_entry_pc[ne]=o->entry[i].addr; core_entry_bank[ne]=(uint16_t)o->entry[i].bank; }
    if(seedset_build(&cfg,&mks,core_entry_pc,core_entry_bank,ne,nbanks,wlo,&kseeds)) { dp_music_seeds_kernel_free(&mks); m->close(m); return 0; }
    if(!cfg.rom_image) { cfg.rom_image=rom; cfg.rom_image_size=size; }   // B8/C4: the image feeds the speculative gaps and the static inference
    if(!cfg.hooks) {
        core_sega_hooks=*dp_machine_sega_hooks();
        if(!cfg.no_closure && o->closure.nodes && !getenv("DEEPEEL_NO_SEGA_CLOSURE")) {   // C3 option B: the provenance closure inside the kernel
            core_clo.main_m=m; core_clo.main_b=b; core_clo.sys=s; core_clo.rom=rom; core_clo.size=size; core_clo.opt=*o; core_clo.list=NULL;
            core_sega_hooks.before_step=core_clo_before; core_sega_hooks.finish=core_clo_finish;
        }
        cfg.hooks=&core_sega_hooks;
    }
    if(!cfg.visits_per_pc || cfg.visits_per_pc<262144) cfg.visits_per_pc=262144;   // the old engine loop_cap: boot delay loops run 65535 x n iterations
    DpSegaTas tas; memset(&tas,0,sizeof tas);
    if(o->tas_path && o->tas_path[0]) {
        char terr[256]="";
        if(dp_sega_tas_load(o->tas_path,&tas,terr,sizeof terr)) {
            cfg.tas_masks=tas.masks; cfg.tas_frames=tas.frames;
            dp_machine_sega_tas(m,!strcmp(s->id,"gg")||msx,!strcmp(s->id,"sms") && !cfg.random_movie?dp_sega_tas_smshawk_profile(&tas):0);
        }
        else fprintf(out,"tas: %s\n",terr);
    }
    DpCoreResult r;
    if(cfg.watch_every && !watch_dump) dp_fbwatch_start("deepeel sega watch");
    int ok=dp_core_explore_cli(m,size,&cfg,&r,core_summary_path?core_summary_path:core_text_path);
    if(cfg.watch_every && !watch_dump) dp_fbwatch_stop();
    if(watch_dump && dp_fb_dump_ppm(watch_dump)) fprintf(stderr,"deepeel: no frame to dump\n");
    dp_sega_tas_free(&tas);
    if(ok) {
        dp_core_report(out,s->id,&r);
        dp_core_write_cov(core_cov_path,s->id,m,&r);
        if(dp_core_write_map(core_map_path,s->id,core_rom_name,&r)<0) fprintf(stderr,"deepeel: cannot write %s\n",core_map_path);
        if(dp_core_write_summary(core_summary_path,s->id,&r)<0) fprintf(stderr,"deepeel: cannot write %s\n",core_summary_path);
        if(core_rtcode_path && core_rtcode_path[0] && r.ra.n) { FILE *rf=fopen(core_rtcode_path,"w"); if(rf) { dp_ramarch_write(&r.ra,rf,s->id); fclose(rf); } }
        if(core_text_path && core_text_path[0]) {   // --text: the same report, written to a file
            FILE *tf=fopen(core_text_path,"w");
            if(tf) { dp_core_report(tf,s->id,&r); fclose(tf); }
        }
    }
    dp_core_result_free(&r); m->close(m);
    dp_music_seeds_kernel_free(&mks); seedset_free(&kseeds);
    return ok;
}


int dp_sega_explore_command(int argc,char **argv) {
    if(argc<5) goto usage;
    const DpSegaSystem *s=!strcmp(argv[3],"msx")?dp_msx_system():dp_sega_system(argv[3]); if(!s) goto usage;
    DpSegaOptions o=dp_sega_options(); if(!strcmp(s->id,"msx")) o.seed_vectors=0;
    DpCoreConfig kcfg; dp_core_config_default(&kcfg);
    const char *text=NULL,*map=NULL,*summary=NULL,*cov=NULL,*rtcode=NULL;
    const char *midx_file=NULL; DpMusicSeed *music_seeds=NULL; size_t music_n=0;
    if(!strcmp(s->id,"sms") || !strcmp(s->id,"gg")) {   // coverage-first defaults: every core, every bank seeded, a deep queue
        o.fork_cap=DP_DEFAULT_BRANCH_FORK_CAP;
        long cpus=sysconf(_SC_NPROCESSORS_ONLN); o.jobs=cpus>0?(int)cpus:1;
        o.seed_banks=1; o.queue_max=65536;
    }
    for(int i=5;i<argc;i++) {
        if(!strcmp(argv[i],"--mapwin")) { if(i+1<argc && argv[i+1][0]>='1' && argv[i+1][0]<='8' && !argv[i+1][1]) i++; continue; }   /* the live map is installed by main() */
        if(!strcmp(argv[i],"--core")) continue;   // the kernel is the only engine: kept as a no-op for scripts
        { int kn=dp_core_parse_option(&kcfg,argc,argv,i); if(kn) { i+=kn-1; continue; } }
        if(!strcmp(argv[i],"--no-vector-seeds")) { o.seed_vectors=0; continue; }
        if(!strcmp(argv[i],"--seed-banks")) { o.seed_banks=1; continue; }
        if(!strcmp(argv[i],"--music-index")) {   // optional FILE: the default index is output/music_index/music_index.bin
            if(i+1<argc && argv[i+1][0]!='-') midx_file=argv[++i]; else midx_file="";
            continue;
        }
        if(!strcmp(argv[i],"--entry")) {
            if(i+1>=argc || o.entry_count>=32 || !parse_entry(argv[++i],&o)) goto usage;
            continue;
        }
        if(!strcmp(argv[i],"--no-block-closure")) { o.block_closure=0; continue; }
        if(!strcmp(argv[i],"--no-target-closure")) { o.target_closure=0; continue; }
        if(!strcmp(argv[i],"--no-closure")) { o.closure.nodes=0; kcfg.no_closure=1; continue; }
        if(!strcmp(argv[i],"--watch")) {   // the kernel publishes the replayed movie's screen at 1..60 img/s (default 30)
            int fps=30; if(i+1<argc && argv[i+1][0]>='0' && argv[i+1][0]<='9') fps=atoi(argv[++i]);
            if(fps<1) fps=1;
            if(fps>60) fps=60;
            kcfg.watch_every=(unsigned)(60/fps); continue;
        }
        if(!strcmp(argv[i],"--watch-dump")) { if(i+1>=argc) goto usage; watch_dump=argv[++i]; setenv("DP_WATCH_SMOOTH","0",0); if(!kcfg.watch_every) kcfg.watch_every=2; continue; }
        if(!strcmp(argv[i],"--tas")) { if(i+1>=argc) goto usage; o.tas_path=argv[++i]; continue; }
        if(!strcmp(argv[i],"--jobs")) {
            if(i+1>=argc) goto usage;
            const char *v=argv[++i]; uint64_t n;
            if(!strcmp(v,"auto")) { core_jobs_auto=1; long cpus=sysconf(_SC_NPROCESSORS_ONLN); n=cpus>0?(uint64_t)cpus:1; }
            else if(!number(v,&n) || !n || n>65536) goto usage;
            o.jobs=(int)n; continue;
        }
        if(i+1>=argc) goto usage;
        const char *key=argv[i++],*value=argv[i];
        if(!strcmp(key,"--text")) text=value;
        else if(!strcmp(key,"--map")) map=value;
        else if(!strcmp(key,"--summary")) summary=value;
        else if(!strcmp(key,"--cov")) cov=value;
        else if(!strcmp(key,"--rtcode")) rtcode=value;   // the kernel's runtime code archive (exec_wram_rom)
        else if(!strcmp(key,"--evidence") || !strcmp(key,"--romdata")) fprintf(stderr,"deepeel: %s: not produced by the kernel yet (ignored)\n",key);
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
            if(!strcmp(key,"--max-total")) o.max_total=n;
            else if(!strcmp(key,"--seed-cap") || !strcmp(key,"--closure-nodes") || !strcmp(key,"--closure-seeds") || !strcmp(key,"--closure-views") || !strcmp(key,"--closure-contexts")) {
                if(n>UINT32_MAX) goto usage;
                if(!strcmp(key,"--closure-nodes")) o.closure.nodes=(uint32_t)n;
                else if(!strcmp(key,"--closure-seeds")) o.closure.seeds=(uint32_t)n;
                else if(!strcmp(key,"--closure-views")) o.closure.views=(uint32_t)n;
                else if(!strcmp(key,"--closure-contexts")) o.closure.contexts=(uint32_t)n;
            }
            else if(n>UINT32_MAX) goto usage;
            else if(!strcmp(key,"--max-steps")) o.max_steps=(uint32_t)n;
            else if(!strcmp(key,"--fork-cap") && n<=255) o.fork_cap=(uint32_t)n;
            else if(!strcmp(key,"--queue-max")) { if(n>65536) goto usage; o.queue_max=(uint32_t)n; }
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
    // music-rip index (DPMDX002, tools/music_index.py export --bin): the projected entry points of this game's
    // rips become confirmed seeds; KSS is the rip format of MSX and the Sega 8-bit systems.
    static const char *const plats_sg[]={"SCSG","KSS"},*plats_sms[]={"SCSG","MK3","KSS"},*plats_gg[]={"GG","KSS"},*plats_msx[]={"MSX","KSS"};
    const char *const *plats=!strcmp(s->id,"sms")?plats_sms:!strcmp(s->id,"gg")?plats_gg:!strcmp(s->id,"msx")?plats_msx:plats_sg;
    DpMusicSeedStats mst;
    static const char *const rip_dirs[]={"output/gbs","output/joshw"};
    const char *rb=strrchr(argv[4],'/'); rb=rb?rb+1:argv[4];
    DpMusicSeedCtx mctx={rip_dirs,2,image+header,(size_t)end-header,rb};   // the verification reads the image
    int mn=dp_music_seeds_cli(midx_file,plats,!strcmp(s->id,"sms")?2:1,image,(size_t)end,
                              (size_t)end-header,NULL,&music_seeds,&music_n,&mst,NULL,&mctx);
    if(mn==DP_MUSIC_SEEDS_LOAD_ERR && midx_file) { free(image); return 1; }
    if(mn==DP_MUSIC_SEEDS_ALLOC) { free(image); return 1; }
    if(mn>=0) { o.music_seeds=music_seeds; o.music_seed_count=(unsigned)mn; }
    core_cov_path=cov; core_text_path=text; core_map_path=map; core_summary_path=summary; core_rom_name=argv[4]; core_rtcode_path=rtcode;
    int cok=sega_core_run(s,image+header,(size_t)end-header,&o,&kcfg,stdout);
    free(image); free(music_seeds);
    if(!cok) fprintf(stderr,"invalid machine profile/mapper/ROM size, or allocation failure\n");
    return cok?0:1;
usage:
    fprintf(stderr,"deepeel z80 explore <sg|sms|gg|msx> ROM [--mapper NAME (SMS/GG: auto|sega|codemasters|korean150; MSX: auto|plain|ascii8|ascii16|konami|konami-scc)] [--max-total N] [--max-steps N] [--fork-cap N] [--queue-max N] [--jobs N|auto] [--entry BANK:ADDR]... [--seed-banks] [--no-vector-seeds] [--music-index [FILE]] [--tas FILE] [--no-closure] [--no-target-closure] [--no-block-closure] [--closure-nodes N] [--closure-seeds N] [--closure-views N] [--closure-contexts N] [kernel options: --attract-frames, --sessions, --unbounded, --state-dir, --resume, ... see dp_core_parse_option] [--text FILE] [--map FILE] [--cov FILE] [--summary FILE] [--mapwin [SCALE]] (--evidence/--romdata: accepted, not produced yet; --rtcode FILE: the runtime code archive)\n"); return 2;
}
