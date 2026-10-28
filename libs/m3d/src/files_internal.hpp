// SPDX-License-Identifier: Apache-2.0
// The checks model.js runs on untrusted files (isNum, isPt, Number.isInteger) and the JSON shapes it writes.
#pragma once

#include <algorithm>
#include <span>

#include "cc/base/value.hpp"
#include "cc/m3d/model.hpp"

namespace cc::m3d::detail {

/// Number.isInteger(v)
[[nodiscard]] bool is_int(const base::Value& v);
/// isNum: a finite number under 1e7 in size
[[nodiscard]] bool is_num(const base::Value& v);
/// isPt: [x, y, z] of isNum
[[nodiscard]] bool is_pt(const base::Value& p);
[[nodiscard]] Vec3 pt_of(const base::Value& p);
[[nodiscard]] base::Value pt_value(const Vec3& p);
[[nodiscard]] base::Value ids_value(std::span<const Id> ids);
[[nodiscard]] base::Value mat_value(const Mat4& m);
[[nodiscard]] bool truthy(const base::Value* v);
/// A number as an id: truncated; -1 (no table has it) when it is not finite or past ±9e15.
[[nodiscard]] inline Id to_id(double x) {
  return x == x && x > -9e15 && x < 9e15 ? static_cast<Id>(x) : -1;  // NOLINT(misc-redundant-expression): NaN
}
/// A number as a count in [lo, hi] (NaN: lo), as a script's sides, segments and axes are read.
[[nodiscard]] inline int to_count(double x, int lo, int hi) {
  if (!(x == x)) {  // NOLINT(misc-redundant-expression): NaN
    return lo;
  }
  return static_cast<int>(std::clamp(x, static_cast<double>(lo), static_cast<double>(hi)));
}

}  // namespace cc::m3d::detail
