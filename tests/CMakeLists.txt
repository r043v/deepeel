# fixtures are generated at build time; no ROM ever lands in the repository
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(test_z80 test_z80.c)
target_link_libraries(test_z80 PRIVATE deepeel_z80)
add_test(NAME z80 COMMAND test_z80)
add_executable(test_provenance test_provenance.c)
target_link_libraries(test_provenance PRIVATE deepeel_z80)
add_test(NAME provenance COMMAND test_provenance)
add_executable(test_sega test_sega.c)
target_link_libraries(test_sega PRIVATE deepeel_sega)
add_test(NAME sega COMMAND test_sega)
add_executable(test_sega_copies test_sega_copies.c)
target_link_libraries(test_sega_copies PRIVATE deepeel_sega)
add_test(NAME sega_copies COMMAND test_sega_copies)
add_executable(test_sega_inputs test_sega_inputs.c)
target_link_libraries(test_sega_inputs PRIVATE deepeel_sega)
add_test(NAME sega_inputs COMMAND test_sega_inputs)

add_executable(test_sega_closure test_sega_closure.c)
target_link_libraries(test_sega_closure PRIVATE deepeel_sega)
add_test(NAME sega_closure COMMAND test_sega_closure)

add_executable(test_sega_visits test_sega_visits.c)
target_link_libraries(test_sega_visits PRIVATE deepeel_sega)
add_test(NAME sega_visits COMMAND test_sega_visits)
set_tests_properties(sega_visits PROPERTIES TIMEOUT 30)

add_executable(test_sega_mappers test_sega_mappers.c)
target_link_libraries(test_sega_mappers PRIVATE deepeel_sega)
add_test(NAME sega_mappers COMMAND test_sega_mappers)
set_tests_properties(sega_mappers PROPERTIES TIMEOUT 30)

add_executable(test_sega_hardware test_sega_hardware.c)
target_link_libraries(test_sega_hardware PRIVATE deepeel_sega)
add_test(NAME sega_hardware COMMAND test_sega_hardware)

add_executable(test_sega_seeds test_sega_seeds.c)
target_link_libraries(test_sega_seeds PRIVATE deepeel_sega)
add_test(NAME sega_seeds COMMAND test_sega_seeds)
set_tests_properties(sega_seeds PROPERTIES TIMEOUT 30)

add_executable(test_sega_sections test_sega_sections.c)
target_link_libraries(test_sega_sections PRIVATE deepeel_sega)
add_test(NAME sega_sections COMMAND test_sega_sections)
set_tests_properties(sega_sections PROPERTIES TIMEOUT 30)

add_executable(test_sega_spool test_sega_spool.c)
target_link_libraries(test_sega_spool PRIVATE deepeel_sega)
add_test(NAME sega_spool COMMAND test_sega_spool)
set_tests_properties(sega_spool PROPERTIES TIMEOUT 30)

add_executable(test_sega_indirect test_sega_indirect.c)
target_link_libraries(test_sega_indirect PRIVATE deepeel_sega)
add_test(NAME sega_indirect COMMAND test_sega_indirect)
set_tests_properties(sega_indirect PROPERTIES TIMEOUT 30)

add_executable(test_sega_play test_sega_play.c)
target_link_libraries(test_sega_play PRIVATE deepeel_sega)
add_test(NAME sega_play COMMAND test_sega_play)
set_tests_properties(sega_play PROPERTIES TIMEOUT 30)

add_executable(test_sega_tas test_sega_tas.c)
target_link_libraries(test_sega_tas PRIVATE deepeel_sega)
add_test(NAME sega_tas COMMAND test_sega_tas)
set_tests_properties(sega_tas PROPERTIES TIMEOUT 30)

add_executable(test_sega_jobs test_sega_jobs.c)
target_link_libraries(test_sega_jobs PRIVATE deepeel_sega)
add_test(NAME sega_jobs COMMAND test_sega_jobs)
set_tests_properties(sega_jobs PROPERTIES TIMEOUT 30)

