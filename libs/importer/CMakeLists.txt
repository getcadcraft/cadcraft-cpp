cc_library(importer LAYER 4
  SOURCES src/importer.cpp
  DEPS cc::base)

if(CC_BUILD_TESTS AND NOT EMSCRIPTEN AND NOT WIN32)
  # A helper that crashes on purpose, for the crash-guard test (never shipped).
  add_executable(test_crashing_importer tests/crashing_importer.cpp)
  target_compile_features(test_crashing_importer PRIVATE cxx_std_23)
  cc_test(importer
    SOURCES tests/importer_test.cpp
    DEPS cc::importer cc::io-dwg)
  if(TARGET test_importer)
    add_dependencies(test_importer test_crashing_importer cadcraft-importer)
    target_compile_definitions(test_importer PRIVATE
      CC_CRASHING_IMPORTER="$<TARGET_FILE:test_crashing_importer>"
      CC_IMPORTER="$<TARGET_FILE:cadcraft-importer>"
      CC_CORPUS_DIR="${PROJECT_SOURCE_DIR}/build/corpus")
  endif()
endif()
