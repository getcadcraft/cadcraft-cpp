cc_executable(cadcraft-cli
  SOURCES main.cpp
  DEPS cc::base cc::i18n cc::cad cc::doc cc::drawing cc::io-dwg cc::io-dxf cc::importer cc::save cc::cmd cc::docview)
target_compile_definitions(cadcraft-cli PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
