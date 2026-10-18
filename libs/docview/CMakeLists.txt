cc_library(docview LAYER 4
  SOURCES src/walk.cpp src/walk_events.cpp src/to_drawing.cpp
  DEPS cc::base cc::cad cc::doc cc::drawing)
target_include_directories(cc_docview PRIVATE src)

cc_test(docview
  SOURCES tests/walk_test.cpp
  DEPS cc::docview cc::doc cc::cad cc::io-dwg cc::i18n)
if(TARGET test_docview)
  target_include_directories(test_docview PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-walk.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-walk.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_docview PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus"
                                                  CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()
