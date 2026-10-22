// SPDX-License-Identifier: Apache-2.0
// Math functions with JavaScript's last bit: V8 runs Math.atan / Math.atan2 through fdlibm's algorithms
// (src/base/ieee754.cc), which differ from the C library's by an ulp now and then. A port compares equal with the JS
// within its tolerance either way — except where a result is folded into a range (normAngle: −2.2e-16 → 2π, 0 → 0),
// so the ports call these where the JS calls Math.atan2. Node's Math.sin / Math.cos are fdlibm's too (measured: one ulp
// from the C library's now and then), and a written DWG keeps every bit: js_sin / js_cos. (The rest stay the C
// library's until a parity difference asks for them.)
#pragma once

#include <array>
#include <span>

namespace cc::base {

/// Math.hypot as V8 computes it (builtins math.tq): the arguments scaled by the largest, squared and summed with
/// Kahan's compensation, then sqrt × the largest; ±Infinity anywhere wins over NaN, NaN over the rest. Differs from
/// the C library's hypot in the last bit now and then, which a written DWG keeps.
[[nodiscard]] double js_hypot(std::span<const double> v) noexcept;
[[nodiscard]] inline double js_hypot(double a, double b) noexcept {
  const std::array<double, 2> v{a, b};
  return js_hypot(v);
}
[[nodiscard]] inline double js_hypot(double a, double b, double c) noexcept {
  const std::array<double, 3> v{a, b, c};
  return js_hypot(v);
}

/// Math.sin / Math.cos: fdlibm's, as V8 (Node) computes them — not always the nearest double, unlike the C library's.
[[nodiscard]] double js_sin(double x) noexcept;
[[nodiscard]] double js_cos(double x) noexcept;
/// Math.tan (fdlibm's, as V8 has it).
[[nodiscard]] double js_tan(double x) noexcept;
/// Math.atan
[[nodiscard]] double js_atan(double x) noexcept;
/// Math.atan2
[[nodiscard]] double js_atan2(double y, double x) noexcept;

}  // namespace cc::base