add_executable(test_nes_cpu test_nes_cpu.c)
target_link_libraries(test_nes_cpu PRIVATE deepeel_nes)
add_test(NAME nes_cpu COMMAND test_nes_cpu)

add_executable(test_nes test_nes.c)
target_link_libraries(test_nes PRIVATE deepeel_nes deepeel_core)
add_test(NAME nes COMMAND test_nes)
set_tests_properties(nes PROPERTIES TIMEOUT 60)

add_executable(test_msx test_msx.c)
target_link_libraries(test_msx PRIVATE deepeel_msx deepeel_sega)
add_test(NAME msx COMMAND test_msx)

add_executable(test_z80_blocks test_z80_blocks.c)
target_link_libraries(test_z80_blocks PRIVATE deepeel_sega)
add_test(NAME z80_blocks COMMAND test_z80_blocks)
set_tests_properties(z80_blocks PROPERTIES TIMEOUT 30)

add_executable(test_z80_static_data test_z80_static_data.c)
target_link_libraries(test_z80_static_data PRIVATE deepeel_sega)
add_test(NAME z80_static_data COMMAND test_z80_static_data)
set_tests_properties(z80_static_data PROPERTIES TIMEOUT 30)

add_executable(test_z80_stores test_z80_stores.c)
target_link_libraries(test_z80_stores PRIVATE deepeel_sega)
add_test(NAME z80_stores COMMAND test_z80_stores)
set_tests_properties(z80_stores PROPERTIES TIMEOUT 30)

add_executable(test_z80_targets test_z80_targets.c)
target_link_libraries(test_z80_targets PRIVATE deepeel_sega)
add_test(NAME z80_targets COMMAND test_z80_targets)
set_tests_properties(z80_targets PROPERTIES TIMEOUT 30)

add_executable(test_msx_closure test_msx_closure.c)
target_link_libraries(test_msx_closure PRIVATE deepeel_sega)
add_test(NAME msx_closure COMMAND test_msx_closure)
set_tests_properties(msx_closure PROPERTIES TIMEOUT 30)

add_executable(test_msx_explore test_msx_explore.c)
target_link_libraries(test_msx_explore PRIVATE deepeel_sega)
add_test(NAME msx_explore COMMAND test_msx_explore)
set_tests_properties(msx_explore PROPERTIES TIMEOUT 30)

add_executable(test_msx_bios test_msx_bios.c)
target_link_libraries(test_msx_bios PRIVATE deepeel_msx)
add_test(NAME msx_bios COMMAND test_msx_bios)
set_tests_properties(msx msx_bios PROPERTIES TIMEOUT 30)

# Differential check: instrumented BIOS and untouched upstream BIOS must
# produce the same CPU, memory and device state, in separate executables.
add_library(msx_bios_native STATIC
  ${M0BOY_DIR}/targets/sg1000/runtime/sg1000.c
  ${M0BOY_DIR}/targets/sg1000/runtime/tms9918.c
  ${M0BOY_DIR}/targets/msx/runtime/msx.c
  ${M0BOY_DIR}/targets/msx/runtime/msx_ay.c
  ${CMAKE_SOURCE_DIR}/src/platform/msx/backend.c)
target_compile_definitions(msx_bios_native PRIVATE SG_MSX=1 SG_MSX_RAM=65536)
target_include_directories(msx_bios_native PRIVATE
  ${M0BOY_DIR}/targets/sg1000/runtime ${M0BOY_DIR}/targets/common/runtime)
target_link_libraries(msx_bios_native PUBLIC deepeel_z80)
add_executable(msx_bios_reference msx_bios_digest.c)
target_link_libraries(msx_bios_reference PRIVATE msx_bios_native)
add_executable(msx_bios_instrumented msx_bios_digest.c)
target_link_libraries(msx_bios_instrumented PRIVATE deepeel_msx)
add_test(NAME msx_bios_equivalence COMMAND ${CMAKE_COMMAND}
  -DNATIVE=$<TARGET_FILE:msx_bios_reference> -DTRACED=$<TARGET_FILE:msx_bios_instrumented>
  -P ${CMAKE_CURRENT_SOURCE_DIR}/msx_bios_compare.cmake)
