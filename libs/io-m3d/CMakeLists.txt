cc_library(io-m3d LAYER 3
  SOURCES src/export.cpp src/export_cad.cpp src/export_all.cpp
  DEPS cc::base cc::i18n cc::m3d)
target_include_directories(cc_io-m3d PRIVATE src)

cc_test(io-m3d
  SOURCES tests/io_test.cpp
  DEPS cc::io-m3d cc::m3d)
if(TARGET test_io-m3d)
  target_include_directories(test_io-m3d PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-m3d-io.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-m3d-io.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_io-m3d PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
                                                 CC_SCRIPTS_DIR="${PROJECT_SOURCE_DIR}/tests/m3d"
                                                 CC_GOLDEN_DIR="${_golden}")
endif()
