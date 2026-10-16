cc_executable(cadcraft-shot
  SOURCES main.cpp
  DEPS cc::base cc::engine cc::image cc::render cc::text cc::ui-imgui)
target_compile_definitions(cadcraft-shot PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
