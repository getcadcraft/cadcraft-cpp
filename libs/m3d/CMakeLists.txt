cc_library(m3d LAYER 2
  SOURCES src/model.cpp src/earcut.cpp src/drawing3d.cpp
  DEPS cc::base)

cc_test(m3d
  SOURCES tests/model_test.cpp
  DEPS cc::m3d)
