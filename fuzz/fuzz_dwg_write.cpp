// SPDX-License-Identifier: Apache-2.0
// The DWG writer on whatever model the reader makes of hostile bytes: write_dwg must never crash, hang or trip a
// sanitizer, and its file must read back. The first byte picks the version written (the rest is the file).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/dwg/read.hpp"
#include "cc/dwg/write.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) {
    return 0;
  }
  static constexpr std::array<const char*, 6> kVersions = {"AC1015", "AC1018", "AC1021", "AC1024", "AC1027", "AC1032"};
  const auto bytes = std::as_bytes(std::span(data, size)).subspan(1);
  const cc::base::Value model = cc::dwg::read_dwg(bytes, {.threads = 1});
  cc::dwg::WriteOptions o;
  o.version = kVersions[data[0] % kVersions.size()];
  o.created_ms = 0;
  o.random = [](std::span<std::uint8_t> b) {
    for (std::uint8_t& x : b) {
      x = 0x5A;
    }
  };
  const cc::dwg::WriteResult r = cc::dwg::write_dwg(model, o);
  const cc::base::Value back = cc::dwg::read_dwg(std::as_bytes(std::span(r.bytes)), {.threads = 1});
  (void)back;
  return 0;
}
