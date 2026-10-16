# Linux x64, GCC + libstdc++ (cmake/toolchains/gcc.cmake), static libraries, release builds only.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_ENV_PASSTHROUGH CC_GCC CC_GXX)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/toolchains/gcc.cmake")
