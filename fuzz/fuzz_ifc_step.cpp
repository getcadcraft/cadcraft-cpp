// SPDX-License-Identifier: Apache-2.0
// cc-ifc's STEP reader on hostile bytes: scan_step, then every instance parsed and written as the parity kit's NDJSON
// must never crash, hang or trip a sanitizer.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "cc/ifc/step.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::as_bytes(std::span(data, size));
  const auto ix = cc::ifc::scan_step(bytes);
  if (ix) {
    std::string out = cc::ifc::step_head_json(*ix);
    for (std::size_t i = 0; i < ix->count; ++i) {
      cc::ifc::step_line_json(bytes, *ix, i, out);
    }
  }
  return 0;
}
