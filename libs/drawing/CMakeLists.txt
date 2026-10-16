cc_library(drawing LAYER 2
  SOURCES src/compile.cpp
  DEPS cc::base)

cc_test(drawing
  SOURCES tests/compile_test.cpp tests/golden_test.cpp
  DEPS cc::drawing cc::io-dwg cc::io-dxf)
if(TARGET test_drawing)
  target_include_directories(test_drawing PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # BimCraft 0.5's compileDrawing output for the public fixtures (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(NOT EXISTS ${_golden}/compile)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-compile.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_drawing PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus")
endif()
