cc_library(cmd LAYER 5
  SOURCES src/engine.cpp src/draw.cpp src/modify.cpp src/trim.cpp src/reshape.cpp src/corner.cpp src/annotate.cpp src/hatch.cpp src/dims.cpp src/blocks.cpp src/inquiry.cpp src/select.cpp src/views.cpp src/script.cpp
  DEPS cc::base cc::i18n cc::cad cc::doc cc::docview)
target_include_directories(cc_cmd PRIVATE src)

cc_test(cmd
  SOURCES tests/cmd_test.cpp
  DEPS cc::cmd cc::doc cc::i18n)
if(TARGET test_cmd)
  target_include_directories(test_cmd PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-commands.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-commands.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_cmd PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
                                              CC_SCRIPTS_DIR="${PROJECT_SOURCE_DIR}/tests/commands"
                                              CC_GOLDEN_DIR="${_golden}")
endif()
