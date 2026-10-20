# wuffs (Apache-2.0 OR MIT): memory-safe PNG and JPEG decoding for pictures on screen (AGENTS.md section 3: "wuffs for
# PNG/JPEG decode, not stb_image"). v0.4.0-alpha.10 (2026-06-23), its single-file release pinned by commit and
# hash-checked; not in vcpkg at our baseline. Built as C++ (the project has no C) with only the modules we use
# (third_party/wuffs/wuffs_config.h); warnings off for it, as for any vendored code.
include_guard(GLOBAL)
set(_wuffs_dir ${CMAKE_BINARY_DIR}/_deps/wuffs)
set(_wuffs_c ${_wuffs_dir}/wuffs-v0.4.c)
set(_wuffs_sha 1f8039ef82911604c063f6ac2ed57254bdb17d742aebdeae06356530d4a0fde7)
if(EXISTS ${_wuffs_c})
  file(SHA256 ${_wuffs_c} _have)
endif()
if(NOT EXISTS ${_wuffs_c} OR NOT _have STREQUAL _wuffs_sha)
  file(DOWNLOAD
    https://raw.githubusercontent.com/google/wuffs/ba25980637db55730c62b466f188fae33b9289f6/release/c/wuffs-v0.4.c
    ${_wuffs_c} EXPECTED_HASH SHA256=${_wuffs_sha} TLS_VERIFY ON STATUS _st)
  list(GET _st 0 _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "wuffs: download failed: ${_st}")
  endif()
endif()
add_library(wuffs STATIC ${PROJECT_SOURCE_DIR}/third_party/wuffs/wuffs_impl.cpp)
target_include_directories(wuffs SYSTEM PUBLIC ${_wuffs_dir} ${PROJECT_SOURCE_DIR}/third_party/wuffs)
target_compile_features(wuffs PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(wuffs PRIVATE /W0)
else()
  target_compile_options(wuffs PRIVATE -w)
endif()
set_target_properties(wuffs PROPERTIES CXX_CLANG_TIDY "" POSITION_INDEPENDENT_CODE ON)
