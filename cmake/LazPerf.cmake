# laz-perf (Apache-2.0, hobuinc/laz-perf): LAZ (LASzip-compressed LAS) point decoding for point clouds, the library
# CadCraft 0.5 runs as WebAssembly (laz-perf 0.0.7 on npm). 3.4.0, its source archive pinned by commit and
# hash-checked; not in vcpkg (the laszip port is LGPL). Only cpp/lazperf is built; warnings and tidy off for it, as for
# any vendored code. Files from strangers reach it: fuzz target `cloud` runs it under ASan + UBSan.
include_guard(GLOBAL)
set(_lp_dir ${CMAKE_BINARY_DIR}/_deps/laz-perf)
set(_lp_tar ${_lp_dir}/laz-perf-3.4.0.tar.gz)
set(_lp_sha 17df34ca64cc60e107f0c214db4729c54a514df4e32de5bc1b8b7b7c5a805a56)
set(_lp_src ${_lp_dir}/laz-perf-b7bbe26109dc986f42d4fc80b8de3d2b6ca634ce)
if(EXISTS ${_lp_tar})
  file(SHA256 ${_lp_tar} _have)
endif()
if(NOT EXISTS ${_lp_tar} OR NOT _have STREQUAL _lp_sha)
  file(DOWNLOAD
    https://codeload.github.com/hobuinc/laz-perf/tar.gz/b7bbe26109dc986f42d4fc80b8de3d2b6ca634ce
    ${_lp_tar} EXPECTED_HASH SHA256=${_lp_sha} TLS_VERIFY ON STATUS _st)
  list(GET _st 0 _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "laz-perf: download failed: ${_st}")
  endif()
  file(REMOVE_RECURSE ${_lp_src})
endif()
if(NOT EXISTS ${_lp_src}/cpp/lazperf/readers.hpp)
  file(ARCHIVE_EXTRACT INPUT ${_lp_tar} DESTINATION ${_lp_dir} PATTERNS "*/cpp/lazperf/*" "*/COPYING")
  file(REMOVE ${_lp_src}/.cc-patched)
endif()

