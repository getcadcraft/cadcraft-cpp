cc_library(solid LAYER 2
  SOURCES src/solid_ops.cpp src/cases.cpp
  DEPS cc::base
  EXTERNAL manifold)

cc_test(solid
  SOURCES tests/solid_test.cpp
  DEPS cc::solid)
if(TARGET test_solid)
  target_compile_definitions(test_solid PRIVATE CC_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden")
endif()
