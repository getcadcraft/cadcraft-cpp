cc_library(text LAYER 2
  SOURCES src/font.cpp
  DEPS cc::base
  PRIVATE_EXTERNAL Freetype::Freetype)

cc_test(text
  SOURCES tests/font_test.cpp
  DEPS cc::text)
if(TARGET test_text)
  target_compile_definitions(test_text PRIVATE CC_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/archivo/Archivo-VF.ttf")
endif()
