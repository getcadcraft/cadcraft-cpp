cc_library(ui-imgui LAYER 6
  SOURCES src/gpu.cpp src/scene_renderer.cpp src/viewer.cpp src/props_panel.cpp src/constraints_view.cpp src/cloud_view.cpp src/host.cpp
  DEPS cc::base cc::i18n cc::engine cc::render cc::picture-decode cc::text cc::cad cc::doc cc::docview cc::cmd cc::save cc::plot cc::sheets cc::image
  EXTERNAL dawn::webgpu_dawn SDL3::SDL3 imgui)
if(TARGET cc::engine AND NOT EMSCRIPTEN)
  cc_test(ui-imgui SOURCES tests/viewer_touch_test.cpp tests/picture_render_test.cpp DEPS cc::ui-imgui cc::save cc::doc cc::i18n imgui SDL3::SDL3 dawn::webgpu_dawn)
  if(TARGET test_ui-imgui)
    target_compile_definitions(test_ui-imgui PRIVATE CC_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
    if(CC_SANITIZE)
      target_compile_definitions(test_ui-imgui PRIVATE CC_SANITIZED=1)  # the GPU driver left out (it leaks at exit)
    endif()
  endif()
endif()
