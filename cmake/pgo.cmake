# Profile-guided build with link-time optimisation of deepeel for this machine (cmake -P, gcc): instrumented build,
# training on a ROM, optimised build.
#   cmake -DROM=<rom> [-DOUT=<dir>] [-DJOBS=<n>] [-DCONFIGURE=<extra cmake arguments, ;-separated>] -P cmake/pgo.cmake
# OUT (default build-pgo) then holds the optimised deepeel.  The ROM stays outside the repository.
if(NOT ROM)
  message(FATAL_ERROR "usage: cmake -DROM=<rom> [-DOUT=<dir>] -P cmake/pgo.cmake")
endif()
get_filename_component(SRC "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT OUT)
  set(OUT "${SRC}/build-pgo")
endif()
if(NOT JOBS)
  set(JOBS 4)
endif()
get_filename_component(ROM "${ROM}" ABSOLUTE)
get_filename_component(OUT "${OUT}" ABSOLUTE)
set(PROF "${OUT}/profile")
set(B "${OUT}/obj")   # the same path for both stages: the profile files are named after the objects
find_program(GCC_AR gcc-ar REQUIRED)
find_program(GCC_RANLIB gcc-ranlib REQUIRED)
file(REMOVE_RECURSE "${PROF}" "${OUT}/train")
file(MAKE_DIRECTORY "${PROF}" "${OUT}/train")

# response files: the GB core's own build script passes CMAKE_C_FLAGS as one argument
file(WRITE "${OUT}/gen.rsp" "-march=native -flto=auto -fprofile-update=atomic -fprofile-generate=${PROF}\n")
file(WRITE "${OUT}/use.rsp" "-march=native -flto=auto -fprofile-use=${PROF} -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch\n")

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE r OUTPUT_QUIET ERROR_VARIABLE e)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "pgo: ${ARGN}\n${e}")
  endif()
endfunction()
function(build stage)
  file(REMOVE_RECURSE "${B}")
  run(${CMAKE_COMMAND} -S "${SRC}" -B "${B}" "-DCMAKE_C_FLAGS=@${OUT}/${stage}.rsp" "-DCMAKE_EXE_LINKER_FLAGS=@${OUT}/${stage}.rsp"
      "-DCMAKE_AR=${GCC_AR}" "-DCMAKE_RANLIB=${GCC_RANLIB}" ${CONFIGURE})
  run(${CMAKE_COMMAND} --build "${B}" --target deepeel -j${JOBS})
endfunction()

message(STATUS "pgo: instrumented build")
build(gen)
message(STATUS "pgo: training (observation runs, then forks)")
run("${B}/deepeel" explore "${ROM}" --jobs 1 --observe --no-closure --no-start-attract --no-infer --no-spec --no-frontier
    --state-dir "${OUT}/train/observe" --random 4 --random-min 3600 --random-stall 600)
run("${B}/deepeel" explore "${ROM}" --jobs 1 --no-infer --no-spec --no-frontier --state-dir "${OUT}/train/fork" --max-time 60)
message(STATUS "pgo: optimised build")
build(use)
file(COPY "${B}/deepeel" DESTINATION "${OUT}")
file(REMOVE_RECURSE "${OUT}/train")
message(STATUS "pgo: ${OUT}/deepeel")
