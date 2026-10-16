cc_executable(cadcraft-cli
  SOURCES main.cpp
  DEPS cc::base cc::drawing cc::io-dwg cc::io-dxf cc::importer)
