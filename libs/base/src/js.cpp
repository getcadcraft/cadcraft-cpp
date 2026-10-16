// SPDX-License-Identifier: Apache-2.0
#include "cc/base/js.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::base {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

unsigned byte_at(std::string_view s, std::size_t k) noexcept {
  return k < s.size() ? static_cast<unsigned char>(s[k]) : 0U;
}

/// The length of the white space sequence that starts at i, or 0.
std::size_t space_at(std::string_view s, std::size_t i) noexcept {
  const unsigned c = byte_at(s, i);
  if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') {
    return 1;
  }
  const unsigned c1 = byte_at(s, i + 1);
  const unsigned c2 = byte_at(s, i + 2);
  if (c == 0xC2 && c1 == 0xA0) {  // U+00A0
    return 2;
  }
  const bool three = (c == 0xE1 && c1 == 0x9A && c2 == 0x80) ||  // U+1680
                     (c == 0xE2 && c1 == 0x80 &&
                      (c2 <= 0x8A || c2 == 0xA8 || c2 == 0xA9 || c2 == 0xAF)) ||  // U+2000–200A, 2028, 2029, 202F
                     (c == 0xE2 && c1 == 0x81 && c2 == 0x9F) ||                   // U+205F
                     (c == 0xE3 && c1 == 0x80 && c2 == 0x80) ||                   // U+3000
                     (c == 0xEF && c1 == 0xBB && c2 == 0xBF);                     // U+FEFF
  return three && c2 >= 0x80 ? 3 : 0;
}

/// One code point of WTF-8 at i (a stray byte decodes as itself) and its length.
std::uint32_t code_point_at(std::string_view s, std::size_t i, std::size_t& len) noexcept {
  const unsigned c = byte_at(s, i);
  auto cont = [&](std::size_t k) { return (byte_at(s, i + k) & 0xC0U) == 0x80U; };
  if (c >= 0xF0 && c <= 0xF4 && i + 3 < s.size() && cont(1) && cont(2) && cont(3)) {
    len = 4;
    return ((c & 0x07U) << 18U) | ((byte_at(s, i + 1) & 0x3FU) << 12U) | ((byte_at(s, i + 2) & 0x3FU) << 6U) |
           (byte_at(s, i + 3) & 0x3FU);
  }
  if (c >= 0xE0 && c <= 0xEF && i + 2 < s.size() && cont(1) && cont(2)) {
    len = 3;
    return ((c & 0x0FU) << 12U) | ((byte_at(s, i + 1) & 0x3FU) << 6U) | (byte_at(s, i + 2) & 0x3FU);
  }
  if (c >= 0xC2 && c <= 0xDF && i + 1 < s.size() && cont(1)) {
    len = 2;
    return ((c & 0x1FU) << 6U) | (byte_at(s, i + 1) & 0x3FU);
  }
  len = 1;
  return c;
}

}  // namespace

std::string_view js_trim_start(std::string_view s) noexcept {
  for (std::size_t n = space_at(s, 0); n != 0; n = space_at(s, 0)) {
    s.remove_prefix(n);
  }
  return s;
}

std::string_view js_trim(std::string_view s) noexcept {
  s = js_trim_start(s);
  for (bool cut = true; cut;) {
    cut = false;
    for (std::size_t back = 1; back <= 3 && back <= s.size(); ++back) {
      if (space_at(s, s.size() - back) == back) {
        s.remove_suffix(back);
        cut = true;
        break;
      }
    }
  }
  return s;
}

bool js_blank(std::string_view s) noexcept {
  return js_trim_start(s).empty();
}

