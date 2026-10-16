cc_library(render LAYER 3
  SOURCES src/view.cpp src/scene.cpp
  DEPS cc::drawing cc::text)

cc_test(render
  SOURCES tests/render_test.cpp
  DEPS cc::render cc::io-dxf)
if(TARGET test_render)
  target_compile_definitions(test_render PRIVATE CC_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/archivo/Archivo-VF.ttf"
                                                 CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus")
endif()
