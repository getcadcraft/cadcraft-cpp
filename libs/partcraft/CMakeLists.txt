cc_library(partcraft LAYER 3
  SOURCES src/script.cpp src/ops_shapes.cpp
  DEPS cc::base cc::shapes)
target_include_directories(cc_partcraft PRIVATE src)

cc_test(partcraft
  SOURCES tests/golden_test.cpp
  DEPS cc::partcraft)
if(TARGET test_partcraft)
  target_include_directories(test_partcraft PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # CadCraft 0.5's answers to the part call scripts (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-part.tar.gz)  # every configure: the references may have changed
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-part.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_partcraft PRIVATE CC_GOLDEN_DIR="${_golden}")
endif()
