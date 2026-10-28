// SPDX-License-Identifier: Apache-2.0
// Studio's control channel (libs/control) on hostile bytes from a connection: the HTTP reader, then control.rs's rules
// with a stand-in hub, as many requests as the bytes hold. Never a crash, never more than the caps.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "cc/control/control.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static cc::control::Running run(47615, "tok");
  const std::span<const std::uint8_t> in(data, size);
  std::size_t at = 0;
  const auto recv = [&](std::span<char> b) -> std::ptrdiff_t {
    const std::size_t n = std::min({b.size(), in.size() - at, std::size_t{97}});  // small reads: every boundary
    for (std::size_t i = 0; i < n; ++i) {
      b[i] = static_cast<char>(in[at + i]);
    }
    at += n;
    return static_cast<std::ptrdiff_t>(n);
  };
  const auto hub = [](const cc::base::Value& m, const std::string&) -> std::optional<std::optional<cc::base::Value>> {
    if (!m.has("id")) {
      return std::optional<cc::base::Value>{};
    }
    return std::optional<cc::base::Value>{m};
  };
  std::string carry;
  for (int i = 0; i < 32; ++i) {
    auto req = cc::control::read_request(recv, carry);
    if (!req) {
      break;
    }
    req->headers.emplace_back("Authorization", "Bearer tok");  // reach past the token check too
    (void)cc::control::write_response(run.handle(*req, hub));
  }
  return 0;
}
