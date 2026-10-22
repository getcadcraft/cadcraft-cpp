# Manifold (Apache-2.0, github.com/elalish/manifold) and Clipper2 (BSL-1.0, github.com/AngusJohnson/Clipper2): the
# mesh booleans and 2D cross sections behind cc::solid (libs/solid, `SolidOps`). CadCraft 0.5 runs the same code as
# WebAssembly: manifold-3d 3.5.4 from npm (`vendor/manifold-3d/3.5.4` in the frozen copy), built from tag v3.5.4
# (commit ce50d78) with the Clipper2 commit its build fetches (46f6391). Both source archives are pinned by commit and
# hash-checked; not from vcpkg (its manifold is 3.5.2 and links TBB, and its clipper2 is another commit).
#
# Built so that it computes what the WebAssembly computes (tools/parity/manifold.mjs compares them bit for bit):
# - sequential (MANIFOLD_PAR=-1, no TBB), as the npm build;
# - no contraction into FMA (-ffp-contract=off, as Manifold's own build asks), no fast-math;
# - Manifold's trigonometry is its own (include/manifold/math.h: musl's), so no libm difference reaches it;
# - std::sort / std::stable_sort from libc++: libc++ 22's sort.h is libc++ 21's (the wasm's), and its heap changes only
#   reorder arithmetic values that are equal anyway. A libstdc++ or MSVC build sorts equal keys in another order, so
#   only the libc++ builds are held to the parity (docs/gaps.md).
# Warnings and tidy are off for it, as for any vendored code; it gets the hardened standard library and the sanitizers.
include_guard(GLOBAL)
set(_mf_dir ${CMAKE_BINARY_DIR}/_deps/manifold)
set(_mf_commit ce50d78021d64507f89e8c9fc2c2e51018117857)
set(_mf_sha c7b3626593380866b599f7b5927de857d4780e94fdb97bf2f015a879d7084537)
set(_c2_commit 46f639177fe418f9689e8ddb74f08a870c71f5b4)
set(_c2_sha 73f5783e0c88299976334f48e3e356c756b96212c652ecc8bceec3bd99455bcb)

function(_mf_fetch name url sha tar)
  if(EXISTS ${tar})
    file(SHA256 ${tar} _have)
  endif()
  if(NOT EXISTS ${tar} OR NOT _have STREQUAL sha)
    file(DOWNLOAD ${url} ${tar} EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON STATUS _st)
    list(GET _st 0 _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "${name}: download failed: ${_st}")
    endif()
  endif()
endfunction()