set_tests_properties(msx_bios_equivalence PROPERTIES TIMEOUT 30)

set(FIXTURES
  ${CMAKE_CURRENT_BINARY_DIR}/fix_sega_mapper.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_branch.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_mbc5.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_dma.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_irq.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_jumptable.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_loop.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_unreach.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_closure.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_bankdata.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_vblank.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_copy1.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_hdma.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_copyx.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsecho.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsdata.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsstack.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_dma.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_z80.md
)

add_custom_command(
  OUTPUT ${FIXTURES}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/build_fixtures.py ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/build_fixtures.py
  COMMENT "building synthetic ROM fixtures"
)

add_custom_target(fixtures ALL DEPENDS ${FIXTURES})

add_test(NAME sega_mapper_cli COMMAND ${CMAKE_COMMAND}
  -DCLI=$<TARGET_FILE:deepeel> -DROM=${CMAKE_CURRENT_BINARY_DIR}/fix_sega_mapper.sms
  -P ${CMAKE_CURRENT_SOURCE_DIR}/sega_mapper_cli.cmake)
set_tests_properties(sega_mapper_cli PROPERTIES TIMEOUT 30)


add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE deepeel_core m)
add_test(NAME core COMMAND test_core)

add_executable(test_exec test_exec.c)
target_link_libraries(test_exec PRIVATE deepeel_core m)
add_test(NAME exec COMMAND test_exec)

add_executable(test_explorer test_explorer.c)
target_link_libraries(test_explorer PRIVATE deepeel_core m)
add_dependencies(test_explorer fixtures)
add_test(NAME explorer COMMAND test_explorer ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_sessions test_sessions.c)
target_link_libraries(test_sessions PRIVATE deepeel_core m)
add_dependencies(test_sessions fixtures)
add_test(NAME sessions COMMAND test_sessions ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_md test_md.c)
target_link_libraries(test_md PRIVATE deepeel_core m)
add_dependencies(test_md fixtures)
add_test(NAME md COMMAND test_md ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_md_explore test_md_explore.c)
target_link_libraries(test_md_explore PRIVATE deepeel_core m)
add_dependencies(test_md_explore fixtures)
add_test(NAME md_explore COMMAND test_md_explore ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_export test_export.c)
target_link_libraries(test_export PRIVATE deepeel_core m)
add_test(NAME export COMMAND test_export)

if(TARGET deepeel AND DP_HAS_GUI)
  add_executable(test_gui test_gui.c)
  target_link_libraries(test_gui PRIVATE deepeel_core m ${DP_GUI_LIBS})
  target_include_directories(test_gui PRIVATE ${SDL2_INCLUDE_DIRS})
  add_dependencies(test_gui fixtures)
  add_test(NAME gui COMMAND test_gui ${CMAKE_CURRENT_BINARY_DIR})
endif()

add_executable(test_pce test_pce.c)
target_link_libraries(test_pce PRIVATE deepeel_pce deepeel_core)
add_test(NAME pce COMMAND test_pce)
set_tests_properties(pce PROPERTIES TIMEOUT 60)
add_executable(test_coverage test_coverage.c)
target_link_libraries(test_coverage PRIVATE deepeel_core m)
add_test(NAME coverage COMMAND test_coverage)
add_executable(test_sega_coverage test_sega_coverage.c)
target_link_libraries(test_sega_coverage PRIVATE deepeel_sega)
add_test(NAME sega_coverage COMMAND test_sega_coverage)
add_test(NAME frontier_cli COMMAND ${CMAKE_COMMAND} -E env SEGA_ROM=${CMAKE_CURRENT_BINARY_DIR}/fix_sega_mapper.sms sh ${CMAKE_CURRENT_SOURCE_DIR}/frontier_cli.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_branch.gb)

add_executable(test_dpmachine test_dpmachine.c)
target_link_libraries(test_dpmachine PRIVATE deepeel_core m)
add_dependencies(test_dpmachine fixtures)
add_test(NAME dpmachine COMMAND test_dpmachine ${CMAKE_CURRENT_BINARY_DIR})
