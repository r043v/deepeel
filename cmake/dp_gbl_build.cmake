# Build-time companion of the M0BOY_DIR wiring (invoked with cmake -P):
# compile the m0boy GB runtime once per cartridge-shape variant, clone each
# variant into DP_MACHINES prefixed instances (objcopy --redefine-syms from
# the objects' own symbol list, libc calls untouched), archive everything.
#
# In: DP_GBL_SRC DP_GBL_OUT CMAKE_C_COMPILER BASE_FLAGS DP_GBL_VARIANTS
#     DP_MACHINES
# Out: ${DP_GBL_OUT}/libgblmach.a

set(SOURCES gbl_core.c gbl_cpu.c gbl_ppu.c gbl_apu.c)
file(MAKE_DIRECTORY ${DP_GBL_OUT})
# the variant list travels comma-separated (semicolons cannot survive the
# shell ninja runs the command through)
string(REPLACE "," ";" DP_GBL_VARIANTS "${DP_GBL_VARIANTS}")

list(LENGTH DP_GBL_VARIANTS nv)
math(EXPR nv "${nv} - 1")
set(ALL_OBJS "")

foreach(v RANGE ${nv})
  list(GET DP_GBL_VARIANTS ${v} spec)
  string(REPLACE ":" ";" kv "${spec}")
  list(GET kv 0 mbc)
  list(GET kv 1 eram)
  set(cfg ${DP_GBL_OUT}/cfg${v})
  file(MAKE_DIRECTORY ${cfg})
  file(WRITE ${cfg}/gbl_config.h "\
#ifndef GBL_CONFIG_H
#define GBL_CONFIG_H
#define GBL_MBC ${mbc}
#define GBL_ROM_BANKS 512
#define GBL_ERAM_SIZE ${eram}
#define GBL_ERAM_ON_SD 0
#define GBL_BATTERY 0
#define GBL_ROM_STREAM 0
#define GBL_ROM_PACKED 0
#define GBL_TITLE \"DEEPEEL\"
#endif
")
  set(vobjs "")
  foreach(src ${SOURCES})
    string(REPLACE ".c" "" base "${src}")
    set(obj ${DP_GBL_OUT}/v${v}_${base}.o)
    # the interpreter is the only consumer of gbl_rd/gbl_wr: rename them
    # there so every CPU load/store is observed (the definitions in
    # gbl_core.c keep their names, the forwarders call them).
    set(hooks "")
    if(src STREQUAL "gbl_cpu.c")
      set(hooks -Dgbl_rd=dp_gbl_rd -Dgbl_wr=dp_gbl_wr)
    endif()
    execute_process(
      COMMAND ${CMAKE_C_COMPILER} -c -O2 -w -fno-stack-protector
        -I${DP_GBL_SRC} -I${cfg}
        -DGBL_COVERAGE -DGBL_TRACE -DGBL_VRAM_HOOK=1 -DGBL_ACCESS_BLOCK=1
        ${hooks}
        ${BASE_FLAGS}
        ${DP_GBL_SRC}/${src} -o ${obj}
      RESULT_VARIABLE r)
    if(NOT r EQUAL 0)
      message(FATAL_ERROR "dp_gbl: compile failed (${src}, variant ${v} = MBC${mbc}/${eram})")
    endif()
    list(APPEND vobjs ${obj})
  endforeach()

  # every gbl_*/dp_gbl_*/gbr symbol (defined here or cross-object), leaving
  # libc alone: those are the names the instance prefix must rewrite.
  string(JOIN " " vobjs_sp ${vobjs})
  set(syms ${DP_GBL_OUT}/v${v}_syms.txt)
  execute_process(
    COMMAND sh -c "nm --defined-only ${vobjs_sp} | awk '\$2~/^[TDRBWVG]\$/{print \$3}'; nm -u ${vobjs_sp} | awk '{print \$2}'"
    COMMAND grep -E "^(gbl_|dp_gbl_)|^gbr$"
    COMMAND sort -u
    OUTPUT_FILE ${syms}
    RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "dp_gbl: symbol collection failed (variant ${v})")
  endif()

  math(EXPR last "${DP_MACHINES} - 1")
  foreach(i RANGE ${last})
    set(pfx v${v}m${i}_)
    set(redef ${DP_GBL_OUT}/v${v}m${i}_redef.txt)
    execute_process(
      COMMAND awk -v p=${pfx} "{print \$1 \" \" p \$1}" ${syms}
      OUTPUT_FILE ${redef})
    foreach(obj ${vobjs})
      get_filename_component(base ${obj} NAME)
      string(REPLACE "v${v}_" "" base "${base}")
      set(out ${DP_GBL_OUT}/${pfx}${base})
      execute_process(COMMAND objcopy --redefine-syms=${redef} ${obj} ${out})
      list(APPEND ALL_OBJS ${out})
    endforeach()
  endforeach()
endforeach()

execute_process(COMMAND ar rcs ${DP_GBL_OUT}/libgblmach.a ${ALL_OBJS} RESULT_VARIABLE r)
if(NOT r EQUAL 0)
  message(FATAL_ERROR "dp_gbl: ar failed")
endif()
