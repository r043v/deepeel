cmake_minimum_required(VERSION 3.16)
project(memscan VERSION 0.2.0 LANGUAGES C)

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

# --- core library: everything but main(); the gui file follows SDL2 availability
set(MS_GUI_FILE src/gui/gui_stub.c)
set(MS_GUI_LIBS "")
if(SDL2_FOUND AND BUILD_GUI)
  set(MS_GUI_FILE src/gui/gui.c)
  set(MS_GUI_LIBS ${SDL2_LIBRARIES})
endif()

add_library(memscan_core STATIC
  src/core/cart.c
  src/core/machine.c
  src/core/snapshot_png.c
  src/cpu/sm83/sm83_exec.c
  src/cpu/sm83/sm83_disasm.c
  src/cpu/sm83/sm83_cycles.c
  src/platform/gb/gb_plat.c
  src/platform/gb/gb_tick.c
  src/platform/gb/gb_ppu.c
  src/explore/explorer.c
  src/explore/sections.c
  src/explore/copies.c
  src/explore/sessions.c
  src/report/report.c
  src/gui/font5x7.c
  ${MS_GUI_FILE}
)
find_package(Threads REQUIRED)
target_link_libraries(memscan_core PUBLIC Threads::Threads)
target_include_directories(memscan_core PUBLIC src include ${SDL2_INCLUDE_DIRS})
if(SDL2_FOUND AND BUILD_GUI)
  set(MS_HAS_GUI 1)
  add_compile_definitions(MS_HAS_GUI=1)
endif()

add_executable(memscan src/memscan.c)
target_link_libraries(memscan PRIVATE memscan_core m ${MS_GUI_LIBS})

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
