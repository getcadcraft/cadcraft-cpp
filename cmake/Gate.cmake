# The quality gate (AGENTS.md section 11) as ctest entries of the `ci` preset, so `cmake --workflow --preset ci`
# runs all of it. Labels: `gate` on every check, `heavy` on the ones that build another preset (CI runs those as
# separate jobs and uses the `ci-quick` test preset).
include_guard(GLOBAL)

set(_src ${PROJECT_SOURCE_DIR})
set(_installed "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

add_test(NAME gate.format COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -P ${_src}/tools/format.cmake)
add_test(NAME gate.layers COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -P ${_src}/tools/layers.cmake)
add_test(NAME gate.tidy COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -DBUILD=${PROJECT_BINARY_DIR} -P ${_src}/tools/tidy.cmake)
add_test(NAME gate.assets COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -P ${_src}/tools/assets.cmake)
add_test(NAME gate.licenses COMMAND ${CMAKE_COMMAND} -DINSTALLED=${_installed} -P ${_src}/tools/licenses.cmake)
add_test(NAME gate.osv COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -DINSTALLED=${_installed} -P ${_src}/tools/osv.cmake)
add_test(NAME gate.identity COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -P ${_src}/tools/identity.cmake)
set_tests_properties(gate.format gate.layers gate.tidy gate.assets gate.licenses gate.osv gate.identity
                     PROPERTIES LABELS "gate")

foreach(p asan tsan wasm fuzz)
  add_test(NAME gate.${p} COMMAND ${CMAKE_COMMAND} --workflow --preset ${p} WORKING_DIRECTORY ${_src})
  set_tests_properties(gate.${p} PROPERTIES LABELS "gate;heavy" RUN_SERIAL TRUE TIMEOUT 7200)
endforeach()
add_test(NAME gate.fuzz-smoke
         COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -DFUZZ_BUILD=${_src}/build/fuzz -P ${_src}/tools/fuzz_smoke.cmake)
set_tests_properties(gate.fuzz-smoke PROPERTIES LABELS "gate;heavy" RUN_SERIAL TRUE DEPENDS gate.fuzz TIMEOUT 7200)
