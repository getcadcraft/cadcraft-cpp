# Linux x64, clang + libc++ (cmake/toolchains/clang-libcxx.cmake), static libraries, release builds only
# (Debug builds of ours link the -O2 dependencies: AGENTS.md section 3, build profiles).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/toolchains/clang-libcxx.cmake")
