cmake_minimum_required(VERSION 3.16)
project(deepeel VERSION 0.292.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
endif()
option(BUILD_GUI "Build the SDL2 live viewer" ON)

# --- the m0boy GB machine (FEMTO8_DIR-style dependency) --------------------
set(M0BOY_DIR "${PROJECT_SOURCE_DIR}/../m0boy" CACHE PATH
    "m0boy repository providing targets/meta/runtime (the GB machine)")
set(DP_GBL_RUNTIME "${M0BOY_DIR}/targets/meta/runtime")
if(NOT EXISTS "${DP_GBL_RUNTIME}/gbl_core.c")
  message(FATAL_ERROR "M0BOY_DIR (${M0BOY_DIR}) must contain targets/meta/runtime/gbl_core.c: \
deepeel embeds m0boy's Game Boy machine instead of its own")
endif()
# --- the m0boy Mega Drive machine (MdFull, 68000 + bus) ----------------------
set(DP_MD_RUNTIME "${M0BOY_DIR}/targets/megadrive/runtime")
set(DP_MD_Z80 "${M0BOY_DIR}/targets/sg1000/runtime")
if(NOT EXISTS "${DP_MD_RUNTIME}/md_full_cpu.c" OR NOT EXISTS "${DP_MD_RUNTIME}/md_trace.h")
  message(FATAL_ERROR "M0BOY_DIR must provide targets/megadrive/runtime with the MD_TRACE hooks")
endif()
# Keep the upstream runtime intact; add a resumable frame entry point to a
# build-tree copy so snapshots taken inside a scanline do not replay events.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DP_MD_RUNTIME}/md_full_cpu.c")
file(READ "${DP_MD_RUNTIME}/md_full_cpu.c" DP_MD_CPU)
set(DP_MD_FRAME_HEAD "void md_full_frame(MdFull *m, int native) {\n    for (unsigned line = 0; line < FRAME_LINES && !m->fault; ++line) {")
set(DP_MD_FRAME_RESUME "void dp_md_full_frame(MdFull *m, int native, int resume) {\n    unsigned first = resume ? m->line : 0;\n    for (unsigned line = first; line < FRAME_LINES && !m->fault; ++line) {")
string(FIND "${DP_MD_CPU}" "${DP_MD_FRAME_HEAD}" DP_MD_FRAME_POS)
if(DP_MD_FRAME_POS EQUAL -1)
  message(FATAL_ERROR "m0boy MD frame implementation changed; update resumable-frame adaptation")
endif()
string(REPLACE "${DP_MD_FRAME_HEAD}" "${DP_MD_FRAME_RESUME}" DP_MD_CPU "${DP_MD_CPU}")
string(FIND "${DP_MD_CPU}" "if (line == ACTIVE_LINES) m->irq_pending = 1;" DP_MD_VBLANK_POS)
string(FIND "${DP_MD_CPU}" "    m->frame_cycles -= FRAME_LINES * LINE_CYCLES;" DP_MD_FRAME_END_POS)
if(DP_MD_VBLANK_POS EQUAL -1 OR DP_MD_FRAME_END_POS EQUAL -1)
  message(FATAL_ERROR "m0boy MD frame events changed; update resumable-frame adaptation")
endif()
string(REPLACE "if (line == ACTIVE_LINES) m->irq_pending = 1;" "if (line == ACTIVE_LINES && !(resume && line == first)) m->irq_pending = 1;" DP_MD_CPU "${DP_MD_CPU}")
string(REPLACE "    m->frame_cycles -= FRAME_LINES * LINE_CYCLES;" "    if (m->fault) return;\n    m->frame_cycles -= FRAME_LINES * LINE_CYCLES;" DP_MD_CPU "${DP_MD_CPU}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_cpu.c" "${DP_MD_CPU}\nvoid md_full_frame(MdFull *m, int native) { dp_md_full_frame(m, native, 0); }\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DP_MD_RUNTIME}/md_full_bus.c")
file(READ "${DP_MD_RUNTIME}/md_full_bus.c" DP_MD_BUS)
set(DP_MD_DMA_READ "uint16_t v = md_full_read16(m, (source++ << 1) & 0xffffffu);")
string(FIND "${DP_MD_BUS}" "${DP_MD_DMA_READ}" DP_MD_DMA_POS)
if(DP_MD_DMA_POS EQUAL -1)
  message(FATAL_ERROR "m0boy MD DMA implementation changed; update DMA observation hook")
endif()
string(REPLACE "${DP_MD_DMA_READ}" "uint32_t dma_addr = (source++ << 1) & 0xffffffu;\n            uint16_t v = md_full_read16(m, dma_addr);\n            if (!m->fault) dp_md_dma_observe(m, dma_addr);" DP_MD_BUS "${DP_MD_BUS}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_bus.c" "#include \"md_full.h\"\nvoid dp_md_dma_observe(MdFull *, uint32_t);\n${DP_MD_BUS}")
# Z80 sound CPU observation: hooks around its step, its ROM-window reads and
# every sound-bus store (generated copy of md_full_audio.c).
file(READ "${DP_MD_RUNTIME}/md_full_audio.c" DP_MD_AUDIO)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DP_MD_RUNTIME}/md_full_audio.c")
function(dp_md_audio_patch old new)
  string(FIND "${DP_MD_AUDIO}" "${old}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "m0boy MD audio changed; update Z80 observation hooks: ${old}")
  endif()
  string(REPLACE "${old}" "${new}" patched "${DP_MD_AUDIO}")
  set(DP_MD_AUDIO "${patched}" PARENT_SCOPE)
endfunction()
dp_md_audio_patch("at += z80_step(z) * 15u;"
  "dp_md_z80_begin(m, z); { unsigned dp_c = z80_step(z); dp_md_z80_end(m); at += dp_c * 15u; }")
dp_md_audio_patch("if (a < md_rom_size) return md_rom[a];"
  "if (a < md_rom_size) { dp_md_z80_rom_read(m, a); return md_rom[a]; }")
dp_md_audio_patch("static void sound_store(MdFull *m, uint16_t addr, uint8_t value, uint64_t at) {"
  "static void sound_store(MdFull *m, uint16_t addr, uint8_t value, uint64_t at) {\n    dp_md_sound_store(m, addr, value);")
# MdFull::no_audio/no_z80 exist only in MD_PERF builds; drop the bench-only
# short-circuits from the generated copy when upstream uses them unguarded.
string(REPLACE "    if (m->no_audio) return;\n" "" DP_MD_AUDIO "${DP_MD_AUDIO}")
string(REPLACE "    if (m->no_z80) return;\n" "" DP_MD_AUDIO "${DP_MD_AUDIO}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_audio.c"
  "#include \"md_full.h\"\nvoid dp_md_z80_begin(MdFull *, z80_t *);\nvoid dp_md_z80_end(MdFull *);\nvoid dp_md_z80_rom_read(MdFull *, uint32_t);\nvoid dp_md_sound_store(MdFull *, uint16_t, uint8_t);\n${DP_MD_AUDIO}")
add_library(dp_mdmach_lib STATIC
  ${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_cpu.c ${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_bus.c
  ${DP_MD_RUNTIME}/md_full_video.c ${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_audio.c
  ${DP_MD_RUNTIME}/md_ym2612.c ${DP_MD_Z80}/z80.c
  src/mach/mdmach.c src/mach/mdrom.c)
target_compile_definitions(dp_mdmach_lib PRIVATE MD_TRACE)
target_include_directories(dp_mdmach_lib PUBLIC src/mach PRIVATE ${DP_MD_RUNTIME} ${DP_MD_Z80})
target_compile_options(dp_mdmach_lib PRIVATE -w)

set(DP_MACHINES "18" CACHE STRING "compiled m0boy machine instances (session workers)")
# GBL_CGB in the m0boy runtime (host, optional): -DDP_GBL_CGB=1
set(DP_GBL_CGB 0 CACHE STRING "build the m0boy runtime with Game Boy Color support")
set(DP_GBL_VARIANTS "0:0;1:0;1:0x800;1:0x2000;1:0x8000;2:0x200;3:0;3:0x800;3:0x2000;3:0x8000;3:0x10000;3:0x20000;5:0;5:0x800;5:0x2000;5:0x8000;5:0x10000;5:0x20000"
    CACHE STRING "cartridge shapes compiled into the machine (MBC:ERAM)")

find_program(DP_OBJCOPY objcopy)
find_program(DP_NM nm)
find_program(DP_AR ar)
if(NOT DP_OBJCOPY OR NOT DP_NM OR NOT DP_AR)
  message(FATAL_ERROR "binutils (objcopy, nm, ar) are required to build the machine instances")
endif()

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})

list(LENGTH DP_GBL_VARIANTS nv)
math(EXPR lasti "${DP_MACHINES} - 1")

# ---- generated: counts header, then one instances file with the prefixed
# forwarders, the per-instance data and the api table.
file(WRITE ${GEN}/dp_gbl_counts.h "\
/* generated by CMakeLists.txt: machine variant/instance counts */
#define DP_GBL_VARIANTS ${nv}
#define DP_GBL_MACHINES ${DP_MACHINES}
")

set(g_names "")
set(g_mbc "")
set(g_eram "")
set(g_body "")
set(g_rows "")
set(v 0)
foreach(spec ${DP_GBL_VARIANTS})
  string(REPLACE ":" ";" kv "${spec}")
  list(GET kv 0 mbc)
  list(GET kv 1 eram)
  list(APPEND g_mbc "${mbc}")
  list(APPEND g_eram "${eram}")
  list(APPEND g_names "\"MBC${mbc}/${eram}\"")
  if(mbc EQUAL 0)
    set(rommax 0x8000)
  elseif(mbc EQUAL 1)
    set(rommax 0x200000)
  elseif(mbc EQUAL 2)
    set(rommax 0x40000)
  elseif(mbc EQUAL 3)
    set(rommax 0x400000)
  else()
    set(rommax 0x800000)
  endif()
  string(APPEND g_body "/* variant ${v}: MBC${mbc}, ${eram} bytes of cartridge RAM, ${rommax} of ROM */
")
  foreach(i RANGE ${lasti})
    set(p "v${v}m${i}_")
    if(eram STREQUAL "0")
      set(eram_ref "NULL")
    else()
      set(eram_ref "${p}gbl_eram")
    endif()
    string(APPEND g_body "\
extern uint8_t ${p}gbl_rd(uint16_t a);
void ${p}gbl_coverage_hit(uint16_t bank, uint16_t addr, int entry) { dp_gbl_hook_coverage(${v}, ${i}, bank, addr, entry); }
void ${p}gbl_trace_hook(uint16_t pc) { dp_gbl_hook_trace(${v}, ${i}, pc); }
uint8_t ${p}dp_gbl_rd(uint16_t a) { uint8_t val = ${p}gbl_rd(a); dp_gbl_hook_read(${v}, ${i}, a, val); return val; }
extern void ${p}gbl_wr(uint16_t a, uint8_t val);
void ${p}dp_gbl_wr(uint16_t a, uint8_t val) { dp_gbl_hook_write(${v}, ${i}, a, val); ${p}gbl_wr(a, val); }
uint16_t *${p}gbl_platform_line_begin(uint8_t ly) { return dp_gbl_hook_line_begin(${v}, ${i}, ly); }
void ${p}gbl_platform_line_end(uint8_t ly) { dp_gbl_hook_line_end(${v}, ${i}, ly); }
int ${p}gbl_platform_line_check(uint8_t ly, uint32_t sig) { (void)ly; (void)sig; return 1; }
void ${p}gbl_platform_vblank(void) { dp_gbl_hook_vblank(${v}, ${i}); }
void ${p}gbl_platform_safe_point(void) { dp_gbl_hook_safe(${v}, ${i}); }
/* the ROM lives here; dp_mach_set_rom copies it in (zero-init: .bss, the
 * untouched variants never cost a page) */
const uint8_t ${p}gbl_rom[${rommax}] __attribute__((section(\".bss.gblrom\")));
const gbl_bank_table ${p}gbl_tables[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
const uint16_t ${p}gbl_table_count = 2;
extern void ${p}gbl_reset(void);
extern void ${p}gbl_run(void);
extern void ${p}gbl_set_buttons(uint8_t pressed);
extern uint32_t ${p}gbl_state_bytes(void);
extern void ${p}gbl_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
extern gbl_cpu_t ${p}gbr;
extern uint8_t ${p}gbl_io[0x80];
extern uint8_t ${p}gbl_ie, ${p}gbl_wram[0x2000], ${p}gbl_vram[0x2000], ${p}gbl_oam[0xA0], ${p}gbl_hram[0x80];
extern uint8_t ${p}gbl_joyp, ${p}gbl_interp_only, ${p}gbl_skip_render;
extern uint8_t ${p}gbl_sync_div_skew, ${p}gbl_sync_vba_off_clock;
extern uint32_t ${p}gbl_sync_pad_delay;
extern uint16_t ${p}gbl_sync_div_extra;
extern uint16_t ${p}gbl_sync_ppu_phase;
extern void ${p}gbl_ppu_phase_apply(void);
extern uint8_t ${p}gbl_sync_echo_old; extern int16_t ${p}gbl_sync_ly_lag; extern int16_t ${p}gbl_sync_irq_trim; extern int16_t ${p}gbl_sync_halt_snd;
extern void ${p}gbl_echo_apply(void);
extern uint8_t ${p}gbl_eram[];
extern void ${p}gbl_wr(uint16_t a, uint8_t v);
extern const uint8_t *${p}gbl_rmap[16];
extern uint32_t ${p}gbl_frame, ${p}gbl_interp_instrs;
extern uint16_t ${p}gbl_rom0_bank;
extern uint32_t ${p}gbl_rom_banks;
extern uint8_t ${p}gbl_mbc1m;
static const DpGblApi inst_${v}_${i} = {
    ${v}, ${p}gbl_reset, ${p}gbl_run, ${p}gbl_set_buttons, ${p}gbl_state_bytes, ${p}gbl_state_io,
    &${p}gbr, ${p}gbl_io, &${p}gbl_ie, ${p}gbl_wram, ${p}gbl_vram, ${p}gbl_oam, ${p}gbl_hram,
    &${p}gbl_joyp, ${eram_ref}, (const uint8_t *const *)&${p}gbl_rmap, &${p}gbl_frame,
    &${p}gbl_interp_instrs, &${p}gbl_interp_only, &${p}gbl_skip_render,
    (uint8_t *)${p}gbl_rom, ${rommax}, ${p}gbl_wr,
    &${p}gbl_sync_div_skew, &${p}gbl_sync_vba_off_clock, &${p}gbl_sync_pad_delay, &${p}gbl_sync_div_extra, &${p}gbl_sync_ppu_phase, ${p}gbl_ppu_phase_apply, &${p}gbl_rom0_bank, &${p}gbl_rom_banks, &${p}gbl_mbc1m,
    &${p}gbl_sync_echo_old, ${p}gbl_echo_apply, &${p}gbl_sync_ly_lag, &${p}gbl_sync_irq_trim, &${p}gbl_sync_halt_snd,
};
")
    string(APPEND g_rows "[${v}][${i}] = &inst_${v}_${i},
")
  endforeach()
  math(EXPR v "${v} + 1")
endforeach()

string(JOIN "," DP_GBL_VARIANTS_COMMA ${DP_GBL_VARIANTS})
string(JOIN ",\n" g_mbc_j ${g_mbc})
string(JOIN ",\n" g_eram_j ${g_eram})
string(JOIN ",\n" g_names_j ${g_names})
file(WRITE ${GEN}/dp_gbl_instances.c "\
/* generated by CMakeLists.txt: prefixed forwarders, data and api table for
 * every (variant, instance) of the m0boy machine. */
#include <stddef.h>
#include \"deepeel/gbl_api.h\"

${g_body}
const DpGblApi *const dp_gbl_inst[DP_GBL_VARIANTS][DP_GBL_MACHINES] = {
${g_rows}};

const uint8_t dp_gbl_variant_mbc[DP_GBL_VARIANTS] = {
${g_mbc_j}};

const uint32_t dp_gbl_variant_eram[DP_GBL_VARIANTS] = {
${g_eram_j}};

const char *dp_gbl_variant_name(int v) {
    static const char *const names[] = {
${g_names_j}};
    return names[v];
}
")

# ---- build-time: compile the runtime per variant, clone instances, archive
set(GBL_ARCH ${GEN}/mach/libgblmach.a)
add_custom_command(OUTPUT ${GBL_ARCH}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}/mach
  COMMAND ${CMAKE_COMMAND}
    -DDP_GBL_SRC=${DP_GBL_RUNTIME}
    -DDP_GBL_OUT=${GEN}/mach
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DOBJCOPY=${DP_OBJCOPY} -DNM=${DP_NM} -DAR=${DP_AR}
    -DBASE_FLAGS=${CMAKE_C_FLAGS}
    -DDP_GBL_VARIANTS=${DP_GBL_VARIANTS_COMMA}
    -DDP_GBL_CGB=${DP_GBL_CGB}
    -DDP_MACHINES=${DP_MACHINES}
    -DDP_GBL_PATCH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/m0boy-tas-compat.patch
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_gbl_build.cmake
  DEPENDS ${DP_GBL_RUNTIME}/gbl_core.c ${DP_GBL_RUNTIME}/gbl_cpu.c
          ${DP_GBL_RUNTIME}/gbl_ppu.c ${DP_GBL_RUNTIME}/gbl_apu.c
          ${DP_GBL_RUNTIME}/gbl.h ${DP_GBL_RUNTIME}/gbl_internal.h
          ${DP_GBL_RUNTIME}/gbl_options.h
          cmake/dp_gbl_build.cmake cmake/m0boy-tas-compat.patch
  COMMENT "Building the m0boy GB machine (${nv} variants x ${DP_MACHINES} instances)")
add_custom_target(dp_gblmach DEPENDS ${GBL_ARCH})
add_library(dp_gblmach_lib STATIC IMPORTED GLOBAL)
set_target_properties(dp_gblmach_lib PROPERTIES IMPORTED_LOCATION ${GBL_ARCH})
add_dependencies(dp_gblmach_lib dp_gblmach)

# --- core library: everything but main(); the gui file follows SDL2 availability
set(DP_GUI_FILE src/gui/gui_stub.c)
set(DP_GUI_LIBS "")
if(SDL2_FOUND AND BUILD_GUI)
  set(DP_GUI_FILE src/gui/gui.c)
  set(DP_GUI_LIBS ${SDL2_LIBRARIES})
endif()

# the version the reports and the .map.png metadata carry
add_compile_definitions(DEEPEEL_VERSION="${PROJECT_VERSION}")

add_library(deepeel_core STATIC
  src/core/cart.c
  src/core/cart_md.c
  src/platform/md/md_registry.c
  src/explore/md_trace.c
  src/cpu/m68k/m68k_disasm.c
  src/platform/md/explore.c
  src/platform/md/jobs.c
  src/platform/md/movie.c
  src/core/snapshot_png.c
  src/core/zip.c
  src/core/bk2.c
  src/core/sha1.c
  src/core/md5.c
  src/core/statefile.c
  src/core/sta.c
  src/platform/gb/gb_registry.c
  src/cpu/sm83/sm83_disasm.c
  src/cpu/sm83/sm83_isa.c
  src/mach/gb_machine.c
  src/mach/m0boy.c
  src/mach/lr_cmd.c
  src/mach/bh_cmd.c
  ${GEN}/dp_gbl_instances.c
  src/explore/explorer.c
  src/explore/core.c
  src/explore/frontier.c
  src/explore/frontier_disk.c
  src/explore/forks.c
  src/explore/streams.c
  src/explore/seeds.c
  src/explore/ctxproj.c
  src/explore/sections.c
  src/explore/copies.c
  src/explore/rtcode.c
  src/explore/ramarch.c
  src/explore/stop.c
  src/explore/sessions.c
  src/explore/gambatte_init.c
  src/explore/tree.c
  src/explore/closure.c
  src/explore/residual.c
  src/explore/provenance.c
  src/explore/tas.c
  src/report/report.c
  src/report/map.c
  src/report/heat.c
    src/report/html.c
  src/gui/font5x7.c
  src/gui/fbwatch.c
  ${DP_GUI_FILE}
)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED) # PNG maps and snapshots: compressed image and zTXt chunks
target_link_libraries(deepeel_core PUBLIC Threads::Threads ZLIB::ZLIB m dp_gblmach_lib dp_mdmach_lib deepeel_libretro ${CMAKE_DL_LIBS})
target_include_directories(deepeel_core PUBLIC
  src include ${GEN} ${DP_GBL_RUNTIME} ${SDL2_INCLUDE_DIRS})
if(SDL2_FOUND AND BUILD_GUI)
  set(DP_HAS_GUI 1)
  add_compile_definitions(DP_HAS_GUI=1)
endif()

add_executable(deepeel src/deepeel.c)
add_library(deepeel_libretro STATIC src/mach/libretro.c src/mach/lr_machine.c)
target_include_directories(deepeel_libretro PUBLIC src)
target_link_libraries(deepeel PRIVATE deepeel_libretro ${CMAKE_DL_LIBS})
add_library(deepeel_z80 STATIC
  ${M0BOY_DIR}/targets/sg1000/runtime/z80.c
  src/cpu/z80/z80.c
  src/cpu/z80/z80_isa.c
  src/mach/sega_machine.c
  src/mach/md_machine.c
  src/cpu/z80/provenance.c
  src/cpu/z80/command.c
  src/platform/sega/sega.c)
target_include_directories(deepeel_z80 PUBLIC
  ${M0BOY_DIR}/targets/sg1000/runtime src)
add_library(deepeel_sg STATIC
  ${M0BOY_DIR}/targets/sg1000/runtime/sg1000.c
  ${M0BOY_DIR}/targets/sg1000/runtime/sg_audio.c
  ${M0BOY_DIR}/targets/sg1000/runtime/tms9918.c
  src/platform/sega/backend_sg.c)
target_compile_definitions(deepeel_sg PRIVATE
  machine=dp_sg_machine sg_stack_limit=dp_sg_stack_limit
  sg_sync=dp_sg_sync sg_event=dp_sg_event sg_halt=dp_sg_halt
  sg_exec=dp_sg_exec sg_interp_block=dp_sg_interp_block sg_step=dp_sg_step)
target_include_directories(deepeel_sg PRIVATE ${M0BOY_DIR}/targets/common/runtime)
target_link_libraries(deepeel_sg PUBLIC deepeel_z80)
set(DP_GG_VDP_SOURCE ${GEN}/dp_gg_vdp.c)
add_custom_command(OUTPUT ${DP_GG_VDP_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${M0BOY_DIR}/targets/gamegear/runtime/gg_vdp.c
    -DOUTPUT=${DP_GG_VDP_SOURCE} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_gg_instrument.cmake
  DEPENDS ${M0BOY_DIR}/targets/gamegear/runtime/gg_vdp.c cmake/dp_gg_instrument.cmake VERBATIM)
add_library(deepeel_gg STATIC
  ${M0BOY_DIR}/targets/gamegear/runtime/gg.c
  ${DP_GG_VDP_SOURCE}
  ${M0BOY_DIR}/targets/gamegear/runtime/gg_psg.c
  src/platform/sega/backend_gg.c)
target_include_directories(deepeel_gg PRIVATE
  ${M0BOY_DIR}/targets/gamegear/runtime ${M0BOY_DIR}/targets/common/runtime)
target_compile_definitions(deepeel_gg PRIVATE GG_ROM_TRACE=1)
target_link_libraries(deepeel_gg PUBLIC deepeel_z80)
# A separate MSX1 instance: shared Z80 CPU, distinct SG/TMS/AY runtime symbols.
set(DP_MSX_SOURCE ${GEN}/dp_msx_runtime.c)
add_custom_command(OUTPUT ${DP_MSX_SOURCE}
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${M0BOY_DIR}/targets/msx/runtime/msx.c -DOUTPUT=${DP_MSX_SOURCE}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_msx_instrument.cmake
  DEPENDS ${M0BOY_DIR}/targets/msx/runtime/msx.c cmake/dp_msx_instrument.cmake
  VERBATIM)
add_library(deepeel_msx_raw STATIC
  ${M0BOY_DIR}/targets/sg1000/runtime/sg1000.c
  ${M0BOY_DIR}/targets/sg1000/runtime/tms9918.c
  ${DP_MSX_SOURCE}
  ${M0BOY_DIR}/targets/msx/runtime/msx_ay.c
  src/platform/msx/backend.c src/platform/msx/registry.c)
target_compile_definitions(deepeel_msx_raw PRIVATE SG_MSX=1 SG_MSX_RAM=65536)
target_include_directories(deepeel_msx_raw PRIVATE
  ${M0BOY_DIR}/targets/sg1000/runtime ${M0BOY_DIR}/targets/common/runtime
  ${M0BOY_DIR}/targets/msx/runtime ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/msx)
set(DP_MSX_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_msx.a)
add_custom_command(OUTPUT ${DP_MSX_ARCHIVE}
  BYPRODUCTS ${DP_MSX_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_msx_raw> -DOUTPUT=${DP_MSX_ARCHIVE}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_msx_prefix.cmake
  DEPENDS deepeel_msx_raw cmake/dp_msx_prefix.cmake VERBATIM)
add_custom_target(dp_msx_runtime DEPENDS ${DP_MSX_ARCHIVE})
add_library(deepeel_msx STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_msx PROPERTIES IMPORTED_LOCATION ${DP_MSX_ARCHIVE}
  INTERFACE_LINK_LIBRARIES deepeel_z80)
add_dependencies(deepeel_msx dp_msx_runtime)
target_link_libraries(deepeel PRIVATE deepeel_msx)
add_library(deepeel_sega STATIC src/platform/sega/explore.c src/platform/sega/closure.c src/platform/sega/visits.c src/platform/sega/tas.c src/platform/sega/registry.c src/explore/frontier.c src/core/bk2.c src/core/md5.c src/core/zip.c src/core/sha1.c src/core/statefile.c src/gui/fbwatch.c src/explore/ramarch.c src/explore/core.c)
target_link_libraries(deepeel_sega PUBLIC ZLIB::ZLIB)
# Second runtime instances for --jobs 2: the same sources compiled again with
# the backend getter renamed, then every global namespaced by objcopy (the
# dp_sega_prefix.cmake generalization of the MSX technique).
add_library(deepeel_sg2_raw STATIC
  ${M0BOY_DIR}/targets/sg1000/runtime/sg1000.c
  ${M0BOY_DIR}/targets/sg1000/runtime/sg_audio.c
  ${M0BOY_DIR}/targets/sg1000/runtime/tms9918.c
  src/platform/sega/backend_sg.c)
target_compile_definitions(deepeel_sg2_raw PRIVATE dp_sega_backend_sg=dp_sega_backend_sg2)
target_include_directories(deepeel_sg2_raw PRIVATE
  ${M0BOY_DIR}/targets/common/runtime ${M0BOY_DIR}/targets/sg1000/runtime)
set(DP_SG2_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_sg2.a)
add_custom_command(OUTPUT ${DP_SG2_ARCHIVE}
  BYPRODUCTS ${DP_SG2_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_sg2_raw> -DOUTPUT=${DP_SG2_ARCHIVE}
    -DPREFIX=dp_sg2_ -DPATTERNS=machine|sg[A-Za-z0-9_]*|tms[A-Za-z0-9_]*
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_sg2_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_library(deepeel_sg2 STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_sg2 PROPERTIES IMPORTED_LOCATION ${DP_SG2_ARCHIVE})
add_custom_target(dp_sg2_runtime DEPENDS ${DP_SG2_ARCHIVE})
add_dependencies(deepeel_sg2 dp_sg2_runtime)
add_library(deepeel_gg2_raw STATIC
  ${M0BOY_DIR}/targets/gamegear/runtime/gg.c
  ${DP_GG_VDP_SOURCE}
  ${M0BOY_DIR}/targets/gamegear/runtime/gg_psg.c
  src/platform/sega/backend_gg.c)
target_compile_definitions(deepeel_gg2_raw PRIVATE GG_ROM_TRACE=1 dp_sega_backend_gg=dp_sega_backend_gg2)
target_include_directories(deepeel_gg2_raw PRIVATE
  ${M0BOY_DIR}/targets/gamegear/runtime ${M0BOY_DIR}/targets/common/runtime
  ${M0BOY_DIR}/targets/sg1000/runtime)
set(DP_GG2_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_gg2.a)
add_custom_command(OUTPUT ${DP_GG2_ARCHIVE}
  BYPRODUCTS ${DP_GG2_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_gg2_raw> -DOUTPUT=${DP_GG2_ARCHIVE}
    -DPREFIX=dp_gg2_ -DPATTERNS=machine|sg[A-Za-z0-9_]*|gg[A-Za-z0-9_]*|tms[A-Za-z0-9_]*
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_gg2_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_library(deepeel_gg2 STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_gg2 PROPERTIES IMPORTED_LOCATION ${DP_GG2_ARCHIVE})
add_custom_target(dp_gg2_runtime DEPENDS ${DP_GG2_ARCHIVE})
add_dependencies(deepeel_gg2 dp_gg2_runtime)
# SMS/GG campaigns can use the full worker pool. Each worker needs an isolated
# copy of the runtime globals; keep the pool size aligned with 16-core hosts.
foreach(DP_SEGA_WORKER RANGE 3 16)
  set(DP_GGN_RAW deepeel_gg${DP_SEGA_WORKER}_raw)
  set(DP_GGN_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_gg${DP_SEGA_WORKER}.a)
  add_library(${DP_GGN_RAW} STATIC
    ${M0BOY_DIR}/targets/gamegear/runtime/gg.c
    ${DP_GG_VDP_SOURCE}
    ${M0BOY_DIR}/targets/gamegear/runtime/gg_psg.c
    src/platform/sega/backend_gg.c)
  target_compile_definitions(${DP_GGN_RAW} PRIVATE GG_ROM_TRACE=1
    dp_sega_backend_gg=dp_sega_backend_gg${DP_SEGA_WORKER})
  target_include_directories(${DP_GGN_RAW} PRIVATE
    ${M0BOY_DIR}/targets/gamegear/runtime ${M0BOY_DIR}/targets/common/runtime
    ${M0BOY_DIR}/targets/sg1000/runtime)
  add_custom_command(OUTPUT ${DP_GGN_ARCHIVE}
    BYPRODUCTS ${DP_GGN_ARCHIVE}.syms
    COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
      -DINPUT=$<TARGET_FILE:${DP_GGN_RAW}> -DOUTPUT=${DP_GGN_ARCHIVE}
      -DPREFIX=dp_gg${DP_SEGA_WORKER}_ -DPATTERNS=machine|sg[A-Za-z0-9_]*|gg[A-Za-z0-9_]*|tms[A-Za-z0-9_]*
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
    DEPENDS ${DP_GGN_RAW} cmake/dp_sega_prefix.cmake VERBATIM)
  add_library(deepeel_gg${DP_SEGA_WORKER} STATIC IMPORTED GLOBAL)
  set_target_properties(deepeel_gg${DP_SEGA_WORKER} PROPERTIES IMPORTED_LOCATION ${DP_GGN_ARCHIVE})
  add_custom_target(dp_gg${DP_SEGA_WORKER}_runtime DEPENDS ${DP_GGN_ARCHIVE})
  add_dependencies(deepeel_gg${DP_SEGA_WORKER} dp_gg${DP_SEGA_WORKER}_runtime)
  target_link_libraries(deepeel_sega PUBLIC deepeel_gg${DP_SEGA_WORKER})
endforeach()
# Native 256-wide Master System instance of the GG runtime (GG_OUT_WIDTH=256,
# the PC Master System panel: no GG window crop, no filter — gg.h) — the
# fidelity bench plays the SMS profile on the console's own screen. Same
# namespacing technique: every global prefixed dp_sms_, backend getter renamed.
add_library(deepeel_sms_raw STATIC
  ${M0BOY_DIR}/targets/gamegear/runtime/gg.c
  ${DP_GG_VDP_SOURCE}
  ${M0BOY_DIR}/targets/gamegear/runtime/gg_psg.c
  src/platform/sega/backend_gg.c)
target_compile_definitions(deepeel_sms_raw PRIVATE GG_ROM_TRACE=1 GG_OUT_WIDTH=256 dp_sega_backend_gg=dp_sega_backend_sms)
target_include_directories(deepeel_sms_raw PRIVATE
  ${M0BOY_DIR}/targets/gamegear/runtime ${M0BOY_DIR}/targets/common/runtime
  ${M0BOY_DIR}/targets/sg1000/runtime)
set(DP_SMS_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_sms.a)
add_custom_command(OUTPUT ${DP_SMS_ARCHIVE}
  BYPRODUCTS ${DP_SMS_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_sms_raw> -DOUTPUT=${DP_SMS_ARCHIVE}
    -DPREFIX=dp_sms_ -DPATTERNS=machine|gg[A-Za-z0-9_]*|sg[A-Za-z0-9_]*|tms[A-Za-z0-9_]*
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_sms_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_library(deepeel_sms STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_sms PROPERTIES IMPORTED_LOCATION ${DP_SMS_ARCHIVE}
  INTERFACE_LINK_LIBRARIES deepeel_z80)
add_custom_target(dp_sms_runtime ALL DEPENDS ${DP_SMS_ARCHIVE})
add_dependencies(deepeel_sms dp_sms_runtime)
# MSX second instance for --jobs 2: backend.c and the instrumented msx.c
# recompiled with the backend getter renamed, then every global (runtime and
# dp_msx_* alike, except the renamed getter) namespaced with dp_msx2_.
add_library(deepeel_msx2_raw STATIC
  ${M0BOY_DIR}/targets/sg1000/runtime/sg1000.c
  ${M0BOY_DIR}/targets/sg1000/runtime/tms9918.c
  ${DP_MSX_SOURCE}
  ${M0BOY_DIR}/targets/msx/runtime/msx_ay.c
  src/platform/msx/backend.c)
target_compile_definitions(deepeel_msx2_raw PRIVATE SG_MSX=1 SG_MSX_RAM=65536 dp_msx_backend=dp_msx_backend2)
target_include_directories(deepeel_msx2_raw PRIVATE
  ${M0BOY_DIR}/targets/sg1000/runtime ${M0BOY_DIR}/targets/common/runtime
  ${M0BOY_DIR}/targets/msx/runtime ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/msx)
set(DP_MSX2_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_msx2.a)
add_custom_command(OUTPUT ${DP_MSX2_ARCHIVE}
  BYPRODUCTS ${DP_MSX2_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_msx2_raw> -DOUTPUT=${DP_MSX2_ARCHIVE}
    "-DPREFIX=dp_msx2_" "-DPATTERNS=machine|sg_[A-Za-z0-9_]*|sg1000_[A-Za-z0-9_]*|tms[A-Za-z0-9_]*|msx_[A-Za-z0-9_]*|dp_msx_(info|observe|bios_[a-z_]*|init|close|cpu|read|write|in|out|advance|rom_index|ram_index|state_size|save|load|buttons|return)"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_msx2_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_library(deepeel_msx2 STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_msx2 PROPERTIES IMPORTED_LOCATION ${DP_MSX2_ARCHIVE}
  INTERFACE_LINK_LIBRARIES deepeel_z80)
add_custom_target(dp_msx2_runtime DEPENDS ${DP_MSX2_ARCHIVE})
add_dependencies(deepeel_msx2 dp_msx2_runtime)
target_link_libraries(deepeel_sega PUBLIC deepeel_sg deepeel_gg deepeel_msx deepeel_msx2 deepeel_sg2 deepeel_gg2 deepeel_z80 Threads::Threads)
target_link_libraries(deepeel PRIVATE deepeel_sega)
# --- the m0boy NES machine (6502 + PPU + APU runtime) ------------------------
# The runtime's globals (`machine`, nes_*/apu_*/ppu_*) are namespaced with
# objcopy — the MSX/Sega technique — so they cannot collide with the other
# embedded runtimes (the GG runtime exports its own `machine`).
set(DP_NES_RUNTIME "${M0BOY_DIR}/targets/nes/runtime")
if(NOT EXISTS "${DP_NES_RUNTIME}/nes_trace.h" OR NOT EXISTS "${DP_NES_RUNTIME}/6502.c")
  message(FATAL_ERROR "M0BOY_DIR must provide targets/nes/runtime with the NES_TRACE hooks")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${DP_NES_RUNTIME}/6502.c" "${DP_NES_RUNTIME}/nes.c")
file(READ "${DP_NES_RUNTIME}/6502.c" DP_NES_CPU)
file(READ "${DP_NES_RUNTIME}/nes.c" DP_NES_NES)
if(NOT DP_NES_CPU MATCHES "if \\(nes_trace\\.stop\\)")
  message(FATAL_ERROR "m0boy NES core lost the trace stop point; update deepeel's machine bridge")
endif()
if(NOT DP_NES_NES MATCHES "nes_frame_rest")
  message(FATAL_ERROR "m0boy NES frame lost the resumable entry point; update deepeel's machine bridge")
endif()
add_library(deepeel_nes_raw STATIC
  src/cpu/m6502/m6502.c
  src/cpu/m6502/m6502_isa.c
  src/mach/nesmach.c
  src/mach/nes_machine.c
  src/platform/nes/registry.c
  src/platform/nes/explore.c
  src/platform/nes/tas.c
  src/platform/nes/jobs.c
  ${DP_NES_RUNTIME}/6502.c
  ${DP_NES_RUNTIME}/nes.c
  ${DP_NES_RUNTIME}/apu.c
  ${DP_NES_RUNTIME}/ppu.c)
target_compile_definitions(deepeel_nes_raw PRIVATE NES_TRACE)
target_include_directories(deepeel_nes_raw PUBLIC src
  ${DP_NES_RUNTIME} "${M0BOY_DIR}/targets/common/runtime")
set(DP_NES_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_nes.a)
add_custom_command(OUTPUT ${DP_NES_ARCHIVE}
  BYPRODUCTS ${DP_NES_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_nes_raw> -DOUTPUT=${DP_NES_ARCHIVE}
    -DPREFIX=dp_nesrt_ -DPATTERNS=machine|nes_[A-Za-z0-9_]*|apu_[A-Za-z0-9_]*|ppu_[A-Za-z0-9_]*
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_nes_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_custom_target(dp_nes_runtime DEPENDS ${DP_NES_ARCHIVE})
add_library(deepeel_nes STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_nes PROPERTIES IMPORTED_LOCATION ${DP_NES_ARCHIVE})
add_dependencies(deepeel_nes dp_nes_runtime)
# second NES instance for --jobs 2: the whole stack compiled again, every defined global
# namespaced dp_nes2_ (the explorer and m0boy's machine are process globals)
add_library(deepeel_nes2_raw STATIC
  src/cpu/m6502/m6502.c src/mach/nesmach.c src/platform/nes/registry.c
  src/platform/nes/explore.c src/platform/nes/tas.c
  ${DP_NES_RUNTIME}/6502.c ${DP_NES_RUNTIME}/nes.c ${DP_NES_RUNTIME}/apu.c ${DP_NES_RUNTIME}/ppu.c)
target_compile_definitions(deepeel_nes2_raw PRIVATE NES_TRACE)
target_include_directories(deepeel_nes2_raw PRIVATE src ${DP_NES_RUNTIME} "${M0BOY_DIR}/targets/common/runtime")
set(DP_NES2_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_nes2.a)
add_custom_command(OUTPUT ${DP_NES2_ARCHIVE}
  BYPRODUCTS ${DP_NES2_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_nes2_raw> -DOUTPUT=${DP_NES2_ARCHIVE}
    -DPREFIX=dp_nes2_ "-DPATTERNS=[A-Za-z_][A-Za-z0-9_]*"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_nes2_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_custom_target(dp_nes2_runtime DEPENDS ${DP_NES2_ARCHIVE})
add_library(deepeel_nes2 STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_nes2 PROPERTIES IMPORTED_LOCATION ${DP_NES2_ARCHIVE})
add_dependencies(deepeel_nes2 dp_nes2_runtime)
target_link_libraries(deepeel PRIVATE deepeel_z80 deepeel_nes deepeel_nes2 deepeel_core)
set(DP_PCE_RUNTIME "${M0BOY_DIR}/targets/pce/runtime")
add_library(deepeel_pce STATIC
  src/platform/pce/hucdec.c
  src/mach/pce_machine.c
  src/mach/huc_isa.c
  src/mach/m68k_isa.c
  src/platform/pce/pcemach.c
  src/platform/pce/registry.c
  src/platform/pce/explore.c
  src/platform/pce/tas.c
  src/platform/pce/jobs.c
  ${DP_PCE_RUNTIME}/huc6280.c
  ${DP_PCE_RUNTIME}/vdc.c
  ${DP_PCE_RUNTIME}/psg.c)
target_include_directories(deepeel_pce PUBLIC src ${DP_PCE_RUNTIME}
  "${M0BOY_DIR}/targets/common/runtime")
target_compile_definitions(deepeel_pce PRIVATE PCE_TRACE)
target_link_libraries(deepeel_pce PUBLIC deepeel_core)
# second PCE instance for --jobs 2: the whole stack compiled again, every
# defined global namespaced dp_pce2_ (the explorer, the bridge and m0boy's
# machine are process globals)
add_library(deepeel_pce2_raw STATIC
  src/platform/pce/hucdec.c src/platform/pce/pcemach.c src/platform/pce/explore.c
  src/platform/pce/tas.c
  ${DP_PCE_RUNTIME}/huc6280.c ${DP_PCE_RUNTIME}/vdc.c ${DP_PCE_RUNTIME}/psg.c)
target_include_directories(deepeel_pce2_raw PRIVATE src ${DP_PCE_RUNTIME}
  "${M0BOY_DIR}/targets/common/runtime")
target_compile_definitions(deepeel_pce2_raw PRIVATE PCE_TRACE)
set(DP_PCE2_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_pce2.a)
add_custom_command(OUTPUT ${DP_PCE2_ARCHIVE}
  BYPRODUCTS ${DP_PCE2_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_pce2_raw> -DOUTPUT=${DP_PCE2_ARCHIVE}
    -DPREFIX=dp_pce2_ "-DPATTERNS=[A-Za-z_][A-Za-z0-9_]*"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_pce2_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_custom_target(dp_pce2_runtime DEPENDS ${DP_PCE2_ARCHIVE})
add_library(deepeel_pce2 STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_pce2 PROPERTIES IMPORTED_LOCATION ${DP_PCE2_ARCHIVE}
  INTERFACE_LINK_LIBRARIES deepeel_core)
add_dependencies(deepeel_pce2 dp_pce2_runtime)
target_link_libraries(deepeel_pce PUBLIC deepeel_pce2)
target_link_libraries(deepeel PRIVATE deepeel_pce)

# second MD instance for --jobs 2: the machine (generated CPU/bus/audio copies, video,
# YM2612, Z80, mdmach) and the explorer compiled again, every defined global namespaced
# dp_md2_ (m0boy's machine and the explorer state are process globals)
add_library(deepeel_md2_raw STATIC
  ${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_cpu.c ${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_bus.c
  ${DP_MD_RUNTIME}/md_full_video.c ${CMAKE_CURRENT_BINARY_DIR}/dp_md_full_audio.c
  ${DP_MD_RUNTIME}/md_ym2612.c ${DP_MD_Z80}/z80.c
  src/mach/mdmach.c src/mach/mdrom.c src/platform/md/explore.c src/explore/md_trace.c)
target_compile_definitions(deepeel_md2_raw PRIVATE MD_TRACE)
target_include_directories(deepeel_md2_raw PRIVATE src src/mach ${DP_MD_RUNTIME} ${DP_MD_Z80})
target_compile_options(deepeel_md2_raw PRIVATE -w)
set(DP_MD2_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libdeepeel_md2.a)
add_custom_command(OUTPUT ${DP_MD2_ARCHIVE}
  BYPRODUCTS ${DP_MD2_ARCHIVE}.syms
  COMMAND ${CMAKE_COMMAND} -DNM=${DP_NM} -DOBJCOPY=${DP_OBJCOPY}
    -DINPUT=$<TARGET_FILE:deepeel_md2_raw> -DOUTPUT=${DP_MD2_ARCHIVE}
    -DPREFIX=dp_md2_ "-DPATTERNS=[A-Za-z_][A-Za-z0-9_]*"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dp_sega_prefix.cmake
  DEPENDS deepeel_md2_raw cmake/dp_sega_prefix.cmake VERBATIM)
add_custom_target(dp_md2_runtime DEPENDS ${DP_MD2_ARCHIVE})
add_library(deepeel_md2 STATIC IMPORTED GLOBAL)
set_target_properties(deepeel_md2 PROPERTIES IMPORTED_LOCATION ${DP_MD2_ARCHIVE}
  INTERFACE_LINK_LIBRARIES deepeel_core)
add_dependencies(deepeel_md2 dp_md2_runtime)
target_link_libraries(deepeel_core PUBLIC deepeel_md2)

target_link_libraries(deepeel PRIVATE deepeel_core m ${DP_GUI_LIBS})

option(BUILD_TESTS "Build the test suite" ON)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SDL2_FOUND AND BUILD_GUI)
  message(STATUS "SDL2 ${SDL2_VERSION}: gui enabled")
else()
  message(STATUS "SDL2 not found: gui disabled")
endif()
