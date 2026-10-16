cc_library(io-dwg LAYER 3
  SOURCES
    src/bits.cpp src/container.cpp src/header_vars.cpp src/messages.cpp src/objects.cpp src/passthrough.cpp
    src/read.cpp src/solid.cpp src/table.cpp src/mleader.cpp src/dynblock.cpp
  DEPS cc::base)
target_include_directories(cc_io-dwg PRIVATE src)

cc_test(io-dwg
  SOURCES tests/read_test.cpp tests/golden_test.cpp tests/bits_test.cpp
  DEPS cc::io-dwg)
if(TARGET test_io-dwg)
  target_include_directories(test_io-dwg PRIVATE src ${PROJECT_SOURCE_DIR}/tests/support)  # the unit tests reach the internal bit reader
  # The JS reader's output for the public fixtures (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(NOT EXISTS ${_golden}/dwg)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-dwg.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_io-dwg PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus"
                                                CC_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
