cc_library(lisp LAYER 4
  SOURCES src/value.cpp src/reader.cpp src/interp.cpp src/builtins.cpp src/vl.cpp src/wild.cpp src/script.cpp
  DEPS cc::base cc::i18n cc::cad)
target_include_directories(cc_lisp PRIVATE src)

cc_test(lisp
  SOURCES tests/lisp_test.cpp
  DEPS cc::lisp cc::base cc::i18n)
if(TARGET test_lisp)
  # CadCraft 0.5's LISP (createLisp) on every script in tests/lisp/ (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-lisp.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-lisp.tar.gz DESTINATION ${_golden})
  endif()
  target_include_directories(test_lisp PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(test_lisp PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
                                               CC_SCRIPTS_DIR="${PROJECT_SOURCE_DIR}/tests/lisp"
                                               CC_GOLDEN_DIR="${_golden}")
endif()
