# Flags every target of ours gets: warnings as errors, a hardened standard library, compiler hardening and the
# sanitizer/fuzz instrumentation chosen by the preset (AGENTS.md section 4). Dependencies are built by vcpkg with
# their own flags.
#
# Options (set by CMakePresets.json):
#   CC_SANITIZE   ""  | "address;undefined" | "thread"
#   CC_FUZZ       OFF | ON   (clang only: libFuzzer targets, code built with -fsanitize=fuzzer-no-link)
#   CC_WERROR     ON
include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

set(CC_SANITIZE "" CACHE STRING "Sanitizers for our code: address;undefined or thread")
option(CC_FUZZ "Build libFuzzer targets" OFF)
option(CC_WERROR "Warnings are errors" ON)

check_cxx_source_compiles("#include <version>
#ifndef _LIBCPP_VERSION
#error not libc++
#endif
int main() { return 0; }" CC_HAVE_LIBCXX)
check_cxx_source_compiles("#include <version>
#ifndef __GLIBCXX__
#error not libstdc++
#endif
int main() { return 0; }" CC_HAVE_LIBSTDCXX)

set(CC_IS_CLANG OFF)
set(CC_IS_GCC OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
  set(CC_IS_CLANG ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CC_IS_GCC ON)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(CC_X86 ON)
else()
  set(CC_X86 OFF)
endif()

if(CC_FUZZ AND NOT CC_IS_CLANG)
  message(FATAL_ERROR "CC_FUZZ needs clang (libFuzzer)")
endif()

function(cc_apply_policy target)
  # ---- warnings
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    if(CC_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast
      -Wcast-align -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
    if(CC_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  # ---- hardened standard library, every build type
  if(CC_HAVE_LIBCXX)
    target_compile_definitions(${target} PRIVATE
      $<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)
  elseif(CC_HAVE_LIBSTDCXX)
    target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
  elseif(MSVC)
    target_compile_definitions(${target} PRIVATE _MSVC_STL_HARDENING=1)
  endif()

  # ---- compiler hardening
  if(EMSCRIPTEN)
    target_compile_options(${target} PRIVATE -ftrivial-auto-var-init=zero)
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /sdl /guard:cf /Qspectre)
    target_link_options(${target} PRIVATE /guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA)
  else()
    target_compile_options(${target} PRIVATE -fstack-protector-strong -fstack-clash-protection
                                             -ftrivial-auto-var-init=zero -fPIE)
    if(CC_X86)
      target_compile_options(${target} PRIVATE -fcf-protection=full)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      target_compile_options(${target} PRIVATE -mbranch-protection=standard)
    endif()
    # _FORTIFY_SOURCE needs optimisation (Debug is -O1 here) and conflicts with ASan's own interceptors.
    if(CC_SANITIZE STREQUAL "" AND NOT CC_FUZZ)
      target_compile_options(${target} PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
    endif()
    if(NOT APPLE)
      target_link_options(${target} PRIVATE -pie LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack)
    endif()
  endif()

  # ---- sanitizers and fuzzing (clang/gcc)
  if(NOT MSVC AND NOT EMSCRIPTEN)
    if(CC_SANITIZE)
      list(JOIN CC_SANITIZE "," san)
      target_compile_options(${target} PRIVATE -fsanitize=${san} -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=${san})
    endif()
    if(CC_FUZZ)
      target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
      target_compile_definitions(${target} PRIVATE CC_FUZZING=1)
    endif()
  endif()

  # ---- reproducible paths in debug info and __FILE__
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -ffile-prefix-map=${PROJECT_SOURCE_DIR}/=)
  endif()
endfunction()
