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

/// String#trimStart / String#trim / String#trimEnd (Unicode white space and line terminators).
/// The length of the JS white space (WhiteSpace or LineTerminator, `\\s`) that starts at byte i of UTF-8, or 0.
[[nodiscard]] std::size_t js_space_at(std::string_view s, std::size_t i) noexcept;
[[nodiscard]] std::string_view js_trim_start(std::string_view s) noexcept;
[[nodiscard]] std::string_view js_trim(std::string_view s) noexcept;
[[nodiscard]] std::string_view js_trim_end(std::string_view s) noexcept;
/// `s.trim() === ''`
[[nodiscard]] bool js_blank(std::string_view s) noexcept;

/// parseFloat(s): the longest decimal (or Infinity) prefix after white space; NaN when there is none.
[[nodiscard]] double js_parse_float(std::string_view s);
/// parseInt(s, 10): sign and decimal digits after white space; NaN when there is none.
[[nodiscard]] double js_parse_int(std::string_view s) noexcept;
/// Number(s) (StringToNumber): the whole string, trimmed, must be a number — decimal, Infinity, or 0x / 0o / 0b
/// digits; "" → 0; anything else → NaN.
[[nodiscard]] double js_number(std::string_view s);

/// Number#toFixed(digits) (0–100): the exact value rounded to `digits` decimals, an exact half away from zero (the C
/// library rounds it to even); |x| ≥ 1e21 and non-finite x as String(x); -0 → "0", a negative that rounds to zero
/// keeps its sign ("-0.00").
[[nodiscard]] std::string js_to_fixed(double x, int digits);
/// Number#toExponential(digits) (0–100): "d.ddde+x" with an exact half rounded away from zero.
[[nodiscard]] std::string js_to_exponential(double x, int digits);

/// UTF-8 → UTF-16 code units (what JS indexes), and the count of them (`s.length`).
[[nodiscard]] std::vector<std::uint16_t> to_utf16(std::string_view s);
[[nodiscard]] std::size_t utf16_length(std::string_view s) noexcept;

/// String#toUpperCase / #toLowerCase: the full Unicode mappings (generated from Node: js_case.inc), not
/// locale-specific; toLowerCase's final-sigma rule (Σ at the end of a word → ς) is not applied (docs/gaps.md).
[[nodiscard]] std::string js_to_upper(std::string_view s);
[[nodiscard]] std::string js_to_lower(std::string_view s);

/// `a.localeCompare(b, undefined, { numeric: true })` for the root locale: < 0, 0 or > 0.
[[nodiscard]] int js_locale_compare_numeric(std::string_view a, std::string_view b);

}  // namespace cc::base
