# Clang + libc++ + lld (Linux). The vcpkg triplet x64-linux-cc chains to this file, so dependencies and our code
# share one standard library. `clang`/`clang++` come from PATH (tools/env.sh puts LLVM 22 first; CI does the same).
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
