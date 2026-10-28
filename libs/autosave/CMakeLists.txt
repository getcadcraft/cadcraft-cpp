cc_library(autosave LAYER 2
  SOURCES src/autosave.cpp
  DEPS cc::base)

cc_test(autosave
  SOURCES tests/autosave_test.cpp
  DEPS cc::autosave)
