cc_library(engine LAYER 5
  SOURCES src/open.cpp
  DEPS cc::base cc::drawing cc::io-dwg cc::io-dxf)

cc_test(engine
  SOURCES tests/open_test.cpp
  DEPS cc::engine)
if(TARGET test_engine)
  target_compile_definitions(test_engine PRIVATE CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus")
endif()
