# Namespace an archive's globals with a prefix, including cross-member
# references: nm lists the defined symbols matching PATTERNS (a POSIX
# extended regex over symbol names), objcopy --redefine-syms rewrites every
# occurrence.  Generalizes dp_msx_prefix.cmake to any Sega/MSX runtime
# instance (--jobs clones).
execute_process(COMMAND "${NM}" -g --defined-only --format=posix "${INPUT}"
  OUTPUT_VARIABLE symbols RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect runtime symbols of ${INPUT}")
endif()
string(REGEX MATCHALL "(^|\n)(${PATTERNS}) [A-Za-z] " matches "${symbols}")
set(mapping "")
foreach(match IN LISTS matches)
  string(STRIP "${match}" match)
  string(REGEX REPLACE " [A-Za-z]$" "" symbol "${match}")
  string(APPEND mapping "${symbol} ${PREFIX}${symbol}\n")
endforeach()
if(mapping STREQUAL "")
  message(FATAL_ERROR "No runtime symbols matched PATTERNS (${PATTERNS}) in ${INPUT}")
endif()
file(WRITE "${OUTPUT}.syms" "${mapping}")
execute_process(COMMAND "${OBJCOPY}" --redefine-syms "${OUTPUT}.syms" "${INPUT}" "${OUTPUT}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot namespace runtime of ${INPUT}")
endif()