# Our patches (laz-perf trusts its input; a damaged or crafted LAZ must not read past a buffer or allocate gigabytes —
# found with the hardened standard library on bit-flipped files, 2026-10-20). Each anchor must be found, or the
# configure stops: a laz-perf update re-checks them. What 0.5's WebAssembly build does there instead (reads leftover
# heap) is not reproducible, so a damaged LAZ's points are not compared (docs/gaps.md).
function(_lp_patch file old new)
  file(READ ${_lp_src}/cpp/lazperf/${file} _c)
  string(FIND "${_c}" "${old}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "laz-perf: patch anchor not found in ${file}: ${old}")
  endif()
  string(REPLACE "${old}" "${new}" _c "${_c}")
  file(WRITE ${_lp_src}/cpp/lazperf/${file} "${_c}")
endfunction()
if(NOT EXISTS ${_lp_src}/.cc-patched)
  # 1. a chunk's bytes read past their end: zeros, not the next allocation
  _lp_patch(streams.hpp [=[        return buf[idx++];]=]
                        [=[        if (idx >= buf.size()) { ++idx; return 0; }  // CadCraft: past the end reads 0
        return buf[idx++];]=])
  # 2. a short read of the file leaves no byte undefined
  _lp_patch(streams.hpp [=[        unsigned char c;
        inCb_(&c, 1);]=]
                        [=[        unsigned char c = 0;  // CadCraft: a short read leaves 0
        inCb_(&c, 1);]=])
  # 3. a layer's byte count comes from the file: at most 256 MB
  _lp_patch(streams.hpp [=[        buf.resize(bytes);
        in.getBytes(buf.data(), bytes);]=]
                        [=[        if (bytes > (size_t(1) << 28))  // CadCraft: a layer's size is the file's word
            throw error("LAZ layer too large");
        buf.resize(bytes);
        in.getBytes(buf.data(), bytes);]=])
  # 4. the chunk count comes from the file: at most 16 M chunks (with 50 000 points each, far past 2^32 points)
  _lp_patch(readers.cpp [=[    chunks.resize(chunk_table_header.chunk_count + 1);]=]
                        [=[    if (chunk_table_header.chunk_count > (1u << 24))  // CadCraft: the file's word
        throw error("Bad chunk table. Too many chunks.");
    chunks.resize(chunk_table_header.chunk_count + 1);]=])
  # 5. more points than the chunk table covers: an error, not a walk past the table
  _lp_patch(readers.cpp [=[            if (current_chunk == nullptr)
                current_chunk = chunks.data();
            else
                current_chunk++;]=]
                        [=[            if (current_chunk == nullptr)
            {
                if (chunks.empty())  // CadCraft: no chunk to read from
                    throw error("No chunks to read.");
                current_chunk = chunks.data();
            }
            else
            {
                if (current_chunk + 1 >= chunks.data() + chunks.size())  // CadCraft: past the table
                    throw error("More points than the chunk table holds.");
                current_chunk++;
            }]=])
  # 6. an extra-bytes record's size comes from the file: at most 16 MB
  _lp_patch(vlr.cpp [=[    std::vector<char> buf(byteSize);
    in.read(buf.data(), buf.size());]=]
                    [=[    if (byteSize < 0 || byteSize > (1 << 24))  // CadCraft: the record's size is the file's word
        throw std::length_error("Extra bytes VLR too large");
    std::vector<char> buf(byteSize);
    in.read(buf.data(), buf.size());]=])
  _lp_patch(vlr.cpp [=[#include <string>]=] [=[#include <stdexcept>
#include <string>]=])
  # 7. seeks to a position from the file (a chunk table offset) are checked as numbers before any pointer is formed
  _lp_patch(charbuf.cpp [=[        if (pos >= egptr() - eback())
            return -1;]=]
                        [=[        if (pos < 0 || pos >= egptr() - eback())  // CadCraft: not before the buffer
            return -1;]=])
  _lp_patch(charbuf.cpp [=[        if (pos > epptr() - m_buf)
            return -1;]=]
                        [=[        if (pos < 0 || pos > epptr() - m_buf)  // CadCraft: not before the buffer
            return -1;]=])
  _lp_patch(charbuf.cpp [=[    std::ios::pos_type pos;
    char *cpos = nullptr;
    if (which & std::ios_base::in)
    {
        switch (dir)
        {
        case std::ios::beg:
            cpos = eback() + off - m_bufOffset;
            break;
        case std::ios::cur:
            cpos = gptr() + off;
            break;
        case std::ios::end:
            cpos = egptr() - off;
            break;
        default:
            break;  // Should never happen.
        }
        if (cpos < eback() || cpos > egptr())
            return -1;
        setg(eback(), cpos, egptr());
        pos = cpos - eback();
    }
    if (which & std::ios_base::out)
    {
        switch (dir)
        {
        case std::ios::beg:
            cpos = m_buf + off - m_bufOffset;
            break;
        case std::ios::cur:
            cpos = pptr() + off;
            break;
        case std::ios::end:
            cpos = egptr() - off;
            break;
        default:
            break;  // Should never happen.
        }
        if (cpos < m_buf || cpos > epptr())
            return -1;
        setp(cpos, epptr());
        pos = cpos - m_buf;
    }
    return pos;]=]
                        [=[    // CadCraft: the target as an offset into the buffer, checked before a pointer is formed
    std::ios::pos_type pos;
    if (which & std::ios_base::in)
    {
        const std::ios::off_type size = egptr() - eback();
        std::ios::off_type at = -1;
        switch (dir)
        {
        case std::ios::beg:
            at = off - std::ios::off_type(m_bufOffset);
            break;
        case std::ios::cur:
            at = (gptr() - eback()) + off;
            break;
        case std::ios::end:
            at = size - off;
            break;
        default:
            break;  // Should never happen.
        }
        if (at < 0 || at > size)
            return -1;
        setg(eback(), eback() + at, egptr());
        pos = at;
    }
    if (which & std::ios_base::out)
    {
        const std::ios::off_type size = epptr() - m_buf;
        std::ios::off_type at = -1;
        switch (dir)
        {
        case std::ios::beg:
            at = off - std::ios::off_type(m_bufOffset);
            break;
        case std::ios::cur:
            at = (pptr() - m_buf) + off;
            break;
        case std::ios::end:
            at = size - off;
            break;
        default:
            break;  // Should never happen.
        }
        if (at < 0 || at > size)
            return -1;
        setp(m_buf + at, epptr());
        pos = at;
    }
    return pos;]=])
  file(TOUCH ${_lp_src}/.cc-patched)
endif()
file(GLOB _lp_srcs ${_lp_src}/cpp/lazperf/*.cpp ${_lp_src}/cpp/lazperf/detail/*.cpp)
add_library(lazperf STATIC ${_lp_srcs})
target_include_directories(lazperf SYSTEM PUBLIC ${_lp_src}/cpp)
target_compile_features(lazperf PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(lazperf PRIVATE /W0)
else()
  target_compile_options(lazperf PRIVATE -w)
endif()
set_target_properties(lazperf PROPERTIES CXX_CLANG_TIDY "" POSITION_INDEPENDENT_CODE ON)
# its asserts are off in every build, as in 0.5's WebAssembly (a release build): one fires on a damaged chunk table, and
# no input may abort the app
target_compile_definitions(lazperf PRIVATE NDEBUG)
# it decodes files from strangers: the same hardened standard library as our code, and under the sanitizer and fuzz
# builds AddressSanitizer and coverage (UBSan stays on our code: its reports would fail the build on upstream style)
if(CC_HAVE_LIBCXX)
  target_compile_definitions(lazperf PRIVATE
    $<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)
elseif(CC_HAVE_LIBSTDCXX)
  target_compile_definitions(lazperf PRIVATE _GLIBCXX_ASSERTIONS)
elseif(MSVC)
  target_compile_definitions(lazperf PRIVATE _MSVC_STL_HARDENING=1)
endif()
if(NOT MSVC)
  target_compile_options(lazperf PRIVATE -ffp-contract=off)
endif()
if(NOT MSVC AND NOT EMSCRIPTEN)
  if(CC_SANITIZE MATCHES "address")
    target_compile_options(lazperf PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  endif()
  if(CC_FUZZ)
    target_compile_options(lazperf PRIVATE -fsanitize=address,fuzzer-no-link)
  endif()
endif()
