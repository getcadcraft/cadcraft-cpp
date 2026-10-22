// SPDX-License-Identifier: Apache-2.0
// The few JavaScript number semantics lib/shapes needs: Math.min / Math.max (NaN wins, -0 below +0), String(number),
// Number(x.toFixed(n)), and a UTF-16 slice (String#slice on text that is mostly BMP).
#pragma once

#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "cc/base/js.hpp"
#include "cc/base/value.hpp"

namespace cc::shapes {

[[nodiscard]] inline double js_min(double a, double b) noexcept {
  if (std::isnan(a) || std::isnan(b)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (a == b) {
    return std::signbit(a) ? a : b;
  }
  return a < b ? a : b;
}
[[nodiscard]] inline double js_max(double a, double b) noexcept {
  if (std::isnan(a) || std::isnan(b)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (a == b) {
    return std::signbit(a) ? b : a;
  }
  return a > b ? a : b;
}
/// String(v) of a number.
[[nodiscard]] inline std::string ns(double v) {
  std::string s;
  base::append_number(s, v);
  return s;
}
/// Number(Number(v).toFixed(digits)).
[[nodiscard]] inline double fixed(double v, int digits) {
  return base::js_number(base::js_to_fixed(v, digits));
}
/// s.slice(0, n) counted in UTF-16 code units (a pair cut in half leaves its lead surrogate, as JS does).
[[nodiscard]] std::string utf16_prefix(std::string_view s, std::size_t n);

}  // namespace cc::shapes
