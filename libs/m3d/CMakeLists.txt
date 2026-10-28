cc_library(m3d LAYER 2
  SOURCES src/vec.cpp src/geom.cpp src/edit.cpp src/paint.cpp src/files.cpp src/model.cpp src/clip.cpp src/script.cpp
          src/earcut.cpp src/drawing3d.cpp src/flatview.cpp
  DEPS cc::base cc::i18n)

cc_test(m3d
  SOURCES tests/model_test.cpp tests/script_test.cpp
  DEPS cc::m3d cc::i18n)
if(TARGET test_m3d)
  target_include_directories(test_m3d PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-m3d.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-m3d.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_m3d PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
                                              CC_SCRIPTS_DIR="${PROJECT_SOURCE_DIR}/tests/m3d"
                                              CC_GOLDEN_DIR="${_golden}")
endif()
