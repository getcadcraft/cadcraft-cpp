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
if(NOT MSVC)
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
if(EMSCRIPTEN)
  set(CC_BUILD_TESTS OFF)
endif()

if(CC_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  include(Catch)
  enable_testing()
endif()

# Libraries in layer order (cc_library refuses a dependency that is not defined yet).
add_subdirectory(libs/base)       # L0
add_subdirectory(libs/io-dwg)     # L3

if(CC_BUILD_APPS AND NOT EMSCRIPTEN)
  add_subdirectory(apps/cadcraft-cli)
endif()
if(CC_FUZZ)
  add_subdirectory(fuzz)
endif()
if(CC_GATE)
  enable_testing()
  include(Gate)
endif()
