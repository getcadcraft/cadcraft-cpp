# Clang + libc++ + lld (Linux). The vcpkg triplet x64-linux-cc chains to this file, so dependencies and our code
# share one standard library. `clang`/`clang++` come from PATH (tools/env.sh puts LLVM 22 first; CI does the same).
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
# ThinLTO objects need LLVM's archiver (the system binutils may be older than the compiler).
find_program(CC_LLVM_AR NAMES llvm-ar-22 llvm-ar)
find_program(CC_LLVM_RANLIB NAMES llvm-ranlib-22 llvm-ranlib)
if(CC_LLVM_AR)
  set(CMAKE_AR "${CC_LLVM_AR}" CACHE FILEPATH "" FORCE)
endif()
if(CC_LLVM_RANLIB)
  set(CMAKE_RANLIB "${CC_LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
endif()
