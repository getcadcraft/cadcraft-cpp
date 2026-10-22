cc_library(live LAYER 2
  SOURCES src/msgpack.cpp src/envelope.cpp src/session_core.cpp
  DEPS cc::base)

cc_test(live
  SOURCES tests/vectors_test.cpp
  DEPS cc::live)
if(TARGET test_live)
  target_compile_definitions(test_live PRIVATE CC_LIVE_VECTORS="${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-live-vectors.json")
endif()
