// test_registry.c: the SMS/GG/SG and MSX port tables as native DpIoReg
// entries — the common registry model every platform explorer shares.  The
// conversion keeps the metadata tables the single source of truth: every
// registered port appears, categories are valid, event registers carry the
// DP_IMPL_EVENT flag (the §3.7 probe contract), direction bits survive.
#include "test_util.h"
#include "../src/platform/platform.h"
#include "../src/platform/sega/sega.h"

int main(void) {
    static const char *cats[] = { "video", "sound", "timer", "input", "serial", "interrupt",
                                  "mapper", "cgb", "sys" };
    const char *ids[] = { "sg", "sms", "gg" };
    for (unsigned k = 0; k < 3; k++) {
        const DpSegaSystem *s = dp_sega_system(ids[k]);
        CHECK(s != NULL);
        const DpPlatform *p = dp_platform_sega(s);
        CHECK(p != NULL && p->io_reg_count > 0);
        CHECK((unsigned)p->io_reg_count == s->port_count); // nothing dropped
        int event = 0;
        for (int i = 0; i < p->io_reg_count; i++) {
            const DpIoReg *r = &p->io_regs[i];
            int cat_ok = 0;
            for (unsigned c = 0; c < sizeof cats / sizeof *cats; c++)
                if (!strcmp(r->category, cats[c])) cat_ok = 1;
            CHECK_MSG(cat_ok, "invalid category");
            CHECK(r->rw >= 1 && r->rw <= 3);
            const DpSegaPort *sp = NULL;
            for (unsigned j = 0; j < s->port_count; j++)
                if (!strcmp(s->ports[j].name, r->name)) sp = &s->ports[j];
            CHECK_MSG(sp != NULL, "invented register");
            CHECK(sp->value == r->lo && sp->value == r->hi);
            CHECK(sp->rw == r->rw);
            if (r->impl & DP_IMPL_EVENT) event++;
        }
        CHECK(event >= 3); // VDP_STATUS + V/H_COUNTER (+ GG_START): the §3.7 surface
    }
    const DpPlatform *m = dp_platform_msx();
    CHECK(m != NULL && m->io_reg_count == 10); // VDP/AY/PPI table complete
    int msx_event = 0;
    for (int i = 0; i < m->io_reg_count; i++)
        if (m->io_regs[i].impl & DP_IMPL_EVENT) msx_event++;
    CHECK(msx_event >= 1); // VDP_STATUS
    TEST_DONE("test_registry");
}
