cc_library(control LAYER 2
  SOURCES src/control.cpp
  DEPS cc::base)
if(WIN32)
  target_link_libraries(cc_control PRIVATE ws2_32)
endif()

cc_test(control
  SOURCES tests/control_test.cpp
  DEPS cc::control)
