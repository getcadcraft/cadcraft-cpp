cc_library(io-dwg LAYER 3
  SOURCES
    src/bits.cpp src/container.cpp src/header_vars.cpp src/messages.cpp src/objects.cpp src/passthrough.cpp
    src/read.cpp src/solid.cpp src/table.cpp src/mleader.cpp src/dynblock.cpp
  DEPS cc::base)
target_include_directories(cc_io-dwg PRIVATE src)

cc_test(io-dwg
  SOURCES tests/read_test.cpp
  DEPS cc::io-dwg)
