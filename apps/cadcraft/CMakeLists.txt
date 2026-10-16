cc_executable(cadcraft
  SOURCES main.cpp
  DEPS cc::base cc::ui-imgui)
target_compile_definitions(cadcraft PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
