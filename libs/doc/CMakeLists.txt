cc_library(doc LAYER 3
  SOURCES src/doc.cpp src/pictures.cpp src/gcons.cpp src/params_doc.cpp src/swsketch.cpp src/ckeep.cpp src/linetypes.cpp
  DEPS cc::base cc::image cc::i18n cc::cad cc::m3d)

cc_test(doc
  SOURCES tests/doc_test.cpp
  DEPS cc::doc cc::cad cc::io-dwg cc::io-dxf cc::i18n)
if(TARGET test_doc)
  target_include_directories(test_doc PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # CadCraft 0.5's Doc after load() for the public fixtures (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-doc.tar.gz)  # every configure: the references may have changed
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-doc.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_doc PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus"
                                              CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()
