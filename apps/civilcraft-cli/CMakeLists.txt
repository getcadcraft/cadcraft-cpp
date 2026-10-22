cc_executable(civilcraft-cli
  SOURCES main.cpp
  DEPS cc::base cc::i18n cc::civil)
target_compile_definitions(civilcraft-cli PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