double js_parse_float(std::string_view s) {
  s = js_trim_start(s);
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    ++i;
  }
  if (s.substr(i, 8) == "Infinity") {
    return s[0] == '-' ? -kInf : kInf;
  }
  const std::size_t d0 = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
  }
  bool any = i > d0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    const std::size_t f0 = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
    }
    any = any || i > f0;
  }
  if (!any) {
    return kNaN;
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    std::size_t j = i + 1;
    if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
      ++j;
    }
    const std::size_t e0 = j;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
      ++j;
    }
    if (j > e0) {
      i = j;
    }
  }
  std::string lit(s.substr(0, i));
  if (lit[0] == '+') {
    lit.erase(0, 1);
  }
  double d = 0;
  const std::string_view v(lit);
  const auto [ptr, ec] =
      std::from_chars(v.data(), v.data() + v.size(), d);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  (void)ptr;
  if (ec == std::errc::result_out_of_range) {  // from_chars leaves d alone: JS gives ±Infinity or ±0
    const bool neg = lit[0] == '-';
    const auto e = lit.find_first_of("eE");
    const bool tiny = e != std::string::npos && e + 1 < lit.size() && lit[e + 1] == '-';
    if (tiny) {
      return neg ? -0.0 : 0.0;
    }
    return neg ? -kInf : kInf;
  }
  return d;
}

double js_parse_int(std::string_view s) noexcept {
  s = js_trim_start(s);
  std::size_t i = 0;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    ++i;
  }
  const std::size_t d0 = i;
  double v = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i] - '0');
    ++i;
  }
  if (i == d0) {
    return kNaN;
  }
  return neg ? -v : v;
}

std::vector<std::uint16_t> to_utf16(std::string_view s) {
  std::vector<std::uint16_t> u;
  u.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    std::size_t n = 1;
    std::uint32_t cp = code_point_at(s, i, n);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      u.push_back(static_cast<std::uint16_t>(0xD800U + (cp >> 10U)));
      u.push_back(static_cast<std::uint16_t>(0xDC00U + (cp & 0x3FFU)));
    } else {
      u.push_back(static_cast<std::uint16_t>(cp));
    }
    i += n;
  }
  return u;
}

std::size_t utf16_length(std::string_view s) noexcept {
  std::size_t units = 0;
  for (std::size_t i = 0; i < s.size();) {
    std::size_t n = 1;
    units += code_point_at(s, i, n) >= 0x10000 ? std::size_t{2} : std::size_t{1};
    i += n;
  }
  return units;
}

// ---------------------------------------------------------------- case
namespace {

struct CaseMap {
  std::uint32_t cp;
  std::array<std::uint32_t, 3> to;
};
#include "js_case.inc"

std::string map_case(std::string_view s, std::span<const CaseMap> table) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    std::size_t n = 1;
    const std::uint32_t cp = code_point_at(s, i, n);
    const auto it = std::ranges::lower_bound(table, cp, {}, &CaseMap::cp);
    if (it != table.end() && it->cp == cp) {
      for (const std::uint32_t c : it->to) {
        if (c != 0) {
          append_code_point(out, c);
        }
      }
    } else {
      out.append(s.substr(i, n));  // as it was (a stray byte stays a stray byte)
    }
    i += n;
  }
  return out;
}

}  // namespace

std::string js_to_upper(std::string_view s) {
  return map_case(s, kUpper);
}
std::string js_to_lower(std::string_view s) {
  return map_case(s, kLower);
}

