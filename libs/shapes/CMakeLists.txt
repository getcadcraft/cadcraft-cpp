cc_library(shapes LAYER 1
  SOURCES src/sections.cpp src/parts.cpp src/items.cpp
  DEPS cc::base)
target_include_directories(cc_shapes PRIVATE src)

cc_test(shapes
  SOURCES tests/shapes_test.cpp
  DEPS cc::shapes)
