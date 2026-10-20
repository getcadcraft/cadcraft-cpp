cc_library(image LAYER 2
  SOURCES src/png.cpp src/picture.cpp
  DEPS cc::base
  PRIVATE_EXTERNAL ZLIB::ZLIB)

cc_test(image
  SOURCES tests/png_test.cpp
  DEPS cc::image)