// ---------------------------------------------------------------- collation
namespace {

/// One collation element: a primary weight (or a number for a digit run), an accent and a tertiary (case) weight.
struct Element {
  std::uint32_t primary = 0;
  std::string number;  // digits without leading zeros, when this is a digit run
  std::uint8_t accent = 64;
  std::uint8_t case_ = 0;
};

/// A character's place in ICU's root order, measured from Node (tools/gen/js-collation.mjs).
struct Collation {
  std::uint32_t cp;
  std::uint8_t kind;  // 0 symbol, 1 letter-like, 2 ignorable
  std::uint16_t rank;
  std::string_view letters;
  std::uint8_t accent;
  std::uint8_t tertiary;
};
#include "js_collation.inc"

constexpr std::uint32_t kSpace = 1;  // white space sorts first
constexpr std::uint32_t kSymbols = 2;
constexpr std::uint32_t kDigits = kSymbols + kSymbolRanks + 1;
constexpr std::uint32_t kLetters = kDigits + 1;
constexpr std::uint32_t kOther = kLetters + 26;

/// Unicode's default-ignorable code points and the C1 controls: no weight at all in ICU's root order.
bool ignorable(std::uint32_t cp) noexcept {
  return (cp >= 0x80 && cp <= 0x9F) || cp == 0x34F || cp == 0x61C || (cp >= 0x180B && cp <= 0x180F) ||
         (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
         (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || (cp >= 0xFFF0 && cp <= 0xFFF8) ||
         (cp >= 0xE0000 && cp <= 0xE0FFF);
}

std::vector<Element> elements(std::string_view s) {
  std::vector<Element> out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    std::size_t n = 1;
    const std::uint32_t cp = code_point_at(s, i, n);
    if (cp >= '0' && cp <= '9') {
      std::size_t j = i;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
        ++j;
      }
      std::string_view run = s.substr(i, j - i);
      while (run.size() > 1 && run[0] == '0') {
        run.remove_prefix(1);
      }
      out.push_back(Element{kDigits, std::string(run), 64, 0});
      i = j;
      continue;
    }
    i += n;
    const auto it = std::ranges::lower_bound(std::span(kCollation), cp, {}, &Collation::cp);
    if (it != std::span(kCollation).end() && it->cp == cp) {
      if (it->kind == 0) {
        out.push_back(Element{kSymbols + it->rank, {}, 64, 0});
      } else if (it->kind == 1) {
        for (const char c : it->letters) {
          out.push_back(Element{kLetters + static_cast<std::uint32_t>(c - 'a'), {}, it->accent, it->tertiary});
        }
      }
      continue;  // kind 2: ignorable
    }
    if (ignorable(cp)) {
      continue;
    }
    if (cp >= 0x300 && cp <= 0x36F) {  // a combining mark: an accent on the letter before it (approximate weight)
      if (!out.empty()) {
        out.back().accent = static_cast<std::uint8_t>(65 + ((cp - 0x300) % 120));
      }
      continue;
    }
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' || cp == '\f' || cp == '\r') {
      out.push_back(Element{kSpace, {}, 64, 0});
    } else if (cp >= 0x20) {                           // (control characters are ignorable)
      out.push_back(Element{kOther + cp, {}, 64, 0});  // beyond the measured set: code point order, after Latin
    }
  }
  return out;
}

int numeric_compare(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  const int c = a.compare(b);
  if (c == 0) {
    return 0;
  }
  return c < 0 ? -1 : 1;
}

}  // namespace

int js_locale_compare_numeric(std::string_view a, std::string_view b) {
  const std::vector<Element> x = elements(a);
  const std::vector<Element> y = elements(b);
  const std::size_t n = std::min(x.size(), y.size());
  for (std::size_t k = 0; k < n; ++k) {  // primary level
    if (x[k].primary != y[k].primary) {
      return x[k].primary < y[k].primary ? -1 : 1;
    }
    if (x[k].primary == kDigits) {
      if (const int c = numeric_compare(x[k].number, y[k].number); c != 0) {
        return c;
      }
    }
  }
  if (x.size() != y.size()) {
    return x.size() < y.size() ? -1 : 1;
  }
  for (std::size_t k = 0; k < n; ++k) {  // accents
    if (x[k].accent != y[k].accent) {
      return x[k].accent < y[k].accent ? -1 : 1;
    }
  }
  for (std::size_t k = 0; k < n; ++k) {  // case: lower first
    if (x[k].case_ != y[k].case_) {
      return x[k].case_ < y[k].case_ ? -1 : 1;
    }
  }
  return 0;
}

}  // namespace cc::base
