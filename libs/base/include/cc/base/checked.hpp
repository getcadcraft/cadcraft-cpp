// SPDX-License-Identifier: Apache-2.0
// Checked integer arithmetic and conversions (AGENTS.md section 4, integer safety).
#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <utility>

#include "cc/base/error.hpp"

namespace cc::base {

template <std::unsigned_integral T>
[[nodiscard]] constexpr Result<T> checked_add(T a, T b) {
  if (a > std::numeric_limits<T>::max() - b) {
    return fail(Errc::overflow, "addition overflows");
  }
  return static_cast<T>(a + b);
}

template <std::unsigned_integral T>
[[nodiscard]] constexpr Result<T> checked_mul(T a, T b) {
  if (a != 0 && b > std::numeric_limits<T>::max() / a) {
    return fail(Errc::overflow, "multiplication overflows");
  }
  return static_cast<T>(a * b);
}

/// Converts between integer types when the value fits; otherwise an error.
template <std::integral To, std::integral From>
[[nodiscard]] constexpr Result<To> narrow(From v) {
  if (!std::in_range<To>(v)) {
    return fail(Errc::overflow, "value does not fit");
  }
  return static_cast<To>(v);
}

/// Converts a double to an integer type only when it is finite and in range (no NaN-to-int).
template <std::integral To>
[[nodiscard]] Result<To> to_int(double v) {
  if (!std::isfinite(v)) {
    return fail(Errc::invalid, "not a finite number");
  }
  const auto t = std::trunc(v);
  // The bounds are exact powers of two (or zero), so the comparisons are exact: max() converts to 2^digits for
  // 64-bit types (rounded up) and is exact for smaller ones, where adding 1 reaches 2^digits.
  constexpr auto lo = static_cast<double>(std::numeric_limits<To>::min());
  constexpr double hi = std::numeric_limits<To>::digits >= 53
                            ? static_cast<double>(std::numeric_limits<To>::max())
                            : static_cast<double>(std::numeric_limits<To>::max()) + 1.0;
  if (t < lo || t >= hi) {
    return fail(Errc::overflow, "number out of range");
  }
  return static_cast<To>(t);
}

/// A double to an integer type without undefined behaviour: truncated toward zero, NaN → 0, values beyond the type's
/// range clamp to its min/max. For numbers read from files (JS numbers in the reference) used as counts or offsets;
/// callers that need to refuse out-of-range values use to_int() instead.
template <std::integral To>
[[nodiscard]] constexpr To clamp_cast(double v) noexcept {
  if (!(v == v)) {  // NaN (no <cmath> in constexpr)
    return 0;
  }
  constexpr auto lo = static_cast<double>(std::numeric_limits<To>::min());
  constexpr double hi = std::numeric_limits<To>::digits >= 53
                            ? static_cast<double>(std::numeric_limits<To>::max())
                            : static_cast<double>(std::numeric_limits<To>::max()) + 1.0;
  if (v <= lo) {
    return std::numeric_limits<To>::min();
  }
  if (v >= hi) {
    return std::numeric_limits<To>::max();
  }
  return static_cast<To>(v);
}

}  // namespace cc::base
