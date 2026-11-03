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
  ${CMAKE_CURRENT_BINARY_DIR}/fix_apu.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pal.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_probes.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_copyx.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsecho.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsdata.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsstack.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_dma.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_z80.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_uxrom.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_pad.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ind.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_static.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_table.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_slide.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_sibling.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_prov.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dispatch.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_cpucopy.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.gg
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sg
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.msx
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

add_executable(test_map_png test_map_png.c)
target_link_libraries(test_map_png PRIVATE deepeel_core m)
add_test(NAME map_png COMMAND test_map_png)
add_test(NAME maprender_cv COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/maprender_cv.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_copy1.gb)

# J11: level-A libretro backend, tested on a mock core built here (no binary committed)
add_library(dpmock_libretro SHARED mock_libretro.c)
set_target_properties(dpmock_libretro PROPERTIES PREFIX "" OUTPUT_NAME "dpmock_libretro")
add_executable(test_libretro test_libretro.c)
target_link_libraries(test_libretro PRIVATE deepeel_libretro ${CMAKE_DL_LIBS})
add_test(NAME libretro COMMAND test_libretro $<TARGET_FILE:dpmock_libretro> ${CMAKE_CURRENT_BINARY_DIR}/mock_rom.bin)
add_test(NAME lr_explore_cli COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/lr_explore_cli.sh $<TARGET_FILE:deepeel> $<TARGET_FILE:dpmock_libretro> ${CMAKE_CURRENT_BINARY_DIR}/mock_rom.bin)
add_executable(test_lr_parity test_lr_parity.c)
target_link_libraries(test_lr_parity PRIVATE deepeel_core m)
add_dependencies(test_lr_parity fixtures)
add_test(NAME lr_parity COMMAND test_lr_parity ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME campaign_fixtures COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/campaign_fixtures.py ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/campaign.md)
set_tests_properties(campaign_fixtures PROPERTIES TIMEOUT 300)
add_test(NAME audit_matrix COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/audit_matrix.py ${CMAKE_SOURCE_DIR}/docs/COUVERTURE.fr.md ${CMAKE_CURRENT_BINARY_DIR}/audit.md)
add_test(NAME nes_entry COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --max-total 200000 --entry 0:8150 --entry 3:8000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_entry.txt >/dev/null 2>&1; grep -q 'entries: seeded 1 refused 1' ${CMAKE_CURRENT_BINARY_DIR}/nes_entry.txt")
set_tests_properties(nes_entry PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_seed_banks COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_uxrom.nes --max-total 200000 --max-steps 1000 --seed-banks --text ${CMAKE_CURRENT_BINARY_DIR}/nes_sb.txt >/dev/null 2>&1; grep -q 'entries: seeded 4 refused 0' ${CMAKE_CURRENT_BINARY_DIR}/nes_sb.txt && grep -q 'sections 4:' ${CMAKE_CURRENT_BINARY_DIR}/nes_sb.txt")
set_tests_properties(nes_seed_banks PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_pad_probe COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_pad.nes --max-total 200000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_pp.txt >/dev/null 2>&1; grep -q 'pad probes: 16 forks at 2 sites' ${CMAKE_CURRENT_BINARY_DIR}/nes_pp.txt")
set_tests_properties(nes_pad_probe PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_indirect COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ind.nes --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_ind.txt >/dev/null 2>&1; grep -q 'indirect jumps: 1 sites, 1 targets, overflow 0' ${CMAKE_CURRENT_BINARY_DIR}/nes_ind.txt")
set_tests_properties(nes_indirect PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_cpu_copy COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_cpucopy.nes --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_cc.txt >/dev/null 2>&1; grep -q 'ch3 src .* len 16' ${CMAKE_CURRENT_BINARY_DIR}/nes_cc.txt")
set_tests_properties(nes_cpu_copy PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_padding COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_pad.txt --map ${CMAKE_CURRENT_BINARY_DIR}/nes_pad.map >/dev/null 2>&1; grep -Eq 'padding [1-9][0-9]* bytes' ${CMAKE_CURRENT_BINARY_DIR}/nes_pad.txt && sed -n 2p ${CMAKE_CURRENT_BINARY_DIR}/nes_pad.map | grep -q 8")
set_tests_properties(nes_padding PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_static COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_static.nes --max-total 20000 --max-steps 2 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_st.txt --map ${CMAKE_CURRENT_BINARY_DIR}/nes_st.map >/dev/null 2>&1; grep -q 'static closure: 7 bytes from 1 roots' ${CMAKE_CURRENT_BINARY_DIR}/nes_st.txt && sed -n 2p ${CMAKE_CURRENT_BINARY_DIR}/nes_st.map | grep -q 3")
set_tests_properties(nes_static PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_table COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_table.nes --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_tb.txt --map ${CMAKE_CURRENT_BINARY_DIR}/nes_tb.map >/dev/null 2>&1; grep -q 'pointer tables: 1 tables, 6 bytes' ${CMAKE_CURRENT_BINARY_DIR}/nes_tb.txt && sed -n 2p ${CMAKE_CURRENT_BINARY_DIR}/nes_tb.map | grep -q 7")
set_tests_properties(nes_table PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_slide COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_slide.nes --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_sl.txt >/dev/null 2>&1; grep -Eq 'padding cuts: [1-9]' ${CMAKE_CURRENT_BINARY_DIR}/nes_sl.txt")
set_tests_properties(nes_slide PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_ioreg COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_pad.nes --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_io.txt >/dev/null 2>&1; grep -Eq 'reg .4016 reads [1-9][0-9]* writes [1-9][0-9]*' ${CMAKE_CURRENT_BINARY_DIR}/nes_io.txt")
set_tests_properties(nes_ioreg PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_seed_vectors COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_uxrom.nes --seed-vectors --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_sv.txt >/dev/null 2>&1; grep -Eq 'vector seeds: 2' ${CMAKE_CURRENT_BINARY_DIR}/nes_sv.txt")
set_tests_properties(nes_seed_vectors PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_evidence COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --max-total 20000 --max-steps 1000 --evidence ${CMAKE_CURRENT_BINARY_DIR}/nes_ev.txt >/dev/null 2>&1; grep -Eq '^0000 0000 0002 09$' ${CMAKE_CURRENT_BINARY_DIR}/nes_ev.txt")
set_tests_properties(nes_evidence PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_cov COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --max-total 20000 --max-steps 1000 --cov ${CMAKE_CURRENT_BINARY_DIR}/nes.cov --romdata ${CMAKE_CURRENT_BINARY_DIR}/nes.rd >/dev/null 2>&1; grep -Eq '^00 0000 1 1$' ${CMAKE_CURRENT_BINARY_DIR}/nes.cov && grep -Eq '^[0-9a-f]+ [1-9][0-9]*$' ${CMAKE_CURRENT_BINARY_DIR}/nes.rd")
set_tests_properties(nes_cov PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_sibling COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_sibling.nes --max-total 20000 --max-steps 2000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_sib.txt >/dev/null 2>&1; grep -Eq 'copy siblings: 1 tables; 2 ranges .32 bytes.' ${CMAKE_CURRENT_BINARY_DIR}/nes_sib.txt")
set_tests_properties(nes_sibling PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_second_pass COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_static.nes --second-pass --max-total 20000 --max-steps 2 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_p2.txt >/dev/null 2>&1; grep -Eq 'second pass: 1 seeds, 0 refused' ${CMAKE_CURRENT_BINARY_DIR}/nes_p2.txt && grep -Eq 'static closure: 0 bytes from 0 roots' ${CMAKE_CURRENT_BINARY_DIR}/nes_p2.txt")
set_tests_properties(nes_second_pass PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_spool_resume COMMAND sh -c "rm -f ${CMAKE_CURRENT_BINARY_DIR}/nes.lot; $<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --spool ${CMAKE_CURRENT_BINARY_DIR}/nes.lot --max-total 3 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_sp1.txt >/dev/null 2>&1; grep -Eq '1 left on disk' ${CMAKE_CURRENT_BINARY_DIR}/nes_sp1.txt && $<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --resume ${CMAKE_CURRENT_BINARY_DIR}/nes.lot --max-total 20000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_sp2.txt >/dev/null 2>&1; grep -Eq '1 read back, 0 left on disk' ${CMAKE_CURRENT_BINARY_DIR}/nes_sp2.txt")
set_tests_properties(nes_spool_resume PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_tree COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --max-total 20000 --max-steps 1000 --tree ${CMAKE_CURRENT_BINARY_DIR}/nes.tree --text ${CMAKE_CURRENT_BINARY_DIR}/nes_tr.txt >/dev/null 2>&1; grep -Eq 'choice tree: 5 nodes' ${CMAKE_CURRENT_BINARY_DIR}/nes_tr.txt && test \"$(head -c 4 ${CMAKE_CURRENT_BINARY_DIR}/nes.tree)\" = DPTZ && test $(wc -c < ${CMAKE_CURRENT_BINARY_DIR}/nes.tree) -eq 72")
set_tests_properties(nes_tree PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_provenance COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_prov.nes --second-pass --max-total 20000 --max-steps 50 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_pv.txt >/dev/null 2>&1; grep -Eq 'provenance: 1 jump-table words traced from PRG reads, 2 roots seeded' ${CMAKE_CURRENT_BINARY_DIR}/nes_pv.txt")
set_tests_properties(nes_provenance PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_bank_dispatch COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dispatch.nes --bank-dispatch --max-total 20000 --max-steps 50 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_bd.txt >/dev/null 2>&1; grep -Eq 'bank dispatch: 1 tables, 7 seeds, 5 bank views refused' ${CMAKE_CURRENT_BINARY_DIR}/nes_bd.txt")
set_tests_properties(nes_bank_dispatch PROPERTIES FIXTURES_REQUIRED fixtures)
