// SPDX-License-Identifier: Apache-2.0
// Math functions with JavaScript's last bit: V8 runs Math.atan / Math.atan2 through fdlibm's algorithms
// (src/base/ieee754.cc), which differ from the C library's by an ulp now and then. A port compares equal with the JS
// within its tolerance either way — except where a result is folded into a range (normAngle: −2.2e-16 → 2π, 0 → 0),
// so the ports call these where the JS calls Math.atan2. (sin, cos and the rest stay the C library's until a parity
// difference asks for them: Chromium no longer runs them through fdlibm.)
#pragma once

namespace cc::base {

/// Math.atan
[[nodiscard]] double js_atan(double x) noexcept;
/// Math.atan2
[[nodiscard]] double js_atan2(double y, double x) noexcept;

}  // namespace cc::base
