# Dear ImGui, docking branch, pinned by commit (AGENTS.md section 3) and hash-checked: tag v1.92.8-docking. Fetched, not
# taken from vcpkg: the vcpkg port's sdl3/webgpu bindings depend on SDL3 and Dawn with their default features, which pull
# in LGPL (libsystemd) and NCSA (DXC) code. The core and the two backends we use are built as one static library.
include_guard(GLOBAL)
include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/b61e56346a92cfcaf1f43a545ca37b0b32239654.tar.gz
  URL_HASH SHA256=100c1e0a0625c538d79e014dc8e73309c5b25c7ed8183fb91ae621a3681414a4
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_features(imgui PUBLIC cxx_std_17)
target_link_libraries(imgui PUBLIC SDL3::SDL3 dawn::webgpu_dawn)
