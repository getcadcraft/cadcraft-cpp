cc_library(cloud LAYER 2
  SOURCES src/cloud.cpp
  DEPS cc::base cc::i18n)

cc_test(cloud
  SOURCES tests/cloud_test.cpp
  DEPS cc::cloud)
