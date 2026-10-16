cc_library(base LAYER 0
  SOURCES src/file.cpp)

cc_test(base
  SOURCES tests/reader_test.cpp tests/checked_test.cpp tests/file_test.cpp
  DEPS cc::base)
