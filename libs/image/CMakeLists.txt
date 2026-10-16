cc_library(image LAYER 3
  SOURCES src/png.cpp
  DEPS cc::base
  PRIVATE_EXTERNAL ZLIB::ZLIB)

cc_test(image
  SOURCES tests/png_test.cpp
  DEPS cc::image)
