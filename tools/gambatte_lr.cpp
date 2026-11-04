// gambatte_lr.cpp: level-B (DP_LR_ENV_TRACE) adapter over a patched gambatte core, in-process.
// Needs tools/gambatte_trace.patch applied to gambatte-core (branch observation / forcing in cpu.cpp).
// Build: see the header of tools/gambatte_trace.patch.  Run: gambatte_lr ROM.gb [path_steps total_steps]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
extern "C" {
#include "mach/libretro.h"
}
#include "gambatte.h"

namespace gambatte { extern int dp_force_cond; extern void (*dp_branch_cb)(unsigned); }
using gambatte::dp_force_cond;
using gambatte::dp_branch_cb;

static gambatte::GB *gb;
static DpLrObservers obs;
static bool have_obs;
static uint32_t cur_pc;
static unsigned execs;                 // opcode fetches seen during the current step
static std::vector<gambatte::uint_least32_t> video(160 * 144), audio(2064 + 64);
static size_t rom_size;

static void on_exec(int32_t a, int64_t) {
    if (getenv("GLR_TRACE")) fprintf(stderr, "%04x%s ", (unsigned)a, execs ? "'" : "");
    // HALT reads the next opcode as a prefetch inside the same step: only the first fetch of a step is an execution
    if (execs++) return;
    cur_pc = (uint32_t)a;
    if (have_obs && obs.exec) obs.exec(obs.user, (uint32_t)a);
}
static void on_read(int32_t a, int64_t) { if (have_obs && obs.read) obs.read(obs.user, (uint32_t)a, 0); }
static void on_write(int32_t a, int64_t) { if (have_obs && obs.write) obs.write(obs.user, (uint32_t)a, 0); }
static unsigned nbr;
static void on_branch(unsigned pc) { nbr++; if (have_obs && obs.branch) obs.branch(obs.user, pc); }

static int t_step(void) {
    execs = 0;
    // HALT: keep the clock running (bounded, ~4 frames) until an interrupt wakes the CPU and an opcode is fetched
    for (unsigned i = 0; i < 20000 && !execs; i++) {
        size_t samples = 1;
        gb->runFor(video.data(), 160, audio.data(), samples);
    }
    return execs ? 1 : 0;              // nothing fetched within the bound = halted for good
}
static void t_set(const DpLrObservers *o) { have_obs = o != NULL; if (o) obs = *o; }
static uint32_t t_pc(void) {
    int r[10]; gb->getRegs(r); return (uint32_t)r[0];
}
static int t_force(int taken) { dp_force_cond = taken; return 1; }
static int64_t t_rom_index(uint32_t a) {
    // compact index from the core's current bank mapping (patched GB::dpRomIndex)
    long i = gb->dpRomIndex(a); return i >= 0 && (size_t)i < rom_size ? (int64_t)i : -1;
}
static size_t t_ssize(void) { return gb->saveState(NULL, 0, NULL); }
static bool t_ser(void *p, size_t n) { return gb->saveState(NULL, 0, (char *)p) <= n; }
static bool t_unser(const void *p, size_t n) { return gb->loadState((const char *)p, n); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: gambatte_lr ROM.gb [path_steps total_steps]\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    rom_size = rom.size();
    gb = new gambatte::GB;
    if (gb->load(rom.data(), rom.size(), gambatte::GB::NO_BIOS)) { fprintf(stderr, "load failed\n"); return 1; }
    gb->setExecCallback(on_exec);
    gb->setReadCallback(on_read);
    gb->setWriteCallback(on_write);
    dp_branch_cb = on_branch;
    DpLrTraceApi api = {DP_LR_TRACE_VERSION, t_step, t_set, t_pc, t_force, t_rom_index};
    DpLibretro *lr = dp_lr_open_native("gambatte", t_ssize, t_ser, t_unser, &api);
    if (!lr) { fprintf(stderr, "native backend refused\n"); return 1; }
    DpLrExploreLimits lim = {};
    lim.path_steps = argc > 2 ? strtoull(argv[2], 0, 0) : 20000;
    lim.total_steps = argc > 3 ? strtoull(argv[3], 0, 0) : 2000000;
    lim.max_items = 4096; lim.site_cap = 2; lim.loop_bound = 3;
    std::vector<uint8_t> map(rom_size, 0);
    DpLrExploreStats st = {};
    int rc = dp_lr_explore(lr, map.data(), rom_size, &lim, &st);
    printf("gambatte level B: steps %llu forks %llu paths %llu covered %u branch_sites %u rc %d\n",
           (unsigned long long)st.steps, (unsigned long long)st.forks, (unsigned long long)st.paths, st.covered, st.branch_sites, rc);
    printf("cut: path_steps %llu total %llu items %llu forks %llu loop %llu\n", (unsigned long long)st.cut_path_steps,
           (unsigned long long)st.cut_total, (unsigned long long)st.dropped_items, (unsigned long long)st.capped_forks, (unsigned long long)st.cut_loop);
    if (getenv("GLR_STARTS")) { unsigned n = 0; for (size_t i = 0x100; i < rom_size; i++) if (map[i]) { printf("%04zx ", i); n++; } printf("\nstarts>=0100: %u\n", n); }
    fprintf(stderr, "branch callbacks seen: %u\n", nbr);
    dp_lr_close(lr);
    return 0;
}
