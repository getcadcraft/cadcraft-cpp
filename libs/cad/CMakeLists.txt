cc_library(cad LAYER 2
  SOURCES src/util.cpp src/props.cpp src/parts.cpp src/import_dwg.cpp src/read_dxf.cpp src/tancircle.cpp src/files.cpp src/geom.cpp src/dims.cpp src/save_parts.cpp src/tx.cpp src/edit.cpp src/reshape.cpp src/corners.cpp src/cloud.cpp src/boundary.cpp src/measure.cpp src/expr.cpp src/snap.cpp
  DEPS cc::base cc::i18n)

cc_test(cad
  SOURCES tests/import_test.cpp tests/snap_test.cpp
  DEPS cc::cad cc::io-dwg cc::i18n)
if(TARGET test_cad)
  target_include_directories(test_cad PRIVATE ${PROJECT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # CadCraft 0.5's importDwg output for the public fixtures (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(NOT EXISTS ${_golden}/import)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-import.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_cad PRIVATE CC_GOLDEN_DIR="${_golden}" CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus"
                                              CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
endif()
