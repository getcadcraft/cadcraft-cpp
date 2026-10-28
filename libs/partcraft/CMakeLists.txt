cc_library(partcraft LAYER 3
  SOURCES src/script.cpp src/ops_shapes.cpp src/ops_params.cpp src/ops_modules.cpp src/configs.cpp src/bom.cpp
          src/threads.cpp src/gdt.cpp src/font.cpp src/spline.cpp src/form.cpp
  DEPS cc::base cc::i18n cc::cad cc::shapes)
target_include_directories(cc_partcraft PRIVATE src)

cc_test(partcraft
  SOURCES tests/golden_test.cpp tests/script_test.cpp
  DEPS cc::partcraft)
if(TARGET test_partcraft)
  target_include_directories(test_partcraft PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # CadCraft 0.5's answers to the part call scripts (tests/golden/README.md), unpacked into the build tree.
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-part.tar.gz)  # every configure: the references may have changed
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-part.tar.gz DESTINATION ${_golden})
  endif()
  target_compile_definitions(test_partcraft PRIVATE CC_GOLDEN_DIR="${_golden}" CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
                                                    CC_SCRIPT_SEEDS="${PROJECT_SOURCE_DIR}/fuzz/corpus/part_script")
endif()
