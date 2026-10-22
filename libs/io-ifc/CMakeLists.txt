cc_library(io-ifc LAYER 3
  SOURCES src/step.cpp src/schema.cpp
  DEPS cc::base)

cc_test(io-ifc
  SOURCES tests/step_test.cpp tests/golden_test.cpp tests/schema_test.cpp
  DEPS cc::io-ifc)
if(TARGET test_io-ifc)
  target_include_directories(test_io-ifc PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # cc-ifc 0.5's step.js output for the public IFC fixture (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(NOT EXISTS ${_golden}/ifc-step)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-ifc-step.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_io-ifc PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus")
endif()
