// SPDX-License-Identifier: Apache-2.0
#include "cc/base/js.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

// ---------------------------------------------------------------- collation
namespace {

/// One collation element: a primary weight (or a number for a digit run), an accent and a case weight.
struct Element {
  std::uint32_t primary = 0;
  std::string number;  // digits without leading zeros, when this is a digit run
  std::uint8_t accent = 0;
  std::uint8_t case_ = 0;
};

constexpr std::uint32_t kDigits = 100;
constexpr std::uint32_t kLetters = 200;
constexpr std::uint32_t kOther = 1000;

/// ICU root order of the ASCII punctuation and symbols (measured: they sort before digits, digits before letters).
constexpr std::string_view kPunct = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$";

/// Latin-1 letters → base letter, accent number, upper case; 0 when not a letter we fold.
struct Fold {
  char base;
  char base2;  // second letter of an expansion (ß → ss, æ → ae), or 0
  std::uint8_t accent;
  bool upper;
};
Fold fold_latin1(std::uint32_t cp) noexcept {
  if (cp == 0xDF) {
    return {'s', 's', 1, false};
  }
  if (cp == 0xC6 || cp == 0xE6) {
    return {'a', 'e', 1, cp == 0xC6};
  }
  using namespace std::string_view_literals;
  static constexpr std::string_view kBase =  // U+00C0..U+00FF, '\0' = not a letter
      "AAAAAAACEEEEIIII"
      "DNOOOOO\0OUUUUYTs"
      "aaaaaaaceeeeiiii"
      "dnooooo\0ouuuuyty"sv;
  if (cp < 0xC0 || cp > 0xFF) {
    return {0, 0, 0, false};
  }
  const char b = kBase[cp - 0xC0];
  if (b == '\0') {
    return {0, 0, 0, false};
  }
  const bool upper = b >= 'A' && b <= 'Z';
  return {static_cast<char>(upper ? b - 'A' + 'a' : b), 0, static_cast<std::uint8_t>(2 + (cp & 7U)), upper};
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
      out.push_back(Element{kDigits, std::string(run), 0, 0});
      i = j;
      continue;
    }
    i += n;
    if (cp >= 'a' && cp <= 'z') {
      out.push_back(Element{kLetters + (cp - 'a'), {}, 0, 0});
    } else if (cp >= 'A' && cp <= 'Z') {
      out.push_back(Element{kLetters + (cp - 'A'), {}, 0, 1});
    } else if (const Fold f = fold_latin1(cp); f.base != 0) {
      out.push_back(Element{kLetters + static_cast<std::uint32_t>(f.base - 'a'),
                            {},
                            f.accent,
                            f.upper ? std::uint8_t{1} : std::uint8_t{0}});
      if (f.base2 != 0) {
        out.push_back(Element{kLetters + static_cast<std::uint32_t>(f.base2 - 'a'), {}, f.accent, 0});
      }
    } else if (cp == '\t' || cp == '\n' || cp == '\v' || cp == '\f' || cp == '\r') {
      out.push_back(Element{1, {}, 0, 0});
    } else if (cp < 0x20 || cp == 0x7F) {
      continue;  // control characters are ignorable
    } else if (cp < 0x80) {
      const auto at = kPunct.find(static_cast<char>(cp));
      out.push_back(
          Element{2 + static_cast<std::uint32_t>(at == std::string_view::npos ? kPunct.size() : at), {}, 0, 0});
    } else {
      out.push_back(Element{kOther + cp, {}, 0, 0});
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
