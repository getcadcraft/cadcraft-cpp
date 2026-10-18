cc_library(save LAYER 4
  SOURCES src/save_dwg.cpp src/dyn_save.cpp src/dxf_common.cpp src/save_dxf.cpp src/save_dxf2000.cpp
  DEPS cc::base cc::i18n cc::cad cc::m3d cc::doc cc::io-dwg)
target_include_directories(cc_save PRIVATE src)

cc_test(save
  SOURCES tests/save_test.cpp
  DEPS cc::save cc::doc cc::cad cc::io-dwg cc::i18n)
if(TARGET test_save)
  target_include_directories(test_save PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # CadCraft 0.5's saveDwg results (size, CRC-32, report) for the public fixtures (tests/golden/README.md)
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(NOT EXISTS ${_golden}/dwg-save AND EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-dwg-save.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-dwg-save.tar.gz DESTINATION ${_golden})
  endif()
  if(NOT EXISTS ${_golden}/dxf-save AND EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-dxf-save.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-dxf-save.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_save PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus"
                                               CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()
