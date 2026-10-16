// SPDX-License-Identifier: Apache-2.0
// The whole DWG reader on hostile bytes: read_dwg must never crash, hang or trip a sanitizer, whatever the input.
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/dwg/read.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::as_bytes(std::span(data, size));
  const cc::base::Value model = cc::dwg::read_dwg(bytes);
  (void)model;
  return 0;
}
