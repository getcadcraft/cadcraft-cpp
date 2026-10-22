// SPDX-License-Identifier: Apache-2.0
// The PartCraft call scripts (cc::partcraft::run_script, what `cadcraft-cli part` runs): any JSON — ops with arguments
// of any type, records with missing or odd members — gives a result or an {err} per call, never a crash, a hang or a
// sanitizer report.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/partcraft/script.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 4096) {  // a few hundred calls: sectionRows answers 1335 records each
    return 0;
  }
  const std::string_view text = cc::base::as_text(std::as_bytes(std::span(data, size)));
  cc::base::JsonLimits limits;
  limits.max_depth = 64;
  const auto script = cc::base::parse_json(text, limits);
  if (!script) {
    return 0;
  }
  (void)cc::partcraft::run_script(*script);
  return 0;
}
