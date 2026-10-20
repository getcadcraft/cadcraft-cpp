cc_library(image LAYER 2
  SOURCES src/png.cpp src/picture.cpp
  DEPS cc::base
  PRIVATE_EXTERNAL ZLIB::ZLIB)

# a picture's pixels for the screen (wuffs): only where something draws them
if(CC_BUILD_RENDER)
  cc_library(picture-decode LAYER 2
    SOURCES src/decode.cpp
    DEPS cc::base
    PRIVATE_EXTERNAL wuffs)
endif()

cc_test(image
  SOURCES tests/png_test.cpp
  DEPS cc::image)
