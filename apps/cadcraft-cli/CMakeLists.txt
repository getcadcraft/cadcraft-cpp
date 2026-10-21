cc_executable(cadcraft-cli
  SOURCES main.cpp
  DEPS cc::base cc::i18n cc::cad cc::doc cc::drawing cc::io-dwg cc::io-dxf cc::importer cc::save cc::cmd cc::docview cc::sheets cc::plot cc::engine cc::cloud)
target_compile_definitions(cadcraft-cli PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
if(TARGET cc_image)  # zlib for compressed PDFs (the render libraries; the GCC preset builds without them)
  target_link_libraries(cadcraft-cli PRIVATE cc::image)
  target_compile_definitions(cadcraft-cli PRIVATE CC_HAVE_IMAGE=1)
endif()
