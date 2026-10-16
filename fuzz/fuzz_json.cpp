// SPDX-License-Identifier: Apache-2.0
// parse_json on hostile text; whatever parses must write back and parse again to the same value.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::as_bytes(std::span(data, size));
  const std::string_view text = cc::base::as_text(bytes);
  const auto v = cc::base::parse_json(text);
  if (v) {
    const std::string back = cc::base::to_json(*v);
    const auto again = cc::base::parse_json(back);
    if (!again || cc::base::to_json(*again) != back) {
      std::abort();
    }
  }
  return 0;
}
