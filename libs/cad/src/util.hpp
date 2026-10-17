// SPDX-License-Identifier: Apache-2.0
// cc::cad internals: the JavaScript semantics the ported CadCraft modules lean on (property access that may meet
// undefined, ToNumber, String()), the 2D affine helpers of geom.js and the text clean-ups of dxf.js / dwg-import.js.
// Ported from CadCraft 0.5 (assets/apps/cadcraft/dev/geom.js, dxf.js, dwg-import.js), function for function.
#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>

#include "cc/base/value.hpp"

namespace cc::cad::detail {

using base::Array;
using base::Object;
using base::Value;

inline constexpr double kTau = 2 * std::numbers::pi;
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// A shared undefined.
const Value& undef();
/// o.k for a container member (undefined when o is not an object; a compact point throws: read points with get()).
const Value& at(const Value& o, std::string_view k);
/// a[i] (undefined outside the array, or when a is not an array).
const Value& item(const Value& a, std::size_t i);
/// The same of a temporary (g() returns by value): the member comes back by value, so a reference bound to the result
/// never outlives the temporary it was taken from.
// NOLINTBEGIN(cppcoreguidelines-rvalue-reference-param-not-moved) — the overloads exist to catch temporaries, not to
// move
inline Value at(Value&& o, std::string_view k) {
  return at(static_cast<const Value&>(o), k);
}
inline Value item(Value&& a, std::size_t i) {
  return item(static_cast<const Value&>(a), i);
}
// NOLINTEND(cppcoreguidelines-rvalue-reference-param-not-moved)
/// o.k by value: works on compact points too (x, y, z).
inline Value g(const Value& o, std::string_view k) {
  return o.get(k);
}
/// ToNumber(v): what JS arithmetic does with v.
inline double num(const Value& v) {
  return base::js_to_number(v);
}
/// Number.isFinite(v)
inline bool finite(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}
/// v || d / v ?? d
inline Value or_(const Value& v, const Value& d) {
  return v.truthy() ? v : d;
}
inline bool nullish(const Value& v) {
  return v.is_undefined() || v.is_null();
}
inline Value nn(const Value& v, const Value& d) {
  return nullish(v) ? d : v;
}
/// String(v)
inline std::string str(const Value& v) {
  return base::js_string(v);
}
/// Array.isArray(v) && v.length
inline std::size_t len(const Value& v) {
  return v.is_array() ? v.as_array().size() : 0;
}

// ---- geom.js
/// normAngle: into [0, 2π).
double norm_angle(double a);
using Mat = std::array<double, 6>;  // [a b c d e f]: x' = a x + c y + e, y' = b x + d y + f
Mat m_mul(const Mat& m, const Mat& n);
inline Mat m_translate(double x, double y) {
  return {1, 0, 0, 1, x, y};
}
inline Mat m_scale(double sx, double sy) {
  return {sx, 0, 0, sy, 0, 0};
}
Mat m_rotate(double r);
inline double m_det(const Mat& m) {
  return m[0] * m[3] - m[1] * m[2];
}
/// mInverse: false when m is singular.
bool m_inverse(const Mat& m, Mat& out);
/// insertMatrix(e, blk) for a reference e of block blk.
Mat insert_matrix(const Value& e, const Value& blk);
/// txEntity(e, m) for a text entity.
Value tx_text(const Value& e, const Mat& m);

// ---- text
/// dxf.js mtextPlain: MTEXT formatting codes → plain text with \n line breaks.
std::string mtext_plain(std::string_view in);
/// dwg-import.js plain(): %%c ⌀, %%d °, %%p ±, %%u / %%o dropped, %%% %, \U+XXXX.
std::string plain(std::string_view s);
/// geom.js normTag: an attribute tag as CAD programs keep it (trimmed, upper case, white space runs → _, 255 units).
std::string norm_tag(const Value& t);
/// attrs.js oneLine: runs of \r / \n → one space.
std::string one_line(std::string_view s);
/// /^standard$/i
bool is_standard(std::string_view s);
/// s.toUpperCase() === other (ASCII and Unicode upper case, as JS)
bool upper_eq(std::string_view s, std::string_view upper);
/// String#slice(0, n) in UTF-16 units.
std::string js_slice(std::string_view s, std::size_t n);
/// '#' + (n & 0xffffff).toString(16).padStart(6, '0')
std::string hex_color(double n);

}  // namespace cc::cad::detail
