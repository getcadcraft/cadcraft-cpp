// SPDX-License-Identifier: Apache-2.0
// The JS expressions the writer ports lean on, read off base::Value: `o?.k`, `v ?? d`, `v | 0`, `num(v, d)` (a finite
// number or the default), `(o.k || [])`. Records hold compact points (base::Pt2/Pt3) as well as objects: coordinates
// are read with get / numk, never through ref (which sees members of plain objects only).
#pragma once

#include <cmath>
#include <string_view>
#include <utility>

#include "cc/base/value.hpp"
#include "wbits.hpp"

namespace cc::dwg::w {

using base::Array;

[[nodiscard]] inline const Value& undef() {
  static const Value u;
  return u;
}
[[nodiscard]] inline const Array& empty_array() {
  static const Array a;
  return a;
}
[[nodiscard]] inline bool nullish(const Value& v) noexcept {
  return v.is_undefined() || v.is_null();
}
/// Number.isFinite(v) ? v : d
[[nodiscard]] inline double num(const Value& v, double d = 0) {
  return v.is_number() && std::isfinite(v.as_number()) ? v.as_number() : d;
}
/// o?.k (a copy: works on compact points)
[[nodiscard]] inline Value get(const Value& o, std::string_view k) {
  return o.is_object() ? o.get(k) : Value();
}
/// o.k of a plain object (undefined otherwise), by reference
[[nodiscard]] inline const Value& ref(const Value& o, std::string_view k) {
  if (o.is_object() && !o.is_point()) {
    const Value* p = o.find(k);
    return p != nullptr ? *p : undef();
  }
  return undef();
}
/// num(o?.k, d)
[[nodiscard]] inline double numk(const Value& o, std::string_view k, double d = 0) {
  if (o.is_point()) {
    return num(o.get(k), d);
  }
  return num(ref(o, k), d);
}
/// (v ?? d) used as a number
[[nodiscard]] inline double nnum(const Value& v, double d) {
  if (nullish(v)) {
    return d;
  }
  return v.is_number() ? v.as_number() : base::js_to_number(v);
}
/// v | 0
[[nodiscard]] inline double iv(const Value& v) {
  return static_cast<double>(to_i32(v));
}
/// o.k | 0
[[nodiscard]] inline double ivk(const Value& o, std::string_view k) {
  return iv(o.is_point() ? o.get(k) : ref(o, k));
}
/// !!o.k
[[nodiscard]] inline bool tr(const Value& o, std::string_view k) {
  return o.is_point() ? o.get(k).truthy() : ref(o, k).truthy();
}
/// (o.k || []) as an array
[[nodiscard]] inline const Array& arr(const Value& o, std::string_view k) {
  const Value& v = ref(o, k);
  return v.is_array() ? v.as_array() : empty_array();
}
/// v === x (a number)
[[nodiscard]] inline bool is(const Value& v, double x) {
  return v.is_number() && v.as_number() == x;
}
[[nodiscard]] inline bool is_false(const Value& v) {
  return v.is_bool() && !v.as_bool();
}

/// parseInt(s.substr(at, 2), 16), NaN as 0 (the byte it then becomes)
[[nodiscard]] inline double parse_hex2(std::string_view s, std::size_t at) noexcept {
  double v = 0;
  bool any = false;
  for (std::size_t i = at; i < at + 2 && i < s.size(); ++i) {
    const char c = s[i];
    int d = -1;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    }
    if (d < 0) {
      break;
    }
    v = v * 16 + d;
    any = true;
  }
  return any ? v : 0;
}

/// String(v ?? '')
[[nodiscard]] inline std::string str_or_empty(const Value& v) {
  if (nullish(v)) {
    return {};
  }
  return v.is_string() ? v.as_string() : base::js_string(v);
}
/// v || d, as a value
[[nodiscard]] inline Value or_value(const Value& v, const Value& d) {
  return v.truthy() ? v : d;
}
/// hex digits of v (lower or upper case), padded to width
[[nodiscard]] inline std::string hex_digits(std::uint64_t v, bool up, int width = 0) {
  std::string s;
  do {
    const auto d = static_cast<unsigned>(v & 15U);
    s.insert(s.begin(), static_cast<char>(d < 10 ? '0' + d : (up ? 'A' : 'a') + d - 10));
    v >>= 4U;
  } while (v != 0);
  while (std::cmp_less(s.size(), width)) {
    s.insert(s.begin(), '0');
  }
  return s;
}

struct D3 {
  double x = 0, y = 0, z = 0;
};
/// P(p): { x: num(p?.x), y: num(p?.y), z: num(p?.z) }
[[nodiscard]] inline D3 P(const Value& p) {
  return {numk(p, "x"), numk(p, "y"), numk(p, "z")};
}

struct JulianPair {
  double day = 0, ms = 0;
};

}  // namespace cc::dwg::w
