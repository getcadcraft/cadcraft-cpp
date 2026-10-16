cc_library(i18n LAYER 1
  SOURCES src/i18n.cpp
  DEPS cc::base)

cc_test(i18n
  SOURCES tests/i18n_test.cpp
  DEPS cc::i18n)
if(TARGET test_i18n)
  target_compile_definitions(test_i18n PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()
