// SPDX-License-Identifier: Apache-2.0
#include "cc/base/random.hpp"

#include <algorithm>
#include <cstddef>

#if defined(_WIN32)
// clang-format off
#include <windows.h>  // before bcrypt.h
#include <bcrypt.h>
// clang-format on
#elif defined(__APPLE__)
#include <stdlib.h>  // arc4random_buf
#elif defined(__EMSCRIPTEN__)
#include <unistd.h>  // getentropy
#else
#include <sys/random.h>
#endif

namespace cc::base {

void os_random(std::span<std::uint8_t> out) noexcept {
#if defined(_WIN32)
  for (std::size_t at = 0; at < out.size();) {
    const auto n = static_cast<ULONG>(std::min<std::size_t>(out.size() - at, 1U << 20U));
    if (BCryptGenRandom(nullptr, out.subspan(at).data(), n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
      at += n;
    }
  }
#elif defined(__APPLE__)
  arc4random_buf(out.data(), out.size());
#elif defined(__EMSCRIPTEN__)
  for (std::size_t at = 0; at < out.size();) {
    const std::size_t n = std::min<std::size_t>(out.size() - at, 256);  // getentropy gives at most 256 bytes a call
    if (getentropy(out.subspan(at).data(), n) == 0) {
      at += n;
    }
  }
#else
  for (std::size_t at = 0; at < out.size();) {
    const ssize_t n = getrandom(out.subspan(at).data(), out.size() - at, 0);
    if (n > 0) {
      at += static_cast<std::size_t>(n);
    }
  }
#endif
}

}  // namespace cc::base
