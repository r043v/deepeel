#include "../src/platform/sega/explore.h"
#include "test_util.h"
#include <string.h>

// GB parity: executed-run sections, per-context bookkeeping and necessity
// verdicts. The fixture makes the natural and the forced side of a branch
// converge on the same entry state: the second arrival at $0200 is a
// same-context eviction. Copies from LDIR attribute to their section and
// force its REQUIRED verdict; a seeded section with no boot, IRQ or copy
// evidence stays FORCED.
static uint8_t rom[0x10000];
static DpSegaResults result;

static DpSegaOptions options(void) {
    DpSegaOptions o=dp_sega_options();
    o.max_total=40000; o.max_steps=512; o.max_paths=64; o.queue_cap=64;
    o.seed_vectors=0; o.seed_cap=0; o.input_masks=0; o.closure.nodes=0;
    return o;
}
static void fixture(void) {
    memset(rom,0xff,sizeof(rom));
    rom[0]=0xc3; rom[1]=0x00; rom[2]=0x01;
    rom[0x100]=0x31; rom[0x101]=0x00; rom[0x102]=0xf0; // ld sp,$F000
    rom[0x103]=0x3e; rom[0x104]=0x01;                  // ld a,1
    rom[0x105]=0xfe; rom[0x106]=0x01;                  // cp 1
    rom[0x107]=0xca; rom[0x108]=0x00; rom[0x109]=0x02; // jp z,$0200 (natural taken)
    rom[0x10a]=0xc3; rom[0x10b]=0x60; rom[0x10c]=0x01; // jp $0160 (forced side)
    rom[0x130]=0x76;                                   // halt (entry seed target)
    rom[0x160]=0x3e; rom[0x161]=0x01;                  // ld a,1
    rom[0x162]=0xfe; rom[0x163]=0x01;                  // cp 1
    rom[0x164]=0xc3; rom[0x165]=0x00; rom[0x166]=0x02; // jp $0200
    rom[0x200]=0x21; rom[0x201]=0x00; rom[0x202]=0x03; // ld hl,$0300
    rom[0x203]=0x11; rom[0x204]=0x00; rom[0x205]=0xc0; // ld de,$C000
    rom[0x206]=0x01; rom[0x207]=0x04; rom[0x208]=0x00; // ld bc,4
    rom[0x209]=0xed; rom[0x20a]=0xb0;                  // ldir
    rom[0x20b]=0x76;                                   // halt
    rom[0x300]=0x11; rom[0x301]=0x22; rom[0x302]=0x33; rom[0x303]=0x44;
}
static void run(DpSegaOptions o) {
    dp_sega_results_free(&result);
    CHECK(dp_sega_explore(dp_sega_system("sms"),rom,sizeof(rom),&o,&result));
}
static DpSegaSection *section_at(unsigned physical) {
    for(uint32_t i=0;i<result.section_count;i++) {
        DpSegaSection *s=&result.sections[i];
        if(s->bank==(uint8_t)(physical>>14) && (physical&0x3fff)>=s->lo && (physical&0x3fff)<=s->hi) return s;
    }
    return NULL;
}
static void verdicts_and_eviction(void) {
    fixture();
    DpSegaOptions o=options();
    o.entry[0].bank=0; o.entry[0].addr=0x130; // seeded halt: FORCED verdict
    o.entry[1].bank=0; o.entry[1].addr=0x200; // reset-context entry
    o.entry[2].bank=0; o.entry[2].addr=0x200; // duplicate: evicted on re-entry
    o.entry_count=3;
    run(o);
    CHECK(result.section_count>=4);
    DpSegaSection *natural=section_at(0x200);
    DpSegaSection *side=section_at(0x160);
    DpSegaSection *seeded=section_at(0x130);
    CHECK(natural && side && seeded);
    // three legitimate contexts reach $0200: the boot path (sp $F000, a=1),
    // the forced side (reset SP, a=1) and the first entry seed (reset state);
    // the duplicate entry seed was evicted as an identical context
    CHECK(result.evicted_ctx>=1);
    CHECK(natural->ctx_count==3);
    CHECK(result.ctx_grew<1000 && !result.section_cuts);
    // the reset path is boot evidence: REQUIRED despite no copies there
    DpSegaSection *head=section_at(0x100);
    CHECK(head && head->boot_hits>0 && head->verdict==DP_SEGA_VERDICT_REQUIRED);
    // LDIR copies attribute to their pc's section and force REQUIRED
    CHECK(natural->copies>=1 && natural->verdict==DP_SEGA_VERDICT_REQUIRED);
    // the forced branch side and the seeded halt have no boot/IRQ/copy proof
    CHECK(side->forced_hits>0 && side->boot_hits==0 && side->verdict==DP_SEGA_VERDICT_FORCED);
    CHECK(seeded->verdict==DP_SEGA_VERDICT_FORCED);
    // LDIR read only ROM and wrote WRAM: no WRAM page is live in any section
    CHECK(natural->page_mask_wram==0);
}
static void eviction_disabled(void) {
    fixture();
    DpSegaOptions o=options();
    o.entry[0].bank=0; o.entry[0].addr=0x200;
    o.entry[1].bank=0; o.entry[1].addr=0x200;
    o.entry_count=2;
    o.no_context_evict=1;
    run(o);
    CHECK(result.evicted_ctx==0);
    // both duplicate entry seeds actually ran at $0200
    DpSegaSection *natural=section_at(0x200);
    CHECK(natural && natural->forced_hits>=2);
}
int main(void) {
    verdicts_and_eviction();
    eviction_disabled();
    dp_sega_results_free(&result);
    TEST_DONE("sega_sections");
}
