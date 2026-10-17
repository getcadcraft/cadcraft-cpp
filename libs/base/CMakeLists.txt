cc_library(base LAYER 0
  SOURCES src/file.cpp src/value.cpp src/json.cpp src/js.cpp src/js_math.cpp)

cc_test(base
  SOURCES tests/reader_test.cpp tests/checked_test.cpp tests/file_test.cpp tests/value_test.cpp tests/json_test.cpp tests/js_test.cpp
  DEPS cc::base)
