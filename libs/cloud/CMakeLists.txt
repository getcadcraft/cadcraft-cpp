cc_library(cloud LAYER 2
  SOURCES src/cloud.cpp src/attach.cpp
  DEPS cc::base cc::i18n
  PRIVATE_EXTERNAL lazperf)

cc_test(cloud
  SOURCES tests/cloud_test.cpp
  DEPS cc::cloud)
if(TARGET test_cloud)
  target_compile_definitions(test_cloud PRIVATE CC_CLOUD_SEEDS="${PROJECT_SOURCE_DIR}/fuzz/corpus/cloud")
endif()
