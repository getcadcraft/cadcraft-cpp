// SPDX-License-Identifier: Apache-2.0
// What a person types into a Properties field (propexpr.js readValue → expr.js): any text, in every kind and with
// metric, imperial and unset units, gives a value or a reason — never a crash, a hang or a sanitizer report.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "cc/base/reader.hpp"
#include "cc/cmd/props.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 4096) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;  // no catalog: the words are their keys
  const std::string_view text = cc::base::as_text(std::as_bytes(std::span(data, size)));
  for (const std::string_view kind : {"len", "ang", "num", "int"}) {
    for (const auto& [lunits, units] : {std::pair{2.0, ""}, std::pair{4.0, ""}, std::pair{2.0, "meters"}}) {
      const cc::cmd::ReadCtx ctx{
          .lunits = lunits, .units = units, .paper = {}, .gt0 = kind == "int", .min = -1e6, .max = 1e6, .vars = {}};
      (void)cc::cmd::read_value(text, kind, ctx, kWords);
    }
  }
  return 0;
}
