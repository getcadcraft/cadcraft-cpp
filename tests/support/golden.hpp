// SPDX-License-Identifier: Apache-2.0
// The parity kit's comparison for golden tests: its canon() (undefined dropped, NaN/Inf as "$NaN"/"$Inf"/"$-Inf",
// -0 → 0) and its diff with the DWG tolerance (abs 1e-9 + rel 1e-12). `want` is the JS reference, `got` ours.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::test {

using cc::base::Value;

// NOLINTNEXTLINE(misc-no-recursion) — bounded by the model's depth
inline void compare(const Value& want, const Value& got, const std::string& path, std::vector<std::string>& out) {
  if (out.size() >= 20) {
    return;
  }
  if (got.is_number() && std::isnan(got.as_number()) && want.is_string() && want.as_string() == "$NaN") {
    return;
  }
  if (got.is_number() && std::isinf(got.as_number()) && want.is_string() &&
      want.as_string() == (got.as_number() > 0 ? "$Inf" : "$-Inf")) {
    return;
  }
  if (want.is_number() && got.is_number()) {
    const double a = want.as_number();
    const double b = got.as_number();
    if (a != b && !(std::abs(a - b) <= 1e-9 + 1e-12 * std::max(std::abs(a), std::abs(b)))) {
      out.push_back(path + ": " + std::to_string(a) + " vs " + std::to_string(b));
    }
    return;
  }
  if (want.kind() != got.kind()) {
    if (want.is_null() && got.is_undefined()) {
      return;  // an undefined array item is null in JSON
    }
    out.push_back(path + ": type differs");
    return;
  }
  if (want.is_array()) {
    const auto& a = want.as_array();
    const auto& b = got.as_array();
    if (a.size() != b.size()) {
      out.push_back(path + ".length: " + std::to_string(a.size()) + " vs " + std::to_string(b.size()));
    }
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
      compare(a[i], b[i], path + "[" + std::to_string(i) + "]", out);
    }
    return;
  }
  if (want.is_object()) {
    for (const auto& m : want.as_object()) {
      const Value* g = got.find(m.key);
      if (g == nullptr || g->is_undefined()) {
        out.push_back(path + "." + m.key + ": missing");
        continue;
      }
      compare(m.value, *g, path + "." + m.key, out);
    }
    for (const auto& m : got.as_object()) {
      if (!m.value.is_undefined() && !want.has(m.key)) {
        out.push_back(path + "." + m.key + ": extra");
      }
    }
    return;
  }
  if (!(want == got)) {
    out.push_back(path + ": value differs");
  }
}

}  // namespace cc::test
