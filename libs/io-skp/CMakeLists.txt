cc_library(io-skp LAYER 3
  SOURCES src/read.cpp src/common.cpp src/zip.cpp src/vff.cpp src/legacy.cpp
  DEPS cc::base cc::i18n
  PRIVATE_EXTERNAL ZLIB::ZLIB)

cc_test(io-skp
  SOURCES tests/skp_test.cpp
  DEPS cc::io-skp)
if(TARGET test_io-skp)
  target_include_directories(test_io-skp PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(test_io-skp PRIVATE CC_SKP_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/skp")
endif()
