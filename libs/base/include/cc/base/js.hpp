// SPDX-License-Identifier: Apache-2.0
// JavaScript's string and number semantics, for ports that must give what the 0.5 JS gives: String#trim's white space,
// parseFloat, parseInt(s, 10), UTF-16 lengths and units, and the ICU root collation that
// `a.localeCompare(b, undefined, { numeric: true })` uses (measured in Node with ICU 78; tests/js_test.cpp).
// Strings are UTF-8 (lone surrogates as WTF-8, as cc::base::Value keeps them).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cc::base {

/// String#trimStart / String#trim (Unicode white space and line terminators).
[[nodiscard]] std::string_view js_trim_start(std::string_view s) noexcept;
[[nodiscard]] std::string_view js_trim(std::string_view s) noexcept;
/// `s.trim() === ''`
[[nodiscard]] bool js_blank(std::string_view s) noexcept;

/// parseFloat(s): the longest decimal (or Infinity) prefix after white space; NaN when there is none.
[[nodiscard]] double js_parse_float(std::string_view s);
/// parseInt(s, 10): sign and decimal digits after white space; NaN when there is none.
[[nodiscard]] double js_parse_int(std::string_view s) noexcept;

/// UTF-8 → UTF-16 code units (what JS indexes), and the count of them (`s.length`).
[[nodiscard]] std::vector<std::uint16_t> to_utf16(std::string_view s);
[[nodiscard]] std::size_t utf16_length(std::string_view s) noexcept;

/// `a.localeCompare(b, undefined, { numeric: true })` for the root locale: < 0, 0 or > 0.
[[nodiscard]] int js_locale_compare_numeric(std::string_view a, std::string_view b);

}  // namespace cc::base
