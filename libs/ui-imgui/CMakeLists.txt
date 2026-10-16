cc_library(ui-imgui LAYER 6
  SOURCES src/gpu.cpp src/scene_renderer.cpp src/viewer.cpp src/host.cpp
  DEPS cc::base cc::i18n cc::engine cc::render cc::text
  EXTERNAL dawn::webgpu_dawn SDL3::SDL3 imgui)
