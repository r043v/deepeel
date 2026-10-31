# fixtures are generated at build time; no ROM ever lands in the repository
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(FIXTURES
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
  ${CMAKE_CURRENT_BINARY_DIR}/fix_copyx.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsecho.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsdata.gb
  ${CMAKE_CURRENT_BINARY_DIR}/fix_nsstack.gb
)

add_custom_command(
  OUTPUT ${FIXTURES}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/build_fixtures.py ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/build_fixtures.py
  COMMENT "building synthetic GB fixtures"
)

add_custom_target(fixtures ALL DEPENDS ${FIXTURES})

add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE memscan_core m)
add_test(NAME core COMMAND test_core)

add_executable(test_exec test_exec.c)
target_link_libraries(test_exec PRIVATE memscan_core m)
add_test(NAME exec COMMAND test_exec)

add_executable(test_explorer test_explorer.c)
target_link_libraries(test_explorer PRIVATE memscan_core m)
add_dependencies(test_explorer fixtures)
add_test(NAME explorer COMMAND test_explorer ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_sessions test_sessions.c)
target_link_libraries(test_sessions PRIVATE memscan_core m)
add_dependencies(test_sessions fixtures)
add_test(NAME sessions COMMAND test_sessions ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_export test_export.c)
target_link_libraries(test_export PRIVATE memscan_core m)
add_test(NAME export COMMAND test_export)

if(TARGET memscan AND MS_HAS_GUI)
  add_executable(test_gui test_gui.c)
  target_link_libraries(test_gui PRIVATE memscan_core m ${MS_GUI_LIBS})
  target_include_directories(test_gui PRIVATE ${SDL2_INCLUDE_DIRS})
  add_dependencies(test_gui fixtures)
  add_test(NAME gui COMMAND test_gui ${CMAKE_CURRENT_BINARY_DIR})
endif()
