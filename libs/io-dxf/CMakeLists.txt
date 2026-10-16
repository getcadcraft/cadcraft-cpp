cc_library(io-dxf LAYER 3
  SOURCES src/read.cpp
  DEPS cc::base)

cc_test(io-dxf
  SOURCES tests/read_test.cpp tests/golden_test.cpp
  DEPS cc::io-dxf)
if(TARGET test_io-dxf)
  target_include_directories(test_io-dxf PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # BimCraft 0.5's readDxfModel output for the public fixtures (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(NOT EXISTS ${_golden}/dxf)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-dxf.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_io-dxf PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus")
endif()
