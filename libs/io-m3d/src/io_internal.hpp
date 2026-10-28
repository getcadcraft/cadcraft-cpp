// SPDX-License-Identifier: Apache-2.0
// io.js's number formats and small helpers, shared by the exporters and importers.
#pragma once

#include <string>
#include <string_view>

namespace cc::m3d::io::detail {

/// String(x) for a number (JS Number#toString).
[[nodiscard]] std::string num_text(double x);
/// +x.toPrecision(p) (the number it reads back as).
[[nodiscard]] double to_precision(double x, int p);
/// xp(c): an imported texture exports as its colour (its faces then share one material).
[[nodiscard]] std::string xp(std::string_view c);
/// f6(x): +x.toFixed(6) as text, -0 as 0 (OBJ).
[[nodiscard]] std::string f6(double x);
/// s.replace(/\s+/g, '_')
[[nodiscard]] std::string replace_spaces(std::string_view s);

}  // namespace cc::m3d::io::detail
