# Emscripten for the wasm32-emscripten-cc dependencies: vcpkg applies VCPKG_C(XX)_FLAGS only through its own toolchains,
# so a chain-loaded one sets them itself. Wasm exceptions, wasm setjmp/longjmp (FreeType), threads, SIMD — the same as
# our code (the linker refuses to mix objects with and without shared-memory atomics).
include($ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake)
set(_cc_wasm "-fwasm-exceptions -sSUPPORT_LONGJMP=wasm -pthread -msimd128")
string(APPEND CMAKE_C_FLAGS_INIT " ${_cc_wasm}")
string(APPEND CMAKE_CXX_FLAGS_INIT " ${_cc_wasm}")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -pthread")
