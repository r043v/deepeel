# fixtures are generated at build time; no ROM ever lands in the repository
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME compare_residual COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/compare_residual.py ${CMAKE_SOURCE_DIR}/tools/compare_residual.py)
add_test(NAME residual_immediates COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/residual_immediates.py ${CMAKE_SOURCE_DIR}/tools/residual_immediates.py)
add_test(NAME core_residual COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_residual.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_branch.gb ${CMAKE_SOURCE_DIR}/tools/compare_residual.py)
add_test(NAME core_gb_entry COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_gb_entry.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_mbc5.gb)
add_test(NAME core_unbounded COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_unbounded.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_branch.gb)
add_test(NAME core_spool_fair COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_spool_fair.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_branch.gb)
set_tests_properties(core_spool_fair PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME core_spool_fair_z80 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_spool_fair.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms sms)
set_tests_properties(core_spool_fair_z80 PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME core_spool_fallback COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_spool_fallback.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_branch.gb)
set_tests_properties(core_spool_fallback PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME core_condstack COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_condstack.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(core_condstack PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME core_wram_overwrite COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_wram_overwrite.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_wram_overwrite.gb)
set_tests_properties(core_wram_overwrite PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME core_wram_fork_source COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_wram_fork_source.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_wram_fork_source.gb)
set_tests_properties(core_wram_fork_source PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME core_bank_deferred COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_bank_deferred.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_mbc_fork.gb)
set_tests_properties(core_bank_deferred PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
set_tests_properties(core_unbounded PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_executable(test_z80 test_z80.c)
target_link_libraries(test_z80 PRIVATE deepeel_z80)
add_test(NAME z80 COMMAND test_z80)
add_executable(test_provenance test_provenance.c)
target_link_libraries(test_provenance PRIVATE deepeel_z80)
add_test(NAME provenance COMMAND test_provenance)
add_executable(test_registry test_registry.c)
target_link_libraries(test_registry PRIVATE deepeel_sega)
add_test(NAME registry COMMAND test_registry)










# the DpIsa kernel interface (spec §7.3), pinned per CPU
add_executable(test_dpisa test_dpisa.c)
target_link_libraries(test_dpisa PRIVATE deepeel_z80 deepeel_core deepeel_nes deepeel_pce)
target_include_directories(test_dpisa PRIVATE ${M0BOY_DIR}/targets/pce/runtime)
add_test(NAME dpisa COMMAND test_dpisa)
set_tests_properties(dpisa PROPERTIES TIMEOUT 30)


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



add_executable(test_z80_stores test_z80_stores.c)
target_link_libraries(test_z80_stores PRIVATE deepeel_sega)
add_test(NAME z80_stores COMMAND test_z80_stores)
set_tests_properties(z80_stores PROPERTIES TIMEOUT 30)




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
  ${CMAKE_CURRENT_BINARY_DIR}/fix_p1afc.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_p1afcneg.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_verdict_nat.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_verdict_forced.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_verdict_spec.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_verdict_mix.gb
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
  ${CMAKE_CURRENT_BINARY_DIR}/fix_dispatch3.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_residual.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsecho.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsdata.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsstack.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_dma.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ref.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4j.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4a.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4w.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_s4p.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_pad6.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_longloop.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_hint.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_hint_masked.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_jptable_de.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_provde.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_provbyte.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ramgen.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ramexg.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_ramsrc.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_z80.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_save.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_rng.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_index.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_save.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_save.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_rng.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_index.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_save.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_rng.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_index.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_nmi.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_irq.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_rng.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_index.nes
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
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_xwin.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_table.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_mmc1table.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_slide.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_sibling.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_prov.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_rts.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_rtssplit.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dispatch.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dispatch1.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dispatch4.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dispatch8.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ctx.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_jam.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_cpucopy.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_wramcopy.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_oamchain.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_sramcopy.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_apucopy.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_index.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_rng.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_save.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_pause.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_pad.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_irq.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ppupoll.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_decomp.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_cells.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_cells.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_index40.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_decomp.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_lyprobe.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_status.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_bz_a.bk2
  ${CMAKE_CURRENT_BINARY_DIR}/fix_bz_b.bk2
  ${CMAKE_CURRENT_BINARY_DIR}/fix_bz_axis.bk2
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_closure.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_closure.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_page.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_tam.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_tam2.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_closure.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_closure.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_closure.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_regclosure.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_stackret.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_siblings.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_jumptable.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_ixtable.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_deadpage.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dmc.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_status.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_decomp.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_decomp.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_decomp.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_md_cells.md
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_cells.nes
  ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_cells.pce
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.gg
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sg
  ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.msx
  ${CMAKE_CURRENT_BINARY_DIR}/fix_msx_v9938.msx
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gbs.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_gbs.gbs
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
set_tests_properties(sega_mapper_cli PROPERTIES TIMEOUT 120)


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



add_executable(test_md test_md.c)
target_link_libraries(test_md PRIVATE deepeel_core m)
add_dependencies(test_md fixtures)
add_test(NAME md_milestone COMMAND test_md ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(md_milestone PROPERTIES FIXTURES_REQUIRED fixtures)   # A7: the save/load/save determinism covers the whole MdFull (the sound Z80 rides every snapshot)
add_test(NAME md COMMAND test_md ${CMAKE_CURRENT_BINARY_DIR})


add_executable(test_export test_export.c)
target_link_libraries(test_export PRIVATE deepeel_core m)
add_test(NAME export COMMAND test_export)


add_executable(test_pce test_pce.c)
target_link_libraries(test_pce PRIVATE deepeel_pce deepeel_core)
add_test(NAME pce COMMAND test_pce)
set_tests_properties(pce PROPERTIES TIMEOUT 60)
add_executable(test_coverage test_coverage.c)
target_link_libraries(test_coverage PRIVATE deepeel_core m)
add_test(NAME coverage COMMAND test_coverage)

add_test(NAME zip_cli COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/zip_cli.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_branch.gb ${Python3_EXECUTABLE})
set_tests_properties(zip_cli PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)

add_executable(test_dpmachine test_dpmachine.c)
target_link_libraries(test_dpmachine PRIVATE deepeel_z80 deepeel_sega deepeel_nes deepeel_pce deepeel_core m)
target_include_directories(test_dpmachine PRIVATE ${M0BOY_DIR}/targets/pce/runtime)
target_include_directories(test_dpmachine PRIVATE ${M0BOY_DIR}/targets/sg1000/runtime)
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
add_test(NAME campaign_fixtures COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/campaign_fixtures.py ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/campaign.md --smoke)
set_tests_properties(campaign_fixtures PROPERTIES TIMEOUT 1800)
add_test(NAME audit_matrix COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/audit_matrix.py ${CMAKE_SOURCE_DIR}/docs/COUVERTURE.fr.md ${CMAKE_CURRENT_BINARY_DIR}/audit.md)
add_test(NAME nes_cov COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --attract-frames 0 --max-total 20000 --max-steps 1000 --cov ${CMAKE_CURRENT_BINARY_DIR}/nes.cov >/dev/null 2>&1 && grep -Eq '^00 0000 1 ([2-9]|[1-9][0-9]+)$' ${CMAKE_CURRENT_BINARY_DIR}/nes.cov")
set_tests_properties(nes_cov PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_cov COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce --attract-frames 0 --max-total 20000 --max-steps 100 --cov ${CMAKE_CURRENT_BINARY_DIR}/pce_cov.cov >/dev/null 2>&1 && grep -Eq '^00 0000 1 [1-9][0-9]*$' ${CMAKE_CURRENT_BINARY_DIR}/pce_cov.cov")
set_tests_properties(pce_cov PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_summary COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_slide.pce; $B pce explore $F --attract-frames 0 --max-total 20000 --max-steps 200 --summary ${CMAKE_CURRENT_BINARY_DIR}/pce_sum.json >/dev/null 2>&1; grep -q '\"platform\":\"pce\"' ${CMAKE_CURRENT_BINARY_DIR}/pce_sum.json && grep -q '\"known_pct\":[0-9]' ${CMAKE_CURRENT_BINARY_DIR}/pce_sum.json && python3 -c \"import json; json.load(open('${CMAKE_CURRENT_BINARY_DIR}/pce_sum.json'))\"")
set_tests_properties(pce_summary PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_coverage_png COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/platform_coverage_png.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes nes noheat)
set_tests_properties(nes_coverage_png PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME pce_coverage_png COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/platform_coverage_png.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce pce noheat)
set_tests_properties(pce_coverage_png PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME nes_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/nes_w.ppm; $B nes explore $D/fix_nes_picture.nes --watch-dump $D/nes_w.ppm --text $D/nes_w.txt >/dev/null 2>&1; head -c 15 $D/nes_w.ppm | grep -q '160 128' && [ $(wc -c < $D/nes_w.ppm) -eq 61455 ] && [ $(tail -c +16 $D/nes_w.ppm | od -An -tx1 -v | tr -s ' \\n' '\\n\\n' | sort -u | grep -c ff) -ge 1 ]")
set_tests_properties(nes_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME pce_watch_dump COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/pce_w.ppm; $B pce explore $D/fix_pce_branch.pce --attract-frames 0 --play 100 --watch-dump $D/pce_w.ppm --text $D/pce_w.txt >/dev/null 2>&1; head -c 15 $D/pce_w.ppm | grep -q '^P6'")
set_tests_properties(pce_watch_dump PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME gb_jp_table_de COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_jptable_de.gb --cov $D/jtd.cov --text $D/jtd.txt >/dev/null 2>&1; grep -Eq 'S4 operand tables of executed JP HL: [1-9][0-9]* tables, [1-9][0-9]* entries' $D/jtd.txt && grep -Eq '^00 0190 [1-9]' $D/jtd.cov && grep -Eq '^00 01a0 [1-9]' $D/jtd.cov")
set_tests_properties(gb_jp_table_de PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME wram_exec_trace COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_wramexec.gb --cov $D/we2.cov --text $D/we2.txt >/dev/null 2>&1; grep -q '^01 4000 [1357]' $D/we2.cov && grep -q 'traced to ROM copy sources' $D/we2.txt && ! grep -q '(0 traced' $D/we2.txt")
set_tests_properties(wram_exec_trace PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME rtcode_archive COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_wramexec.gb --rtcode $D/we.rt --text $D/we3.txt >/dev/null 2>&1; grep -Eq '^runtime code archive: [1-9][0-9]* distinct.*, [1-9][0-9]* rewrote an earlier' $D/we3.txt && grep -q '^# deepeel rtcode v1' $D/we.rt && grep -Eq '^c[0-9a-f]{3} [0-9a-f]+ [0-9]+ -?[0-9]+ -?[0-9]+$' $D/we.rt")
set_tests_properties(rtcode_archive PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME rtcode_generated COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_wramgen.gb --rtcode $D/wg.rt --text $D/wg.txt >/dev/null 2>&1; grep -Eq '0 attached to a ROM copy source, [1-9][0-9]* generated .no ROM source; [1-9][0-9]* with an approximate ROM producer' $D/wg.txt && grep -Eq '^c000 3e42 [0-9]+ -1 16384$' $D/wg.rt")
set_tests_properties(rtcode_generated PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME event_probe_ly COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_lywait.gb --text $D/ly.txt >/dev/null 2>&1; grep -Eq '^event probes .*: [1-9][0-9]* forced variants' $D/ly.txt; r1=$?; $B explore $D/fix_lywait.gb --no-event-fork --text $D/ly2.txt >/dev/null 2>&1; grep -Eq '^event probes .*: 0 forced variants' $D/ly2.txt && [ $r1 = 0 ]")
set_tests_properties(event_probe_ly PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
add_test(NAME gb_jp_table3 COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_jptable3.gb --cov $D/jt3.cov --text $D/jt3.txt >/dev/null 2>&1; grep -Eq 'S4 operand tables of executed JP HL: [1-9][0-9]* tables, [1-9][0-9]* entries' $D/jt3.txt && grep -Eq 'S4 operand tables of executed JP HL: [1-9][0-9]* tables, ([3-9]|[1-9][0-9]+) entries' $D/jt3.txt")
add_test(NAME gb_jp_table COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_jptable.gb --cov $D/jt2.cov --text $D/jt2.txt >/dev/null 2>&1; grep -Eq 'S4 operand tables of executed JP HL: [1-9][0-9]* tables, [1-9][0-9]* entries' $D/jt2.txt && grep -Eq '^00 0190 [1-9]' $D/jt2.cov && grep -Eq '^00 01a0 [1-9]' $D/jt2.cov")
set_tests_properties(gb_jp_table PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)

add_test(NAME lr_gpgx_trace COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/lr_gpgx_trace.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_md_fork.md)

add_executable(test_tas_logkey test_tas_logkey.c)
target_link_libraries(test_tas_logkey PRIVATE deepeel_core)
add_test(NAME tas_logkey COMMAND test_tas_logkey)
set_tests_properties(tas_logkey PROPERTIES TIMEOUT 30)

add_test(NAME rtcode_sms_source COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B z80 explore sms $D/fix_sms_ramsrc.sms --rtcode $D/sr.rt --text $D/sr.txt >/dev/null 2>&1; grep -Eq '^c000 3e42 [0-9]+ 512 -?[0-9]+$' $D/sr.rt && grep -Eq '^c002 c9 [0-9]+ 514 -?[0-9]+$' $D/sr.rt")
set_tests_properties(rtcode_sms_source PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)




# Level-B parity against a real core (patched gambatte). Optional: needs DEEPEEL_GAMBATTE_LR (path to the built
# tools/gambatte_lr, see tools/gambatte_lr_build.sh); skipped (exit 77) otherwise. Start counts >= $0100 equal the
# m0boy counts plus the entry nop at $0100.
add_test(NAME lr_gambatte_parity COMMAND sh -c "L=\"\$DEEPEEL_GAMBATTE_LR\"; D=${CMAKE_CURRENT_BINARY_DIR}; [ -x \"$L\" ] || exit 77; for p in branch:20 loop:3 copy1:13 copyx:14 jumptable:17 nsdata:28 vblank:28 probes:12; do n=\${p%%:*}; w=\${p##*:}; g=$(GLR_STARTS=1 \"$L\" $D/fix_$n.gb 200000 50000000 2>/dev/null | sed -n 's/^starts>=0100: //p'); [ \"$g\" = \"$w\" ] || { echo \"$n: $g != $w\"; exit 1; }; done; for n in dma hdma; do c=$(GLR_CGB=1 \"$L\" $D/fix_$n.gb 200000 50000000 2>&1 >/dev/null | sed -n 's/.*dma \\([0-9]*\\),.*/\\1/p'); [ \"$c\" = 1 ] || { echo \"$n dma=$c\"; exit 1; }; done; b=$(GLR_CGB=1 \"$L\" $D/fix_cgbbank.gb 2>&1 >/dev/null | sed -n 's/.*bank \\([0-9]*\\)$/\\1/p'); [ \"$b\" = 2 ] || { echo \"cgbbank bank=$b\"; exit 1; }; m=$(GLR_CGB=1 \"$L\" $D/fix_cgbbank.gb 2>&1 | sed -n 's/^explorer observed: vram mask \\([0-9a-f]*\\) wram mask \\([0-9a-f]*\\).*/\\1:\\2/p'); [ \"$m\" = 02:08 ] || { echo \"cgbbank masks=$m\"; exit 1; }; for t in dma:3000-fe00-160 hdma:4000-8000-256; do n=\${t%%:*}; w=\${t##*:}; g=$(GLR_COPIES=1 GLR_CGB=1 \"$L\" $D/fix_$n.gb 2>&1 >/dev/null | sed -n 's/^copy 0*\\([0-9a-f]*\\) -> 0*\\([0-9a-f]*\\) len \\([0-9]*\\)$/\\1-\\2-\\3/p'); [ \"$g\" = \"$w\" ] || { echo \"$n copy=$g != $w\"; exit 1; }; done; gb=$(GLR_COPIES=1 \"$L\" $D/fix_mbc5.gb 2>&1 >/dev/null | sed -n 's/^bank 1 \\([0-9]*\\)$/\\1/p' | sed 1d | tr '\\n' ' '); [ \"$gb\" = '2 3 ' ] || { echo \"mbc5 banks=$gb\"; exit 1; }; w0=$(GLR_COPIES=1 \"$L\" $D/fix_mbc1m1.gb 2>&1 >/dev/null | sed -n 's/^bank 0 \\([0-9]*\\)$/\\1/p' | tail -1); [ \"$w0\" = 32 ] || { echo \"mbc1 mode1 rom0 window=$w0\"; exit 1; }")
set_tests_properties(lr_gambatte_parity PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120 SKIP_RETURN_CODE 77)


add_test(NAME gb_prov_de COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_provde.gb --text $D/pvde.txt >/dev/null 2>&1; grep -Eq 'provenance: 1 jp-hl targets, 2 data bytes static' $D/pvde.txt")
set_tests_properties(gb_prov_de PROPERTIES FIXTURES_REQUIRED fixtures)
# S3 GB byte-granular BC: the jp hl devised from ld b,n/ld c,n is proven, the
# unknown-A witness is not
add_test(NAME gb_prov_byte COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_provbyte.gb --text $D/pvbt.txt >/dev/null 2>&1; grep -Eq 'provenance: 1 jp-hl targets' $D/pvbt.txt")
set_tests_properties(gb_prov_byte PROPERTIES FIXTURES_REQUIRED fixtures)

add_test(NAME nes_summary_heat COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $D/fix_nes_branch.nes --attract-frames 0 --max-total 20000 --max-steps 1000 --summary $D/nes_sum.json >/dev/null 2>&1; grep -q '\"platform\":\"nes\"' $D/nes_sum.json && grep -q '\"known_pct\"' $D/nes_sum.json")
set_tests_properties(nes_summary_heat PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)


add_executable(test_nes_jmpind test_nes_jmpind.c)
target_link_libraries(test_nes_jmpind PRIVATE deepeel_nes deepeel_core m)
add_dependencies(test_nes_jmpind fixtures)
add_test(NAME nes_jmpind COMMAND test_nes_jmpind ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_nes_mmc3 test_nes_mmc3.c)
target_link_libraries(test_nes_mmc3 PRIVATE deepeel_nes deepeel_core m)
add_dependencies(test_nes_mmc3 fixtures)
add_test(NAME nes_mmc3 COMMAND test_nes_mmc3 ${CMAKE_CURRENT_BINARY_DIR})


add_executable(test_kernel test_kernel.c)
target_link_libraries(test_kernel PRIVATE deepeel_z80 deepeel_sega deepeel_core m)
target_include_directories(test_kernel PRIVATE ${M0BOY_DIR}/targets/sg1000/runtime)
add_dependencies(test_kernel fixtures)
add_test(NAME kernel_explore COMMAND test_kernel ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME core_cli_sega COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms --core > ${CMAKE_CURRENT_BINARY_DIR}/core_cli.txt 2>&1; grep -q 'generic kernel' ${CMAKE_CURRENT_BINARY_DIR}/core_cli.txt && grep -q 'forced [1-9]' ${CMAKE_CURRENT_BINARY_DIR}/core_cli.txt")
set_tests_properties(core_cli_sega PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_exec_outside COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms --core 2>&1 | grep -q 'exec_outside [1-9]' && $<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms --core 2>&1 | grep -q 'exec_outside 0'")
set_tests_properties(core_exec_outside PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_copies COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms --core 2>&1 | grep -q '(3 distinct ROM sources)'")
set_tests_properties(core_copies PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_forced_ram_cut COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms --core 2>&1 | grep -q 'forced RAM cuts [1-9]'")
set_tests_properties(core_forced_ram_cut PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_options COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms --core --no-forced-ram-cut --no-copies --attract-frames 0 2>&1 | grep -q 'forced RAM cuts 0' && $<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms --core --no-copies 2>&1 | grep -q 'copy bytes 0'")
set_tests_properties(core_options PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_vector_seeds COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms --core 2>&1 | grep -q 'seeded entry points 10' && $<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms --core --no-vector-seeds 2>&1 | grep -q 'seeded entry points 2'")
set_tests_properties(core_vector_seeds PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_jobs COMMAND sh -c "for f in fix_z80_branch fix_sms_ramsrc fix_sega_mapper; do a=$($<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/$f.sms --core --attract-frames 0 | grep 'rom bytes' | sed 's/,.*//'); b=$($<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/$f.sms --core --attract-frames 0 --jobs 2 | grep 'rom bytes' | sed 's/,.*//'); [ \"$a\" = \"$b\" ] || { echo $f: $a vs $b; exit 1; }; done; $<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.sms --core --jobs 4 | grep -q 'workers 4 (short 0)'")
set_tests_properties(core_jobs PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_novelty COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 | grep -q 'novelty picks [1-9] (new ROM bytes: novelty [1-9]' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 --no-novelty-order | grep -q 'novelty picks 0'")
set_tests_properties(core_novelty PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cli_nes COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_branch.nes --core --attract-frames 0 > ${CMAKE_CURRENT_BINARY_DIR}/core_nes.txt 2>&1; grep -q 'deepeel nes (generic kernel)' ${CMAKE_CURRENT_BINARY_DIR}/core_nes.txt && grep -q 'forced [1-9]' ${CMAKE_CURRENT_BINARY_DIR}/core_nes.txt && $<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_cpucopy.nes --core --attract-frames 0 2>&1 | grep -q 'rom->ram copy bytes [1-9]'")
set_tests_properties(core_cli_nes PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cli_pce COMMAND sh -c "$<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce --core --attract-frames 0 2>&1 | grep -q 'deepeel pce (generic kernel)' && $<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_branch.pce --core --attract-frames 0 2>&1 | grep -q 'forced [1-9]'")
set_tests_properties(core_cli_pce PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cli_md COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_cpu_vram.md --core --max-total 20000 --visits-per-pc 64 2>&1 | grep -q 'deepeel md (generic kernel)' && $<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_cpu_vram.md --core --max-total 20000 --visits-per-pc 64 2>&1 | grep -q 'forced [1-9]'")
set_tests_properties(core_cli_md PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cli_msx COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore msx ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.msx --core --attract-frames 0 2>&1 | grep -q 'deepeel msx (generic kernel)' && $<TARGET_FILE:deepeel> z80 explore msx ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_branch.msx --core --attract-frames 0 2>&1 | grep -q 'forced [1-9]'")
set_tests_properties(core_cli_msx PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cells COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_cells.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 --cells 0xC000-0xC0FF | grep -q 'cell-pruned forks [1-9]' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 --no-cells | grep -q 'cell-pruned forks 0'")
set_tests_properties(core_cells PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cells_auto_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_cells.gb; $<TARGET_FILE:deepeel> explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'cell-pruned forks [1-9]' && $<TARGET_FILE:deepeel> explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-cells | grep -q 'cell-pruned forks 0'")
set_tests_properties(core_cells_auto_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cells_auto_huc6280 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_cells.pce; $<TARGET_FILE:deepeel> pce explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'cell-pruned forks [1-9]' && $<TARGET_FILE:deepeel> pce explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-cells | grep -q 'cell-pruned forks 0'")
set_tests_properties(core_cells_auto_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cells_auto_z80 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_cells.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 | grep -q 'cell-pruned forks [1-9]'")
set_tests_properties(core_cells_auto_z80 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cells_auto_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_cells.md; $<TARGET_FILE:deepeel> md explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'cell-pruned forks [1-9]' && $<TARGET_FILE:deepeel> md explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-cells | grep -q 'cell-pruned forks 0'")
set_tests_properties(core_cells_auto_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_cells_auto_6502 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_cells.nes; $<TARGET_FILE:deepeel> nes explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'cell-pruned forks [1-9]' && $<TARGET_FILE:deepeel> nes explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-cells | grep -q 'cell-pruned forks 0'")
set_tests_properties(core_cells_auto_6502 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_decompress COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_decomp.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 | grep -q 'decompress runs [1-9][0-9]*, compressed ROM bytes 3[0-9]' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 --no-decompress | grep -q 'decompress runs 0, compressed ROM bytes 0' && $<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_sms_ramsrc.sms --core --jobs 1 --attract-frames 0 | grep -q 'decompress runs 0,'")
set_tests_properties(core_decompress PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_event_probes COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_ppupoll.nes; $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --visits-per-pc 8 | grep -q 'event probes [1-9][0-9]* at 1 sites' && $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --visits-per-pc 8 --no-event-probes | grep -q 'event probes 0 at 0'")
set_tests_properties(core_event_probes PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_irq_fork COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_irq.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --max-total 200000 | grep -q 'IRQ forks [1-9]'")
set_tests_properties(core_irq_fork PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_irq_jitter COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_irq.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --max-total 200000 | grep -q 'timing jitter [1-9]' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --max-total 200000 --no-irq-jitter | grep -q 'timing jitter 0'")
set_tests_properties(core_irq_jitter PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_pad_probes COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_pad.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 | grep -q 'natural 14' && ! $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-event-probes --no-sessions | grep -q 'natural 14'")
set_tests_properties(core_pad_probes PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_pause_probe COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_pause.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 | grep -q 'rom-index 102-108 natural' && ! $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-event-probes --no-sessions | grep -q 'rom-index 102-108 natural [1-9]'")
set_tests_properties(core_pause_probe PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_sessions COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_pad.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 | grep -q 'held-pad sessions 16' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions | grep -q 'held-pad sessions 0'")
set_tests_properties(core_sessions PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_seed_ram COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_save.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --seed-ram 0xC100-0xC100 | grep -q 'natural 15' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --no-learn-sig --no-solve | grep -q 'natural 11'")
set_tests_properties(core_seed_ram PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_taint_cut COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_save.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions | grep -q 'forced taint cuts' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --no-taint-cut | grep -q 'taint cuts: indirect 0, ret 0'")
set_tests_properties(core_taint_cut PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_site_score COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_save.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions | grep -q 'site-scored picks' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --no-site-score | grep -q 'site-scored picks 0'")
set_tests_properties(core_site_score PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_learn_sig COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_save.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions | grep -q 'natural 15' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --no-learn-sig --no-solve | grep -q 'natural 11'")
set_tests_properties(core_learn_sig PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_rng_bytes COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_rng.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions | grep -q 'natural 18' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --no-rng-learn --no-solve | grep -q 'natural 14'")
set_tests_properties(core_rng_bytes PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_index_replay COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_index.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions | grep -q 'natural 23' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --no-index-replay | grep -q 'natural 19'")
set_tests_properties(core_index_replay PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_solve COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_solve.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions | grep -q 'natural 17' && $<TARGET_FILE:deepeel> z80 explore sms $f --core --attract-frames 0 --no-sessions --no-solve | grep -q 'natural 15'")
set_tests_properties(core_solve PROPERTIES FIXTURES_REQUIRED fixtures)

# techniques 3, 4, 7 on the 6502 (NES): the technique covers the other side as natural, its counter is never silent
add_test(NAME core_learn_sig_6502 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_save.nes; $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --no-sessions | grep -Eq 'learned signatures ([2-9]|[1-9][0-9]+)$' && $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --no-sessions --no-solve --no-learn-sig | grep -q 'rom bytes: natural 12'")
set_tests_properties(core_learn_sig_6502 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_rng_bytes_6502 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_rng.nes; $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --no-sessions | grep -q 'rng-byte forks 8' && $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --no-sessions --no-solve --no-rng-learn | grep -q 'rom bytes: natural 15'")
set_tests_properties(core_rng_bytes_6502 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_index_replay_6502 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_index.nes; $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --no-sessions | grep -q 'index replays 16' && $<TARGET_FILE:deepeel> nes explore $f --core --attract-frames 0 --no-sessions --no-solve --no-index-replay | grep -q 'rom bytes: natural 16'")
set_tests_properties(core_index_replay_6502 PROPERTIES FIXTURES_REQUIRED fixtures)

# techniques 3, 4, 7 on the 68000 (Mega Drive)
add_test(NAME core_learn_sig_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_save.md; $<TARGET_FILE:deepeel> md explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'learned signatures 2' && $<TARGET_FILE:deepeel> md explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-learn-sig | grep -q 'rom bytes: natural 16'")
set_tests_properties(core_learn_sig_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_rng_bytes_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_rng.md; $<TARGET_FILE:deepeel> md explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'rng-byte forks 8' && $<TARGET_FILE:deepeel> md explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-rng-learn | grep -q 'rom bytes: natural 24'")
set_tests_properties(core_rng_bytes_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_index_replay_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_index.md; $<TARGET_FILE:deepeel> md explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'index replays 16' && $<TARGET_FILE:deepeel> md explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-index-replay | grep -q 'rom bytes: natural 32'")
set_tests_properties(core_index_replay_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_irq_fork_nes COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_nmi.nes; a='--core --attract-frames 0 --no-sessions --max-total 400000 --visits-per-pc 100000'; $<TARGET_FILE:deepeel> nes explore $f $a | grep -q 'IRQ forks 2 (timing jitter 4)' && $<TARGET_FILE:deepeel> nes explore $f $a --no-irq-jitter | grep -q 'IRQ forks 1 (timing jitter 0)'")
set_tests_properties(core_irq_fork_nes PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_irq_fork_pce COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_irq.pce; a='--core --attract-frames 0 --no-sessions --max-total 400000 --visits-per-pc 100000'; $<TARGET_FILE:deepeel> pce explore $f $a | grep -q 'IRQ forks 2 (timing jitter 4)' && $<TARGET_FILE:deepeel> pce explore $f $a --no-irq-jitter | grep -q 'IRQ forks 2 (timing jitter 0)'")
set_tests_properties(core_irq_fork_pce PROPERTIES FIXTURES_REQUIRED fixtures)

# techniques 3, 4, 7 on the SM83 (GB) and the HuC6280 (PCE)
add_test(NAME core_learn_sig_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_save.gb; $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -Eq 'learned signatures ([2-9]|[1-9][0-9]+)$' && $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-learn-sig | grep -q 'rom bytes: natural 15'")
set_tests_properties(core_learn_sig_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_rng_bytes_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_rng.gb; $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'rng-byte forks 8' && $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-rng-learn | grep -q 'rom bytes: natural 18'")
set_tests_properties(core_rng_bytes_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_index_replay_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_index.gb; $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'index replays 16' && $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-index-replay | grep -q 'rom bytes: natural 23'")
set_tests_properties(core_index_replay_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_learn_sig_huc6280 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_save.pce; $<TARGET_FILE:deepeel> pce explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'learned signatures 2' && $<TARGET_FILE:deepeel> pce explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-learn-sig | grep -q 'rom bytes: natural 15'")
set_tests_properties(core_learn_sig_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_rng_bytes_huc6280 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_rng.pce; $<TARGET_FILE:deepeel> pce explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'rng-byte forks 8' && $<TARGET_FILE:deepeel> pce explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-rng-learn | grep -q 'rom bytes: natural 19'")
set_tests_properties(core_rng_bytes_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_index_replay_huc6280 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_index.pce; $<TARGET_FILE:deepeel> pce explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'index replays 16' && $<TARGET_FILE:deepeel> pce explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --no-solve --no-index-replay | grep -q 'rom bytes: natural 21'")
set_tests_properties(core_index_replay_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_index_max_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_index40.gb; $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 --index-max 63 | grep -q 'index replays 64' && $<TARGET_FILE:deepeel> explore $f --core --attract-frames 0 --no-sessions --visits-per-pc 64 | grep -q 'index replays 16'")
set_tests_properties(core_index_max_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_decompress_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_decomp.gb; $<TARGET_FILE:deepeel> explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'decompress runs [1-9][0-9]*, compressed ROM bytes [1-9]' && $<TARGET_FILE:deepeel> explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-decompress | grep -q 'decompress runs 0, compressed ROM bytes 0'")
set_tests_properties(core_decompress_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_decompress_huc6280 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_decomp.pce; $<TARGET_FILE:deepeel> pce explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'decompress runs [1-9][0-9]*, compressed ROM bytes [1-9]' && $<TARGET_FILE:deepeel> pce explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-decompress | grep -q 'decompress runs 0, compressed ROM bytes 0'")
set_tests_properties(core_decompress_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_decompress_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_decomp.md; $<TARGET_FILE:deepeel> md explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'decompress runs [1-9][0-9]*, compressed ROM bytes [1-9]' && $<TARGET_FILE:deepeel> md explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-decompress | grep -q 'decompress runs 0, compressed ROM bytes 0'")
set_tests_properties(core_decompress_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_decompress_6502 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_decomp.nes; $<TARGET_FILE:deepeel> nes explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'decompress runs [1-9][0-9]*, compressed ROM bytes [1-9]' && $<TARGET_FILE:deepeel> nes explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-decompress | grep -q 'decompress runs 0, compressed ROM bytes 0'")
set_tests_properties(core_decompress_6502 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_event_probes_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_lyprobe.gb; $<TARGET_FILE:deepeel> explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'event probes [1-9][0-9]* at 1 sites' && $<TARGET_FILE:deepeel> explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-event-probes | grep -q 'event probes 0 at 0 sites'")
set_tests_properties(core_event_probes_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_event_probes_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_status.md; $<TARGET_FILE:deepeel> md explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'event probes [1-9][0-9]* at 1 sites' && $<TARGET_FILE:deepeel> md explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-event-probes | grep -q 'event probes 0 at 0 sites'")
set_tests_properties(core_event_probes_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_event_probes_huc6280 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_status.pce; $<TARGET_FILE:deepeel> pce explore $f --core --jobs 1 --attract-frames 0 --no-sessions | grep -q 'event probes [1-9][0-9]* at 1 sites' && $<TARGET_FILE:deepeel> pce explore $f --core --jobs 1 --attract-frames 0 --no-sessions --no-event-probes | grep -q 'event probes 0 at 0 sites'")
set_tests_properties(core_event_probes_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_sm83 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_closure.gb; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> explore $f $a | grep -q 'static closure: roots [1-9][0-9]*, bytes [0-9]*, static-only 0, provisional 4' && $<TARGET_FILE:deepeel> explore $f $a --no-closure | grep -q 'static closure: roots 0, bytes 0, static-only 0, provisional 0'")
set_tests_properties(core_closure_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_huc6280 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_closure.pce; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> pce explore $f $a | grep -q 'static closure: roots [1-9][0-9]*, bytes [0-9]*, static-only 0, provisional 4' && $<TARGET_FILE:deepeel> pce explore $f $a --no-closure | grep -q 'static closure: roots 0, bytes 0, static-only 0, provisional 0'")
set_tests_properties(core_closure_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_closure.md; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> md explore $f $a | grep -q 'static closure: roots [1-9][0-9]*, bytes [0-9]*, static-only 0, provisional 4' && $<TARGET_FILE:deepeel> md explore $f $a --no-closure | grep -q 'static closure: roots 0, bytes 0, static-only 0, provisional 0'")
set_tests_properties(core_closure_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_6502 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_closure.nes; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> nes explore $f $a | grep -q 'static closure: roots [1-9][0-9]*, bytes [0-9]*, static-only 0, provisional 4' && $<TARGET_FILE:deepeel> nes explore $f $a --no-closure | grep -q 'static closure: roots 0, bytes 0, static-only 0, provisional 0'")
set_tests_properties(core_closure_6502 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_z80 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_closure.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> z80 explore sms $f $a | grep -q 'static closure: roots [1-9][0-9]*, bytes [0-9]*, static-only 0, provisional 5' && $<TARGET_FILE:deepeel> z80 explore sms $f $a --no-closure | grep -q 'static closure: roots 0, bytes 0, static-only 0, provisional 0'")
set_tests_properties(core_closure_z80 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_reg_z80 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_regclosure.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> z80 explore sms $f $a | grep -q 'closure registers (C3): seed walks [1-9][0-9]*, indirect resolved [1-9][0-9]*,' && $<TARGET_FILE:deepeel> z80 explore sms $f $a --no-closure-regs | grep -q 'static closure: roots' && ! $<TARGET_FILE:deepeel> z80 explore sms $f $a --no-closure-regs | grep -q 'closure registers (C3)'")
set_tests_properties(core_closure_reg_z80 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_campaign_z80 COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -rf $D/camp_z80.state; $B z80 explore sms $D/fix_z80_cells.sms --core --jobs 1 --attract-frames 0 --no-sessions --campaign --max-time 2 --state-dir $D/camp_z80.state --text $D/camp_z80.txt >$D/camp_z80.out 2>$D/camp_z80.err & P=$!; sleep 6; kill -INT $P 2>/dev/null; wait $P; grep -qE 'states resumed, [0-9]+ saved' $D/camp_z80.txt && grep -q 'heuristic pruning remains disabled under --unbounded' $D/camp_z80.err && grep -q 'campaign phase 1 (bounded)' $D/camp_z80.err && grep -qE 'deferred [1-9]' $D/camp_z80.err && grep -q 'resumed first-cover actors' $D/camp_z80.err")
set_tests_properties(core_campaign_z80 PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 180)
add_test(NAME core_campaign_epoch COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -rf $D/camp_ep.state; $B explore $D/fix_epoch.gb --fork-site-cap 1 --tas-ckpt-every 30 --attract-frames 600 --play-frames 600 --max-total 600000 --campaign --max-time 3 --state-dir $D/camp_ep.state --text $D/camp_ep.txt >$D/camp_ep.out 2>$D/camp_ep.err & P=$!; sleep 8; kill -INT $P 2>/dev/null; wait $P; grep -q 'campaign phase 1 (bounded)' $D/camp_ep.err && grep -qE 'thanks to a checkpoint epoch [1-9]' $D/camp_ep.err")   # P5: the epoch crosses the frontier save/resume (phase 2 keeps forking through epochs until SIGINT)   # P5/P7: the epoch and the capped forks cross the frontier save/resume (deferred > 0)
set_tests_properties(core_campaign_epoch PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 300)
add_test(NAME z80_cov_geom COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B z80 explore sms $D/fix_z80_branch.sms --attract-frames 0 --max-total 200000 --cov $D/z80g.cov >/dev/null 2>&1; grep -q '^00 0000 [159]' $D/z80g.cov && ! grep -Eq '^00 [4-9a-f]' $D/z80g.cov && ! grep -Eq '^0[1-9a-f] [0-3]' $D/z80g.cov && grep -q '^01 4' $D/z80g.cov")   # P10: bank 0 stays at $0000, a switchable bank maps at its window base $4000
set_tests_properties(z80_cov_geom PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_ixtable_z80 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_ixtable.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> z80 explore sms $f $a | grep -q 'S4 operand tables of executed JP HL: 1 tables, [3-9] entries'")
set_tests_properties(core_closure_ixtable_z80 PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME core_closure_stackret_z80 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_stackret.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> z80 explore sms $f $a | grep -q 'closure registers (C3):.*stack returns [1-9][0-9]*,' && ! $<TARGET_FILE:deepeel> z80 explore sms $f $a --no-closure-regs | grep -q 'closure registers (C3)'")
set_tests_properties(core_closure_stackret_z80 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_stackret_6502 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_stackret.nes; a='--core --jobs 1 --attract-frames 0 --no-sessions --max-total 300000'; $<TARGET_FILE:deepeel> nes explore $f $a | grep -q 'closure registers (C3):.*stack returns [1-9][0-9]*,' && ! $<TARGET_FILE:deepeel> nes explore $f $a --no-closure-regs | grep -q 'closure registers (C3)'")
set_tests_properties(core_closure_stackret_6502 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_stackret_68000 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_md_stackret.md; a='--core --jobs 1 --attract-frames 0 --no-sessions --max-total 300000'; $<TARGET_FILE:deepeel> md explore $f $a | grep -q 'closure registers (C3):.*stack returns [1-9][0-9]*,' && ! $<TARGET_FILE:deepeel> md explore $f $a --no-closure-regs | grep -q 'closure registers (C3)'")
set_tests_properties(core_closure_stackret_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
# C3 port_effect: the OUT to the primary slot select discards the walk's returns (the callee's
# ret is not followed); the harmless VDP OUT leaves the walk's concrete-stack return intact
add_test(NAME core_closure_portout_msx COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_msx_portout.msx; a='--core --jobs 1 --attract-frames 0 --no-sessions --max-total 200000'; $<TARGET_FILE:deepeel> z80 explore msx $f $a --text ${CMAKE_CURRENT_BINARY_DIR}/core_portout.txt >/dev/null && grep -q 'ghost returns discarded on memory-control OUT (port_effect): [1-9]' ${CMAKE_CURRENT_BINARY_DIR}/core_portout.txt && grep -q 'closure registers (C3):.*stack returns [1-9][0-9]*,' ${CMAKE_CURRENT_BINARY_DIR}/core_portout.txt")
set_tests_properties(core_closure_portout_msx PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_closure_seedcap_z80 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_stackret.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> z80 explore sms $f $a --closure-seed-cap 8 | grep -q 'closure registers (C3):.*stack returns [1-9][0-9]*,' && $<TARGET_FILE:deepeel> z80 explore sms $f $a --closure-seed-cap 1 | grep -q 'closure registers (C3): seed walks [1-9]'")
set_tests_properties(core_closure_seedcap_z80 PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME core_closure_seedcap_zero COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_stackret.sms; $<TARGET_FILE:deepeel> z80 explore sms $f --core --jobs 1 --attract-frames 0 --no-sessions --closure-seed-cap 0 | grep -q 'closure registers (C3): seed walks [1-9]'")
set_tests_properties(core_closure_seedcap_zero PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
# "zero data marked code" points 1+3: on the ground-truth fixture, the tile dump behind the
# call's fall-through is refused at the untrusted entry (counter) and the map audit against
# the generator's truth reports no data marked code
add_test(NAME core_closure_truth_z80 COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_truth.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; o=${CMAKE_CURRENT_BINARY_DIR}/core_truth_z80; $<TARGET_FILE:deepeel> z80 explore sms $f $a --map $o.map --text $o.txt >/dev/null && grep -q 'untrusted walk entries refused (data plausibility, point 3): [1-9]' $o.txt && python3 ${CMAKE_SOURCE_DIR}/tests/truth_check.py $o.map ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_truth.truth")
set_tests_properties(core_closure_truth_z80 PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)
add_test(NAME core_map_sm83 COMMAND sh -c "o=${CMAKE_CURRENT_BINARY_DIR}/core_map_sm83; rm -f $o.png $o.json $o.map; $<TARGET_FILE:deepeel> explore ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_closure.gb --core --jobs 1 --attract-frames 0 --no-sessions --map $o.png --summary $o.json >/dev/null && head -c 8 $o.png | grep -q PNG && grep -q '\"probable-code\":[1-9]' $o.json && $<TARGET_FILE:deepeel> explore ${CMAKE_CURRENT_BINARY_DIR}/fix_gb_closure.gb --core --jobs 1 --attract-frames 0 --no-sessions --map $o.map >/dev/null && grep -q 'map v1' $o.map")
set_tests_properties(core_map_sm83 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_map_huc6280 COMMAND sh -c "o=${CMAKE_CURRENT_BINARY_DIR}/core_map_huc6280; rm -f $o.png $o.json $o.map; $<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_closure.pce --core --jobs 1 --attract-frames 0 --no-sessions --map $o.png --summary $o.json >/dev/null && head -c 8 $o.png | grep -q PNG && grep -q '\"probable-code\":[1-9]' $o.json && $<TARGET_FILE:deepeel> pce explore ${CMAKE_CURRENT_BINARY_DIR}/fix_pce_closure.pce --core --jobs 1 --attract-frames 0 --no-sessions --map $o.map >/dev/null && grep -q 'map v1' $o.map")
set_tests_properties(core_map_huc6280 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_map_68000 COMMAND sh -c "o=${CMAKE_CURRENT_BINARY_DIR}/core_map_68000; rm -f $o.png $o.json $o.map; $<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_closure.md --core --jobs 1 --attract-frames 0 --no-sessions --map $o.png --summary $o.json >/dev/null && head -c 8 $o.png | grep -q PNG && grep -q '\"probable-code\":[1-9]' $o.json && $<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_closure.md --core --jobs 1 --attract-frames 0 --no-sessions --map $o.map >/dev/null && grep -q 'map v1' $o.map")
set_tests_properties(core_map_68000 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_map_6502 COMMAND sh -c "o=${CMAKE_CURRENT_BINARY_DIR}/core_map_6502; rm -f $o.png $o.json $o.map; $<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_closure.nes --core --jobs 1 --attract-frames 0 --no-sessions --map $o.png --summary $o.json >/dev/null && head -c 8 $o.png | grep -q PNG && grep -q '\"probable-code\":[1-9]' $o.json && $<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_closure.nes --core --jobs 1 --attract-frames 0 --no-sessions --map $o.map >/dev/null && grep -q 'map v1' $o.map")
set_tests_properties(core_map_6502 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_map_z80 COMMAND sh -c "o=${CMAKE_CURRENT_BINARY_DIR}/core_map_z80; rm -f $o.png $o.json $o.map; $<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_closure.sms --core --jobs 1 --attract-frames 0 --no-sessions --map $o.png --summary $o.json >/dev/null && head -c 8 $o.png | grep -q PNG && grep -q '\"probable-code\":[1-9]' $o.json && $<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_closure.sms --core --jobs 1 --attract-frames 0 --no-sessions --map $o.map >/dev/null && grep -q 'map v1' $o.map")
set_tests_properties(core_map_z80 PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME bizhawk_reader COMMAND sh -c "d=${CMAKE_CURRENT_BINARY_DIR}; x=$<TARGET_FILE:deepeel>; $x tas info $d/fix_bz_axis.bk2 | grep -q 'frames 3, columns 4 (2 buttons, 2 axes)' && $x tas info $d/fix_bz_a.bk2 | grep -q 'core NesHawk' && $x tas diff $d/fix_bz_a.bk2 $d/fix_bz_a.bk2 | grep -q identical && ! $x tas diff $d/fix_bz_a.bk2 $d/fix_bz_b.bk2 | grep -q identical && $x tas diff $d/fix_bz_a.bk2 $d/fix_bz_b.bk2 | grep -q 'first differing frame: 2'")
set_tests_properties(bizhawk_reader PROPERTIES FIXTURES_REQUIRED fixtures)

add_test(NAME desync_finder COMMAND sh -c "d=${CMAKE_CURRENT_BINARY_DIR}; x=$<TARGET_FILE:deepeel>; python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/desync_fixture.py $d && $x desync $d/dsy_a.dsy $d/dsy_a.dsy | grep -q 'identical over 6' && ! $x desync $d/dsy_a.dsy $d/dsy_b.dsy > $d/dsy.out; grep -q 'compared frame 4' $d/dsy.out && grep -q '+0010  A=aa  B=ab' $d/dsy.out && grep -q 'window (5 frames' $d/dsy.out && $x desync $d/dsy_a.dsy $d/dsy_b.dsy --ignore wram:10 | grep -q 'identical over 6'")

# Features the kernel does not have yet (play sessions, the legacy spool/lot frontier) stay pinned to the old GB explorer, which is kept but no longer the default.
add_test(NAME frontier_kernel COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/frontier_kernel.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_irq.gb)

# E1 witness: the GB P1 poll probe on `ld a,(C)` (fork_p1) reads $FF00+C --
# C=$00 is P1 (positive witness), C=$FF is the IE register (negative witness).

# E3 witness: GB section verdicts — a seed-only section is speculative, a
# fork-reachable one stays forced, the flow fork outranks the seed provenance.

# E7/E8 witnesses (AUDIT-PARITE-MOTEURS.fr.md 8): the legacy PCE IRQ fork on
# fix_pce_irq and the GB same-context eviction on fix_vblank.

# GBS rip verification (src/core/gbs.c): header parse, per-bank best match of a patched
# rip, init/play mapping; and the CLI: the confirmed seeds execute, a rip that matches
# nothing fails loudly.
add_executable(test_gbs test_gbs.c)
target_link_libraries(test_gbs PRIVATE deepeel_core m)
add_dependencies(test_gbs fixtures)
add_test(NAME gbs COMMAND test_gbs ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(gbs PROPERTIES FIXTURES_REQUIRED fixtures)
add_test(NAME core_gbs COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_gbs.py $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR}/fix_gbs.gb ${CMAKE_CURRENT_BINARY_DIR}/fix_gbs.gbs)
set_tests_properties(core_gbs PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)

# Music index reader (src/core/music_index.c): DPMDX002 layout walk, CRC/title/game
# binary searches, entry decode, header-title normalization; truncated file fails at load.
add_executable(test_music_index test_music_index.c)
target_link_libraries(test_music_index PRIVATE deepeel_core m)
add_test(NAME music_index COMMAND test_music_index ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_p0_core test_p0_core.c)
target_link_libraries(test_p0_core PRIVATE deepeel_z80 deepeel_sega deepeel_nes deepeel_pce deepeel_core m)
add_dependencies(test_p0_core fixtures)
add_test(NAME p0_core COMMAND test_p0_core ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_p3_core test_p3_core.c)
target_link_libraries(test_p3_core PRIVATE deepeel_z80 deepeel_sega deepeel_nes deepeel_pce deepeel_core m)
add_dependencies(test_p3_core fixtures)
add_test(NAME p3_core COMMAND test_p3_core ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_p4_core test_p4_core.c)
target_link_libraries(test_p4_core PRIVATE deepeel_nes deepeel_core m)
add_dependencies(test_p4_core fixtures)
add_test(NAME p4_core COMMAND test_p4_core ${CMAKE_CURRENT_BINARY_DIR})

# B7: a mapper write forks on the banks the game never selected (bank 2 and 3 code of fix_mbc_fork.gb)
add_test(NAME core_bank_forks COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_mbc_fork.gb; O=${CMAKE_CURRENT_BINARY_DIR}; $B explore $F --core --no-sessions --attract-frames 0 --no-pointer-scan --text $O/bf_on.txt >/dev/null 2>&1 && $B explore $F --core --no-bank-forks --no-sessions --attract-frames 0 --no-pointer-scan --text $O/bf_off.txt >/dev/null 2>&1 && grep -q 'rom-index 32768-32773' $O/bf_on.txt && grep -q 'rom-index 49152-49157' $O/bf_on.txt && ! grep -q 'rom-index 32768-32773' $O/bf_off.txt && grep -q 'bank forks: 4' $O/bf_on.txt")
set_tests_properties(core_bank_forks PROPERTIES FIXTURES_REQUIRED fixtures)

# B8: an uncalled routine is a speculative gap run; undefined opcodes and padding are not (witnesses)
add_test(NAME core_spec_gaps COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_gap.gb; O=${CMAKE_CURRENT_BINARY_DIR}; $B explore $F --core --no-sessions --attract-frames 0 --no-pointer-scan --no-closure --text $O/spec_on.txt >/dev/null 2>&1 && $B explore $F --core --no-spec --no-sessions --attract-frames 0 --no-pointer-scan --no-closure --text $O/spec_off.txt >/dev/null 2>&1 && grep -q 'speculative gaps: 1 runs, 8 bytes' $O/spec_on.txt && grep -q '0 runs reinforced' $O/spec_on.txt && grep -q 'speculative gaps: 0 runs' $O/spec_off.txt && grep -q 'natural 7, forced 0' $O/spec_on.txt")
set_tests_properties(core_spec_gaps PROPERTIES FIXTURES_REQUIRED fixtures)

# B10: the cartridge RAM window the machine declares is seeded without --seed-ram (NES $6000, PRG-RAM poke)
add_test(NAME core_auto_seed COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_sram.nes; O=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $F --core --no-sessions --attract-frames 0 --text $O/as_on.txt >/dev/null 2>&1 && $B nes explore $F --core --no-auto-seed --no-sessions --attract-frames 0 --text $O/as_off.txt >/dev/null 2>&1 && grep -q 'seeded RAM states 4' $O/as_on.txt && grep -q 'seeded RAM states 0' $O/as_off.txt")
set_tests_properties(core_auto_seed PROPERTIES FIXTURES_REQUIRED fixtures)

# A22/B3: the movie's checkpoints become free paths in the kernel; --no-tas-ckpt turns them off
add_test(NAME core_tas_ckpt COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_pad.md --core --attract-frames 0 --tas $D/fix_md_movie.bk2 --tas-any-rom --tas-ckpt-every 1 --max-total 100000 --text $D/tc_on.txt >/dev/null 2>&1 && $B md explore $D/fix_md_pad.md --core --attract-frames 0 --tas $D/fix_md_movie.bk2 --tas-any-rom --no-tas-ckpt --max-total 100000 --text $D/tc_off.txt >/dev/null 2>&1 && grep -Eq 'checkpoints continued as free paths: [1-9]' $D/tc_on.txt && grep -q 'checkpoints continued as free paths: 0' $D/tc_off.txt")
set_tests_properties(core_tas_ckpt PROPERTIES FIXTURES_REQUIRED fixtures)

# B9: an executed `ld hl,nn` designating a 2-instruction gap routine reinforces it (3 are needed without the reference)
add_test(NAME core_spec_ref COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_ref.gb; O=${CMAKE_CURRENT_BINARY_DIR}; $B explore $F --core --no-sessions --attract-frames 0 --no-pointer-scan --no-closure --text $O/ref.txt >/dev/null 2>&1 && grep -q 'speculative gaps: 1 runs' $O/ref.txt && grep -q '1 runs reinforced' $O/ref.txt")
set_tests_properties(core_spec_ref PROPERTIES FIXTURES_REQUIRED fixtures)

# B3: the vector seeds also start from the first live movie checkpoints
add_test(NAME core_live_seeds COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; printf '|.......S|\\n|.......S|\\n|.......S|\\n|.......S|\\n' > $D/ls.log; $B explore $D/fix_gb_ref.gb --core --tas $D/ls.log --tas-any-rom --attract-frames 0 --tas-ckpt-every 1 --max-total 100000 --text $D/ls_on.txt >/dev/null 2>&1 && $B explore $D/fix_gb_ref.gb --core --tas $D/ls.log --tas-any-rom --attract-frames 0 --tas-ckpt-every 1 --no-live-seeds --max-total 100000 --text $D/ls_off.txt >/dev/null 2>&1 && grep -Eq 'vector seeds from live states: [1-9]' $D/ls_on.txt && grep -q 'vector seeds from live states: 0' $D/ls_off.txt")
set_tests_properties(core_live_seeds PROPERTIES FIXTURES_REQUIRED fixtures)


# B8 on NES: the window model (base $8000, last bank fixed high) lets the speculative disassembly run on the PRG image
add_test(NAME core_spec_nes COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_gap.nes; O=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $F --core --no-sessions --attract-frames 0 --text $O/sn_on.txt >/dev/null 2>&1 && $B nes explore $F --core --no-spec --no-sessions --attract-frames 0 --text $O/sn_off.txt >/dev/null 2>&1 && grep -q 'speculative gaps: 1 runs, 8 bytes' $O/sn_on.txt && grep -q 'speculative gaps: 0 runs' $O/sn_off.txt")
set_tests_properties(core_spec_nes PROPERTIES FIXTURES_REQUIRED fixtures)

# B4 on 6502: the proven table length (12 words) widens the index replay past the default bound of 15
add_test(NAME core_index_proven_6502 COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_nes_index20.nes; O=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $F --core --no-sessions --attract-frames 0 --text $O/ip_on.txt >/dev/null 2>&1 && $B nes explore $F --core --no-sessions --attract-frames 0 --index-max 15 --text $O/ip_off.txt >/dev/null 2>&1 && grep -q 'index replays 25' $O/ip_on.txt && grep -q 'index replays 16' $O/ip_off.txt")
set_tests_properties(core_index_proven_6502 PROPERTIES FIXTURES_REQUIRED fixtures)

# A13: the fall-through after a call is untrusted; inline data read by the callee stops the static walk
add_test(NAME core_inline_data COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_inline.gb; O=${CMAKE_CURRENT_BINARY_DIR}; $B explore $F --core --no-sessions --attract-frames 0 --no-pointer-scan --text $O/inl.txt >/dev/null 2>&1 && grep -Eq 'static walks stopped or retracted on data [1-9]' $O/inl.txt")
set_tests_properties(core_inline_data PROPERTIES FIXTURES_REQUIRED fixtures)

# A13: an untrusted static walk stops on a padding run
add_test(NAME core_pad_stop COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_padstop.gb; O=${CMAKE_CURRENT_BINARY_DIR}; $B explore $F --core --no-sessions --attract-frames 0 --no-pointer-scan --text $O/pad.txt >/dev/null 2>&1 && grep -Eq 'retracted on data [1-9]' $O/pad.txt && grep -q 'static-only 0' $O/pad.txt")
set_tests_properties(core_pad_stop PROPERTIES FIXTURES_REQUIRED fixtures)

# A13: an untrusted static walk stops inside a run of pointer-like words
add_test(NAME core_ptr_stop COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_gb_ptrstop.gb; O=${CMAKE_CURRENT_BINARY_DIR}; $B explore $F --core --no-sessions --attract-frames 0 --no-pointer-scan --text $O/ptr.txt >/dev/null 2>&1 && grep -Eq 'retracted on data [1-9]' $O/ptr.txt && grep -q 'static-only 0' $O/ptr.txt")
set_tests_properties(core_ptr_stop PROPERTIES FIXTURES_REQUIRED fixtures)

# B9 on the 68000: an executed lea/movea.l/pea immediate that designates the ROM is a typed reference
add_test(NAME core_imm_ref_68000 COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B md explore $D/fix_md_ref.md --core --jobs 1 --attract-frames 0 --no-sessions --text $D/mdref.txt >/dev/null 2>&1 && grep -Eq '\\(1 references seen\\)' $D/mdref.txt && grep -q '1 runs reinforced' $D/mdref.txt")
set_tests_properties(core_imm_ref_68000 PROPERTIES FIXTURES_REQUIRED fixtures)

# B9 on 6502: two nearby 8-bit immediates form a ROM address, a typed reference
add_test(NAME core_imm_ref_6502 COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B nes explore $D/fix_nes_ref.nes --core --no-sessions --attract-frames 0 --text $D/nesref.txt >/dev/null 2>&1 && grep -Eq '[(]1 references seen[)]' $D/nesref.txt")
set_tests_properties(core_imm_ref_6502 PROPERTIES FIXTURES_REQUIRED fixtures)

# B8/B9 on PCE: the fixed bank 0 ($E000) is modelled; a split-immediate reference reinforces an uncalled routine
add_test(NAME core_spec_pce COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B pce explore $D/fix_pce_ref.pce --core --jobs 1 --no-sessions --text $D/pceref.txt >/dev/null 2>&1 && grep -q 'speculative gaps: 1 runs' $D/pceref.txt && grep -q '1 runs reinforced' $D/pceref.txt")
set_tests_properties(core_spec_pce PROPERTIES FIXTURES_REQUIRED fixtures)
# B8 on PCE beyond the fixed bank: the kernel journals the MPR window each page ran at; the uncalled routine in page 1 is found at $4010
add_test(NAME core_spec_pce_page COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B pce explore $D/fix_pce_page.pce --core --jobs 1 --no-sessions --text $D/pcepage.txt >/dev/null 2>&1 && grep -q 'speculative gaps: 1 runs, 8 bytes' $D/pcepage.txt")
set_tests_properties(core_spec_pce_page PROPERTIES FIXTURES_REQUIRED fixtures)

# C3 (option B): the Sega provenance closure runs inside the kernel; its seeds count as closure roots and its bytes never lower the static-only total
add_test(NAME core_sega_closure_hook COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_closure.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; sum='s/.*static-only \\([0-9]*\\), provisional \\([0-9]*\\).*/\\1+\\2/p'; roots='s/.*static closure: roots \\([0-9]*\\),.*/\\1/p'; on=$($<TARGET_FILE:deepeel> z80 explore sms $f $a | sed -n \"$sum\" | head -1); off=$(DEEPEEL_NO_SEGA_CLOSURE=1 $<TARGET_FILE:deepeel> z80 explore sms $f $a | sed -n \"$sum\" | head -1); ron=$($<TARGET_FILE:deepeel> z80 explore sms $f $a | sed -n \"$roots\" | head -1); roff=$(DEEPEEL_NO_SEGA_CLOSURE=1 $<TARGET_FILE:deepeel> z80 explore sms $f $a | sed -n \"$roots\" | head -1); test -n \"$on\" && test $((on)) -ge $((off)) && test $((ron)) -gt $((roff))")
set_tests_properties(core_sega_closure_hook PROPERTIES FIXTURES_REQUIRED fixtures)

# C9: with no engine flag the kernel runs on every platform (the only engine)
add_test(NAME core_default_engine COMMAND sh -c "d=$<TARGET_FILE:deepeel>; b=${CMAKE_CURRENT_BINARY_DIR}; $d nes explore $b/fix_nes_branch.nes --attract-frames 0 2>&1 | grep -q 'deepeel nes (generic kernel)' && $d pce explore $b/fix_pce_branch.pce --attract-frames 0 2>&1 | grep -q 'deepeel pce (generic kernel)' && $d z80 explore sms $b/fix_z80_branch.sms --attract-frames 0 2>&1 | grep -q 'generic kernel' && ! $d nes explore $b/fix_nes_branch.nes --legacy 2>&1 | grep -q 'generic kernel'")
set_tests_properties(core_default_engine PROPERTIES FIXTURES_REQUIRED fixtures)

# C4: the common static inference tags the siblings of a copied table (never as an observation)
add_test(NAME core_infer_siblings COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_siblings.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> z80 explore sms $f $a | grep -q 'copy tables [1-9][0-9]* (32 sibling bytes' && $<TARGET_FILE:deepeel> z80 explore sms $f $a --no-infer | grep -vq 'static inference'")
set_tests_properties(core_infer_siblings PROPERTIES FIXTURES_REQUIRED fixtures)

# C4: the common static inference tags the word run around an observed table read as a jump table
add_test(NAME core_infer_jumptable COMMAND sh -c "f=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_jumptable.sms; a='--core --jobs 1 --attract-frames 0 --no-sessions'; $<TARGET_FILE:deepeel> z80 explore sms $f $a | grep -q 'jump tables 1 (6 bytes)'")
set_tests_properties(core_infer_jumptable PROPERTIES FIXTURES_REQUIRED fixtures)

# B8: a ROM page the mapper never ran is scanned at the windows other pages ran at (PCE)
add_test(NAME core_spec_deadpage COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_pce_deadpage.pce; O=${CMAKE_CURRENT_BINARY_DIR}; $B pce explore $F --core --no-sessions --attract-frames 0 --text $O/dp.txt >/dev/null 2>&1; grep -q 'speculative gaps: [1-9]' $O/dp.txt")
set_tests_properties(core_spec_deadpage PROPERTIES FIXTURES_REQUIRED fixtures)

# B11: a VDP DMA from ROM is a hardware copy the kernel records (on_copy); the CPU never read those bytes
add_test(NAME core_md_dma_copy COMMAND sh -c "$<TARGET_FILE:deepeel> md explore ${CMAKE_CURRENT_BINARY_DIR}/fix_md_dma.md --core --no-sessions --attract-frames 0 | grep -q 'rom->ram copy bytes 32 (32 distinct ROM sources)'")
set_tests_properties(core_md_dma_copy PROPERTIES FIXTURES_REQUIRED fixtures)

# B11: the NES DMC sample DMA reaches the kernel as a hardware copy (17 bytes)
add_test(NAME core_nes_dmc_copy COMMAND sh -c "$<TARGET_FILE:deepeel> nes explore ${CMAKE_CURRENT_BINARY_DIR}/fix_nes_dmc.nes --core --no-sessions --attract-frames 0 | grep -q 'rom->ram copy bytes 17 (17 distinct'")
set_tests_properties(core_nes_dmc_copy PROPERTIES FIXTURES_REQUIRED fixtures)

# C11: a section reports the RAM pages it read on natural paths (its liveness)
add_test(NAME core_section_live COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B z80 explore sms $D/fix_z80_rng.sms --core --no-sessions --attract-frames 0 --jobs 1 | grep -q 'section 1 .* live-ram-pages 1' && $B z80 explore sms $D/fix_z80_closure.sms --core --no-sessions --attract-frames 0 --jobs 1 | grep -q 'section 1 .* live-ram-pages 0'")
set_tests_properties(core_section_live PROPERTIES FIXTURES_REQUIRED fixtures)

# B3: attract/play sessions deposit live checkpoints into the pool; --no-tas-ckpt turns them off
add_test(NAME core_session_ckpt COMMAND sh -c "B=$<TARGET_FILE:deepeel>; F=${CMAKE_CURRENT_BINARY_DIR}/fix_z80_rng.sms; a=\"z80 explore sms $F --core --attract-frames 100 --tas-ckpt-every 20 --jobs 1 --ckpt-burst-frames 5\"; $B $a | grep -q 'pad bursts from them: [1-9]' && $B $a --no-ckpt-bursts | grep -q 'pad bursts from them: 0' && $B $a --no-tas-ckpt | grep -q 'session checkpoints continued as free paths: 0'")
set_tests_properties(core_session_ckpt PROPERTIES FIXTURES_REQUIRED fixtures)
set_tests_properties(core_session_ckpt PROPERTIES TIMEOUT 400)

# B3: live checkpoints are ordered by the visit count of their cell (the code location they resume at), covered or not
add_test(NAME core_ckpt_cell COMMAND sh -c "$<TARGET_FILE:deepeel> z80 explore sms ${CMAKE_CURRENT_BINARY_DIR}/fix_z80_rng.sms --core --attract-frames 100 --tas-ckpt-every 20 --jobs 1 --no-ckpt-bursts | grep -q 'site-scored picks [1-9]'")
set_tests_properties(core_ckpt_cell PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 180)

# Step 3 of the legacy retirement: the kernel alone against the frozen legacy proof sets (tests/golden/)
add_executable(test_parity_golden test_parity_golden.c)
target_link_libraries(test_parity_golden PRIVATE deepeel_core deepeel_nes m)
add_dependencies(test_parity_golden fixtures)
add_test(NAME parity_golden COMMAND test_parity_golden ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/golden)

# Stage 1 of the legacy retirement: the live ROM map window runs on the kernel for every platform (generic DpCoreResult view)
add_test(NAME mapwin_kernel COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mapwin_kernel.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(mapwin_kernel PROPERTIES TIMEOUT 300)

# Stage 1: the kernel's generic --watch (DpMachine.render) publishes the replayed movie's screen; --watch-dump writes the last frame
add_test(NAME watch_kernel COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/wk.ppm; SDL_VIDEODRIVER=dummy $B md explore $D/fix_md_pad.md --core --attract-frames 0 --tas $D/fix_md_movie.bk2 --tas-any-rom --max-total 100000 --watch 60 --watch-dump $D/wk.ppm >/dev/null 2>&1; head -c 2 $D/wk.ppm | grep -q P6")
add_test(NAME watch_kernel_gb COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; rm -f $D/wkg.ppm; SDL_VIDEODRIVER=dummy $B explore $D/fix_gb_ref.gb --core --tas $D/ls.log --tas-any-rom --attract-frames 0 --max-total 100000 --watch 60 --watch-dump $D/wkg.ppm >/dev/null 2>&1; head -c 2 $D/wkg.ppm | grep -q P6")

add_test(NAME kernel_fixtures COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/kernel_fixtures.sh $<TARGET_FILE:deepeel> ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(kernel_fixtures PROPERTIES FIXTURES_REQUIRED fixtures)
# typed register accesses in the kernel report (0.374.0): registry entry, category, counts, per platform
add_test(NAME core_reg_typing COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_apu.gb --no-sessions --text $D/rt_gb.txt >/dev/null 2>&1; grep -q 'reg .FF12-.FF12 NR12 .sound. reads 0 writes' $D/rt_gb.txt && $B nes explore $D/fix_nes_apucopy.nes --no-sessions --text $D/rt_nes.txt >/dev/null 2>&1; grep -q 'reg .4000-.4013 APU .sound.' $D/rt_nes.txt && $B md explore $D/fix_md_cpu_vram.md --text $D/rt_md.txt >/dev/null 2>&1; grep -q 'reg .C00004-.C00007 VDP_CTRL .video.' $D/rt_md.txt && $B z80 explore sms $D/fix_sms_ym2413.sms --text $D/rt_sms.txt >/dev/null 2>&1; grep -q 'reg .00F1-.00F1 YM2413_DATA .sound.' $D/rt_sms.txt && $B z80 explore msx $D/fix_msx_v9938.msx --text $D/rt_msx.txt >/dev/null 2>&1; grep -q 'reg .0099-.0099 VDP_STATUS .video.' $D/rt_msx.txt")

# B3 (0.379.0): a movie checkpoint also launches two pad bursts from its live state
add_test(NAME core_movie_bursts COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; python3 -c \"print('|.......S|\\n'*400,end='')\" > $D/mv_bursts.log; $B z80 explore sms $D/fix_z80_branch.sms --tas $D/mv_bursts.log --attract-frames 0 --tas-ckpt-every 20 --max-total 400000 --text $D/mv_bursts.txt >/dev/null 2>&1; grep -Eq 'movie checkpoints continued as free paths: [1-9].*pad bursts from them: [1-9]' $D/mv_bursts.txt")
set_tests_properties(core_movie_bursts PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 120)

# B3 (0.379.3): the per-site fork cap restarts from zero per checkpoint epoch (fix_epoch saturates one site at cap 1)
add_test(NAME core_epoch_cap COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_epoch.gb --fork-site-cap 1 --tas-ckpt-every 30 --attract-frames 600 --play-frames 600 --max-total 6000000 --text $D/epoch_cap.txt >/dev/null 2>&1; grep -Eq 'thanks to a checkpoint epoch [1-9]' $D/epoch_cap.txt")
set_tests_properties(core_epoch_cap PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 180)

# C3 (0.380.0): the static walk replays `ld a,n; ld (bankreg),a` through the machine's mapper and counts it
add_test(NAME core_closure_bank_sim COMMAND sh -c "B=$<TARGET_FILE:deepeel>; D=${CMAKE_CURRENT_BINARY_DIR}; $B explore $D/fix_bankwalk.gb --max-total 200000 --text $D/bankwalk.txt >/dev/null 2>&1; grep -Eq 'closure bank writes simulated during walks: [1-9]' $D/bankwalk.txt")
set_tests_properties(core_closure_bank_sim PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT 60)
