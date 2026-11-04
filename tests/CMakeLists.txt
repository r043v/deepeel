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
  ${CMAKE_CURRENT_BINARY_DIR}/fix_sgb.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_smc.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_mbc5.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_mbc1m1.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_dma.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_irq.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_jumptable.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_loop.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_unreach.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_closure.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_bankdata.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_vblank.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_copy1.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_wramexec.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_wramgen.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ym2413.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_lywait.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_hdma.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_cgbbank.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_apu.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pal.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_probes.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_copyx.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsecho.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsdata.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsstack.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_dma.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4j.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4a.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4w.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4p.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_pad6.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_longloop.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_jptable_de.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_provde.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ramgen.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ramexg.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ramsrc.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_z80.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_adpcm.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_sgx.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_slide.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_ctx.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_sibling.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_ind.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_prov.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_uxrom.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_pad.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ind.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_static.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_table.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_slide.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_sibling.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_prov.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dispatch.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ctx.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_jam.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_cpucopy.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.gg
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sg
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.msx
  ${CMAKE_CURRENT_BINARY_DIR}/fix_msx_v9938.msx
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

add_executable(gbconform gbconform.c)
target_link_libraries(gbconform PRIVATE deepeel_core m)
add_executable(gbshot gbshot.c)
target_link_libraries(gbshot PRIVATE deepeel_core m)
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
add_test(NAME lr_play_cli COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/lr_play_cli.sh $<TARGET_FILE:deepeel> $<TARGET_FILE:dpmock_libretro>)
add_test(NAME mapwin_cli COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mapwin_cli.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_bankdata.gb)
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
add_test(NAME nes_pad_probe COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_pad.nes --max-total 200000 --max-steps 1000 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_pp.txt >/dev/null 2>&1; grep -q 'pad probes: 38 forks at 2 sites' ${CMAKE_CURRENT_BINARY_DIR}/nes_pp.txt")
set_tests_properties(nes_pad_probe PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_pad_full COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_pad.nes --max-total 400000 --max-steps 1000 --pad-full --text ${CMAKE_CURRENT_BINARY_DIR}/nes_pf.txt >/dev/null 2>&1; grep -q 'pad probes: 512 forks at 2 sites' ${CMAKE_CURRENT_BINARY_DIR}/nes_pf.txt")
set_tests_properties(nes_pad_full PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_pad_full COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_pad6.md --pad-full --max-total 3000000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_pf.txt >/dev/null 2>&1; grep -q 'pad probes: 258 ' ${CMAKE_CURRENT_BINARY_DIR}/md_pf.txt")
set_tests_properties(md_pad_full PROPERTIES FIXTURES_REQUIRED fixtures)
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
add_test(NAME nes_ctx_cap COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ctx.nes; $B nes explore $F --ctx-cap 3 --max-total 5000 --max-steps 40 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_cc1.txt >/dev/null 2>&1; $B nes explore $F --max-total 5000 --max-steps 40 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_cc0.txt >/dev/null 2>&1; grep -q 'context eviction: [1-9]' ${CMAKE_CURRENT_BINARY_DIR}/nes_cc1.txt && ! grep -q 'context eviction' ${CMAKE_CURRENT_BINARY_DIR}/nes_cc0.txt")
set_tests_properties(nes_ctx_cap PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_jam COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_jam.nes --max-total 5000 --max-steps 40 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_jam.txt >/dev/null 2>&1; grep -Eq 'faults 1 ' ${CMAKE_CURRENT_BINARY_DIR}/nes_jam.txt")
set_tests_properties(nes_jam PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_evidence COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_ev.txt --evidence ${CMAKE_CURRENT_BINARY_DIR}/pce_ev.ev >/dev/null 2>&1; grep -q '^0000 0018 001A 02' ${CMAKE_CURRENT_BINARY_DIR}/pce_ev.ev")
set_tests_properties(pce_evidence PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_slide COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_slide.pce --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_sl.txt >/dev/null 2>&1; grep -Eq 'padding cuts: [1-9]' ${CMAKE_CURRENT_BINARY_DIR}/pce_sl.txt")
set_tests_properties(pce_slide PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_entry COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce --entry 0xE018 --entry 0xE025 --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_en.txt >/dev/null 2>&1; grep -q 'entries: 2 seeded, 0 refused' ${CMAKE_CURRENT_BINARY_DIR}/pce_en.txt")
set_tests_properties(pce_entry PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_ctx_cap COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_ctx.pce; $B pce explore $F --ctx-cap 3 --max-total 5000 --max-steps 60 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_cc1.txt >/dev/null 2>&1; $B pce explore $F --max-total 5000 --max-steps 60 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_cc0.txt >/dev/null 2>&1; grep -q 'context eviction: [1-9]' ${CMAKE_CURRENT_BINARY_DIR}/pce_cc1.txt && ! grep -q 'context eviction' ${CMAKE_CURRENT_BINARY_DIR}/pce_cc0.txt")
set_tests_properties(pce_ctx_cap PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_entry COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_md_loop.md; $B md explore $F --no-cut-padding --entry 0x2f0 --entry 0x1 --max-total 200000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_en1.txt >/dev/null 2>&1; $B md explore $F --no-cut-padding --max-total 200000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_en0.txt >/dev/null 2>&1; grep -q 'entries: 1 seeded, 1 refused' ${CMAKE_CURRENT_BINARY_DIR}/md_en1.txt && grep -q 'only via forks: 0;' ${CMAKE_CURRENT_BINARY_DIR}/md_en0.txt && ! grep -q 'only via forks: 0;' ${CMAKE_CURRENT_BINARY_DIR}/md_en1.txt")
set_tests_properties(md_entry PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_sibling COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_sibling.pce --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_sib.txt >/dev/null 2>&1; grep -q 'copy siblings: 1 tables, 2 entries, 64 bytes typed' ${CMAKE_CURRENT_BINARY_DIR}/pce_sib.txt")
set_tests_properties(pce_sibling PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_indirect COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_ind.pce --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_ind.txt >/dev/null 2>&1; grep -q 'indirect jumps: 2 sites, 2 targets, overflow 0' ${CMAKE_CURRENT_BINARY_DIR}/pce_ind.txt")
set_tests_properties(pce_indirect PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_provenance COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_prov.pce --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_prov.txt >/dev/null 2>&1; grep -q 'provenance: 1 jump-table words' ${CMAKE_CURRENT_BINARY_DIR}/pce_prov.txt")
set_tests_properties(pce_provenance PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_indirect COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ind.md --max-total 200000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_ind.txt >/dev/null 2>&1; grep -q 'indirect jumps: 1 sites, 1 targets, overflow 0' ${CMAKE_CURRENT_BINARY_DIR}/md_ind.txt")
set_tests_properties(md_indirect PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_padding COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_zeros.md --max-total 200000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_pad.txt >/dev/null 2>&1; grep -q 'padding cuts: 1$' ${CMAKE_CURRENT_BINARY_DIR}/md_pad.txt")
set_tests_properties(md_padding PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_sibling COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_sib.md --max-total 200000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_sib.txt >/dev/null 2>&1; grep -q 'copy siblings: 1 tables, 2 entries, 64 bytes, 0 table caps' ${CMAKE_CURRENT_BINARY_DIR}/md_sib.txt")
set_tests_properties(md_sibling PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_tree COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce --max-total 20000 --max-steps 200 --tree ${CMAKE_CURRENT_BINARY_DIR}/pce_tree.bin --text ${CMAKE_CURRENT_BINARY_DIR}/pce_tree.txt >/dev/null 2>&1; grep -q 'choice tree: 5 nodes' ${CMAKE_CURRENT_BINARY_DIR}/pce_tree.txt && test -s ${CMAKE_CURRENT_BINARY_DIR}/pce_tree.bin")
set_tests_properties(pce_tree PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_tree COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_dbcc.md --max-total 100000 --tree ${CMAKE_CURRENT_BINARY_DIR}/md_tree.bin --text ${CMAKE_CURRENT_BINARY_DIR}/md_tree.txt >/dev/null 2>&1; grep -q 'choice tree: 3 nodes' ${CMAKE_CURRENT_BINARY_DIR}/md_tree.txt && test -s ${CMAKE_CURRENT_BINARY_DIR}/md_tree.bin")
set_tests_properties(md_tree PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_ctx_cap COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_md_pad.md; $B md explore $F --max-total 100000 --ctx-cap 1 --text ${CMAKE_CURRENT_BINARY_DIR}/md_cc1.txt >/dev/null 2>&1; $B md explore $F --max-total 100000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_cc0.txt >/dev/null 2>&1; grep -q 'context eviction' ${CMAKE_CURRENT_BINARY_DIR}/md_cc1.txt && grep -q 'in 2 paths' ${CMAKE_CURRENT_BINARY_DIR}/md_cc1.txt && grep -q 'in 15 paths' ${CMAKE_CURRENT_BINARY_DIR}/md_cc0.txt && ! grep -q 'context eviction' ${CMAKE_CURRENT_BINARY_DIR}/md_cc0.txt")
set_tests_properties(md_ctx_cap PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_second_pass COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_slide.pce; $B pce explore $F --max-total 20000 --max-steps 200 --second-pass --text ${CMAKE_CURRENT_BINARY_DIR}/pce_p2.txt >/dev/null 2>&1; $B pce explore $F --max-total 20000 --max-steps 200 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_p0.txt >/dev/null 2>&1; grep -q 'second pass: 1 seeds, 0 refused' ${CMAKE_CURRENT_BINARY_DIR}/pce_p2.txt && ! grep -q 'second pass' ${CMAKE_CURRENT_BINARY_DIR}/pce_p0.txt")
set_tests_properties(pce_second_pass PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_second_pass COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_md_fork_fault.md; $B md explore $F --no-cut-padding --max-total 100000 --second-pass --text ${CMAKE_CURRENT_BINARY_DIR}/md_p2.txt >/dev/null 2>&1; $B md explore $F --no-cut-padding --max-total 100000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_p0.txt >/dev/null 2>&1; grep -q 'second pass: 1 seeds, 0 refused' ${CMAKE_CURRENT_BINARY_DIR}/md_p2.txt && ! grep -q 'second pass' ${CMAKE_CURRENT_BINARY_DIR}/md_p0.txt")
set_tests_properties(md_second_pass PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_bank_dispatch COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_disp.pce; $B pce explore $F --max-total 20000 --max-steps 100 --bank-dispatch --text ${CMAKE_CURRENT_BINARY_DIR}/pce_d1.txt >/dev/null 2>&1; $B pce explore $F --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_d0.txt >/dev/null 2>&1; grep -q 'bank dispatch: 1 tables, 3 seeds, 3 refused views' ${CMAKE_CURRENT_BINARY_DIR}/pce_d1.txt && ! grep -q 'bank dispatch' ${CMAKE_CURRENT_BINARY_DIR}/pce_d0.txt")
set_tests_properties(pce_bank_dispatch PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_dispatch COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_md_disp.md; $B md explore $F --max-total 100000 --bank-dispatch --text ${CMAKE_CURRENT_BINARY_DIR}/md_d1.txt >/dev/null 2>&1; $B md explore $F --max-total 100000 --text ${CMAKE_CURRENT_BINARY_DIR}/md_d0.txt >/dev/null 2>&1; grep -q 'dispatch tables: 1 tables, 3 seeds, 0 refused' ${CMAKE_CURRENT_BINARY_DIR}/md_d1.txt && ! grep -q 'dispatch tables' ${CMAKE_CURRENT_BINARY_DIR}/md_d0.txt")
set_tests_properties(md_dispatch PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_jobs COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_md_disp.md; D=${CMAKE_CURRENT_BINARY_DIR}; E='--entry 0x300 --entry 0x310 --entry 0x320'; $B md explore $F --max-total 100000 $E --jobs 2 --text $D/md_j2.txt >/dev/null 2>&1; $B md explore $F --max-total 100000 $E --text $D/md_j1.txt >/dev/null 2>&1; grep -q 'jobs: 1 secondary instance folded' $D/md_j2.txt && ! grep -q 'jobs:' $D/md_j1.txt && [ \"$(grep -v 'jobs:' $D/md_j2.txt)\" = \"$(cat $D/md_j1.txt)\" ]")
set_tests_properties(md_jobs PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_jobs COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes; D=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $F --play 20 --sessions 3 --jobs 2 --text $D/nes_j2.txt >/dev/null 2>&1; $B nes explore $F --play 20 --sessions 3 --text $D/nes_j1.txt >/dev/null 2>&1; grep -q 'jobs: 1 secondary instance folded' $D/nes_j2.txt && ! grep -q 'jobs:' $D/nes_j1.txt && [ \"$(grep -v 'jobs:' $D/nes_j2.txt | head -31)\" = \"$(head -31 $D/nes_j1.txt)\" ]")
set_tests_properties(nes_jobs PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME nes_novelty COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes; D=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $F --novelty 300000 --novelty-frames 30 --novelty-cells --text $D/nes_nv1.txt >/dev/null 2>&1; $B nes explore $F --text $D/nes_nv0.txt >/dev/null 2>&1; grep -q 'novelty: depth [1-9][0-9]* frames, [1-9][0-9]* trials' $D/nes_nv1.txt && grep -q '[1-9][0-9]* hits' $D/nes_nv1.txt && ! grep -q 'novelty:' $D/nes_nv0.txt")
set_tests_properties(nes_novelty PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME md_novelty COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_sram_overlay.md --novelty 300000 --novelty-frames 30 --novelty-cells --text $D/md_nv1.txt >/dev/null 2>&1; $B md explore $D/fix_md_fork.md --novelty 300000 --novelty-frames 30 --text $D/md_nv2.txt >/dev/null 2>&1; $B md explore $D/fix_md_fork.md --text $D/md_nv0.txt >/dev/null 2>&1; grep -q 'novelty: depth [1-9][0-9]* frames, [1-9][0-9]* trials' $D/md_nv1.txt && grep -q '1 stalled' $D/md_nv2.txt && ! grep -q 'novelty:' $D/md_nv0.txt")
set_tests_properties(md_novelty PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME md_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/md_w.ppm; $B md explore $D/fix_md_sram_overlay.md --watch-dump $D/md_w.ppm --text $D/md_w.txt >/dev/null 2>&1; grep -q 'watch: [1-9][0-9]* frames published' $D/md_w.txt && head -c 15 $D/md_w.ppm | grep -q '^P6' && [ $(wc -c < $D/md_w.ppm) -gt 200000 ]")
set_tests_properties(md_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME nes_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/nes_w.ppm; $B nes explore $D/fix_nes_picture.nes --watch-dump $D/nes_w.ppm --text $D/nes_w.txt >/dev/null 2>&1; grep -q 'watch: [1-9][0-9]* frames published' $D/nes_w.txt && head -c 15 $D/nes_w.ppm | grep -q '160 128' && [ $(wc -c < $D/nes_w.ppm) -eq 61455 ] && [ $(tail -c +16 $D/nes_w.ppm | od -An -tx1 -v | tr -s ' \\n' '\\n\\n' | sort -u | grep -c ff) -ge 1 ]")
set_tests_properties(nes_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME sms_play_pool COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B z80 explore sms $D/fix_z80_branch.sms --play-steps 900000 --play-sessions 3 --play-pool 8 --text $D/sms_pp.txt >/dev/null 2>&1; grep -Eq 'checkpoint pool: [1-9][0-9]* kept, [1-9][0-9]* sessions started from one' $D/sms_pp.txt")
set_tests_properties(sms_play_pool PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME sms_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/sms_w.ppm; $B z80 explore sms $D/fix_z80_branch.sms --play-steps 300000 --watch-dump $D/sms_w.ppm --text $D/sms_w.txt >/dev/null 2>&1; grep -q 'watch: [1-9][0-9]* frames published' $D/sms_w.txt && head -c 15 $D/sms_w.ppm | grep -q '160 192'")
set_tests_properties(sms_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME gg_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/gg_w.ppm; $B z80 explore gg $D/fix_z80_branch.gg --play-steps 300000 --watch-dump $D/gg_w.ppm --text $D/gg_w.txt >/dev/null 2>&1; grep -q 'watch: [1-9][0-9]* frames published' $D/gg_w.txt && head -c 15 $D/gg_w.ppm | grep -q '160 144'")
set_tests_properties(gg_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME sg_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/sg_w.ppm; $B z80 explore sg $D/fix_z80_branch.sg --play-steps 300000 --watch-dump $D/sg_w.ppm --text $D/sg_w.txt >/dev/null 2>&1; grep -q 'watch: [1-9][0-9]* frames published' $D/sg_w.txt && head -c 15 $D/sg_w.ppm | grep -q '160 192'")
set_tests_properties(sg_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME msx_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/msx_w.ppm; $B z80 explore msx $D/fix_z80_branch.msx --play-steps 300000 --watch-dump $D/msx_w.ppm --text $D/msx_w.txt >/dev/null 2>&1; grep -q 'watch: [1-9][0-9]* frames published' $D/msx_w.txt && head -c 15 $D/msx_w.ppm | grep -q '160 192'")
set_tests_properties(msx_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME gb_session_cells COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_branch.gb --play --session-cells --time-limit 8 2>&1 | grep -q 'screen cells archived into the checkpoint pool'")
add_test(NAME gb_session_cells_resume COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -rf $D/cells_st; $B explore $D/fix_branch.gb --play --session-cells --state-dir $D/cells_st --time-limit 6 2>&1 | grep -q ': 1 screen cells archived'; $B explore $D/fix_branch.gb --play --session-cells --resume --state-dir $D/cells_st --time-limit 6 2>&1 | grep -q ': 0 screen cells archived'")
set_tests_properties(gb_session_cells_resume PROPERTIES FIXTURES_REQUIRED fixtures)
set_tests_properties(gb_session_cells PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME pce_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/pce_w.ppm; $B pce explore $D/fix_pce_branch.pce --play 100 --watch-dump $D/pce_w.ppm --text $D/pce_w.txt >/dev/null 2>&1; grep -q 'watch: [1-9][0-9]* frames published' $D/pce_w.txt && head -c 15 $D/pce_w.ppm | grep -q '^P6'")
set_tests_properties(pce_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME export_state COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/e_nes.dpst $D/e_md.dpst $D/e_pce.dpst; $B nes explore $D/fix_nes_picture.nes --novelty 300000 --novelty-frames 30 --novelty-cells --export-state $D/e_nes.dpst >/dev/null 2>&1; $B md explore $D/fix_md_sram_overlay.md --novelty 300000 --novelty-frames 30 --novelty-cells --export-state $D/e_md.dpst >/dev/null 2>&1; $B pce explore $D/fix_pce_branch.pce --novelty 3000000 --novelty-frames 30 --novelty-cells --export-state $D/e_pce.dpst >/dev/null 2>&1; python3 -I -c \"import struct,zlib,sys\nfor n,p in (('nes','e_nes'),('md','e_md'),('pce','e_pce')):\n d=open('$D/'+p+'.dpst','rb').read()\n assert d[:4]==b'DPST' and d[8:8+len(n)]==n.encode()\n v,pl,crc,rs,dep,us,zl=struct.unpack('<I8sIIIII',d[4:36])\n assert v==1 and zl==len(d)-36 and len(zlib.decompress(d[36:]))==us and dep>0\"")
set_tests_properties(export_state PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME msx_export_state COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/e_msx.dpst; $B z80 explore msx $D/fix_z80_branch.msx --novelty 200 --export-state $D/e_msx.dpst >/dev/null 2>&1; [ $(head -c 4 $D/e_msx.dpst | od -An -c | tr -d ' ') = DPST ]")
set_tests_properties(msx_export_state PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_play_sessions COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_sram_overlay.md --play-frames 60 --play-sessions 3 --text $D/md_ps1.txt >/dev/null 2>&1; $B md explore $D/fix_md_sram_overlay.md --play-frames 60 --play-sessions 3 --text $D/md_ps2.txt >/dev/null 2>&1; grep -q 'played sessions: 3, 180 frames' $D/md_ps1.txt && [ \"$(grep -E 'executed|observed union' $D/md_ps1.txt)\" = \"$(grep -E 'executed|observed union' $D/md_ps2.txt)\" ]")
set_tests_properties(md_play_sessions PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_table COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4.md --text $D/md_s4.txt >/dev/null 2>&1; grep -q 'jump tables (S4, jmp d(pc,Xn)): 1 tables, 2 handlers seeded' $D/md_s4.txt && grep -q 'only via forks: [1-9]' $D/md_s4.txt")
set_tests_properties(md_s4_table PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_jsr COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4j.md --text $D/md_s4j.txt >/dev/null 2>&1; grep -q 'jump tables (S4, jmp d(pc,Xn)): 1 tables, 2 handlers seeded' $D/md_s4j.txt")
set_tests_properties(md_s4_jsr PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_an COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4a.md --text $D/md_s4a.txt >/dev/null 2>&1; grep -q ': 1 tables, 2 handlers seeded' $D/md_s4a.txt")
set_tests_properties(md_s4_an PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_ptr COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4p.md --text $D/md_s4p.txt >/dev/null 2>&1; grep -q ': 1 tables, 2 handlers seeded' $D/md_s4p.txt")
set_tests_properties(md_s4_ptr PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME md_s4_ptr_a2 COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4p2.md --text $D/md_s4p2.txt >/dev/null 2>&1; grep -q ': 1 tables, 2 handlers seeded' $D/md_s4p2.txt")
set_tests_properties(md_s4_ptr_a2 PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_ptr_dn COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4p3.md --text $D/md_s4p3.txt >/dev/null 2>&1; grep -q ': 1 tables, 2 handlers seeded' $D/md_s4p3.txt")
set_tests_properties(md_s4_ptr_dn PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_ptr_dn_copy COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4p4.md --text $D/md_s4p4.txt >/dev/null 2>&1; grep -q ': 1 tables, 2 handlers seeded' $D/md_s4p4.txt")
set_tests_properties(md_s4_ptr_dn_copy PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_ptr_disp COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4p5.md --text $D/md_s4p5.txt >/dev/null 2>&1; grep -q ': 1 tables, 2 handlers seeded' $D/md_s4p5.txt")
set_tests_properties(md_s4_ptr_disp PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_s4_words COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_s4w.md --text $D/md_s4w.txt >/dev/null 2>&1; grep -q ': 1 tables, 2 handlers seeded' $D/md_s4w.txt")
set_tests_properties(md_s4_words PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME z80_novelty_cells COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; for s in sms msx; do $B z80 explore $s $D/fix_z80_branch.$s --novelty 300000 --novelty-frames 30 --novelty-cells 1 --text $D/zc_$s.txt >/dev/null 2>&1; grep -q 'novelty: depth [1-9][0-9]* frames, [1-9][0-9]* trials' $D/zc_$s.txt || exit 1; done")
set_tests_properties(z80_novelty_cells PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME gb_jp_table_de COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_jptable_de.gb --cov $D/jtd.cov --text $D/jtd.txt >/dev/null 2>&1; grep -Eq 'S4 operand tables of executed JP HL: [1-9][0-9]* tables, [1-9][0-9]* entries' $D/jtd.txt && grep -Eq '^00 0190 [1-9]' $D/jtd.cov && grep -Eq '^00 01a0 [1-9]' $D/jtd.cov")
set_tests_properties(gb_jp_table_de PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_play_jobs COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_sram_overlay.md --play-frames 60 --play-sessions 4 --text $D/md_pj1.txt >/dev/null 2>&1; $B md explore $D/fix_md_sram_overlay.md --play-frames 60 --play-sessions 4 --jobs 2 --text $D/md_pj2.txt >/dev/null 2>&1; grep -q 'played sessions: 4, 240 frames' $D/md_pj2.txt && grep -q 'secondary instance folded' $D/md_pj2.txt && [ \"$(grep -E 'observed union' $D/md_pj1.txt)\" = \"$(grep -E 'observed union' $D/md_pj2.txt)\" ]")
set_tests_properties(md_play_jobs PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME sms_ym2413_typed COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B z80 explore sms $D/fix_sms_ym2413.sms --text $D/ym.txt >/dev/null 2>&1; grep -Eq 'YM2413 typed writes: user instrument 0, rhythm [1-9][0-9]*, F-number 0, key/block [1-9][0-9]*, instrument/volume [1-9][0-9]*, other 0' $D/ym.txt")
set_tests_properties(sms_ym2413_typed PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME msx_v9938_ports COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B z80 explore msx $D/fix_msx_v9938.msx --text $D/v99.txt >/dev/null 2>&1; grep -Eq 'MSX2 V9938 ports .*palette .9A [1-9][0-9]*, indirect register .9B [1-9][0-9]*' $D/v99.txt")
set_tests_properties(msx_v9938_ports PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME pce_multitap_option COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B pce explore $D/fix_pce_branch.pce --multitap --play 20 --text $D/pt.txt >/dev/null 2>&1; grep -q 'five-pad multitap model on' $D/pt.txt")
set_tests_properties(pce_multitap_option PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME pce_adpcm_typed COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B pce explore $D/fix_pce_adpcm.pce --text $D/pa.txt >/dev/null 2>&1; grep -Eq 'CD-ROM2 interface writes .*: ADPCM [1-9][0-9]*, other [1-9][0-9]*' $D/pa.txt")
set_tests_properties(pce_adpcm_typed PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME pce_sgx_typed COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B pce explore $D/fix_pce_sgx.pce --text $D/px.txt >/dev/null 2>&1; grep -Eq 'SuperGrafx writes .*: VPC [1-9][0-9]*, VDC2 [1-9][0-9]*' $D/px.txt")
set_tests_properties(pce_sgx_typed PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_play_longloop COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_longloop.md --play-frames 30 --play-sessions 1 --text $D/ll.txt >/dev/null 2>&1; grep -q 'played sessions: 1, 30 frames' $D/ll.txt")
set_tests_properties(md_play_longloop PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_demo_frames COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_longloop.md --demo-frames 40 --text $D/dm.txt >/dev/null 2>&1; grep -q 'demo (no button pressed, from power-on): 40 frames' $D/dm.txt")
set_tests_properties(md_demo_frames PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_play_pool COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_longloop.md --play-frames 600 --play-sessions 3 --play-pool 8 --text $D/pp.txt >/dev/null 2>&1; grep -Eq 'checkpoint pool: [1-9][0-9]* kept, [1-9][0-9]* sessions started from one' $D/pp.txt")
set_tests_properties(md_play_pool PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME md_exact_dedup COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_pad_effect.md --text $D/md_ed0.txt >/dev/null 2>&1; $B md explore $D/fix_md_pad_effect.md --exact-dedup --text $D/md_ed1.txt >/dev/null 2>&1; grep -q 'exact dedup: [1-9][0-9]* forks dropped' $D/md_ed1.txt && ! grep -q 'exact dedup' $D/md_ed0.txt && [ \"$(grep -E 'executed|exec ' $D/md_ed0.txt)\" = \"$(grep -E 'executed|exec ' $D/md_ed1.txt)\" ]")
set_tests_properties(md_exact_dedup PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_map_inference COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_pad_effect.md --text ${CMAKE_CURRENT_BINARY_DIR}/md_inf.txt >/dev/null 2>&1; grep -q 'map inference (S7, not observations): [0-9]* padding' ${CMAKE_CURRENT_BINARY_DIR}/md_inf.txt")
set_tests_properties(md_map_inference PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_class_map COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_pad_effect.md --map $D/md_cm.map >/dev/null 2>&1; [ \"$(head -1 $D/md_cm.map)\" = 'md map v1' ] && [ \"$(sed -n 2p $D/md_cm.map | tr -d '\\n' | wc -c)\" = \"$(wc -c < $D/fix_md_pad_effect.md)\" ] && sed -n 2p $D/md_cm.map | grep -q '[12]'")
set_tests_properties(md_class_map PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_map_png COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_pad_effect.md --map $D/md_mp.map >/dev/null 2>&1; $B maprender $D/fix_md_pad_effect.md $D/md_mp.map $D/md_mp.png --sys md && [ \"$(head -c 4 $D/md_mp.png | tail -c 3)\" = PNG ]")
set_tests_properties(md_map_png PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_summary COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_pad_effect.md --summary $D/md_sum.json >/dev/null 2>&1; python3 -I -c \"import json,sys; d=json.load(open('$D/md_sum.json')); assert d['platform']=='md' and sum(d['classes'].values())==d['rom_size'] and d['counters']['paths']>0\"")
set_tests_properties(md_summary PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_heat COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_pad_effect.md --heat $D/md_heat.txt >/dev/null 2>&1; head -1 $D/md_heat.txt | grep -q 'md heat v1' && grep -v '^#' $D/md_heat.txt | awk '{s+=$3} END {exit !(s>0)}'")
set_tests_properties(md_heat PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME wram_exec_trace COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_wramexec.gb --cov $D/we2.cov --text $D/we2.txt >/dev/null 2>&1; grep -q '^01 4000 [1357]' $D/we2.cov && grep -q 'traced to ROM copy sources' $D/we2.txt && ! grep -q '(0 traced' $D/we2.txt")
set_tests_properties(wram_exec_trace PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME rtcode_archive COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_wramexec.gb --rtcode $D/we.rt --text $D/we3.txt >/dev/null 2>&1; grep -Eq '^runtime code archive: [1-9][0-9]* distinct.*, [1-9][0-9]* rewrote an earlier' $D/we3.txt && grep -q '^# deepeel rtcode v1' $D/we.rt && grep -Eq '^c[0-9a-f]{3} [0-9a-f]+ [0-9]+ -?[0-9]+ -?[0-9]+$' $D/we.rt")
set_tests_properties(rtcode_archive PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME rtcode_generated COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_wramgen.gb --rtcode $D/wg.rt --text $D/wg.txt >/dev/null 2>&1; grep -Eq '0 attached to a ROM copy source, [1-9][0-9]* generated .no ROM source; [1-9][0-9]* with an approximate ROM producer' $D/wg.txt && grep -Eq '^c000 3e42 [0-9]+ -1 16384$' $D/wg.rt")
set_tests_properties(rtcode_generated PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME event_probe_ly COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_lywait.gb --text $D/ly.txt >/dev/null 2>&1; grep -Eq '^event probes .*: [1-9][0-9]* forced variants' $D/ly.txt; r1=$?; $B explore $D/fix_lywait.gb --no-event-fork --text $D/ly2.txt >/dev/null 2>&1; grep -Eq '^event probes .*: 0 forced variants' $D/ly2.txt && [ $r1 = 0 ]")
set_tests_properties(event_probe_ly PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_bk2_movie COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_pad.md --tas $D/fix_md_movie.bk2 --tas-any-rom --max-total 100000 --text $D/md_bk2.txt >/dev/null 2>&1; grep -Eq '^GMV replay: 4 / 4 frames' $D/md_bk2.txt")
set_tests_properties(md_bk2_movie PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME unbounded_spool COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; (ulimit -f 400000; timeout 60 $B explore $D/fix_branch.gb --queue 1 --queue-max 1 --max-paths 2 --unbounded --text $D/ub.txt >/dev/null 2>&1); grep -q '^budgets: 0 paths cap hit, 0 step-wall hit, 0 queue overflows' $D/ub.txt && grep -Eq '^spool: [1-9][0-9]* written, [1-9][0-9]* read back, 0 refused, 0 bytes pending' $D/ub.txt")
set_tests_properties(unbounded_spool PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 90)
add_test(NAME md_static_layer COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_static.md --fork-cap 0 --text $D/ms.txt --map $D/ms.map >/dev/null 2>&1; grep -Eq 'static layer \\(S1/S2, no machine\\): [0-9]+ padding, 20 table, [1-9][0-9]* code bytes' $D/ms.txt && [ \"$(sed -n 2p $D/ms.map | cut -c517-520)\" = '3333' ] && [ \"$(sed -n 2p $D/ms.map | cut -c1025-1028)\" = '7777' ]")
set_tests_properties(md_static_layer PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME gb_jp_table COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_jptable.gb --cov $D/jt2.cov --text $D/jt2.txt >/dev/null 2>&1; grep -Eq 'S4 operand tables of executed JP HL: [1-9][0-9]* tables, [1-9][0-9]* entries' $D/jt2.txt && grep -Eq '^00 0190 [1-9]' $D/jt2.cov && grep -Eq '^00 01a0 [1-9]' $D/jt2.cov")
set_tests_properties(gb_jp_table PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME md_sections COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_disp.md --max-total 100000 --bank-dispatch --text $D/msec.txt --summary $D/msec.json >/dev/null 2>&1; grep -Eq '^sections: [1-9][0-9]* executed runs, [0-9]+ required \\(natural path\\), [1-9][0-9]* forced only' $D/msec.txt && grep -q '\"sections_required\"' $D/msec.json")
set_tests_properties(md_sections PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)

add_executable(test_sega_streams test_sega_streams.c)
target_link_libraries(test_sega_streams PRIVATE deepeel_sega)
add_test(NAME sega_streams COMMAND test_sega_streams)
set_tests_properties(sega_streams PROPERTIES TIMEOUT 60)
add_test(NAME lr_gpgx_trace COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/lr_gpgx_trace.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_md_fork.md)

add_executable(test_tas_logkey test_tas_logkey.c)
target_link_libraries(test_tas_logkey PRIVATE deepeel_core)
add_test(NAME tas_logkey COMMAND test_tas_logkey)
set_tests_properties(tas_logkey PROPERTIES TIMEOUT 30)

add_test(NAME rtcode_sms_source COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B z80 explore sms $D/fix_sms_ramsrc.sms --rtcode $D/sr.rt --text $D/sr.txt >/dev/null 2>&1; grep -Eq '^c000 3e42 [0-9]+ 512 -?[0-9]+$' $D/sr.rt && grep -Eq '^c002 c9 [0-9]+ 514 -?[0-9]+$' $D/sr.rt")
set_tests_properties(rtcode_sms_source PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)

add_test(NAME rtcode_md_source COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_ramsrc.md --rtcode $D/mr.rt --text $D/mr.txt >/dev/null 2>&1; grep -Eq '^0000 7042 [0-9]+ 1024 1024$' $D/mr.rt && grep -Eq '^0002 4e75 [0-9]+ 1026 1026$' $D/mr.rt")
set_tests_properties(rtcode_md_source PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)

add_test(NAME rtcode_md_generated COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_ramgen.md --rtcode $D/mg.rt --text $D/mg.txt >/dev/null 2>&1; grep -Eq '^0000 7042 [0-9]+ -1 1024$' $D/mg.rt && grep -Eq '^0002 4e75 [0-9]+ -1 -1$' $D/mg.rt && grep -Eq 'runtime code archive: .* [1-9][0-9]* generated with an approximate ROM producer' $D/mg.txt")
set_tests_properties(rtcode_md_generated PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)

add_test(NAME rtcode_md_exg_movem COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_ramexg.md --rtcode $D/me.rt --text $D/me.txt >/dev/null 2>&1; grep -Eq '^0000 4e75 [0-9]+ -1 1024$' $D/me.rt")
set_tests_properties(rtcode_md_exg_movem PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)

# Level-B parity against a real core (patched gambatte). Optional: needs DEEPEEL_GAMBATTE_LR (path to the built
# tools/gambatte_lr, see tools/gambatte_lr_build.sh); skipped (exit 77) otherwise. Start counts >= $0100 equal the
# m0boy counts plus the entry nop at $0100.
add_test(NAME lr_gambatte_parity COMMAND sh -c "L=\"\$DEEPEEL_GAMBATTE_LR\"; D=${CMAKE_CURRENT_BINARY_DIR}; [ -x \"$L\" ] || exit 77; for p in branch:20 loop:3 copy1:13 copyx:14 jumptable:17 nsdata:28 vblank:28 probes:12; do n=\${p%%:*}; w=\${p##*:}; g=$(GLR_STARTS=1 \"$L\" $D/fix_$n.gb 200000 50000000 2>/dev/null | sed -n 's/^starts>=0100: //p'); [ \"$g\" = \"$w\" ] || { echo \"$n: $g != $w\"; exit 1; }; done; for n in dma hdma; do c=$(GLR_CGB=1 \"$L\" $D/fix_$n.gb 200000 50000000 2>&1 >/dev/null | sed -n 's/.*dma \\([0-9]*\\),.*/\\1/p'); [ \"$c\" = 1 ] || { echo \"$n dma=$c\"; exit 1; }; done; b=$(GLR_CGB=1 \"$L\" $D/fix_cgbbank.gb 2>&1 >/dev/null | sed -n 's/.*bank \\([0-9]*\\)$/\\1/p'); [ \"$b\" = 2 ] || { echo \"cgbbank bank=$b\"; exit 1; }; m=$(GLR_CGB=1 \"$L\" $D/fix_cgbbank.gb 2>&1 | sed -n 's/^explorer observed: vram mask \\([0-9a-f]*\\) wram mask \\([0-9a-f]*\\).*/\\1:\\2/p'); [ \"$m\" = 02:08 ] || { echo \"cgbbank masks=$m\"; exit 1; }; for t in dma:3000-fe00-160 hdma:4000-8000-256; do n=\${t%%:*}; w=\${t##*:}; g=$(GLR_COPIES=1 GLR_CGB=1 \"$L\" $D/fix_$n.gb 2>&1 >/dev/null | sed -n 's/^copy 0*\\([0-9a-f]*\\) -> 0*\\([0-9a-f]*\\) len \\([0-9]*\\)$/\\1-\\2-\\3/p'); [ \"$g\" = \"$w\" ] || { echo \"$n copy=$g != $w\"; exit 1; }; done; gb=$(GLR_COPIES=1 \"$L\" $D/fix_mbc5.gb 2>&1 >/dev/null | sed -n 's/^bank 1 \\([0-9]*\\)$/\\1/p' | sed 1d | tr '\\n' ' '); [ \"$gb\" = '2 3 ' ] || { echo \"mbc5 banks=$gb\"; exit 1; }; w0=$(GLR_COPIES=1 \"$L\" $D/fix_mbc1m1.gb 2>&1 >/dev/null | sed -n 's/^bank 0 \\([0-9]*\\)$/\\1/p' | tail -1); [ \"$w0\" = 32 ] || { echo \"mbc1 mode1 rom0 window=$w0\"; exit 1; }")
set_tests_properties(lr_gambatte_parity PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120 SKIP_RETURN_CODE 77)

add_test(NAME nes_demo_frames COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --max-total 200000 --demo 30 --text ${CMAKE_CURRENT_BINARY_DIR}/nes_dm.txt >/dev/null 2>&1; grep -q 'demo (no button pressed, from power-on): 30 frames' ${CMAKE_CURRENT_BINARY_DIR}/nes_dm.txt")
set_tests_properties(nes_demo_frames PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME pce_demo_frames COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_adpcm.pce --max-total 200000 --demo 30 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_dm.txt >/dev/null 2>&1; grep -q 'demo (no button pressed, from power-on): 30 frames' ${CMAKE_CURRENT_BINARY_DIR}/pce_dm.txt")
set_tests_properties(pce_demo_frames PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME resume_cli COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/resume_cli.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_md_dbcc.md)

add_test(NAME gb_prov_de COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_provde.gb --text $D/pvde.txt >/dev/null 2>&1; grep -Eq 'provenance: 1 jp-hl targets, 2 data bytes static' $D/pvde.txt")
set_tests_properties(gb_prov_de PROPERTIES FIXTURES_REQUIRED fixtures)

add_test(NAME pce_provenance_roots COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_prov.pce --second-pass --max-total 20000 --max-steps 100 --text ${CMAKE_CURRENT_BINARY_DIR}/pce_provr.txt >/dev/null 2>&1; grep -q 'provenance: 1 jump-table words traced from ROM reads, 1 roots seeded' ${CMAKE_CURRENT_BINARY_DIR}/pce_provr.txt")
set_tests_properties(pce_provenance_roots PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_queue_grow COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $D/fix_nes_pad.nes --max-total 400000 --max-steps 1000 --queue 1 --queue-max 64 --text $D/nes_qg.txt >/dev/null 2>&1; grep -q 'queue grown [1-9]' $D/nes_qg.txt && grep -q 'parked 0 ' $D/nes_qg.txt")
set_tests_properties(nes_queue_grow PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
