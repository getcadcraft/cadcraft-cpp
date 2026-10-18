cmake_minimum_required(VERSION 3.28)
project(cadcraft_cpp VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "Clang 18+ is required (found ${CMAKE_CXX_COMPILER_VERSION}); source tools/env.sh")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "GCC 13+ is required (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# Build profiles (AGENTS.md section 3): Debug -O1 for our code; Release -O2 + ThinLTO + split debug info.
if(EMSCRIPTEN)
  # the web build ships: -O3, no debug info in the .wasm (AGENTS.md section 10, web performance)
  set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-O3")
elseif(NOT MSVC)
  set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g")
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -DNDEBUG")
  if(NOT EMSCRIPTEN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -gsplit-dwarf")
  endif()
endif()
option(CC_LTO "ThinLTO in Release builds" ON)
if(CC_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " -flto=thin")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -flto=thin")
endif()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(Policy)
include(CcLibrary)

option(CC_BUILD_TESTS "Build the Catch2 suites" ON)
option(CC_BUILD_APPS "Build the apps (layer 7)" ON)
option(CC_GATE "Register the quality-gate checks as tests (the ci preset)" OFF)
option(CC_BUILD_RENDER "The text and render libraries (FreeType: the vcpkg 'gui' feature)" ON)
option(CC_BUILD_GUI "The viewer UI and the desktop and web apps (SDL3, Dawn, Dear ImGui)" ON)
if(CC_BUILD_GUI)
  set(CC_BUILD_RENDER ON)
endif()

if(EMSCRIPTEN)
  set(CC_BUILD_TESTS OFF)
endif()

if(CC_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  include(Catch)
  enable_testing()
endif()

if(CC_BUILD_RENDER)
  find_package(Freetype REQUIRED)
  find_package(ZLIB REQUIRED)
endif()
if(CC_BUILD_GUI)
  find_package(SDL3 CONFIG REQUIRED)
  find_package(Dawn CONFIG REQUIRED)
  include(ImGui)  # Dear ImGui (docking), pinned by commit: cmake/ImGui.cmake
endif()

# Libraries in layer order (cc_library refuses a dependency that is not defined yet).
add_subdirectory(libs/base)       # L0
add_subdirectory(libs/i18n)       # L1
add_subdirectory(libs/drawing)    # L2
add_subdirectory(libs/cad)        # L2
add_subdirectory(libs/m3d)        # L2
if(CC_BUILD_RENDER)
  add_subdirectory(libs/text)     # L2
endif()
add_subdirectory(libs/io-dwg)     # L3
add_subdirectory(libs/io-dxf)     # L3
add_subdirectory(libs/doc)        # L3
if(CC_BUILD_RENDER)
  add_subdirectory(libs/render)   # L3
  add_subdirectory(libs/image)    # L3
endif()
add_subdirectory(libs/importer)   # L4
add_subdirectory(libs/save)       # L4
add_subdirectory(libs/engine)     # L5
add_subdirectory(libs/cmd)        # L5
if(CC_BUILD_GUI)
  add_subdirectory(libs/ui-imgui) # L6
endif()

if(CC_BUILD_APPS AND NOT EMSCRIPTEN)
  add_subdirectory(apps/cadcraft-cli)
  if(CC_BUILD_GUI)
    add_subdirectory(apps/cadcraft)
    add_subdirectory(apps/cadcraft-shot)
  endif()
  if(NOT WIN32)
    add_subdirectory(apps/cadcraft-importer)  # the Windows helper (job object) is not built yet: docs/gaps.md
  endif()
endif()
if(CC_BUILD_APPS AND EMSCRIPTEN AND CC_BUILD_GUI)
  add_subdirectory(apps/cadcraft-web)
endif()
if(CC_FUZZ)
  add_subdirectory(fuzz)
endif()
if(CC_GATE)
  enable_testing()
  include(Gate)
endif()
