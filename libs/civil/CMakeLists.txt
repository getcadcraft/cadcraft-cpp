# CivilCraft's engine (CivilCraft 0.5: units.js, points.js, tin.js, …) and its call scripts (tools/parity/civil.mjs).
cc_library(civil LAYER 3
  SOURCES src/predicates.cpp src/delaunator.cpp src/tin.cpp src/units.cpp src/points.cpp src/alignment.cpp src/design.cpp src/sections.cpp src/drain.cpp
    src/featureline.cpp src/survey.cpp src/codes.cpp src/deeds.cpp src/drive.cpp src/project.cpp
    src/script.cpp src/script_core.cpp src/script_alignment.cpp src/script_design.cpp src/script_sections.cpp
    src/script_drain.cpp src/script_featureline.cpp src/script_survey.cpp src/script_codes.cpp src/script_deeds.cpp
    src/script_drive.cpp src/script_project.cpp
  DEPS cc::base cc::i18n cc::appkit)

cc_test(civil
  SOURCES tests/civil_test.cpp
  DEPS cc::civil cc::i18n)
if(TARGET test_civil)
  set(_golden ${CMAKE_CURRENT_BINARY_DIR}/golden-js-0.5)
  if(EXISTS ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-civil.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-civil.tar.gz DESTINATION ${_golden})
  endif()
  target_include_directories(test_civil PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(test_civil PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets" CC_GOLDEN_DIR="${_golden}")
endif()
