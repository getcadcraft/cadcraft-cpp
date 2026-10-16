// SPDX-License-Identifier: Apache-2.0
// The DXF path BimCraft takes on hostile bytes: decode_text then read_dxf_model must never crash, hang or trip a
// sanitizer; whatever it builds must write as JSON.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "cc/base/value.hpp"
#include "cc/dxf/read.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::as_bytes(std::span(data, size));
  const auto model = cc::dxf::read_dxf_model(cc::dxf::decode_text(bytes));
  if (model) {
    std::string out;
    cc::base::write_json(*model, out);
  }
  return 0;
}