set(_mf_tar ${_mf_dir}/manifold-${_mf_commit}.tar.gz)
set(_c2_tar ${_mf_dir}/clipper2-${_c2_commit}.tar.gz)
_mf_fetch(manifold https://codeload.github.com/elalish/manifold/tar.gz/${_mf_commit} ${_mf_sha} ${_mf_tar})
_mf_fetch(clipper2 https://codeload.github.com/AngusJohnson/Clipper2/tar.gz/${_c2_commit} ${_c2_sha} ${_c2_tar})
set(_mf_src ${_mf_dir}/manifold-${_mf_commit})
set(_c2_src ${_mf_dir}/Clipper2-${_c2_commit}/CPP/Clipper2Lib)
# The marker holds the patch set's number (as cmake/LazPerf.cmake): a tree patched by an older set is extracted again.
set(_mf_patchset 1)
if(EXISTS ${_mf_src}/.cc-patched)
  file(READ ${_mf_src}/.cc-patched _mf_have)
  if(NOT _mf_have STREQUAL "${_mf_patchset}")
    file(REMOVE_RECURSE ${_mf_src})
  endif()
endif()
if(NOT EXISTS ${_mf_src}/src/manifold.cpp)
  file(ARCHIVE_EXTRACT INPUT ${_mf_tar} DESTINATION ${_mf_dir} PATTERNS "*/src/*" "*/include/*" "*/LICENSE")
  file(REMOVE ${_mf_src}/.cc-patched)
endif()
function(_mf_patch file old new)
  file(READ ${_mf_src}/${file} _c)
  string(FIND "${_c}" "${old}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "manifold: patch anchor not found in ${file}: ${old}")
  endif()
  string(REPLACE "${old}" "${new}" _c "${_c}")
  file(WRITE ${_mf_src}/${file} "${_c}")
endfunction()
if(NOT EXISTS ${_mf_src}/.cc-patched)
  # 1. AssembleHalfedges walks a face's outline with std::multimap::find. Up to libc++ 21 (the wasm's) find returns
  #    the first of equal keys; libc++ 22 returns whichever its tree search meets first, so a face whose outline
  #    visits a vertex twice was assembled in another order (found by tools/parity/manifold.mjs; the Rust fork's
  #    notes say the same). lower_bound is the first of equal keys in every libc++. And where no edge continues the
  #    outline (a non-manifold result, hostile input only) the C++ dereferences end(): the walk stops instead.
  _mf_patch(src/face_op.cpp [=[    const auto result = vert_edge.find((start + thisEdge)->endVert);
    DEBUG_ASSERT(result != vert_edge.end(), topologyErr, "non-manifold edge");]=]
                            [=[    const int want = (start + thisEdge)->endVert;  // CadCraft: the first of equal keys
    const auto result = vert_edge.lower_bound(want);
    if (result == vert_edge.end() || result->first != want) break;]=])
  file(WRITE ${_mf_src}/.cc-patched "${_mf_patchset}")
endif()
if(NOT EXISTS ${_c2_src}/src/clipper.engine.cpp)
  file(ARCHIVE_EXTRACT INPUT ${_c2_tar} DESTINATION ${_mf_dir} PATTERNS "*/CPP/Clipper2Lib/*" "*/LICENSE")
endif()

function(_mf_vendored target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W0 /fp:precise)
  else()
    target_compile_options(${target} PRIVATE -w -ffp-contract=off -fno-fast-math)
  endif()
  set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "" POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(${target} PRIVATE NDEBUG)
  if(CC_HAVE_LIBCXX)
    target_compile_definitions(${target} PRIVATE
      $<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)
  elseif(CC_HAVE_LIBSTDCXX)
    target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
  elseif(MSVC)
    target_compile_definitions(${target} PRIVATE _MSVC_STL_HARDENING=1)
  endif()
  if(NOT MSVC AND NOT EMSCRIPTEN)
    if(CC_SANITIZE MATCHES "address")
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    endif()
    if(CC_FUZZ)
      target_compile_options(${target} PRIVATE -fsanitize=address,fuzzer-no-link)
    endif()
  endif()
endfunction()

add_library(clipper2 STATIC ${_c2_src}/src/clipper.engine.cpp ${_c2_src}/src/clipper.offset.cpp
                            ${_c2_src}/src/clipper.rectclip.cpp)
target_include_directories(clipper2 SYSTEM PUBLIC ${_c2_src}/include)
target_compile_features(clipper2 PUBLIC cxx_std_17)
_mf_vendored(clipper2)

add_library(manifold STATIC
  ${_mf_src}/src/boolean3.cpp ${_mf_src}/src/boolean_result.cpp ${_mf_src}/src/constructors.cpp
  ${_mf_src}/src/csg_tree.cpp ${_mf_src}/src/edge_op.cpp ${_mf_src}/src/execution_impl.cpp
  ${_mf_src}/src/face_op.cpp ${_mf_src}/src/impl.cpp ${_mf_src}/src/manifold.cpp ${_mf_src}/src/minkowski.cpp
  ${_mf_src}/src/polygon.cpp ${_mf_src}/src/properties.cpp ${_mf_src}/src/quickhull.cpp ${_mf_src}/src/sdf.cpp
  ${_mf_src}/src/smoothing.cpp ${_mf_src}/src/sort.cpp ${_mf_src}/src/subdivision.cpp ${_mf_src}/src/tree2d.cpp
  ${_mf_src}/src/cross_section/cross_section.cpp)
target_include_directories(manifold SYSTEM PUBLIC ${_mf_src}/include)
target_include_directories(manifold PRIVATE ${_mf_src}/src)
target_compile_definitions(manifold PRIVATE MANIFOLD_PAR=-1 PUBLIC MANIFOLD_CROSS_SECTION)
target_compile_features(manifold PUBLIC cxx_std_17)
target_link_libraries(manifold PRIVATE clipper2)
_mf_vendored(manifold)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fexcess-precision=standard CC_HAVE_EXCESS_PRECISION)
if(CC_HAVE_EXCESS_PRECISION)
  target_compile_options(manifold PRIVATE -fexcess-precision=standard)
endif()
