cc_library(mcp LAYER 2
  SOURCES src/dispatcher.cpp src/env.cpp
  DEPS cc::base)

cc_test(mcp
  SOURCES tests/dispatcher_test.cpp
  DEPS cc::mcp)
if(TARGET test_mcp)
  target_include_directories(test_mcp PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(test_mcp PRIVATE CC_MCP_GOLDEN="${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-mcp-dispatcher.json")
endif()
