// SPDX-License-Identifier: Apache-2.0
// A CivilCraft call script (libs/civil run_script: units, points, TIN surfaces and their contours, bands, slopes,
// outlines…) with hostile arguments — NaN, infinities, duplicates, crossings, odd shapes: a result or an error for
// every call, never a crash, a hang or a sanitizer report.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/civil/script.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 16)) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;
  const std::string_view text = cc::base::as_text(std::as_bytes(std::span(data, size)));
  cc::base::JsonLimits limits;
  limits.max_depth = 32;
  const auto script = cc::base::parse_json(text, limits);
  if (!script) {
    return 0;
  }
  (void)cc::civil::run_script(*script, kWords);
  return 0;
}
